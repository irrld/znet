//
//    Copyright 2026 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//
// API stability: experimental (see the wiki, API Stability)

#ifndef ZNET_P2P_TRAVERSAL_SERVER_H_
#define ZNET_P2P_TRAVERSAL_SERVER_H_

#include "znet/compat.h"
#include "znet/inet_addr.h"
#include "znet/metrics.h"
#include "znet/task.h"
#include "znet/types.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

struct sockaddr;

namespace znet {
namespace backends {
class AdmissionControl;
class UDPSocket;
}  // namespace backends

namespace p2p {

class TraversalServer;

/** @brief What a Reflector counts. Experimental with the rest of traversal,
 *         which is why it does not live in metrics.h. */
struct ReflectorMetrics {
  uint64_t probes_answered = 0;
  uint64_t probes_refused = 0;  /**< Too short, or over the per-source throttle. */
};

/** @brief What a Relay counts. Experimental with the rest of traversal, which
 *         is why it does not live in metrics.h. */
struct RelayMetrics {
  uint64_t allocations_active = 0;  /**< Sampled, not accumulated. */
  uint64_t allocations_total = 0;
  uint64_t allocations_expired = 0;  /**< Freed unbound, idle, or by Free(). */
  uint64_t binds_accepted = 0;
  uint64_t binds_refused = 0;  /**< Wrong token, or both slots taken. */
  uint64_t datagrams_relayed = 0;
  uint64_t bytes_relayed = 0;
  /** @brief Unknown channel, a source that never bound, nobody bound on the
   *         other side yet, or a datagram that was neither a bind nor a relay. */
  uint64_t datagrams_dropped = 0;
};

/** @brief What a Reflector answers with and how hard it is throttled. */
struct ReflectorConfig {
  /** @brief Reflect probes one source may send per probe_window; beyond it
   *         they are dropped. Zero disables the throttle. */
  uint32_t max_probes_per_source = 30;
  std::chrono::milliseconds probe_window{10000};
};

/** @brief What a Relay holds and when it frees pairings. */
struct RelayConfig {
  /** @brief An allocation nobody has bound both sides of is freed after
   *         this. */
  std::chrono::milliseconds bind_timeout{15000};
  /** @brief A bound allocation with no traffic is freed after this. ZDT
   *         keepalives keep a live session well inside it. */
  std::chrono::milliseconds idle_timeout{30000};
  /** @brief Allocate() refuses with Result::ServerFull beyond this. */
  size_t max_allocations = 4096;
};

/** @brief What a TraversalServer listens on and which halves it runs. */
struct TraversalServerConfig {
  std::string bind_address = "0.0.0.0";
  /** @brief The one port both halves share. Zero picks an ephemeral one;
   *         read it back with address(). */
  PortNumber port = 5002;
  /** @brief Run the reflector half, reachable at reflector(). */
  bool enable_reflector = true;
  /** @brief Run the relay half, reachable at relay(). */
  bool enable_relay = true;
  ReflectorConfig reflector;
  RelayConfig relay;
};

/**
 * @brief The reflect half: answers a Reflect probe with the address it arrived
 *        from, which is how Agent::Gather learns a socket's public mapping.
 *
 * This is znet's own STUN: a peer sends Reflect, the reflector replies with the
 * observed source address. A reply is padded larger than its question so the
 * port cannot amplify, and a per-source throttle keeps it a low-rate path.
 *
 * Owned by a TraversalServer and driven from its thread; construct one through
 * the server, not on its own.
 */
class Reflector {
 public:
  explicit Reflector(const ReflectorConfig& config);
  ~Reflector();
  Reflector(const Reflector&) = delete;

  /** @brief A snapshot of the counters. Zeroed with metrics compiled out. */
  ZNET_NODISCARD ReflectorMetrics metrics() const;

 private:
  friend class TraversalServer;

  // answers one Reflect probe. The server's loop has already peeked the id.
  void OnReflect(backends::UDPSocket& socket, const uint8_t* data, size_t len,
                 const sockaddr* from, SockLen from_len);

  std::unique_ptr<backends::AdmissionControl> throttle_;
  mutable std::mutex mutex_;
  ReflectorMetrics metrics_;
};

/**
 * @brief The relay half: forwards datagrams between the two sources bound to
 *        each pairing, told apart by a channel header. The fallback for two
 *        peers that cannot punch.
 *
 * A broker (RendezvousServer, or one of your own) calls Allocate() when it
 * pairs two peers and hands both the same token as a Relayed candidate at the
 * server's address(). Each peer sends RelayBind with the token from its punch
 * socket until RelayBound comes back naming the pairing's channel; from then on
 * everything either sends wrapped in that channel's header is forwarded to the
 * other, header and all. Nothing inside is touched, so the ZDT handshake and
 * the session encryption run through it exactly as they would directly, and the
 * relay holds nothing that decrypts: it reads the four-byte header, and the
 * offline header of a bind.
 *
 * What keeps it safe: a random 64-bit token per allocation, exactly two sources
 * per pairing (a third is refused), nothing forwarded from a source that did
 * not bind that pairing, binds and replies never forwarded, allocations that
 * nobody binds or that go idle freed on their timers, and a cap on how many
 * exist.
 *
 * What keeps it cheap: a forwarded datagram costs a recvfrom into a stack
 * buffer, a hash lookup on the channel, a compare against two addresses and one
 * sendto, with no allocation. Pairings cost memory, not ports or descriptors.
 *
 * Owned by a TraversalServer and driven from its thread; construct one through
 * the server, not on its own.
 */
class Relay {
 public:
  /** @brief One pairing: the channel its datagrams carry and the token both
   *         peers bind with. The token is what the broker hands out; peers
   *         learn the channel when they bind. */
  struct Allocation {
    uint32_t channel = 0;
    uint64_t token = 0;
  };

  explicit Relay(const RelayConfig& config);
  ~Relay();
  Relay(const Relay&) = delete;

  /**
   * @brief Opens one pairing. Any thread.
   *
   * @return Success with `out` filled; ServerFull at max_allocations;
   *         AlreadyStopped before the server started.
   */
  Result Allocate(Allocation& out);

  /**
   * @brief Frees one allocation before its timers would, e.g. when the broker
   *        knows the pairing is over. Any thread. PeerNotFound when no
   *        allocation has that channel.
   */
  Result Free(uint32_t channel);

  /** @brief Live allocations, bound or not. */
  ZNET_NODISCARD size_t allocation_count() const;

  /** @brief A snapshot of the counters. Zeroed with metrics compiled out. */
  ZNET_NODISCARD RelayMetrics metrics() const;

 private:
  friend class TraversalServer;

  // a bound source, kept as the raw sockaddr it arrived as so the forward
  // path compares and sends without building an InetAddress
  struct Slot {
    alignas(8) unsigned char address[128] = {};  // holds a sockaddr_storage
    SockLen length = 0;                          // zero until bound
  };

  struct Pairing {
    uint32_t channel = 0;
    uint64_t token = 0;
    Slot slots[2];
    std::chrono::steady_clock::time_point created;
    std::chrono::steady_clock::time_point last_traffic;
  };

  void Start();
  void Stop();
  // forwards one relayed datagram. The server's loop parsed the channel.
  void OnForward(backends::UDPSocket& socket, uint32_t channel, uint8_t* data,
                 size_t len, const sockaddr* from,
                 std::chrono::steady_clock::time_point now);
  void OnBind(backends::UDPSocket& socket, const uint8_t* data, size_t len,
              const sockaddr* from, SockLen from_len,
              std::chrono::steady_clock::time_point now);
  // a datagram that was neither a bind nor a relay: counts one drop
  void OnUnrecognized();
  void Expire(std::chrono::steady_clock::time_point now);
  // drops one pairing; caller holds mutex_
  void Release(uint32_t channel, const char* why);

  RelayConfig config_;
  std::atomic<bool> running_{false};

  // pairings_ and metrics_: the server's loop owns them, Allocate(), Free()
  // and the readers come from any thread
  mutable std::mutex mutex_;
  std::unordered_map<uint32_t, Pairing> pairings_;
  std::unordered_map<uint64_t, uint32_t> channels_by_token_;
  uint32_t next_channel_ = 1;
  RelayMetrics metrics_;
};

/**
 * @brief One UDP port that runs a Reflector and a Relay together: znet's STUN
 *        and TURN on a single socket, the way coturn serves both.
 *
 * The port answers Reflect probes (the reflector) and forwards relayed sessions
 * (the relay); a broker gathers peers against address() and allocates relay
 * fallbacks through relay(). Either half can be turned off in the config, for a
 * reflect-only or relay-only deployment.
 *
 * One socket and one thread carry both. A datagram is classified once by its
 * first bytes and handed to the half that owns it; the two halves share nothing
 * but the socket. See Reflector and Relay for what each does and how it stays
 * safe and cheap.
 */
class TraversalServer {
 public:
  explicit TraversalServer(const TraversalServerConfig& config);
  ~TraversalServer();
  TraversalServer(const TraversalServer&) = delete;

  /** @brief Binds the port and starts the thread that drives both halves. */
  Result Start();

  /** @brief Frees every allocation and releases the socket. */
  void Stop();

  /** @brief The endpoint as bound: what peers probe, bind and relay through.
   *         Null before Start(). */
  ZNET_NODISCARD std::shared_ptr<InetAddress> address() const {
    return address_;
  }

  /** @brief The relay half, or null when enable_relay is false. */
  ZNET_NODISCARD Relay* relay() { return relay_.get(); }
  ZNET_NODISCARD const Relay* relay() const { return relay_.get(); }

  /** @brief The reflector half, or null when enable_reflector is false. */
  ZNET_NODISCARD Reflector* reflector() { return reflector_.get(); }
  ZNET_NODISCARD const Reflector* reflector() const { return reflector_.get(); }

 private:
  void Loop();

  TraversalServerConfig config_;
  std::shared_ptr<backends::UDPSocket> socket_;
  std::shared_ptr<InetAddress> address_;
  std::unique_ptr<Reflector> reflector_;
  std::unique_ptr<Relay> relay_;
  Task task_;
  std::atomic<bool> running_{false};
};

}  // namespace p2p
}  // namespace znet

#endif  // ZNET_P2P_TRAVERSAL_SERVER_H_

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

#ifndef ZNET_P2P_RELAY_SERVER_H_
#define ZNET_P2P_RELAY_SERVER_H_

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

namespace znet {
namespace backends {
class AdmissionControl;
class UDPSocket;
}  // namespace backends

namespace p2p {

/** @brief What a RelayServer counts. Experimental with the rest of the
 *         relay, which is why it does not live in metrics.h. */
struct RelayMetrics {
  uint64_t allocations_active = 0;  /**< Sampled, not accumulated. */
  uint64_t allocations_total = 0;
  uint64_t allocations_expired = 0;  /**< Freed unbound, idle, or by Free(). */
  uint64_t binds_accepted = 0;
  uint64_t binds_refused = 0;  /**< Wrong token, or both slots taken. */
  uint64_t datagrams_relayed = 0;
  uint64_t bytes_relayed = 0;
  /** @brief Unknown channel, a source that never bound, or nobody bound on
   *         the other side yet. */
  uint64_t datagrams_dropped = 0;
  uint64_t probes_answered = 0;
  uint64_t probes_refused = 0;  /**< Too short, or over the per-source throttle. */
};

/** @brief What a RelayServer listens on and when it frees pairings. */
struct RelayServerConfig {
  std::string bind_address = "0.0.0.0";
  /** @brief The one port everything happens on. Zero picks an ephemeral
   *         one; read it back with address(). */
  PortNumber port = 5002;
  /** @brief An allocation nobody has bound both sides of is freed after
   *         this. */
  std::chrono::milliseconds bind_timeout{15000};
  /** @brief A bound allocation with no traffic is freed after this. ZDT
   *         keepalives keep a live session well inside it. */
  std::chrono::milliseconds idle_timeout{30000};
  /** @brief Allocate() refuses with Result::ServerFull beyond this. */
  size_t max_allocations = 4096;
  /** @brief Reflect probes one source may send per probe_window; beyond
   *         it they are dropped. Zero disables the throttle. */
  uint32_t max_probes_per_source = 30;
  std::chrono::milliseconds probe_window{10000};
};

/**
 * @brief The fallback for two peers that cannot punch: one UDP port that
 *        forwards datagrams between the two sources bound to each pairing,
 *        told apart by a channel header. Also the reflector peers gather
 *        their public mapping from.
 *
 * A broker (RendezvousServer, or one of your own) calls Allocate() when it
 * pairs two peers and hands both the same token as a Relayed candidate at
 * address(). Each peer sends RelayBind with the token from its punch socket
 * until RelayBound comes back naming the pairing's channel; from then on
 * everything either sends wrapped in that channel's header is forwarded to
 * the other, header and all. Nothing inside is touched, so the ZDT handshake
 * and the session encryption run through it exactly as they would directly,
 * and the relay holds nothing that decrypts: it reads the four-byte header,
 * and the offline header of a bind or a probe.
 *
 * What keeps it safe: a random 64-bit token per allocation, exactly two
 * sources per pairing (a third is refused), nothing forwarded from a source
 * that did not bind that pairing, binds and replies never forwarded,
 * allocations that nobody binds or that go idle freed on their timers, a
 * cap on how many exist, and a per-source throttle on probes.
 *
 * What keeps it cheap: one socket and one thread, a forwarded datagram
 * costing a recvfrom into a stack buffer, a hash lookup on the channel, a
 * compare against two addresses and one sendto, with no allocation.
 * Pairings cost memory, not ports or descriptors.
 *
 * The same port answers Reflect with the address the probe arrived from,
 * which is how Host::Gather learns a socket's public mapping. A Reflect is
 * padded to be larger than its answer, so the port cannot amplify.
 */
class RelayServer {
 public:
  /** @brief One pairing: the channel its datagrams carry and the token both
   *         peers bind with. The token is what the broker hands out; peers
   *         learn the channel when they bind. */
  struct Allocation {
    uint32_t channel = 0;
    uint64_t token = 0;
  };

  explicit RelayServer(const RelayServerConfig& config);
  ~RelayServer();
  RelayServer(const RelayServer&) = delete;

  /** @brief Binds the port and starts the forwarding thread. */
  Result Start();

  /** @brief Frees every allocation and releases the socket. */
  void Stop();

  /**
   * @brief Opens one pairing. Any thread.
   *
   * @return Success with `out` filled; ServerFull at max_allocations;
   *         AlreadyStopped before Start().
   */
  Result Allocate(Allocation& out);

  /**
   * @brief Frees one allocation before its timers would, e.g. when the
   *        broker knows the pairing is over. Any thread. PeerNotFound when
   *        no allocation has that channel.
   */
  Result Free(uint32_t channel);

  /** @brief The endpoint as bound: what peers bind, probe and relay through.
   *         Null before Start(). */
  ZNET_NODISCARD std::shared_ptr<InetAddress> address() const {
    return address_;
  }

  /** @brief Live allocations, bound or not. */
  ZNET_NODISCARD size_t allocation_count() const;

  /** @brief A snapshot of the counters. Zeroed with metrics compiled out. */
  ZNET_NODISCARD RelayMetrics metrics() const;

 private:
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

  void Loop();
  void OnDatagram(uint8_t* data, size_t len, const Slot& source,
                  std::chrono::steady_clock::time_point now);
  void OnBind(const uint8_t* data, size_t len, const Slot& source,
              std::chrono::steady_clock::time_point now);
  void OnReflect(const uint8_t* data, const Slot& source);
  void Expire(std::chrono::steady_clock::time_point now);
  // drops one pairing; caller holds mutex_
  void Release(uint32_t channel, const char* why);

  RelayServerConfig config_;
  std::shared_ptr<backends::UDPSocket> socket_;
  std::shared_ptr<InetAddress> address_;
  std::unique_ptr<backends::AdmissionControl> probe_throttle_;
  Task task_;
  std::atomic<bool> running_{false};

  // pairings_ and metrics_: the loop owns them, Allocate(), Free() and the
  // readers come from any thread
  mutable std::mutex mutex_;
  std::unordered_map<uint32_t, Pairing> pairings_;
  std::unordered_map<uint64_t, uint32_t> channels_by_token_;
  uint32_t next_channel_ = 1;
  RelayMetrics metrics_;
};

}  // namespace p2p
}  // namespace znet

#endif  // ZNET_P2P_RELAY_SERVER_H_

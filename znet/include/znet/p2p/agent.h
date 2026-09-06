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

#ifndef ZNET_P2P_AGENT_H_
#define ZNET_P2P_AGENT_H_

#include "znet/inet_addr.h"
#include "znet/p2p/punch.h"
#include "znet/peer_session.h"
#include "znet/task.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace znet {
namespace backends {
class UDPSocket;
class ZDTTransportLayer;
class CookieSecret;
}  // namespace backends

namespace p2p {
namespace internal {
class ReflectProbe;
class ZDTPunch;
struct PunchOutcome;
}  // namespace internal

/** @brief What an Agent binds and builds its sessions with. */
struct AgentConfig {
  std::string bind_address = "0.0.0.0";
  /** @brief Zero picks an ephemeral port; read it back with punch_port(). */
  PortNumber bind_port = 0;
  /** @brief Options every punched session is built with. */
  SessionOptions session_options;
};

/**
 * @brief One UDP socket carrying every punched peer: the gathering that
 *        learns its addresses, the punches in flight and the sessions they
 *        become.
 *
 * A NAT hands out one public mapping per local socket, so every peer must
 * be reached through the same socket, and the mapping has to be learned from
 * that socket too. The agent owns it, runs the gather and punch state
 * machines on it (asynchronously; neither call blocks), and drives the
 * resulting sessions on one thread the way a server worker does. ZDT only;
 * TCP has its own path in p2p/tcp/punch.h.
 *
 * See the wiki's Peer-to-Peer page for the full flow; tests/p2p_agent.cc is a
 * working three-player mesh.
 */
class Agent {
 public:
  /**
   * @brief Runs on the agent's thread when a punch resolves: Success with a
   *        session that has finished its handshake and is ready, or a failure
   *        with null. Setting the session's codec and handler inside the
   *        callback is the intended pattern, exactly like a connected event.
   *
   * Readiness is what makes that pattern safe: the handshake owns the codec
   * and handler until it finishes, so installing over it would stall.
   */
  using PunchCallback =
      std::function<void(Result, std::shared_ptr<PeerSession>)>;

  /**
   * @brief What a gather resolved to: the outcome, the candidates to hand the
   *        broker (reflexive first, then host), and the NAT-type verdict the
   *        reflectors revealed.
   *
   * result is Success once any reflector answered, or none were asked; Timeout
   * when every reflector stayed silent, with the host candidates still filled
   * since they are worth offering on their own; AlreadyStopped with nothing if
   * the agent was stopped underneath. nat_type needs two distinct reflectors to
   * be anything but Unknown.
   */
  struct GatherResult {
    Result result = Result::Success;
    std::vector<Candidate> candidates;
    NatType nat_type = NatType::Unknown;
  };

  /** @brief Runs on the agent's thread when a gather resolves. */
  using GatherCallback = std::function<void(GatherResult)>;

  explicit Agent(const AgentConfig& config);
  ~Agent();
  Agent(const Agent&) = delete;

  /** @brief Opens and binds the socket and starts the tick thread. */
  Result Start();

  /**
   * @brief Fails outstanding gathers and punches (Result::AlreadyStopped),
   *        closes every punched session and releases the socket.
   *
   * Joins the tick thread, so it must not be called from a gather or punch
   * callback or a packet handler, which run on it.
   */
  void Stop();

  /**
   * @brief Learns what this socket can be reached at.
   *
   * Host candidates come straight off the interfaces at punch_port().
   * Reflexive ones take a round trip: a Reflect goes to each reflector and
   * the answer names the public mapping it saw. A reflector is a
   * TraversalServer's address, speaking znet's own probe, not a STUN server.
   * Two reflectors that report different mappings expose a symmetric NAT;
   * both are offered and the punch races them.
   *
   * @param timeout how long to wait for the reflectors before resolving
   *        with whatever came back.
   */
  void Gather(std::vector<std::shared_ptr<InetAddress>> reflectors,
              std::chrono::milliseconds timeout, GatherCallback on_done);

  /**
   * @brief Begins one asynchronous punch toward the peer the offer describes.
   *
   * Every candidate in the offer is raced at once and whichever answers
   * first wins. A punch toward a peer that already has a live session
   * resolves with that session instead of punching again. A Relayed
   * candidate without a token is refused with Result::InvalidArgument.
   */
  void Punch(PunchOffer offer, PunchCallback on_done);

  /**
   * @brief Moves every punched session to a new local address, keeping them.
   *
   * The agent owns one socket for all peers, so a rebind moves them together:
   * it binds a fresh socket and lets each peer re-path by the connection id
   * (the analog of Client::Rebind for the mesh). Needs
   * enable_connection_migration on both ends; direct sessions only, since a
   * relayed peer is reached through the relay's fixed address. Thread-safe: the
   * swap is applied on the agent's thread. Same local-address Result values as a
   * bind.
   */
  Result Rebind(const std::string& ip, PortNumber port);

  /** @brief The UDP port every punch and session speaks from. */
  ZNET_NODISCARD PortNumber punch_port() const {
    auto address = local_address();
    return address ? address->port() : 0;
  }

  ZNET_NODISCARD std::shared_ptr<InetAddress> local_address() const {
    // Rebind() reassigns this from the agent thread, so reads take the lock
    std::lock_guard<std::mutex> lock(address_mutex_);
    return local_address_;
  }

  /** @brief Live punched sessions. Approximate off the agent thread. */
  ZNET_NODISCARD size_t session_count() const {
    return session_count_.load(std::memory_order_relaxed);
  }

 private:
  // the state machines live behind pointers so this header needs none of
  // the ZDT backend headers they are built on
  struct PunchInFlight {
    std::unique_ptr<internal::ZDTPunch> machine;
    PunchCallback on_done;
  };

  struct Gathering {
    std::unique_ptr<internal::ReflectProbe> probe;
    GatherCallback on_done;
  };

  struct Route {
    // owned by the session; valid exactly as long as the session lives
    backends::ZDTTransportLayer* transport = nullptr;
    std::shared_ptr<PeerSession> session;
    // the connection ids the peer and we stamp on our datagrams. A datagram
    // from a new address carrying remote_guid is this peer having moved; a
    // PathChallenge naming local_guid is a peer asking us to prove our move.
    uint64_t remote_guid = 0;
    uint64_t local_guid = 0;
    // callers still owed a resolution for this peer
    std::vector<PunchCallback> waiters;
    std::chrono::steady_clock::time_point ready_deadline;
  };

  void TickLoop();
  bool DrainSocket();
  bool TickGathers();
  bool TickPunches();
  bool ProcessSessions();
  // the offer a finished gather produces: its verdict, the reflexive mappings,
  // this socket's own networks, and, for a symmetric NAT, its predicted ports
  GatherResult CollectGather(const internal::ReflectProbe& probe) const;
  // what routes_ is keyed by: the peer's address, plus the relay channel for
  // a relayed session, since every relayed peer shares the relay's address
  static std::string RouteKey(const InetAddress& address, uint32_t channel);
  void HandleOffline(const std::shared_ptr<InetAddress>& from,
                     uint32_t channel, const uint8_t* data, size_t len);
  // consumes punches_[index] on the outcome its machine reported
  void CompletePunch(size_t index, const internal::PunchOutcome& outcome);
  void FailPunch(size_t index, Result reason);
  static void ResolveWaiters(Route& route, Result result);

  // connection migration, the peer-to-peer mirror of the ZDT server/client
  // split: a direct datagram from a new address carrying a known remote_guid is
  // a peer that moved, so challenge the path and re-key on a valid response;
  // a PathChallenge from a peer means we moved, so answer it. All gated on
  // enable_connection_migration and on direct (unrelayed) sessions. Agent thread.
  // true when a live session claims this cid, so the datagram was a moved peer
  // and is dropped as unproven; false leaves it for HandleOffline (an in-flight
  // punch's first datagram also arrives cid-stamped and unrouted).
  bool ChallengeNewPath(uint64_t cid, const std::shared_ptr<InetAddress>& from);
  void CompletePathMigration(const std::shared_ptr<InetAddress>& from,
                             const uint8_t* data, size_t len);
  void AnswerPathChallenge(const std::shared_ptr<InetAddress>& from,
                           const uint8_t* data, size_t len);
  // swaps in a socket a Rebind() bound, retargeting every session's send path
  void ApplyPendingRebind();

  AgentConfig config_;
  std::shared_ptr<backends::UDPSocket> socket_;
  mutable std::mutex address_mutex_;  // guards local_address_ across a Rebind
  std::shared_ptr<InetAddress> local_address_;
  Task task_;
  std::atomic<bool> running_{false};
  // signing secret for path-validation cookies (agent thread only). Behind a
  // pointer so this header stays clear of the ZDT backend definitions.
  std::unique_ptr<backends::CookieSecret> cookie_secret_;

  // handoff from Gather, Punch and Rebind (any thread) to the tick thread
  std::mutex pending_mutex_;
  std::vector<PunchInFlight> pending_;
  std::vector<Gathering> pending_gathers_;
  std::shared_ptr<backends::UDPSocket> pending_rebind_;

  // tick thread only
  std::vector<PunchInFlight> punches_;
  std::vector<Gathering> gathers_;
  std::unordered_map<std::string, Route> routes_;  // by RouteKey
  std::atomic<size_t> session_count_{0};
};

}  // namespace p2p
}  // namespace znet


#endif  // ZNET_P2P_AGENT_H_

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

#ifndef ZNET_P2P_RENDEZVOUS_SERVER_H_
#define ZNET_P2P_RENDEZVOUS_SERVER_H_

#include "znet/p2p/relay_server.h"
#include "znet/p2p/rendezvous.h"
#include "znet/server.h"
#include "znet/server_events.h"
#include "znet/task.h"

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>

namespace znet {
namespace p2p {

/** @brief What a RendezvousServer listens on, and the relay it runs. */
struct RendezvousServerConfig {
  std::string bind_address = "0.0.0.0";
  PortNumber bind_port = 5001;
  ConnectionType punch_connection_type = ConnectionType::ZDT;
  /**
   * @brief Listener options for the rendezvous itself: allow/deny lists,
   *        the per-source connection throttle and max_connections all
   *        apply. See ServerOptions.
   */
  ServerOptions options;
  /**
   * @brief Requests one client may make per request_window; gathering and
   *        connect-peer both count. Beyond it the client is disconnected:
   *        a spammer costs a reconnect, not the pairing thread. Zero
   *        disables it.
   */
  uint32_t max_requests_per_window = 30;
  /** @brief The window max_requests_per_window is counted over. */
  std::chrono::milliseconds request_window{10000};
  /** @brief Run a relay alongside, configured by `relay`. */
  bool relay_enabled = false;
  RelayServerConfig relay;
  /**
   * @brief The host peers reach the relay at, when it is not the host they
   *        reached the rendezvous at: the two are advertised together and
   *        an empty value means "same host as me". Resolved once at
   *        Start(), which fails with InvalidAddress if it does not.
   */
  std::string relay_host;
  /**
   * @brief Reflectors advertised beside the embedded relay, each a znet relay
   *        control endpoint (build with InetAddress::from). A second one on a
   *        distinct IP is what lets a peer tell an endpoint-independent NAT
   *        from a symmetric one; without it every gather reports Unknown. The
   *        welcome advertises the relay first, then these, capped at
   *        kMaxReflectors.
   */
  std::vector<std::shared_ptr<InetAddress>> extra_reflectors;
};

/**
 * @brief The rendezvous broker: names peers, takes their gatherings, pairs
 *        mutual asks into punch offers, and hands out a relay when it runs one.
 *
 * Instantiable, so it can run inside a larger process or a test as easily as
 * in the standalone rendezvous-server binary. The rendezvous link is always
 * TCP; `punch_connection_type` is the transport the punched peer-to-peer
 * connection will use, decided here so both peers always agree.
 *
 * With `relay_enabled` it also runs a RelayServer: every welcome names the
 * relay as the reflector, and every ZDT offer carries a relayed candidate
 * both peers can fall back to.
 */
class RendezvousServer {
 public:
  explicit RendezvousServer(const RendezvousServerConfig& config);
  ~RendezvousServer();
  RendezvousServer(const RendezvousServer&) = delete;

  /** @brief Binds, listens, starts the pairing thread and the relay if any. */
  Result Start();

  void Stop();

  /** @brief Blocks until the rendezvous stops. */
  void Wait();

  /** @brief Resolved after Start(), so a bind_port of 0 can be read back. */
  ZNET_NODISCARD std::shared_ptr<InetAddress> bind_address() const {
    return server_.bind_address();
  }

  /** @brief The relay this rendezvous runs, or null without one. */
  ZNET_NODISCARD const RelayServer* relay() const { return relay_.get(); }

 private:
  friend class RendezvousPacketHandler;

  // one connected client, as the pairing thread sees it. All fields are
  // guarded by mutex_: packet handlers run on the rendezvous's session workers.
  struct ClientData {
    std::shared_ptr<PeerSession> session;
    std::string peer_name;
    // every name this client is currently asking for. A set, not a single
    // slot: a client asks for several peers, and one slot made each ask
    // clobber the previous one. An entry is consumed when its pair forms.
    std::set<std::string> pending_targets;
    // what the client gathered, relayed to its match; empty until it did
    std::vector<Candidate> candidates;
    // the port the client punches from; zero falls back to the observed one
    PortNumber punch_port = 0;
    std::chrono::steady_clock::time_point request_window_start;
    uint32_t request_count = 0;
  };

  void OnEvent(Event& event);
  bool OnConnectEvent(IncomingClientConnectedEvent& event);
  bool OnDisconnectEvent(IncomingClientDisconnectedEvent& event);
  /** @brief Counts one request; false means over the limit. Caller holds
      mutex_. */
  bool AllowRequest(ClientData& data);
  void PairingLoop();
  void Welcome(const std::shared_ptr<PeerSession>& session);
  void TryPair(const std::shared_ptr<PeerSession>& session,
               const std::string& target);
  // the candidates a match is told to punch, in the order to try them
  std::vector<Candidate> OfferCandidates(const ClientData& client,
                                         const Candidate* relayed) const;
  // the relay's host as peers should address it: relay_host_ at `port`
  std::shared_ptr<InetAddress> RelayEndpoint(PortNumber port) const;
  std::string GenerateUniqueName();

  RendezvousServerConfig config_;
  Server server_;
  std::unique_ptr<RelayServer> relay_;
  // relay_host resolved once at Start(); the unspecified address
  // when it is empty, meaning "the host you reached me at"
  std::shared_ptr<InetAddress> relay_host_;
  Task pairing_task_;

  std::mutex mutex_;
  std::condition_variable cv_;
  bool stop_ = false;
  std::deque<std::shared_ptr<PeerSession>> welcome_queue_;
  // each entry names the target it asked for at the time; reading the
  // client's latest state instead would let a newer ask rewrite older ones
  std::deque<std::pair<std::shared_ptr<PeerSession>, std::string>>
      connect_peer_queue_;
  std::deque<std::string> clear_queue_;
  // written only on the pairing thread, under mutex_ like the rest
  std::unordered_map<std::string, std::shared_ptr<ClientData>> registry_;
  std::mt19937_64 punch_id_rng_;
};

}  // namespace p2p
}  // namespace znet


#endif  // ZNET_P2P_RENDEZVOUS_SERVER_H_

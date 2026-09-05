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

#ifndef ZNET_P2P_LOCATOR_H_
#define ZNET_P2P_LOCATOR_H_

#include "znet/client.h"
#include "znet/client_events.h"
#include "znet/compat.h"
#include "znet/p2p/events.h"
#include "znet/p2p/host.h"
#include "znet/p2p/rendezvous.h"

#include <vector>

namespace znet {
namespace p2p {
namespace internal {
template <typename Locator>
class LinkPacketHandler;
}  // namespace internal

/** @brief Where a PeerLocator finds its rendezvous and how it punches. */
struct PeerLocatorConfig {
  std::string server_address;
  PortNumber server_port = 0;
  /** @brief The shared punch socket: where it binds and what every
   *         punched session is built with. */
  HostConfig host;
  /** @brief How long the reflectors get before the gathering is sent with
   *         whatever came back. */
  std::chrono::milliseconds gather_timeout{2000};
  /** @brief PunchOffer::timeout for every punch. */
  std::chrono::milliseconds punch_timeout{5000};
  /** @brief PunchOffer::relay_delay for every punch. */
  std::chrono::milliseconds relay_delay{1000};
};

/**
 * @brief The whole flow over znet's rendezvous, for two players or a mesh:
 *        gather, exchange, punch, with the relay as the fallback.
 *
 * Connect() brings up a Host on the shared punch socket and the link to the
 * rendezvous. The welcome names this peer and says where to gather from;
 * the gathering goes back, and PeerLocatorReadyEvent fires. From then on
 * AskPeer may be called any number of times and the punches overlap freely
 * on the one socket. Each success fires PeerConnectedEvent, on the host's
 * thread, with a session whose handshake has finished, which is why that
 * event is the place for its codec and handler.
 *
 * Losing the link ends matchmaking, not the mesh: punched sessions live
 * until Disconnect() (or their own idle timers) close them, and Connect()
 * may be called again to rejoin with the mesh intact.
 *
 * ZDT only, which the rendezvous brokers by default; an offer naming TCP
 * fails with Result::InvalidBackend. TCP has its own, two-player path in
 * p2p/tcp/locator.h.
 */
class PeerLocator {
 public:
  explicit PeerLocator(const PeerLocatorConfig& config);
  PeerLocator(const PeerLocator&) = delete;
  ~PeerLocator();

  Result Connect();

  /** @brief Leaves: the link, pending punches and every punched session
   *         all end. Joins the host's thread, so never from inside one of
   *         this locator's events. */
  Result Disconnect();

  /** @brief Asks the rendezvous for a peer. NotReady until
   *         PeerLocatorReadyEvent has fired, NotConnected without a link. */
  Result AskPeer(std::string peer_name);

  /** @brief Blocks until the link ends. The mesh may outlive it. */
  void Wait();

  void SetEventCallback(EventCallbackFn fn) { event_callback_ = std::move(fn); }

  ZNET_NODISCARD std::string peer_name() const;

  /** @brief The socket everything punches from, e.g. for session_count(). */
  ZNET_NODISCARD const Host& host() const { return host_; }

 private:
  template <typename Locator>
  friend class internal::LinkPacketHandler;

  void OnEvent(Event& event);
  bool OnConnectEvent(ClientConnectedToServerEvent& event);
  bool OnDisconnectEvent(ClientDisconnectedFromServerEvent& event);
  bool OnConnectionFailedEvent(ClientConnectionFailedEvent& event);
  void OnWelcome(const WelcomePacket& welcome);
  void OnGathered(Host::GatherResult result);
  void OnPeerNotFound(const std::string& target_peer);
  void OnPunchOffer(const PunchOfferPacket& offer);
  // an unspecified host on the wire means the rendezvous host itself
  std::shared_ptr<InetAddress> AtRendezvous(
      const std::shared_ptr<InetAddress>& address) const;
  void FireFailed(PeerLocatorPhase phase, Result reason,
                  const std::string& target_peer);

  PeerLocatorConfig config_;
  Host host_;
  Client client_;
  EventCallbackFn event_callback_;
  mutable std::mutex mutex_;
  std::string peer_name_;
  std::shared_ptr<InetAddress> observed_;
  std::shared_ptr<PeerSession> link_session_;
  // this peer's own NAT-type verdict from its last gather; a symmetric one
  // sends every punch straight to the relay
  NatType nat_type_ = NatType::Unknown;
  bool is_running_ = false;
  bool is_ready_ = false;
};

}  // namespace p2p
}  // namespace znet


#endif  // ZNET_P2P_LOCATOR_H_

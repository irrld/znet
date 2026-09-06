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

#include <set>
#include <vector>

namespace znet {
namespace p2p {
namespace internal {
template <typename Locator>
class LinkPacketHandler;
}  // namespace internal

/** @brief What drives a Relocate() after the local network changes. */
enum class RelocateTrigger {
  /** @brief The application calls PeerLocator::Relocate() itself, e.g. from a
   *         platform network-change callback. */
  Manual,
  /** @brief The locator watches the network and relocates on a change on its
   *         own. Reserved: the watcher is not built yet, so this behaves like
   *         Manual for now. */
  Watch,
};

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
  /** @brief How a relocate is triggered after a network change. Manual today;
   *         Watch is the seam for a future network watcher. */
  RelocateTrigger relocate_trigger = RelocateTrigger::Manual;
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

  /**
   * @brief Re-gathers on the current network and re-punches every connected
   *        peer: the recovery for a local address change that connection
   *        migration cannot ride out (a new NAT mapping the peers cannot reach).
   *
   * Call it when the OS reports the network changed. It re-sends the gathering
   * with fresh candidates and re-asks each connected peer; the broker nudges
   * those peers to re-ask back, and each pair re-punches into a new session
   * (a fresh PeerConnectedEvent). NotReady until the first gather, NotConnected
   * without a link.
   */
  Result Relocate();

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
  // a former partner re-asked after moving: re-gather and re-ask it back
  void OnRepunchRequest(const std::string& from_peer);
  // re-gather, then re-ask `targets` once the fresh candidates are sent. Shared
  // by Relocate (every connected peer) and OnRepunchRequest (one peer).
  void BeginRepunch(std::vector<std::string> targets);
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
  // the reflectors from the welcome, kept so a Relocate can re-gather
  std::vector<std::shared_ptr<InetAddress>> reflectors_;
  // peers a punch connected, so a Relocate knows whom to re-punch
  std::set<std::string> connected_peers_;
  // peers to re-ask once the next (relocate) gather has sent its candidates
  std::vector<std::string> repunch_targets_;
  // this peer's own NAT-type verdict from its last gather; a symmetric one
  // sends every punch straight to the relay
  NatType nat_type_ = NatType::Unknown;
  bool is_running_ = false;
  bool is_ready_ = false;
};

}  // namespace p2p
}  // namespace znet


#endif  // ZNET_P2P_LOCATOR_H_

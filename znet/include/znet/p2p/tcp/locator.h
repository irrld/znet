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

#ifndef ZNET_P2P_TCP_LOCATOR_H_
#define ZNET_P2P_TCP_LOCATOR_H_

#include "znet/client.h"
#include "znet/client_events.h"
#include "znet/compat.h"
#include "znet/p2p/events.h"
#include "znet/p2p/rendezvous.h"
#include "znet/task.h"

#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

namespace znet {
namespace p2p {
namespace internal {
template <typename Locator>
class LinkPacketHandler;
}  // namespace internal

namespace tcp {

ZNET_INLINE_CONSTEXPR uint64_t kInvalidPunchId =
    (std::numeric_limits<uint64_t>::max)();

/** @brief Where a tcp::PeerLocator finds its rendezvous and how long it punches. */
struct PeerLocatorConfig {
  std::string server_address;
  PortNumber server_port = 0;
  /** @brief Budget for the punch and its handshake together. */
  std::chrono::milliseconds punch_timeout{5000};
};

/**
 * @brief The one-shot TCP path over znet's rendezvous: connect, ask for one
 *        peer, punch from the link's own port with tcp::PunchSync, done.
 *
 * TCP only: run the rendezvous with punch type tcp, since a ZDT offer fails
 * with Result::InvalidBackend. There is no gathering beyond the link's own
 * addresses and no relay; the ZDT flow with both is p2p::PeerLocator.
 *
 * The events are p2p's (PeerLocatorReadyEvent, PeerConnectedEvent,
 * PeerLocatorFailedEvent, PeerLocatorCloseEvent) and every one fires on an
 * internal worker thread; treat the callback like a packet handler and keep
 * it quick. The locator stops once the punch resolves either way, so Wait()
 * returns then; hold the session yourself. To reuse the locator, call
 * Connect() again and handle the events again.
 */
class PeerLocator {
 public:
  explicit PeerLocator(const PeerLocatorConfig& config);
  PeerLocator(const PeerLocator&) = delete;
  ~PeerLocator();

  Result Connect();
  Result Disconnect();

  void Wait();

  Result AskPeer(std::string peer_name);

  void SetEventCallback(EventCallbackFn fn) { event_callback_ = std::move(fn); }

  ZNET_NODISCARD std::string peer_name() const;

 private:
  template <typename Locator>
  friend class internal::LinkPacketHandler;

  void OnEvent(Event&);
  bool OnConnectEvent(ClientConnectedToServerEvent& event);
  bool OnDisconnectEvent(ClientDisconnectedFromServerEvent& event);
  bool OnConnectionFailedEvent(ClientConnectionFailedEvent& event);
  void OnWelcome(const WelcomePacket& welcome);
  void OnPeerNotFound(const std::string& target_peer);
  void OnPunchOffer(const PunchOfferPacket& offer);
  // a former partner re-asked after moving; the two-player TCP path recovers
  // by simply re-asking it, which re-pairs and re-punches.
  void OnRepunchRequest(const std::string& from_peer);
  void FireFailed(PeerLocatorPhase phase, Result reason,
                  const std::string& target_peer);

  PeerLocatorConfig config_;
  EventCallbackFn event_callback_;
  Client client_;

  std::string peer_name_;
  std::shared_ptr<PeerSession> session_;

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  Task task_;

  // the offer, kept for the worker
  std::vector<std::shared_ptr<InetAddress>> target_candidates_;
  ConnectionType connection_type_ = ConnectionType::TCP;
  std::string target_peer_name_;
  uint64_t punch_id_ = kInvalidPunchId;

  bool is_running_ = false;
  bool wake_ = false;  // guarded by mutex_
};

}  // namespace tcp
}  // namespace p2p
}  // namespace znet

#endif  // ZNET_P2P_TCP_LOCATOR_H_

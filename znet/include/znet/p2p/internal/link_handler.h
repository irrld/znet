//
//    Copyright 2026 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//
// API stability: internal (see the wiki, API Stability)

#ifndef ZNET_P2P_INTERNAL_LINK_HANDLER_H_
#define ZNET_P2P_INTERNAL_LINK_HANDLER_H_

#include "znet/p2p/rendezvous.h"
#include "znet/packet_handler.h"

namespace znet {
namespace p2p {
namespace internal {

/**
 * @brief The packet handler on a locator's link to the rendezvous, shared by
 *        both locators: it only dispatches to the locator's OnWelcome,
 *        OnPeerNotFound, OnPunchOffer and OnRepunchRequest.
 */
template <typename Locator>
class LinkPacketHandler
    : public PacketHandler<LinkPacketHandler<Locator>, WelcomePacket,
                           PunchOfferPacket, PeerNotFoundPacket,
                           RepunchRequestPacket> {
 public:
  explicit LinkPacketHandler(Locator& locator) : locator_(locator) {}

  void OnPacket(const WelcomePacket& pk) { locator_.OnWelcome(pk); }

  void OnPacket(const PeerNotFoundPacket& pk) {
    locator_.OnPeerNotFound(pk.target_peer_);
  }

  void OnPacket(const PunchOfferPacket& pk) { locator_.OnPunchOffer(pk); }

  void OnPacket(const RepunchRequestPacket& pk) {
    locator_.OnRepunchRequest(pk.from_peer_);
  }

 private:
  Locator& locator_;
};

}  // namespace internal
}  // namespace p2p
}  // namespace znet

#endif  // ZNET_P2P_INTERNAL_LINK_HANDLER_H_

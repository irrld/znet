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

#ifndef ZNET_P2P_EVENTS_H_
#define ZNET_P2P_EVENTS_H_

#include "znet/compat.h"
#include "znet/event.h"
#include "znet/inet_addr.h"
#include "znet/peer_session.h"

#include <memory>
#include <string>

namespace znet {
namespace p2p {

// The events both locators fire, p2p::PeerLocator and tcp::PeerLocator, so a
// TCP-only user does not have to compile the ZDT host to handle them.

/** @brief Which step of finding a peer failed. */
enum class PeerLocatorPhase : uint8_t {
  Link,      /**< The connection to the rendezvous server. */
  Gather,    /**< Learning this peer's own candidates. */
  Exchange,  /**< The name exchange on the link. */
  Punch,     /**< The hole punch itself. */
};

inline std::string GetPeerLocatorPhaseString(PeerLocatorPhase phase) {
  switch (phase) {
    case PeerLocatorPhase::Link:
      return "Link";
    case PeerLocatorPhase::Gather:
      return "Gather";
    case PeerLocatorPhase::Exchange:
      return "Exchange";
    case PeerLocatorPhase::Punch:
      return "Punch";
    default:
      return "Unknown";
  }
}

class PeerLocatorReadyEvent : public Event {
 public:
  PeerLocatorReadyEvent(std::string peer_name,
                        std::shared_ptr<InetAddress> endpoint)
      : peer_name_(std::move(peer_name)), endpoint_(std::move(endpoint)) {}

  ZNET_NODISCARD const std::string& peer_name() const { return peer_name_; }

  /** @brief The public mapping other peers will be told to punch: what a
   *         reflector saw, or failing that what the rendezvous observed. */
  ZNET_NODISCARD std::shared_ptr<InetAddress> endpoint() const {
    return endpoint_;
  }

  ZNET_EVENT_CLASS_TYPE(PeerLocatorReadyEvent)
  ZNET_EVENT_CLASS_CATEGORY(EventCategoryP2P)
 private:
  std::string peer_name_;
  std::shared_ptr<InetAddress> endpoint_;
};

class PeerLocatorCloseEvent : public Event {
 public:
  PeerLocatorCloseEvent() = default;

  ZNET_EVENT_CLASS_TYPE(PeerLocatorCloseEvent)
  ZNET_EVENT_CLASS_CATEGORY(EventCategoryP2P)
};

/**
 * @brief Something on the way to a peer failed, and this is what and why.
 *
 * An Exchange failure (an unknown peer name) leaves the link up, so AskPeer
 * can simply be called again. A Punch failure costs that one peer. A Link
 * failure is a connection that could not be made, or a rendezvous of the
 * wrong kind, and PeerLocatorCloseEvent follows it; a link that dies later
 * fires the close event alone. A Gather failure means the host was stopped
 * underneath.
 */
class PeerLocatorFailedEvent : public Event {
 public:
  PeerLocatorFailedEvent(PeerLocatorPhase phase, Result reason,
                         std::string target_peer)
      : phase_(phase), reason_(reason), target_peer_(std::move(target_peer)) {}

  ZNET_NODISCARD PeerLocatorPhase phase() const { return phase_; }

  ZNET_NODISCARD Result reason() const { return reason_; }

  /** @brief The peer being sought; empty when none was involved yet. */
  ZNET_NODISCARD const std::string& target_peer() const { return target_peer_; }

  ZNET_EVENT_CLASS_TYPE(PeerLocatorFailedEvent)
  ZNET_EVENT_CLASS_CATEGORY(EventCategoryP2P)
 private:
  PeerLocatorPhase phase_;
  Result reason_;
  std::string target_peer_;
};

class PeerConnectedEvent : public Event {
 public:
  explicit PeerConnectedEvent(std::shared_ptr<PeerSession> session,
                              uint64_t punch_id, std::string self_peer_name,
                              std::string target_peer_name)
      : session_(session),
        punch_id_(punch_id),
        self_peer_name_(self_peer_name),
        target_peer_name_(target_peer_name) {}

  ZNET_NODISCARD std::shared_ptr<PeerSession> session() const {
    return session_;
  }

  ZNET_NODISCARD uint64_t punch_id() const { return punch_id_; }

  ZNET_NODISCARD const std::string& self_peer_name() const {
    return self_peer_name_;
  }

  ZNET_NODISCARD const std::string& target_peer_name() const {
    return target_peer_name_;
  }

  ZNET_EVENT_CLASS_TYPE(PeerConnectedEvent)
  ZNET_EVENT_CLASS_CATEGORY(EventCategoryP2P)
 private:
  std::shared_ptr<PeerSession> session_;
  uint64_t punch_id_;
  std::string self_peer_name_;
  std::string target_peer_name_;
};

}  // namespace p2p
}  // namespace znet

#endif  // ZNET_P2P_EVENTS_H_

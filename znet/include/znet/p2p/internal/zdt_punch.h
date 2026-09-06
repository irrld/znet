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

//
// The ZDT hole-punch state machine, driven from outside by whoever owns the
// socket (p2p::Agent). Internal: usable directly, no stability promise.
//

#ifndef ZNET_P2P_INTERNAL_ZDT_PUNCH_H_
#define ZNET_P2P_INTERNAL_ZDT_PUNCH_H_

#include "znet/backends/zdt/zdt_connection.h"
#include "znet/backends/zdt/zdt_net.h"
#include "znet/backends/zdt/zdt_wire.h"
#include "znet/buffer.h"
#include "znet/compat.h"
#include "znet/inet_addr.h"
#include "znet/p2p/punch.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

namespace znet {
namespace p2p {
namespace internal {

// --- the offline datagrams a punch speaks -------------------------------------
// every one starts with the ZDT offline header (id plus magic), so unrelated
// traffic on the socket is never mistaken for one
Buffer BuildOffline(backends::ZDTOfflineMsg id);
Buffer BuildPunch();
Buffer BuildRequest1(uint64_t local_guid, uint8_t capabilities);
Buffer BuildReply2(uint64_t local_guid, uint16_t mtu, uint8_t capabilities);
Buffer BuildIncompatibleVersion(uint64_t local_guid);
// the relay and reflector side; see znet/p2p/relay_server.h for the flow
Buffer BuildRelayBind(uint64_t token);
Buffer BuildRelayBound(uint64_t token, uint32_t channel);
Buffer BuildReflect(uint64_t nonce);
Buffer BuildReflected(uint64_t nonce, const InetAddress& observed);

/**
 * @brief Sends `datagram` to `address`, wrapped in the relay channel header
 *        when `channel` is nonzero. What every send toward a relay uses.
 */
void SendDatagram(backends::UDPSocket& socket, const InetAddress& address,
                  uint32_t channel, const Buffer& datagram);

/** @brief What one Tick() or OnDatagram() concluded. */
struct PunchOutcome {
  enum class State { Pending, Completed, Failed };
  State state = State::Pending;
  /** @brief Whether Tick() put anything on the wire, so a driver can doze
   *         when nothing did. */
  bool sent = false;
  Result reason = Result::Success;  /**< Failed only. */
  /** @brief Completed: the candidate that answered, which is the peer
   *         address the session is built toward. */
  std::shared_ptr<InetAddress> from;
  /** @brief Completed through a relay: the channel the session must wrap
   *         every datagram in. Zero for a direct path. */
  uint32_t channel = 0;
  /**
   * @brief Completed on the responder: the initiator's first online datagram,
   *        which belongs to the transport about to be built. Points into the
   *        buffer OnDatagram() was given, past any relay header.
   */
  const uint8_t* first_datagram = nullptr;
  size_t first_len = 0;
};

/**
 * @brief The hole punch for one offer, as a state machine with no socket and
 *        no thread of its own.
 *
 * The driver sends what Tick() asks for and feeds every datagram whose
 * source Owns() to OnDatagram(); both report an outcome, and on Completed the
 * driver builds a transport from connection() toward `from`.
 *
 * The flow: Punch datagrams every 50 ms toward every candidate keep the NAT
 * holes open from both sides. The initiator also sends
 * OpenConnectionRequest1 every 100 ms; the responder answers with
 * OpenConnectionReply2 and stays pending until online data proves the
 * initiator connected, so a lost reply is simply re-answered. No cookie: the
 * broker vouched for both peers and the punch itself proves routability.
 *
 * A Relayed candidate is bound from the start (RelayBind with its token
 * every 100 ms until RelayBound, which names the channel) but only joins the
 * race once the offer's relay_delay has passed. From then on it is a
 * candidate like any other, with every datagram to and from it wrapped in
 * the relay channel header: the relay forwards the handshake verbatim, so
 * whichever path answers first wins, and a relayed session is simply one
 * whose peer address is the relay and whose connection carries the channel.
 */
class ZDTPunch {
 public:
  using TimePoint = std::chrono::steady_clock::time_point;

  // local_migration: whether this agent offers connection migration. The peer's
  // offer arrives in the handshake, and the connection enables it only if both
  // asked.
  ZDTPunch(PunchOffer offer, TimePoint now, bool local_migration);

  ZNET_NODISCARD const PunchOffer& offer() const { return offer_; }

  /** @brief Settled by the handshake; what the transport is built from.
   *         relay_channel is left for the driver to set from the outcome. */
  ZNET_NODISCARD const backends::ZDTConnection& connection() const {
    return connection_;
  }

  /**
   * @brief Whether a datagram from `from`, wrapped for `channel` (zero when
   *        bare), is this punch's. Attribution is by source address, plus
   *        the channel for a relay, which a symmetric NAT rewriting ports
   *        defeats, but it defeats the punch itself first.
   */
  ZNET_NODISCARD bool Owns(const InetAddress& from, uint32_t channel) const;

  /** @brief Sends whatever is due at `now`; Failed(Timeout) past the
   *         deadline. */
  PunchOutcome Tick(backends::UDPSocket& socket, TimePoint now);

  /** @brief Handles one datagram from a source Owns() accepted, with the
   *         relay header already stripped and its channel passed along. */
  PunchOutcome OnDatagram(backends::UDPSocket& socket,
                          const std::shared_ptr<InetAddress>& from,
                          uint32_t channel, const uint8_t* data, size_t len);

 private:
  struct Relay {
    Candidate candidate;
    uint32_t channel = 0;  // learned from RelayBound; bound once nonzero
    TimePoint last_bind{};
  };

  // whether a relay carries punch traffic yet: bound, and past relay_delay
  ZNET_NODISCARD bool RelayActive(const Relay& relay, TimePoint now) const;
  // the candidates punch traffic goes to right now
  void SendToActive(backends::UDPSocket& socket, const Buffer& datagram,
                    TimePoint now) const;
  void OnRelayBound(const InetAddress& from, Buffer& in);

  PunchOffer offer_;
  bool local_migration_ = false;
  std::vector<Candidate> direct_;
  std::vector<Relay> relays_;
  backends::ZDTConnection connection_;
  // the responder has answered a Request1, so online data is the initiator
  // connecting rather than a stray from an older session
  bool answered_ = false;
  TimePoint deadline_;
  TimePoint relay_start_;
  TimePoint last_punch_{};
  TimePoint last_request_{};
};

}  // namespace internal
}  // namespace p2p
}  // namespace znet

#endif  // ZNET_P2P_INTERNAL_ZDT_PUNCH_H_

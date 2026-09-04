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

#ifndef ZNET_P2P_TCP_PUNCH_H_
#define ZNET_P2P_TCP_PUNCH_H_

#include "znet/compat.h"
#include "znet/inet_addr.h"
#include "znet/peer_session.h"

#include <chrono>
#include <memory>
#include <vector>

namespace znet {
namespace p2p {
namespace tcp {

/**
 * @brief TCP hole punch: a simultaneous open from both peers, retried for the
 *        whole budget, handed back ready.
 *
 * Deliberately apart from the ZDT path (p2p::Host and the locator over it):
 * this is two players only, it punches from the very port the addresses were
 * exchanged on, and it has neither gathering nor a relay. Reach for it when
 * the connection has to be TCP; ZDT traverses more NATs.
 *
 * Candidates are the same peer at its different addresses, typically public
 * then private. Every one is raced from the one local port and the first to
 * open and pair wins. Both peers call this at the same time, exactly one of
 * them with is_initiator true (p2p::IsInitiator decides which).
 *
 * @param timeout budget for the punch and the handshake together.
 * @param out_result the failure reason when null comes back; pass nothing if
 *        the session alone is enough.
 * @return a self-managed session whose handshake has finished, or null.
 */
std::shared_ptr<PeerSession> PunchSync(
    const std::shared_ptr<InetAddress>& local,
    const std::vector<std::shared_ptr<InetAddress>>& candidates,
    bool is_initiator,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(5000),
    Result* out_result = nullptr);

}  // namespace tcp
}  // namespace p2p
}  // namespace znet

#endif  // ZNET_P2P_TCP_PUNCH_H_

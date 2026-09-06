//
//    Copyright 2026 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//
//
// Stateless return-routability for ZDT: the rotating signing secret and the
// connection-migration challenge built on it. The handshake proves a client's
// source address before the server allocates state (see zdt_backends.cc); a
// migration proves a peer's new address before its route moves there. Both the
// server backend and p2p::Agent keep one secret and hold no per-challenge state.
//
// API stability: internal (see the wiki, API Stability)

#ifndef ZNET_BACKENDS_ZDT_ZDT_COOKIE_H_
#define ZNET_BACKENDS_ZDT_ZDT_COOKIE_H_

#include "znet/backends/zdt/zdt_wire.h"
#include "znet/compat.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <string>

namespace znet {
namespace backends {

// A rotating HMAC secret for the return-routability cookies. Keeps the current
// and the previous secret so a cookie issued just before a rotation still
// verifies. Not thread-safe; guard it with the owner's lock.
class CookieSecret {
 public:
  CookieSecret();  // seeds the current secret; needs znet::Init() for RAND_bytes

  // Rotates the secret when `interval` has elapsed since the last rotation, so
  // even an idle owner ages it. Call before issuing a cookie.
  void MaybeRotate(std::chrono::steady_clock::duration interval,
                   std::chrono::steady_clock::time_point now);

  // The current epoch. It travels on the wire so a verifier can tell which
  // secret a cookie was minted under.
  ZNET_NODISCARD uint32_t epoch() const { return epoch_; }

  // The cookie over `key` at `epoch`. Passing epoch() mints at the current
  // secret (for issuing); a verifier passes the echoed epoch and gets the
  // current or, across a single rotation, the previous secret. Any other epoch
  // falls through to the current secret and simply fails to match.
  ZNET_NODISCARD ZDTCookie Compute(const std::string& key, uint32_t epoch) const;

 private:
  std::array<uint8_t, 32> current_{};
  std::array<uint8_t, 32> previous_{};
  uint32_t epoch_ = 0;
  bool has_previous_ = false;
  std::chrono::steady_clock::time_point last_rotation_;
};

// The migration cookie key: the candidate address bound to the cid, domain
// separated from a bare handshake key (a readable address never starts with the
// prefix) so a handshake cookie can never stand in for a path one.
std::string PathCookieKey(const std::string& peer_readable, uint64_t cid);

// The challenge to send a peer that appeared at `peer_readable` claiming `cid`:
// a cookie only a host receiving at that address can echo back.
ZDTPathMessage MakePathChallenge(const CookieSecret& secret,
                                 const std::string& peer_readable, uint64_t cid);

// True when `response` from `peer_readable` echoes a cookie `secret` would have
// minted for it, which proves the response came back from the challenged path.
bool VerifyPathResponse(const CookieSecret& secret,
                        const std::string& peer_readable,
                        const ZDTPathMessage& response);

}  // namespace backends
}  // namespace znet

#endif  // ZNET_BACKENDS_ZDT_ZDT_COOKIE_H_

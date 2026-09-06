//
//    Copyright 2026 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include "znet/backends/zdt/zdt_cookie.h"

#include <openssl/rand.h>

namespace znet {
namespace backends {

CookieSecret::CookieSecret() {
  RAND_bytes(current_.data(), static_cast<int>(current_.size()));
  last_rotation_ = std::chrono::steady_clock::now();
}

void CookieSecret::MaybeRotate(std::chrono::steady_clock::duration interval,
                               std::chrono::steady_clock::time_point now) {
  if (now - last_rotation_ < interval) {
    return;
  }
  previous_ = current_;
  has_previous_ = true;
  RAND_bytes(current_.data(), static_cast<int>(current_.size()));
  epoch_++;
  last_rotation_ = now;
}

ZDTCookie CookieSecret::Compute(const std::string& key, uint32_t epoch) const {
  // the previous secret only across a single rotation, so a cookie issued just
  // before one still verifies; anything else uses the current secret and fails.
  if (epoch != epoch_ && has_previous_ && epoch == epoch_ - 1) {
    return ComputeCookie(previous_.data(), previous_.size(), key, epoch);
  }
  return ComputeCookie(current_.data(), current_.size(), key, epoch);
}

std::string PathCookieKey(const std::string& peer_readable, uint64_t cid) {
  return "path|" + peer_readable + "|" + std::to_string(cid);
}

ZDTPathMessage MakePathChallenge(const CookieSecret& secret,
                                 const std::string& peer_readable,
                                 uint64_t cid) {
  ZDTPathMessage msg;
  msg.cid = cid;
  msg.epoch = secret.epoch();
  msg.cookie = secret.Compute(PathCookieKey(peer_readable, cid), msg.epoch);
  return msg;
}

bool VerifyPathResponse(const CookieSecret& secret,
                        const std::string& peer_readable,
                        const ZDTPathMessage& response) {
  return ConstTimeEqual(
      response.cookie,
      secret.Compute(PathCookieKey(peer_readable, response.cid),
                     response.epoch));
}

}  // namespace backends
}  // namespace znet

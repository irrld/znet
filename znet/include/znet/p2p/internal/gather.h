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
// Candidate gathering: the local addresses a socket answers at, and the
// reflection probe that learns its public mapping. Internal: usable
// directly, no stability promise.
//

#ifndef ZNET_P2P_INTERNAL_GATHER_H_
#define ZNET_P2P_INTERNAL_GATHER_H_

#include "znet/backends/zdt/zdt_net.h"
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

/** @brief Every local IPv4 address at `port` as a Host candidate, loopback
 *         last, capped at kMaxCandidates. */
std::vector<Candidate> LocalCandidates(PortNumber port);

/** @brief Whether any candidate names `address`. */
inline bool ContainsAddress(const std::vector<Candidate>& candidates,
                            const InetAddress& address) {
  for (const auto& candidate : candidates) {
    if (*candidate.address == address) {
      return true;
    }
  }
  return false;
}

/**
 * @brief Learns a socket's public mapping from reflectors, as a state
 *        machine driven the way ZDTPunch is.
 *
 * Reflect goes to every reflector every 100 ms until each has answered with
 * a Reflected carrying the probe's nonce, or the deadline passes. Every
 * distinct address that comes back is a Reflexive candidate; two reflectors
 * reporting different ports is how a symmetric NAT shows itself.
 */
class ReflectProbe {
 public:
  using TimePoint = std::chrono::steady_clock::time_point;

  ReflectProbe(std::vector<std::shared_ptr<InetAddress>> reflectors,
               TimePoint now, std::chrono::milliseconds timeout);

  /** @brief Whether `from` is a reflector still being asked. */
  ZNET_NODISCARD bool Owns(const InetAddress& from) const;

  /** @brief Sends whatever is due; true when something went out. */
  bool Tick(backends::UDPSocket& socket, TimePoint now);

  /** @brief Takes a Reflected from a reflector Owns() accepted. */
  void OnDatagram(const InetAddress& from, const uint8_t* data, size_t len);

  /** @brief Every reflector answered, or the deadline passed. */
  ZNET_NODISCARD bool Done(TimePoint now) const;

  /** @brief Success once any reflector answered, or none were asked;
   *         Timeout when all of them stayed silent. */
  ZNET_NODISCARD Result result() const;

  /** @brief The distinct mappings reported so far. */
  ZNET_NODISCARD const std::vector<Candidate>& reflexive() const {
    return reflexive_;
  }

 private:
  struct Target {
    std::shared_ptr<InetAddress> reflector;
    uint64_t nonce = 0;
    bool answered = false;
    TimePoint last_sent{};
  };

  std::vector<Target> targets_;
  std::vector<Candidate> reflexive_;
  TimePoint deadline_;
};

}  // namespace internal
}  // namespace p2p
}  // namespace znet

#endif  // ZNET_P2P_INTERNAL_GATHER_H_

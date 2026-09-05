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

#ifndef ZNET_P2P_PUNCH_H_
#define ZNET_P2P_PUNCH_H_

#include "znet/compat.h"
#include "znet/inet_addr.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace znet {
namespace p2p {

/**
 * @brief Where a candidate address came from, which is also how far it is
 *        likely to reach.
 */
enum class CandidateType : uint8_t {
  /** @brief A local interface at the punch port. Reaches a peer on the same
   *         network without touching the NAT. */
  Host = 0,
  /** @brief The public mapping a reflector observed, or the broker's best
   *         guess at it. Reaches a peer across any NAT that lets a punch
   *         through. */
  Reflexive = 1,
  /** @brief A relay allocation both peers were handed. Reaches anything the
   *         relay does, at the cost of the extra hop. */
  Relayed = 2,
};

/** @brief Most candidates one gathering carries or one offer names; what
 *         the rendezvous protocol is sized for. */
ZNET_INLINE_CONSTEXPR size_t kMaxCandidates = 32;

inline std::string GetCandidateTypeString(CandidateType type) {
  switch (type) {
    case CandidateType::Host:
      return "Host";
    case CandidateType::Reflexive:
      return "Reflexive";
    case CandidateType::Relayed:
      return "Relayed";
    default:
      return "Unknown";
  }
}

/**
 * @brief What two reflectors on distinct IPs reveal about the local NAT, which
 *        is how far a direct punch is likely to reach.
 */
enum class NatType : uint8_t {
  /** @brief Fewer than two distinct reflectors answered, so the mapping cannot
   *         be classified. */
  Unknown = 0,
  /** @brief Both reflectors saw the same mapping: one public port whatever the
   *         destination, so a punch reaches it. */
  EndpointIndependent = 1,
  /** @brief The reflectors saw different mappings: a fresh port per
   *         destination (symmetric), so a direct punch usually cannot and the
   *         relay is the way. */
  AddressDependent = 2,
};

inline std::string GetNatTypeString(NatType type) {
  switch (type) {
    case NatType::EndpointIndependent:
      return "EndpointIndependent";
    case NatType::AddressDependent:
      return "AddressDependent";
    case NatType::Unknown:
    default:
      return "Unknown";
  }
}

/**
 * @brief One address a peer can be reached at, and what kind it is.
 *
 * Gathering produces Host and Reflexive candidates from a socket
 * (Host::Gather); a broker adds the Relayed one. Candidates travel as a
 * list and the punch races every one of them.
 */
struct Candidate {
  CandidateType type = CandidateType::Host;
  std::shared_ptr<InetAddress> address;
  /** @brief Relayed only: the allocation's token, which the punch presents
   *         to the relay when it binds. Zero for the other types. */
  uint64_t relay_token = 0;
};

/**
 * @brief Everything one peer punches from: the other peer's candidates and
 *        the terms both sides got from the broker.
 */
struct PunchOffer {
  /** @brief The peer's candidates, every one raced at once. Relayed entries
   *         join the race after relay_delay. */
  std::vector<Candidate> candidates;
  /** @brief Issued by the broker, the same on both sides. IsInitiator derives
   *         the tiebreak from it. */
  uint64_t punch_id = 0;
  /** @brief Exactly one of the two peers passes true; see IsInitiator. */
  bool is_initiator = false;
  /** @brief Allowed for the punch, then again for the handshake. */
  std::chrono::milliseconds timeout{5000};
  /**
   * @brief How long the direct candidates have the race to themselves before
   *        a relayed one starts carrying punch traffic.
   *
   * A working direct path answers well inside this, so the relay only ever
   * wins when nothing else can. It is bound from the start, so nothing is
   * lost to the wait once it is needed.
   */
  std::chrono::milliseconds relay_delay{1000};
};

/**
 * @brief The tiebreak both peers compute from the broker-issued punch id:
 *        exactly one of them comes out the initiator, which is the connecting
 *        side of the punched session. The other one accepts, so its options
 *        decide encryption and compression.
 */
inline bool IsInitiator(uint64_t punch_id, const std::string& self_id,
                        const std::string& peer_id) {
  const bool use_smaller = (punch_id & 1ULL) == 0ULL;
  const bool self_is_smaller = self_id < peer_id;
  return use_smaller == self_is_smaller;
}

}  // namespace p2p
}  // namespace znet

#endif  // ZNET_P2P_PUNCH_H_

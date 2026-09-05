//
//    Copyright 2026 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include "znet/p2p/internal/gather.h"

#include "znet/backends/zdt/zdt_wire.h"
#include "znet/logger.h"
#include "znet/p2p/internal/zdt_punch.h"

#include <algorithm>

namespace znet {
namespace p2p {
namespace internal {

using namespace backends;

namespace {

ZNET_INLINE_CONSTEXPR std::chrono::milliseconds kReflectInterval{100};

}  // namespace

std::vector<Candidate> LocalCandidates(PortNumber port) {
  std::vector<Candidate> candidates;
  for (const auto& address : GetLocalAddresses(InetProtocolVersion::IPv4)) {
    Candidate candidate;
    candidate.type = CandidateType::Host;
    candidate.address = InetAddress::from(address, port);
    if (candidate.address && candidate.address->is_valid()) {
      candidates.push_back(std::move(candidate));
    }
    if (candidates.size() >= kMaxCandidates) {
      break;
    }
  }
  return candidates;
}

std::vector<Candidate> PredictedPorts(const std::vector<Candidate>& reflexive,
                                      size_t count) {
  std::vector<Candidate> out;
  // gather the ports of every reflexive mapping that shares one external host;
  // a symmetric NAT keeps the IP and advances only the port
  std::shared_ptr<InetAddress> host;
  std::vector<int> ports;
  for (const auto& candidate : reflexive) {
    if (candidate.type != CandidateType::Reflexive || !candidate.address) {
      continue;
    }
    if (!host) {
      host = candidate.address;
    } else if (host->host_key() != candidate.address->host_key()) {
      continue;  // a different external IP is not part of this sequence
    }
    ports.push_back(static_cast<int>(candidate.address->port()));
  }
  if (!host || ports.size() < 2) {
    return out;
  }
  std::sort(ports.begin(), ports.end());
  const int stride = ports.back() - ports[ports.size() - 2];
  if (stride <= 0 || stride > kMaxPortStride) {
    return out;  // not a sequential mapping we can extend
  }
  int next = ports.back();
  for (size_t i = 0; i < count; i++) {
    next += stride;
    if (next > 65535) {
      break;
    }
    Candidate candidate;
    candidate.type = CandidateType::Reflexive;
    candidate.address = host->WithPort(static_cast<PortNumber>(next));
    if (candidate.address) {
      out.push_back(std::move(candidate));
    }
  }
  return out;
}

ReflectProbe::ReflectProbe(std::vector<std::shared_ptr<InetAddress>> reflectors,
                           TimePoint now, std::chrono::milliseconds timeout)
    : deadline_(now + timeout) {
  for (auto& reflector : reflectors) {
    if (!reflector || !reflector->is_valid()) {
      continue;
    }
    Target target;
    target.reflector = std::move(reflector);
    target.nonce = GenerateGuid();
    targets_.push_back(std::move(target));
  }
}

bool ReflectProbe::Owns(const InetAddress& from) const {
  for (const auto& target : targets_) {
    if (!target.answered && *target.reflector == from) {
      return true;
    }
  }
  return false;
}

bool ReflectProbe::Tick(UDPSocket& socket, TimePoint now) {
  bool sent = false;
  for (auto& target : targets_) {
    if (target.answered || now - target.last_sent < kReflectInterval) {
      continue;
    }
    Buffer probe = BuildReflect(target.nonce);
    socket.SendTo(*target.reflector, probe.data(), probe.size());
    target.last_sent = now;
    sent = true;
  }
  return sent;
}

void ReflectProbe::OnDatagram(const InetAddress& from, const uint8_t* data,
                              size_t len) {
  Buffer in(reinterpret_cast<const char*>(data), len, Endianness::BigEndian);
  ZDTOfflineMsg id;
  if (!ReadOfflineHeader(in, id) || id != ZDTOfflineMsg::Reflected ||
      in.readable_bytes() < sizeof(uint64_t)) {
    return;
  }
  const uint64_t nonce = in.ReadInt<uint64_t>();
  std::shared_ptr<InetAddress> observed = in.ReadInetAddress();
  if (!observed || !observed->is_valid()) {
    return;
  }
  for (auto& target : targets_) {
    if (target.answered || target.nonce != nonce || *target.reflector != from) {
      continue;
    }
    target.answered = true;
    target.observed = observed;  // kept per target for the NAT-type verdict
    if (!ContainsAddress(reflexive_, *observed)) {
      ZNET_LOG_INFO("Gather: {} sees this socket at {}", from.readable(),
                    observed->readable());
      Candidate candidate;
      candidate.type = CandidateType::Reflexive;
      candidate.address = std::move(observed);
      reflexive_.push_back(std::move(candidate));
    }
    return;
  }
}

bool ReflectProbe::Done(TimePoint now) const {
  if (now >= deadline_) {
    return true;
  }
  for (const auto& target : targets_) {
    if (!target.answered) {
      return false;
    }
  }
  return true;
}

Result ReflectProbe::result() const {
  return targets_.empty() || !reflexive_.empty() ? Result::Success
                                                 : Result::Timeout;
}

NatType ClassifyNat(const std::vector<Reflection>& reflections) {
  const Reflection* baseline = nullptr;
  for (const auto& reflection : reflections) {
    if (!reflection.reflector || !reflection.mapping) {
      continue;
    }
    if (!baseline) {
      baseline = &reflection;
      continue;
    }
    if (baseline->reflector->host_key() == reflection.reflector->host_key()) {
      continue;  // same reflector host tells us nothing new
    }
    return *baseline->mapping == *reflection.mapping
               ? NatType::EndpointIndependent
               : NatType::AddressDependent;
  }
  return NatType::Unknown;
}

NatType ReflectProbe::nat_type() const {
  std::vector<Reflection> reflections;
  for (const auto& target : targets_) {
    if (target.answered && target.observed) {
      reflections.push_back({target.reflector, target.observed});
    }
  }
  return ClassifyNat(reflections);
}

}  // namespace internal
}  // namespace p2p
}  // namespace znet

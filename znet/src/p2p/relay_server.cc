//
//    Copyright 2026 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include "znet/p2p/relay_server.h"

#include "znet/admission.h"
#include "znet/backends/zdt/zdt_net.h"
#include "znet/backends/zdt/zdt_wire.h"
#include "znet/detail/socket_ops.h"
#include "znet/detail/sys_win.h"
#include "znet/error.h"
#include "znet/logger.h"
#include "znet/p2p/internal/zdt_punch.h"

#include <cstring>

namespace znet {
namespace p2p {

using namespace backends;
using clock = std::chrono::steady_clock;

namespace {

// bounds how long Stop() and an expiry sweep wait on an idle relay; a
// datagram returns at once
constexpr std::chrono::milliseconds kRecvTimeout{50};

const sockaddr* AsSockaddr(const unsigned char* bytes) {
  return reinterpret_cast<const sockaddr*>(bytes);
}

// the RelayBind token, or 0 when `data` is not a well-formed bind
uint64_t BindToken(const uint8_t* data, size_t len) {
  ZDTOfflineMsg id;
  if (len < kZDTOfflineHeaderSize + sizeof(uint64_t) ||
      !PeekOfflineHeader(data, len, id) || id != ZDTOfflineMsg::RelayBind) {
    return 0;
  }
  return ReadBigEndian64(data + kZDTOfflineHeaderSize);
}

}  // namespace

RelayServer::RelayServer(const RelayServerConfig& config) : config_(config) {
  static_assert(sizeof(sockaddr_storage) <= sizeof(Slot::address),
                "Slot::address must hold a sockaddr_storage");
}

RelayServer::~RelayServer() {
  Stop();
}

Result RelayServer::Start() {
  if (running_.load()) {
    return Result::AlreadyListening;
  }
  auto bind_address = InetAddress::from(config_.bind_address, config_.port);
  if (!bind_address || !bind_address->is_valid() ||
      bind_address->ipv() == InetProtocolVersion::Unix) {
    return Result::InvalidAddress;
  }
  socket_ = std::make_shared<UDPSocket>();
  if (socket_->Open(bind_address->ipv()) != Result::Success) {
    return Result::CannotCreateSocket;
  }
  // blocking with a timeout: the loop has one socket to wait on, so the
  // kernel can park it rather than a poll
  socket_->SetReceiveTimeout(kRecvTimeout);
  if (socket_->Bind(*bind_address) != Result::Success) {
    socket_->Close();
    socket_ = nullptr;
    return Result::CannotBind;
  }
  address_ = socket_->local_address();
  ServerOptions throttle;
  throttle.max_attempts_per_source = config_.max_probes_per_source;
  throttle.attempt_window = config_.probe_window;
  probe_throttle_.reset(new AdmissionControl(throttle));
  running_.store(true);
  task_.Run([this]() { Loop(); });
  ZNET_LOG_INFO("Relay up on {}", address_->readable());
  return Result::Success;
}

void RelayServer::Stop() {
  if (!running_.exchange(false)) {
    return;
  }
  task_.RequestStop();
  task_.Wait();
  std::lock_guard<std::mutex> lock(mutex_);
  pairings_.clear();
  channels_by_token_.clear();
  if (socket_) {
    socket_->Close();
  }
}

Result RelayServer::Allocate(Allocation& out) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!running_.load()) {
    return Result::AlreadyStopped;
  }
  if (pairings_.size() >= config_.max_allocations) {
    return Result::ServerFull;
  }
  Pairing pairing;
  // the next free channel; the cap keeps this walk short
  while (pairings_.count(next_channel_) != 0) {
    next_channel_ = next_channel_ == kZDTMaxRelayChannel ? 1 : next_channel_ + 1;
  }
  pairing.channel = next_channel_;
  next_channel_ = next_channel_ == kZDTMaxRelayChannel ? 1 : next_channel_ + 1;
  // zero means "no token" to a Candidate, so it is never issued
  do {
    pairing.token = GenerateGuid();
  } while (pairing.token == 0 || channels_by_token_.count(pairing.token) != 0);
  pairing.created = clock::now();
  pairing.last_traffic = pairing.created;
  out.channel = pairing.channel;
  out.token = pairing.token;
  channels_by_token_[pairing.token] = pairing.channel;
  pairings_[pairing.channel] = pairing;
  ZNET_METRIC(metrics_.allocations_total++);
  ZNET_LOG_DEBUG("Relay: allocated channel {}", out.channel);
  return Result::Success;
}

Result RelayServer::Free(uint32_t channel) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (pairings_.count(channel) == 0) {
    return Result::PeerNotFound;
  }
  Release(channel, "freed");
  return Result::Success;
}

void RelayServer::Release(uint32_t channel, const char* why) {
  auto it = pairings_.find(channel);
  ZNET_LOG_DEBUG("Relay: channel {} released ({})", channel, why);
  channels_by_token_.erase(it->second.token);
  pairings_.erase(it);
  ZNET_METRIC(metrics_.allocations_expired++);
}

size_t RelayServer::allocation_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return pairings_.size();
}

RelayMetrics RelayServer::metrics() const {
#if ZNET_ENABLE_METRICS
  std::lock_guard<std::mutex> lock(mutex_);
  RelayMetrics out = metrics_;
  out.allocations_active = pairings_.size();
  return out;
#else
  return {};
#endif
}

void RelayServer::Loop() {
  uint8_t buffer[ZNET_MAX_BUFFER_SIZE];
  auto last_sweep = clock::now();
  while (!task_.IsStopRequested()) {
    size_t len = 0;
    sockaddr_storage from{};
    SockLen from_len = 0;
    const RecvResult result =
        socket_->RecvFrom(buffer, sizeof(buffer), len, from, from_len);
    const auto now = clock::now();
    if (result == RecvResult::Received && len != 0) {
      Slot source;
      std::memcpy(source.address, &from, sizeof(from));
      source.length = from_len;
      std::lock_guard<std::mutex> lock(mutex_);
      OnDatagram(buffer, len, source, now);
    }
    if (now - last_sweep >= kRecvTimeout) {
      std::lock_guard<std::mutex> lock(mutex_);
      Expire(now);
      last_sweep = now;
    }
  }
}

void RelayServer::OnDatagram(uint8_t* data, size_t len, const Slot& source,
                             clock::time_point now) {
  // caller holds mutex_
  uint32_t channel = 0;
  if (ReadRelayHeader(data, len, channel)) {
    auto it = pairings_.find(channel);
    if (it == pairings_.end()) {
      ZNET_METRIC(metrics_.datagrams_dropped++);
      return;
    }
    Pairing& pairing = it->second;
    int slot = -1;
    for (int i = 0; i < 2; i++) {
      if (pairing.slots[i].length != 0 &&
          SameEndpoint(AsSockaddr(pairing.slots[i].address),
                       AsSockaddr(source.address))) {
        slot = i;
        break;
      }
    }
    if (slot < 0) {
      ZNET_METRIC(metrics_.datagrams_dropped++);
      return;  // a source that did not bind this pairing gets nothing through
    }
    const Slot& other = pairing.slots[1 - slot];
    if (other.length == 0) {
      ZNET_METRIC(metrics_.datagrams_dropped++);
      return;  // nowhere to go yet; the punch resends
    }
    pairing.last_traffic = now;
    // header and all: the receiving host routes by it
    socket_->SendTo(AsSockaddr(other.address), other.length, data, len);
    ZNET_METRIC(metrics_.datagrams_relayed++);
    ZNET_METRIC(metrics_.bytes_relayed += len);
    return;
  }
  ZDTOfflineMsg id;
  if (!PeekOfflineHeader(data, len, id)) {
    ZNET_METRIC(metrics_.datagrams_dropped++);
    return;
  }
  if (id == ZDTOfflineMsg::RelayBind) {
    OnBind(data, len, source, now);
  } else if (id == ZDTOfflineMsg::Reflect && len >= kZDTReflectSize) {
    OnReflect(data, source);
  } else if (id == ZDTOfflineMsg::Reflect) {
    // a short probe would let the answer outweigh the question
    ZNET_METRIC(metrics_.probes_refused++);
  } else {
    ZNET_METRIC(metrics_.datagrams_dropped++);
  }
}

void RelayServer::OnBind(const uint8_t* data, size_t len, const Slot& source,
                         clock::time_point now) {
  const uint64_t token = BindToken(data, len);
  auto by_token = token != 0 ? channels_by_token_.find(token)
                             : channels_by_token_.end();
  if (by_token == channels_by_token_.end()) {
    ZNET_METRIC(metrics_.binds_refused++);
    return;
  }
  Pairing& pairing = pairings_[by_token->second];
  int slot = -1;
  for (int i = 0; i < 2; i++) {
    if (pairing.slots[i].length != 0 &&
        SameEndpoint(AsSockaddr(pairing.slots[i].address),
                     AsSockaddr(source.address))) {
      slot = i;  // bound already; the reply was lost
      break;
    }
  }
  if (slot < 0) {
    slot = pairing.slots[0].length == 0 ? 0
           : pairing.slots[1].length == 0 ? 1
                                          : -1;
    if (slot < 0) {
      ZNET_METRIC(metrics_.binds_refused++);
      return;  // both sides are already here; a third is not a peer
    }
    pairing.slots[slot] = source;
    ZNET_METRIC(metrics_.binds_accepted++);
    ZNET_LOG_DEBUG("Relay: channel {} bound side {}", pairing.channel, slot);
  }
  pairing.last_traffic = now;
  const Buffer bound = internal::BuildRelayBound(pairing.token, pairing.channel);
  socket_->SendTo(AsSockaddr(source.address), source.length, bound.data(),
                  bound.size());
}

void RelayServer::OnReflect(const uint8_t* data, const Slot& source) {
  // the answer names the source, so this path builds the address anyway;
  // the throttle keeps it a low-rate path
  sockaddr_storage copy{};
  std::memcpy(&copy, source.address, sizeof(copy));
  auto observed = InetAddress::from(reinterpret_cast<sockaddr*>(&copy));
  if (!observed ||
      probe_throttle_->Admit(*observed) != AdmissionControl::Verdict::Allow) {
    ZNET_METRIC(metrics_.probes_refused++);
    return;
  }
  const uint64_t nonce = ReadBigEndian64(data + kZDTOfflineHeaderSize);
  const Buffer reply = internal::BuildReflected(nonce, *observed);
  socket_->SendTo(AsSockaddr(source.address), source.length, reply.data(),
                  reply.size());
  ZNET_METRIC(metrics_.probes_answered++);
}

void RelayServer::Expire(clock::time_point now) {
  // caller holds mutex_
  for (auto it = pairings_.begin(); it != pairings_.end();) {
    const Pairing& pairing = it->second;
    const bool bound =
        pairing.slots[0].length != 0 && pairing.slots[1].length != 0;
    const bool expired =
        bound ? now - pairing.last_traffic > config_.idle_timeout
              : now - pairing.created > config_.bind_timeout;
    const uint32_t channel = it->first;
    ++it;
    if (expired) {
      Release(channel, bound ? "idle" : "never bound");
    }
  }
}

}  // namespace p2p
}  // namespace znet

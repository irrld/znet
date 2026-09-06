//
//    Copyright 2026 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include "znet/p2p/traversal_server.h"

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

// bounds how long Stop() and an expiry sweep wait on an idle port; a datagram
// returns at once
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

// --- Reflector ---------------------------------------------------------------

Reflector::Reflector(const ReflectorConfig& config) {
  ServerOptions throttle;
  throttle.max_attempts_per_source = config.max_probes_per_source;
  throttle.attempt_window = config.probe_window;
  throttle_ = std::make_unique<AdmissionControl>(throttle);
}

Reflector::~Reflector() = default;

ReflectorMetrics Reflector::metrics() const {
#if ZNET_ENABLE_METRICS
  std::lock_guard<std::mutex> lock(mutex_);
  return metrics_;
#else
  return {};
#endif
}

void Reflector::OnReflect(UDPSocket& socket, const uint8_t* data, size_t len,
                          const sockaddr* from, SockLen from_len) {
  if (len < kZDTReflectSize) {
    // a short probe would let the answer outweigh the question
    std::lock_guard<std::mutex> lock(mutex_);
    ZNET_METRIC(metrics_.probes_refused++);
    return;
  }
  // the answer names the source, so this path builds the address anyway;
  // the throttle keeps it a low-rate path
  sockaddr_storage copy{};
  std::memcpy(&copy, from, from_len);
  auto observed = InetAddress::from(reinterpret_cast<sockaddr*>(&copy));
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!observed ||
        throttle_->Admit(*observed) != AdmissionControl::Verdict::Allow) {
      ZNET_METRIC(metrics_.probes_refused++);
      return;
    }
    ZNET_METRIC(metrics_.probes_answered++);
  }
  const uint64_t nonce = ReadBigEndian64(data + kZDTOfflineHeaderSize);
  const Buffer reply = internal::BuildReflected(nonce, *observed);
  socket.SendTo(from, from_len, reply.data(), reply.size());
}

// --- Relay -------------------------------------------------------------------

Relay::Relay(const RelayConfig& config) : config_(config) {
  static_assert(sizeof(sockaddr_storage) <= sizeof(Slot::address),
                "Slot::address must hold a sockaddr_storage");
}

Relay::~Relay() = default;

void Relay::Start() {
  running_.store(true);
}

void Relay::Stop() {
  running_.store(false);
  std::lock_guard<std::mutex> lock(mutex_);
  pairings_.clear();
  channels_by_token_.clear();
}

Result Relay::Allocate(Allocation& out) {
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

Result Relay::Free(uint32_t channel) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (pairings_.count(channel) == 0) {
    return Result::PeerNotFound;
  }
  Release(channel, "freed");
  return Result::Success;
}

void Relay::Release(uint32_t channel, const char* why) {
  auto it = pairings_.find(channel);
  ZNET_LOG_DEBUG("Relay: channel {} released ({})", channel, why);
  channels_by_token_.erase(it->second.token);
  pairings_.erase(it);
  ZNET_METRIC(metrics_.allocations_expired++);
}

size_t Relay::allocation_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return pairings_.size();
}

RelayMetrics Relay::metrics() const {
#if ZNET_ENABLE_METRICS
  std::lock_guard<std::mutex> lock(mutex_);
  RelayMetrics out = metrics_;
  out.allocations_active = pairings_.size();
  return out;
#else
  return {};
#endif
}

void Relay::OnForward(UDPSocket& socket, uint32_t channel, uint8_t* data,
                      size_t len, const sockaddr* from, clock::time_point now) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = pairings_.find(channel);
  if (it == pairings_.end()) {
    ZNET_METRIC(metrics_.datagrams_dropped++);
    return;
  }
  Pairing& pairing = it->second;
  int slot = -1;
  for (int i = 0; i < 2; i++) {
    if (pairing.slots[i].length != 0 &&
        SameEndpoint(AsSockaddr(pairing.slots[i].address), from)) {
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
  // header and all: the receiving peer routes by it
  socket.SendTo(AsSockaddr(other.address), other.length, data, len);
  ZNET_METRIC(metrics_.datagrams_relayed++);
  ZNET_METRIC(metrics_.bytes_relayed += len);
}

void Relay::OnBind(UDPSocket& socket, const uint8_t* data, size_t len,
                   const sockaddr* from, SockLen from_len,
                   clock::time_point now) {
  const uint64_t token = BindToken(data, len);
  std::lock_guard<std::mutex> lock(mutex_);
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
        SameEndpoint(AsSockaddr(pairing.slots[i].address), from)) {
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
    Slot& bound = pairing.slots[slot];
    std::memcpy(bound.address, from, from_len);
    bound.length = from_len;
    ZNET_METRIC(metrics_.binds_accepted++);
    ZNET_LOG_DEBUG("Relay: channel {} bound side {}", pairing.channel, slot);
  }
  pairing.last_traffic = now;
  const Buffer bound = internal::BuildRelayBound(pairing.token, pairing.channel);
  socket.SendTo(from, from_len, bound.data(), bound.size());
}

void Relay::OnUnrecognized() {
  std::lock_guard<std::mutex> lock(mutex_);
  ZNET_METRIC(metrics_.datagrams_dropped++);
}

void Relay::Expire(clock::time_point now) {
  std::lock_guard<std::mutex> lock(mutex_);
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

// --- TraversalServer ---------------------------------------------------------

TraversalServer::TraversalServer(const TraversalServerConfig& config)
    : config_(config) {
  if (config_.enable_reflector) {
    reflector_ = std::make_unique<Reflector>(config_.reflector);
  }
  if (config_.enable_relay) {
    relay_ = std::make_unique<Relay>(config_.relay);
  }
}

TraversalServer::~TraversalServer() {
  Stop();
}

Result TraversalServer::Start() {
  if (running_.load()) {
    return Result::AlreadyListening;
  }
  if (!reflector_ && !relay_) {
    return Result::InvalidArgument;  // a server that does nothing
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
  if (relay_) {
    relay_->Start();
  }
  running_.store(true);
  task_.Run([this]() { Loop(); });
  ZNET_LOG_INFO("Traversal server up on {}", address_->readable());
  return Result::Success;
}

void TraversalServer::Stop() {
  if (!running_.exchange(false)) {
    return;
  }
  task_.RequestStop();
  task_.Wait();
  if (relay_) {
    relay_->Stop();
  }
  if (socket_) {
    socket_->Close();
  }
}

void TraversalServer::Loop() {
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
      const sockaddr* addr = reinterpret_cast<const sockaddr*>(&from);
      // one classification by the first bytes, then the half that owns it: a
      // relayed datagram carries its channel in front, a bind or a probe an
      // offline header
      uint32_t channel = 0;
      ZDTOfflineMsg id;
      if (ReadRelayHeader(buffer, len, channel)) {
        if (relay_) {
          relay_->OnForward(*socket_, channel, buffer, len, addr, now);
        }
      } else if (PeekOfflineHeader(buffer, len, id) &&
                 id == ZDTOfflineMsg::RelayBind) {
        if (relay_) {
          relay_->OnBind(*socket_, buffer, len, addr, from_len, now);
        }
      } else if (PeekOfflineHeader(buffer, len, id) &&
                 id == ZDTOfflineMsg::Reflect) {
        if (reflector_) {
          reflector_->OnReflect(*socket_, buffer, len, addr, from_len);
        } else if (relay_) {
          relay_->OnUnrecognized();
        }
      } else if (relay_) {
        // the relay is the data sink: anything it cannot place is a drop
        relay_->OnUnrecognized();
      }
    }
    if (relay_ && now - last_sweep >= kRecvTimeout) {
      relay_->Expire(now);
      last_sweep = now;
    }
  }
}

}  // namespace p2p
}  // namespace znet

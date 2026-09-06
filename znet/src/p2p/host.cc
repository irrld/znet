//
//    Copyright 2026 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include "znet/p2p/host.h"

#include "znet/backends/zdt.h"
#include "znet/backends/zdt/zdt_cookie.h"
#include "znet/error.h"
#include "znet/logger.h"
#include "znet/p2p/internal/gather.h"
#include "znet/p2p/internal/zdt_punch.h"

#include <algorithm>
#include <thread>

namespace znet {
namespace p2p {

using namespace backends;
using clock = std::chrono::steady_clock;

namespace {
// how many ports ahead to guess for a sequential symmetric NAT
ZNET_INLINE_CONSTEXPR size_t kPredictedPortCount = 8;
}  // namespace

Host::Host(const HostConfig& config) : config_(config) {}

Host::~Host() {
  Stop();
}

Result Host::Start() {
  if (running_.load()) {
    return Result::AlreadyListening;
  }
  auto bind_address = InetAddress::from(config_.bind_address, config_.bind_port);
  if (!bind_address || !bind_address->is_valid() ||
      bind_address->ipv() == InetProtocolVersion::Unix) {
    return Result::InvalidAddress;
  }
  socket_ = std::make_shared<UDPSocket>();
  if (socket_->Open(bind_address->ipv()) != Result::Success) {
    return Result::CannotCreateSocket;
  }
  socket_->SetBlocking(false);
  socket_->SetDontFragment(true);
  ApplySocketBufferSizes(*socket_,
                         config_.session_options.zdt.socket_recv_buffer,
                         config_.session_options.zdt.socket_send_buffer);
  if (socket_->Bind(*bind_address) != Result::Success) {
    ZNET_LOG_ERROR("P2P host: failed to bind {}: {}", bind_address->readable(),
                   GetLastErrorInfo());
    socket_->Close();
    socket_ = nullptr;
    return Result::CannotBind;
  }
  local_address_ = socket_->local_address();
  // seeds itself; needs znet::Init(), which p2p usage already relies on for
  // GenerateGuid during a punch.
  cookie_secret_.reset(new CookieSecret());
  running_.store(true);
  task_.Run([this]() { TickLoop(); });
  ZNET_LOG_INFO("P2P host up on {}", local_address_->readable());
  return Result::Success;
}

void Host::Stop() {
  if (!running_.exchange(false)) {
    return;
  }
  task_.RequestStop();
  task_.Wait();
  // the tick thread is gone, so its state is safe to touch from here
  {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    for (auto& punch : pending_) {
      punches_.push_back(std::move(punch));
    }
    pending_.clear();
    for (auto& gathering : pending_gathers_) {
      gathers_.push_back(std::move(gathering));
    }
    pending_gathers_.clear();
  }
  for (auto& gathering : gathers_) {
    if (gathering.on_done) {
      gathering.on_done({Result::AlreadyStopped, {}, NatType::Unknown});
    }
  }
  gathers_.clear();
  for (auto& punch : punches_) {
    if (punch.on_done) {
      punch.on_done(Result::AlreadyStopped, nullptr);
    }
  }
  punches_.clear();
  for (auto& item : routes_) {
    ResolveWaiters(item.second, Result::AlreadyStopped);
    item.second.session->Close();
    item.second.session->ReleaseHandler();
  }
  routes_.clear();
  session_count_.store(0);
  if (socket_) {
    socket_->Close();
  }
}

void Host::Gather(std::vector<std::shared_ptr<InetAddress>> reflectors,
                  std::chrono::milliseconds timeout, GatherCallback on_done) {
  Gathering gathering;
  gathering.probe.reset(
      new internal::ReflectProbe(std::move(reflectors), clock::now(), timeout));
  gathering.on_done = std::move(on_done);
  // running_ is checked under the same lock Stop() drains pending_gathers_
  // under, so a gather cannot slip in between the drain and the socket close
  // and lose its callback
  std::unique_lock<std::mutex> lock(pending_mutex_);
  if (!running_.load()) {
    lock.unlock();
    if (gathering.on_done) {
      gathering.on_done({Result::AlreadyStopped, {}, NatType::Unknown});
    }
    return;
  }
  pending_gathers_.push_back(std::move(gathering));
}

void Host::Punch(PunchOffer offer, PunchCallback on_done) {
  Result refusal = Result::Success;
  if (offer.candidates.empty()) {
    refusal = Result::InvalidAddress;
  }
  for (const auto& candidate : offer.candidates) {
    if (!candidate.address || !candidate.address->is_valid()) {
      refusal = Result::InvalidAddress;
    } else if (candidate.type == CandidateType::Relayed &&
               candidate.relay_token == 0) {
      // a relay never accepts it, so the punch would only ever time out;
      // a broker that forgot the token deserves to hear so at once
      refusal = Result::InvalidArgument;
    }
  }
  if (refusal != Result::Success) {
    if (on_done) {
      on_done(refusal, nullptr);
    }
    return;
  }
  PunchInFlight punch;
  punch.machine.reset(new internal::ZDTPunch(std::move(offer), clock::now()));
  punch.on_done = std::move(on_done);
  std::unique_lock<std::mutex> lock(pending_mutex_);
  if (!running_.load()) {
    lock.unlock();
    if (punch.on_done) {
      punch.on_done(Result::AlreadyStopped, nullptr);
    }
    return;
  }
  pending_.push_back(std::move(punch));
}

void Host::TickLoop() {
  while (!task_.IsStopRequested()) {
    ApplyPendingRebind();
    cookie_secret_->MaybeRotate(config_.session_options.zdt.cookie_secret_rotation,
                                clock::now());
    bool worked = DrainSocket();
    worked = TickGathers() || worked;
    worked = TickPunches() || worked;
    worked = ProcessSessions() || worked;
    if (!worked) {
      // same doze as a self-managed session: no spinning core when idle
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
}

std::string Host::RouteKey(const InetAddress& address, uint32_t channel) {
  // the channel alone would let a stray from anywhere claim a relayed
  // session; with the relay's address in the key it has to come from there
  std::string key = address.readable();
  if (channel != 0) {
    key += '#';
    key += std::to_string(channel);
  }
  return key;
}

bool Host::DrainSocket() {
  bool any = false;
  uint8_t buffer[ZNET_MAX_BUFFER_SIZE];
  for (;;) {
    size_t len = 0;
    std::shared_ptr<InetAddress> from;
    if (socket_->RecvFrom(buffer, sizeof(buffer), len, from) !=
            RecvResult::Received ||
        len == 0 || !from) {
      break;
    }
    any = true;
    // a relayed datagram carries its channel in front; nothing else on this
    // socket can start with the marker
    const uint8_t* data = buffer;
    size_t data_len = len;
    uint32_t channel = 0;
    if (buffer[0] == kZDTRelayMarker) {
      if (!ReadRelayHeader(buffer, len, channel)) {
        continue;
      }
      data += kZDTRelayHeaderSize;
      data_len -= kZDTRelayHeaderSize;
      if (data_len == 0) {
        continue;
      }
    }
    // migration is a property of direct sessions: a relayed peer is always
    // reached through the relay's fixed address, so its path never changes here
    const bool migratable =
        channel == 0 && config_.session_options.zdt.enable_connection_migration;
    auto route = routes_.find(RouteKey(*from, channel));
    if (route != routes_.end()) {
      // a PathChallenge from a known peer is an offline control message, not
      // session data; answer it instead of handing it to the transport
      if (migratable && !(data[0] & kFlagOnline)) {
        ZDTOfflineMsg id;
        if (PeekOfflineHeader(data, data_len, id) &&
            id == ZDTOfflineMsg::PathChallenge) {
          AnswerPathChallenge(from, data, data_len);
          continue;
        }
      }
      route->second.transport->OnDatagram(data, data_len);
      continue;
    }
    // an unknown address: a moved peer answering our challenge, or one whose
    // first datagram from the new path should trigger one
    if (migratable) {
      if (!(data[0] & kFlagOnline)) {
        ZDTOfflineMsg id;
        if (PeekOfflineHeader(data, data_len, id) &&
            id == ZDTOfflineMsg::PathResponse) {
          CompletePathMigration(from, data, data_len);
          continue;
        }
      } else {
        uint64_t cid = 0;
        if (PeekCid(data, data_len, cid) && ChallengeNewPath(cid, from)) {
          continue;  // a moved peer; the path is unproven, so drop the datagram
        }
      }
    }
    HandleOffline(from, channel, data, data_len);
  }
  return any;
}

bool Host::TickGathers() {
  {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    for (auto& gathering : pending_gathers_) {
      gathers_.push_back(std::move(gathering));
    }
    pending_gathers_.clear();
  }
  bool any = false;
  const auto now = clock::now();
  for (size_t i = 0; i < gathers_.size();) {
    Gathering& gathering = gathers_[i];
    any = gathering.probe->Tick(*socket_, now) || any;
    if (!gathering.probe->Done(now)) {
      i++;
      continue;
    }
    Gathering done = std::move(gathers_[i]);
    gathers_.erase(gathers_.begin() + static_cast<long>(i));
    if (done.on_done) {
      done.on_done(CollectGather(*done.probe));
    }
    any = true;
  }
  return any;
}

Host::GatherResult Host::CollectGather(
    const internal::ReflectProbe& probe) const {
  GatherResult result;
  result.result = probe.result();
  result.nat_type = probe.nat_type();
  // reflexive first: the broker's best candidate, and what the host ones dedup
  // against
  result.candidates = probe.reflexive();
  auto add_new = [&result](Candidate candidate) {
    if (result.candidates.size() < kMaxCandidates &&
        !internal::ContainsAddress(result.candidates, *candidate.address)) {
      result.candidates.push_back(std::move(candidate));
    }
  };
  for (auto& host : internal::LocalCandidates(punch_port())) {
    add_new(std::move(host));
  }
  // a symmetric NAT will not answer a plain punch, but if its mapping advances
  // sequentially the next ports are guessable, so offer those too
  if (result.nat_type == NatType::AddressDependent) {
    for (auto& predicted :
         internal::PredictedPorts(probe.reflexive(), kPredictedPortCount)) {
      add_new(std::move(predicted));
    }
  }
  return result;
}

bool Host::TickPunches() {
  {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    for (auto& punch : pending_) {
      punches_.push_back(std::move(punch));
    }
    pending_.clear();
  }
  // a punch toward a peer that already routes resolves with the live session,
  // which is what a duplicate pairing amounts to. Only a public identity
  // counts: two peers on different networks can share a private address,
  // and matching on one would hand the second punch the first peer's
  // session. A relayed one is not known until it binds, so it never matches
  for (size_t i = 0; i < punches_.size();) {
    Route* existing = nullptr;
    for (const auto& candidate : punches_[i].machine->offer().candidates) {
      if (candidate.type != CandidateType::Reflexive) {
        continue;
      }
      auto it = routes_.find(RouteKey(*candidate.address, 0));
      if (it != routes_.end()) {
        existing = &it->second;
        break;
      }
    }
    if (existing) {
      PunchInFlight punch = std::move(punches_[i]);
      punches_.erase(punches_.begin() + static_cast<long>(i));
      if (punch.on_done) {
        if (existing->session->IsReady()) {
          punch.on_done(Result::Success, existing->session);
        } else {
          existing->waiters.push_back(std::move(punch.on_done));
        }
      }
      continue;
    }
    i++;
  }
  bool any = false;
  const auto now = clock::now();
  for (size_t i = 0; i < punches_.size();) {
    const internal::PunchOutcome outcome =
        punches_[i].machine->Tick(*socket_, now);
    if (outcome.state == internal::PunchOutcome::State::Failed) {
      FailPunch(i, outcome.reason);
      continue;  // i now names the next punch
    }
    any = any || outcome.sent;
    i++;
  }
  return any;
}

bool Host::ProcessSessions() {
  bool any = false;
  const auto now = clock::now();
  for (auto it = routes_.begin(); it != routes_.end();) {
    auto& route = it->second;
    auto& session = route.session;
    if (!session->IsAlive()) {
      // dying before the handshake finished is a failed punch to whoever asked
      ResolveWaiters(route, Result::CannotConnect);
      // the same teardown a server worker does; see CleanupAndProcessSessions
      session->ReleaseHandler();
      it = routes_.erase(it);
      continue;
    }
    any = session->Process() || any;
    // checked after Process(), which is what advances the handshake
    if (!route.waiters.empty()) {
      if (session->IsReady()) {
        ZNET_LOG_INFO("P2P host: session with {} is ready",
                      session->remote_address()->readable());
        ResolveWaiters(route, Result::Success);
      } else if (now >= route.ready_deadline) {
        ZNET_LOG_WARN("P2P host: {} punched but never finished its handshake",
                      session->remote_address()->readable());
        ResolveWaiters(route, Result::Timeout);
        session->Close();
        session->ReleaseHandler();
        it = routes_.erase(it);
        continue;
      }
    }
    ++it;
  }
  session_count_.store(routes_.size(), std::memory_order_relaxed);
  return any;
}

void Host::ResolveWaiters(Route& route, Result result) {
  // moved out first: a callback may punch again and reach this route
  std::vector<PunchCallback> waiters;
  waiters.swap(route.waiters);
  for (auto& waiter : waiters) {
    if (waiter) {
      waiter(result, result == Result::Success ? route.session : nullptr);
    }
  }
}

void Host::HandleOffline(const std::shared_ptr<InetAddress>& from,
                         uint32_t channel, const uint8_t* data, size_t len) {
  // attribution is by source address: the gather whose reflector answered,
  // else the punch whose candidates hold the sender (and, through a relay,
  // whose channel it is). A stray, or a peer whose punch already resolved,
  // owns nothing.
  if (channel == 0) {
    for (auto& gathering : gathers_) {
      if (gathering.probe->Owns(*from)) {
        gathering.probe->OnDatagram(*from, data, len);
        return;
      }
    }
  }
  for (size_t i = 0; i < punches_.size(); i++) {
    if (!punches_[i].machine->Owns(*from, channel)) {
      continue;
    }
    const internal::PunchOutcome outcome =
        punches_[i].machine->OnDatagram(*socket_, from, channel, data, len);
    if (outcome.state == internal::PunchOutcome::State::Completed) {
      CompletePunch(i, outcome);
    } else if (outcome.state == internal::PunchOutcome::State::Failed) {
      FailPunch(i, outcome.reason);
    }
    return;
  }
}

void Host::CompletePunch(size_t index, const internal::PunchOutcome& outcome) {
  PunchInFlight punch = std::move(punches_[index]);
  punches_.erase(punches_.begin() + static_cast<long>(index));
  const PunchOffer& offer = punch.machine->offer();
  const std::shared_ptr<InetAddress>& from = outcome.from;

  // a relayed session wraps every datagram in the channel the relay handed
  // out, and budgets its MTU for the header
  ZDTConnection connection = punch.machine->connection();
  connection.relay_channel = outcome.channel;
  auto inbox = std::make_shared<ZDTInbox>();
  auto transport = std::make_unique<ZDTTransportLayer>(
      socket_, from, config_.session_options.zdt, /*drains_own_socket=*/false,
      inbox, connection, config_.session_options.common);
  ZDTTransportLayer* raw = transport.get();
  if (outcome.first_datagram != nullptr) {
    raw->OnDatagram(outcome.first_datagram, outcome.first_len);
  }
  auto session = std::make_shared<PeerSession>(
      local_address_, from, std::move(transport), ConnectionType::ZDT,
      offer.is_initiator, /*self_managed=*/false, config_.session_options);
  Route route;
  route.transport = raw;
  route.session = session;
  route.remote_guid = connection.remote_guid;
  route.local_guid = connection.local_guid;
  route.ready_deadline = clock::now() + offer.timeout;
  if (punch.on_done) {
    route.waiters.push_back(std::move(punch.on_done));
  }
  routes_[RouteKey(*from, outcome.channel)] = std::move(route);
  session_count_.store(routes_.size(), std::memory_order_relaxed);
  // ProcessSessions resolves the waiters once the handshake lands.
  ZNET_LOG_INFO("P2P host: punched {} (punch id {}){}, handshaking",
                from->readable(), offer.punch_id,
                outcome.channel != 0 ? " through the relay" : "");
}

void Host::FailPunch(size_t index, Result reason) {
  PunchInFlight punch = std::move(punches_[index]);
  punches_.erase(punches_.begin() + static_cast<long>(index));
  ZNET_LOG_WARN("P2P host: punch {} failed: {}",
                punch.machine->offer().punch_id, GetResultString(reason));
  if (punch.on_done) {
    punch.on_done(reason, nullptr);
  }
}

bool Host::ChallengeNewPath(uint64_t cid,
                            const std::shared_ptr<InetAddress>& from) {
  // a live session must claim this cid, or this is not a moved peer: it may be
  // an in-flight punch's first datagram, which HandleOffline still needs
  bool known = false;
  for (const auto& item : routes_) {
    if (item.second.remote_guid == cid && item.second.session->IsAlive()) {
      known = true;
      break;
    }
  }
  if (!known) {
    return false;
  }
  Buffer out = WritePathMessage(
      ZDTOfflineMsg::PathChallenge,
      MakePathChallenge(*cookie_secret_, from->readable(), cid));
  socket_->SendTo(*from, out.data(), out.size());
  return true;
}

void Host::CompletePathMigration(const std::shared_ptr<InetAddress>& from,
                                 const uint8_t* data, size_t len) {
  Buffer buffer(reinterpret_cast<const char*>(data), len,
                Endianness::BigEndian);
  ZDTOfflineMsg id;
  ZDTPathMessage response;
  if (!ReadOfflineHeader(buffer, id) || !ReadPathMessage(buffer, response)) {
    return;
  }
  const std::string new_key = RouteKey(*from, 0);
  // a matching cookie proves the response came back from the path we probed
  if (!VerifyPathResponse(*cookie_secret_, new_key, response)) {
    return;
  }
  for (auto it = routes_.begin(); it != routes_.end(); ++it) {
    if (it->second.remote_guid != response.cid) {
      continue;
    }
    if (!it->second.session->IsAlive() || it->first == new_key) {
      return;  // gone, or already here from a duplicate response
    }
    Route route = std::move(it->second);
    routes_.erase(it);
    route.transport->MigratePeer(from);
    routes_[new_key] = std::move(route);
    ZNET_LOG_INFO("P2P host: migrated a session to {}", new_key);
    return;
  }
}

void Host::AnswerPathChallenge(const std::shared_ptr<InetAddress>& from,
                               const uint8_t* data, size_t len) {
  Buffer buffer(reinterpret_cast<const char*>(data), len,
                Endianness::BigEndian);
  ZDTOfflineMsg id;
  ZDTPathMessage challenge;
  if (!ReadOfflineHeader(buffer, id) || !ReadPathMessage(buffer, challenge)) {
    return;
  }
  // only answer a challenge for a connection we own, so we cannot be used to
  // reflect responses for a cid that is not ours
  bool ours = false;
  for (const auto& item : routes_) {
    if (item.second.local_guid == challenge.cid &&
        item.second.session->IsAlive()) {
      ours = true;
      break;
    }
  }
  if (!ours) {
    return;
  }
  Buffer out = WritePathMessage(ZDTOfflineMsg::PathResponse, challenge);
  socket_->SendTo(*from, out.data(), out.size());
}

Result Host::Rebind(const std::string& ip, PortNumber port) {
  if (!running_.load()) {
    return Result::AlreadyStopped;
  }
  auto address = InetAddress::from(ip, port);
  if (!address || !address->is_valid() ||
      address->ipv() == InetProtocolVersion::Unix) {
    return Result::InvalidAddress;
  }
  // bind the new socket on the caller's thread; a failure leaves the host as is
  auto new_socket = std::make_shared<UDPSocket>();
  if (new_socket->Open(address->ipv()) != Result::Success) {
    return Result::CannotCreateSocket;
  }
  new_socket->SetBlocking(false);
  new_socket->SetDontFragment(true);
  ApplySocketBufferSizes(*new_socket,
                         config_.session_options.zdt.socket_recv_buffer,
                         config_.session_options.zdt.socket_send_buffer);
  if (new_socket->Bind(*address) != Result::Success) {
    ZNET_LOG_ERROR("P2P host: rebind failed to bind {}: {}", address->readable(),
                   GetLastErrorInfo());
    new_socket->Close();
    return Result::CannotBind;
  }
  // the host thread owns the socket and every transport, so it applies the swap
  std::lock_guard<std::mutex> lock(pending_mutex_);
  if (!running_.load()) {
    return Result::AlreadyStopped;
  }
  pending_rebind_ = std::move(new_socket);
  return Result::Success;
}

void Host::ApplyPendingRebind() {
  std::shared_ptr<UDPSocket> next;
  {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    next = std::move(pending_rebind_);
    pending_rebind_.reset();
  }
  if (!next) {
    return;
  }
  // retarget every session's send path onto the new socket; each peer re-paths
  // to us by cid as our datagrams start arriving from the new address
  for (auto& item : routes_) {
    item.second.transport->MigrateSocket(next);
  }
  socket_ = next;
  std::shared_ptr<InetAddress> bound = socket_->local_address();
  {
    std::lock_guard<std::mutex> lock(address_mutex_);
    local_address_ = bound;
  }
  ZNET_LOG_INFO("P2P host: rebound to {}", bound->readable());
}

}  // namespace p2p
}  // namespace znet

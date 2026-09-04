//
//    Copyright 2026 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include "znet/p2p/rendezvous_server.h"

#include "znet/p2p/internal/gather.h"
#include "znet/util.h"

namespace znet {
namespace p2p {

class RendezvousPacketHandler
    : public PacketHandler<RendezvousPacketHandler, GatheringPacket,
                           ConnectPeerPacket> {
 public:
  RendezvousPacketHandler(RendezvousServer& server,
                          std::shared_ptr<PeerSession> session)
      : server_(server), session_(std::move(session)) {}

  void OnPacket(const GatheringPacket& pk) {
    std::lock_guard<std::mutex> lock(server_.mutex_);
    auto data = session_->user_pointer<RendezvousServer::ClientData>();
    if (!server_.AllowRequest(*data)) {
      return;
    }
    // the claims are only ever relayed to this client's match, so the worst
    // a lie can do is cost that match a few wasted punch candidates. A
    // relayed claim is different: only the server hands those out.
    data->candidates.clear();
    for (const auto& candidate : pk.candidates_) {
      if (!candidate.address || !candidate.address->is_valid() ||
          candidate.address->ipv() == InetProtocolVersion::Unix ||
          candidate.type == CandidateType::Relayed) {
        continue;
      }
      data->candidates.push_back(candidate);
    }
    data->punch_port = pk.punch_port_;
  }

  void OnPacket(const ConnectPeerPacket& pk) {
    {
      std::lock_guard<std::mutex> lock(server_.mutex_);
      auto data = session_->user_pointer<RendezvousServer::ClientData>();
      if (!server_.AllowRequest(*data)) {
        return;
      }
      if (data->peer_name.empty()) {
        ZNET_LOG_INFO(
            "{} asked for peer {} before it was welcomed; ignoring.",
            session_->id(), pk.target_peer_);
        return;
      }
      data->pending_targets.insert(pk.target_peer_);
      server_.connect_peer_queue_.push_front(
          std::make_pair(session_, pk.target_peer_));
    }
    server_.cv_.notify_one();
  }

 private:
  RendezvousServer& server_;
  std::shared_ptr<PeerSession> session_;
};

namespace {

ServerConfig MakeLinkConfig(const RendezvousServerConfig& config) {
  ServerConfig out{};
  out.bind_address = config.bind_address;
  out.bind_port = config.bind_port;
  out.connection_timeout = std::chrono::seconds(5);
  out.connection_type = ConnectionType::TCP;
  // the rendezvous is a Server like any other, so the listener-level
  // protections (lists, per-source connection throttle, max_connections)
  // come from here
  out.options = config.options;
  return out;
}

}  // namespace

RendezvousServer::RendezvousServer(const RendezvousServerConfig& config)
    : config_(config),
      server_(MakeLinkConfig(config)),
      punch_id_rng_(std::random_device{}()) {
  // before Listen(): MainProcessor fires events unconditionally
  server_.SetEventCallback(ZNET_BIND_FN(OnEvent));
}

RendezvousServer::~RendezvousServer() {
  Stop();
  Wait();
}

Result RendezvousServer::Start() {
  if (config_.relay_enabled) {
    // resolved here, once: a hostname costs a lookup, and a name that does
    // not resolve is a configuration error to refuse now rather than a null
    // to trip over on the first client
    relay_host_ = InetAddress::from(
        config_.relay_host.empty() ? "0.0.0.0" : config_.relay_host, 0);
    if (!relay_host_ || !relay_host_->is_valid() ||
        relay_host_->ipv() == InetProtocolVersion::Unix) {
      ZNET_LOG_ERROR("Rendezvous: relay_host \"{}\" does not resolve",
                     config_.relay_host);
      relay_host_ = nullptr;
      return Result::InvalidAddress;
    }
    relay_.reset(new RelayServer(config_.relay));
    const Result result = relay_->Start();
    if (result != Result::Success) {
      ZNET_LOG_ERROR("Rendezvous: the relay failed to start: {}",
                     GetResultString(result));
      relay_ = nullptr;
      return result;
    }
  }
  Result result = server_.Bind();
  if (result == Result::Success) {
    result = server_.Listen();
  }
  if (result != Result::Success) {
    if (relay_) {
      relay_->Stop();  // no listener, so nothing will ever allocate on it
      relay_ = nullptr;
    }
    return result;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = false;
  }
  pairing_task_.Run([this]() { PairingLoop(); });
  return Result::Success;
}

void RendezvousServer::Stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  cv_.notify_all();
  server_.Stop();
  if (relay_) {
    relay_->Stop();
  }
}

void RendezvousServer::Wait() {
  pairing_task_.Wait();
  server_.Wait();
}

void RendezvousServer::OnEvent(Event& event) {
  EventDispatcher dispatcher{event};
  dispatcher.Dispatch<IncomingClientConnectedEvent>(
      ZNET_BIND_FN(OnConnectEvent));
  dispatcher.Dispatch<IncomingClientDisconnectedEvent>(
      ZNET_BIND_FN(OnDisconnectEvent));
}

bool RendezvousServer::OnConnectEvent(IncomingClientConnectedEvent& event) {
  PeerSession& session = *event.session();
  session.SetCodec(BuildRendezvousCodec());
  session.SetHandler(
      std::make_shared<RendezvousPacketHandler>(*this, event.session()));
  auto data = std::make_shared<ClientData>();
  data->session = event.session();
  session.SetUserPointer(data);
  // named on arrival: there is nothing to ask a client before it knows who
  // it is and where to gather from
  {
    std::lock_guard<std::mutex> lock(mutex_);
    welcome_queue_.push_front(event.session());
  }
  cv_.notify_one();
  return false;
}

bool RendezvousServer::OnDisconnectEvent(IncomingClientDisconnectedEvent& event) {
  auto data = event.session()->user_pointer<ClientData>();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (data && !data->peer_name.empty()) {
      clear_queue_.push_front(data->peer_name);
    } else {
      return false;
    }
  }
  cv_.notify_one();
  return false;
}

void RendezvousServer::PairingLoop() {
  for (;;) {
    std::deque<std::shared_ptr<PeerSession>> local_welcome_q;
    std::deque<std::pair<std::shared_ptr<PeerSession>, std::string>>
        local_connect_q;
    std::deque<std::string> local_clear_q;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this]() {
        return stop_ || !welcome_queue_.empty() ||
               !connect_peer_queue_.empty() || !clear_queue_.empty();
      });
      if (stop_) {
        return;
      }
      // O(1) swap: takes ownership of the data, leaves the originals empty
      welcome_queue_.swap(local_welcome_q);
      connect_peer_queue_.swap(local_connect_q);
      clear_queue_.swap(local_clear_q);
    }
    // re-taken per item rather than held across the batch, so the packet
    // handlers on the session workers are never blocked for long. SendPacket
    // only queues, so calling it under the lock costs nothing.
    for (const std::string& peer_name : local_clear_q) {
      std::lock_guard<std::mutex> lock(mutex_);
      registry_.erase(peer_name);
    }
    for (std::shared_ptr<PeerSession>& session : local_welcome_q) {
      Welcome(session);
    }
    for (auto& ask : local_connect_q) {
      TryPair(ask.first, ask.second);
    }
  }
}

bool RendezvousServer::AllowRequest(ClientData& data) {
  if (config_.max_requests_per_window == 0 ||
      config_.request_window.count() <= 0) {
    return true;
  }
  const auto now = std::chrono::steady_clock::now();
  if (data.request_count == 0 ||
      now - data.request_window_start > config_.request_window) {
    data.request_window_start = now;
    data.request_count = 0;
  }
  data.request_count++;
  if (data.request_count <= config_.max_requests_per_window) {
    return true;
  }
  // over the limit the client is dropped, so spam costs a reconnect and the
  // connection throttle in ServerOptions prices those
  ZNET_LOG_WARN("Disconnecting {}: over {} rendezvous requests in the window.",
                data.session->id(), config_.max_requests_per_window);
  CloseOptions options;
  options.Set<NoLingerKey>(true);
  data.session->Close(options);
  return false;
}

std::shared_ptr<InetAddress> RendezvousServer::RelayEndpoint(
    PortNumber port) const {
  return std::shared_ptr<InetAddress>(relay_host_->WithPort(port));
}

void RendezvousServer::Welcome(const std::shared_ptr<PeerSession>& session) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto data = session->user_pointer<ClientData>();
  if (!data->peer_name.empty()) {
    return;  // welcomed already
  }
  data->peer_name = GenerateUniqueName();
  if (data->peer_name.empty()) {
    ZNET_LOG_ERROR("Failed to select a peer name for {}, disconnecting!",
                   session->id());
    session->Close();
    return;
  }
  ZNET_LOG_INFO("{} is {} at {}", session->id(), data->peer_name,
                session->remote_address()->readable());
  registry_[data->peer_name] = data;

  auto welcome = std::make_shared<WelcomePacket>();
  welcome->peer_name_ = data->peer_name;
  welcome->endpoint_ = session->remote_address();
  welcome->connection_type_ = config_.punch_connection_type;
  if (relay_ && relay_->address()) {
    welcome->reflectors_.push_back(
        RelayEndpoint(relay_->address()->port()));
  }
  session->SendPacket(welcome);
}

std::vector<Candidate> RendezvousServer::OfferCandidates(
    const ClientData& client, const Candidate* relayed) const {
  // caller holds mutex_
  std::vector<Candidate> out;
  bool has_reflexive = false;
  for (const auto& candidate : client.candidates) {
    if (candidate.type == CandidateType::Reflexive) {
      has_reflexive = true;
      out.push_back(candidate);
    }
  }
  if (!has_reflexive) {
    // no reflector told the client its mapping, so the link's observed
    // address carries the truth about the IP and the client's punch port is
    // its own claim. Note the assumption: the NAT maps local port N to
    // public port N.
    std::shared_ptr<InetAddress> observed = client.session->remote_address();
    Candidate reflexive;
    reflexive.type = CandidateType::Reflexive;
    reflexive.address = client.punch_port != 0
                            ? std::shared_ptr<InetAddress>(
                                  observed->WithPort(client.punch_port))
                            : observed;
    out.push_back(std::move(reflexive));
  }
  // a host address identical to a reflexive one carries no information
  for (const auto& candidate : client.candidates) {
    if (candidate.type == CandidateType::Host &&
        !internal::ContainsAddress(out, *candidate.address)) {
      out.push_back(candidate);
    }
  }
  // room for the relayed candidate at the end, inside the wire cap
  const size_t cap = relayed != nullptr ? kMaxCandidates - 1 : kMaxCandidates;
  if (out.size() > cap) {
    out.resize(cap);
  }
  if (relayed != nullptr) {
    out.push_back(*relayed);
  }
  return out;
}

void RendezvousServer::TryPair(const std::shared_ptr<PeerSession>& session,
                               const std::string& target) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto data = session->user_pointer<ClientData>();
  if (data->pending_targets.count(target) == 0) {
    return;  // already paired by the other side's ask in this same batch
  }
  auto it = registry_.find(target);
  if (target == data->peer_name) {
    // a client is never its own match, and pairing it with itself would
    // hand it a relay port for nothing, thirty times a window
    ZNET_LOG_INFO("{} asked for itself; refused.", data->peer_name);
    it = registry_.end();
  }
  if (it == registry_.end()) {
    ZNET_LOG_INFO("{} asked for {} but it was not available yet.",
                  data->peer_name, target);
    data->pending_targets.erase(target);
    auto response = std::make_shared<PeerNotFoundPacket>();
    response->target_peer_ = target;
    session->SendPacket(response);
    return;
  }
  std::shared_ptr<ClientData> other_data = it->second;
  if (other_data->pending_targets.count(data->peer_name) == 0) {
    ZNET_LOG_INFO("{} asked for {}, waiting for other peer to do the same.",
                  data->peer_name, target);
    return;
  }
  // both asks are consumed by the pair; a later re-ask starts fresh
  data->pending_targets.erase(target);
  other_data->pending_targets.erase(data->peer_name);
  const uint64_t punch_id = punch_id_rng_();

  // one relay pairing for the two, the same token on both sides; only ZDT
  // can use it
  Candidate relayed;
  const Candidate* relayed_ptr = nullptr;
  if (relay_ && config_.punch_connection_type == ConnectionType::ZDT) {
    RelayServer::Allocation allocation;
    const Result result = relay_->Allocate(allocation);
    if (result == Result::Success) {
      relayed.type = CandidateType::Relayed;
      relayed.address = RelayEndpoint(relay_->address()->port());
      relayed.relay_token = allocation.token;
      relayed_ptr = &relayed;
    } else {
      ZNET_LOG_WARN("No relay for {} and {}: {}; offering direct only.",
                    data->peer_name, other_data->peer_name,
                    GetResultString(result));
    }
  }

  auto offer = std::make_shared<PunchOfferPacket>();
  offer->target_peer_ = other_data->peer_name;
  offer->punch_id_ = punch_id;
  offer->connection_type_ = config_.punch_connection_type;
  offer->candidates_ = OfferCandidates(*other_data, relayed_ptr);
  session->SendPacket(offer);

  offer = std::make_shared<PunchOfferPacket>();
  offer->target_peer_ = data->peer_name;
  offer->punch_id_ = punch_id;
  offer->connection_type_ = config_.punch_connection_type;
  offer->candidates_ = OfferCandidates(*data, relayed_ptr);
  other_data->session->SendPacket(offer);
}

std::string RendezvousServer::GenerateUniqueName() {
  // caller holds mutex_
  std::string name = znet::GeneratePeerName();
  size_t iterations = 0;
  while (registry_.find(name) != registry_.end()) {
    name = znet::GeneratePeerName();
    iterations++;
    if (iterations > 5000) {
      return "";
    }
  }
  return name;
}

}  // namespace p2p
}  // namespace znet

//
//    Copyright 2026 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include "znet/p2p/locator.h"

#include "znet/p2p/internal/link_handler.h"
#include "znet/p2p/punch.h"

namespace znet {
namespace p2p {

namespace {

ClientConfig MakeLinkConfig(const PeerLocatorConfig& config) {
  return ClientConfig{config.server_address, config.server_port,
                      std::chrono::seconds(10), ConnectionType::TCP, {}};
}

}  // namespace

PeerLocator::PeerLocator(const PeerLocatorConfig& config)
    : config_(config),
      host_(config.host),
      client_(MakeLinkConfig(config)) {
  client_.SetEventCallback(ZNET_BIND_FN(OnEvent));
}

PeerLocator::~PeerLocator() {
  Disconnect();
}

Result PeerLocator::Connect() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (is_running_) {
      return Result::AlreadyConnected;
    }
    is_running_ = true;
    is_ready_ = false;
    peer_name_.clear();
  }
  // the host may already be up from a previous stint on the rendezvous; the
  // mesh survives losing the link, so that is not an error
  Result result = host_.Start();
  if (result != Result::Success && result != Result::AlreadyListening) {
    std::lock_guard<std::mutex> lock(mutex_);
    is_running_ = false;
    return result;
  }
  if ((result = client_.Bind()) != Result::Success) {
    std::lock_guard<std::mutex> lock(mutex_);
    is_running_ = false;
    return result;
  }
  if ((result = client_.Connect()) != Result::Success) {
    std::lock_guard<std::mutex> lock(mutex_);
    is_running_ = false;
    return result;
  }
  return Result::Success;
}

Result PeerLocator::Disconnect() {
  Result result = client_.Disconnect();
  host_.Stop();
  std::lock_guard<std::mutex> lock(mutex_);
  is_running_ = false;
  is_ready_ = false;
  link_session_ = nullptr;
  return result;
}

Result PeerLocator::AskPeer(std::string peer_name) {
  std::shared_ptr<PeerSession> session;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    session = link_session_;
    if (session && !is_ready_) {
      return Result::NotReady;  // the gathering has not gone out yet
    }
  }
  if (!session || !session->IsAlive()) {
    return Result::NotConnected;
  }
  auto pk = std::make_shared<ConnectPeerPacket>();
  pk->target_peer_ = std::move(peer_name);
  session->SendPacket(pk);
  return Result::Success;
}

void PeerLocator::Wait() {
  client_.Wait();
}

std::string PeerLocator::peer_name() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return peer_name_;
}

void PeerLocator::OnEvent(Event& event) {
  EventDispatcher dispatcher{event};
  dispatcher.Dispatch<ClientConnectedToServerEvent>(ZNET_BIND_FN(OnConnectEvent));
  dispatcher.Dispatch<ClientDisconnectedFromServerEvent>(ZNET_BIND_FN(OnDisconnectEvent));
  dispatcher.Dispatch<ClientConnectionFailedEvent>(ZNET_BIND_FN(OnConnectionFailedEvent));
}

bool PeerLocator::OnConnectEvent(ClientConnectedToServerEvent& event) {
  std::shared_ptr<PeerSession> session = event.session();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    link_session_ = session;
  }
  session->SetCodec(BuildRendezvousCodec());
  session->SetHandler(
      std::make_shared<internal::LinkPacketHandler<PeerLocator>>(*this));
  // the server speaks first, with the welcome
  return false;
}

bool PeerLocator::OnDisconnectEvent(ClientDisconnectedFromServerEvent& event) {
  (void)event;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    is_running_ = false;
    is_ready_ = false;
    link_session_ = nullptr;
  }
  // matchmaking ended; the mesh lives on
  PeerLocatorCloseEvent close_event;
  if (event_callback_) {
    event_callback_(close_event);
  }
  return false;
}

bool PeerLocator::OnConnectionFailedEvent(ClientConnectionFailedEvent& event) {
  (void)event;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    is_running_ = false;
    link_session_ = nullptr;
  }
  FireFailed(PeerLocatorPhase::Link, Result::CannotConnect, "");
  PeerLocatorCloseEvent close_event;
  if (event_callback_) {
    event_callback_(close_event);
  }
  return false;
}

std::shared_ptr<InetAddress> PeerLocator::AtRendezvous(
    const std::shared_ptr<InetAddress>& address) const {
  if (!address || !IsUnspecifiedHost(*address)) {
    return address;
  }
  auto rendezvous = client_.server_address();
  if (!rendezvous) {
    return address;
  }
  return std::shared_ptr<InetAddress>(rendezvous->WithPort(address->port()));
}

void PeerLocator::OnWelcome(const WelcomePacket& welcome) {
  // both are known here, so neither is worth a gathering and an ask first
  if (welcome.protocol_version_ != kRendezvousProtocolVersion) {
    ZNET_LOG_ERROR("Rendezvous speaks protocol {}, this build {}",
                   welcome.protocol_version_, kRendezvousProtocolVersion);
    FireFailed(PeerLocatorPhase::Link, Result::IncompatibleVersion, "");
    client_.Disconnect();
    return;
  }
  if (welcome.connection_type_ != ConnectionType::ZDT) {
    ZNET_LOG_ERROR(
        "PeerLocator punches ZDT only; run the rendezvous with punch type "
        "zdt, or use tcp::PeerLocator.");
    FireFailed(PeerLocatorPhase::Link, Result::InvalidBackend, "");
    client_.Disconnect();
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    peer_name_ = welcome.peer_name_;
    observed_ = welcome.endpoint_;
  }
  ZNET_LOG_INFO("Rendezvous named this peer {} and sees it at {}",
                welcome.peer_name_, welcome.endpoint_->readable());
  std::vector<std::shared_ptr<InetAddress>> reflectors;
  for (const auto& reflector : welcome.reflectors_) {
    reflectors.push_back(AtRendezvous(reflector));
  }
  host_.Gather(std::move(reflectors), config_.gather_timeout,
               [this](Result result, std::vector<Candidate> candidates) {
                 OnGathered(result, std::move(candidates));
               });
}

void PeerLocator::OnGathered(Result result, std::vector<Candidate> candidates) {
  if (result == Result::AlreadyStopped) {
    FireFailed(PeerLocatorPhase::Gather, result, "");
    return;
  }
  if (result != Result::Success) {
    // the host candidates and the rendezvous's own observation still make a
    // punch worth trying
    ZNET_LOG_WARN("Gather: no reflector answered ({}); offering the local "
                  "addresses only.", GetResultString(result));
  }
  std::shared_ptr<PeerSession> session;
  std::string name;
  std::shared_ptr<InetAddress> observed;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    session = link_session_;
    name = peer_name_;
    observed = observed_;
  }
  if (!session || !session->IsAlive()) {
    return;  // the link went away under the gather; the close event covers it
  }
  auto gathering = std::make_shared<GatheringPacket>();
  gathering->punch_port_ = host_.punch_port();
  gathering->candidates_ = candidates;
  session->SendPacket(gathering);

  // what peers will be told to punch: the reflexive mapping when one was
  // learned, else the rendezvous's view at the punch port
  std::shared_ptr<InetAddress> endpoint;
  for (const auto& candidate : candidates) {
    if (candidate.type == CandidateType::Reflexive) {
      endpoint = candidate.address;
      break;
    }
  }
  if (!endpoint && observed) {
    endpoint = std::shared_ptr<InetAddress>(observed->WithPort(host_.punch_port()));
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    is_ready_ = true;
  }
  PeerLocatorReadyEvent event{name, endpoint};
  if (event_callback_) {
    event_callback_(event);
  }
}

void PeerLocator::OnPeerNotFound(const std::string& target_peer) {
  ZNET_LOG_INFO("Rendezvous does not know a peer named {}.", target_peer);
  FireFailed(PeerLocatorPhase::Exchange, Result::PeerNotFound, target_peer);
}

void PeerLocator::OnPunchOffer(const PunchOfferPacket& pk) {
  ZNET_LOG_INFO("Received a punch offer for {} with {} candidates",
                pk.target_peer_, pk.candidates_.size());
  if (pk.connection_type_ != ConnectionType::ZDT) {
    // the welcome already refused this; a backstop against a broker that
    // changes its mind
    FireFailed(PeerLocatorPhase::Punch, Result::InvalidBackend, pk.target_peer_);
    return;
  }
  std::string self_name;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    self_name = peer_name_;
  }
  const std::string target_peer = pk.target_peer_;
  const uint64_t punch_id = pk.punch_id_;
  PunchOffer offer;
  for (const auto& candidate : pk.candidates_) {
    Candidate resolved = candidate;
    resolved.address = AtRendezvous(candidate.address);
    offer.candidates.push_back(std::move(resolved));
  }
  offer.punch_id = punch_id;
  offer.is_initiator = IsInitiator(punch_id, self_name, target_peer);
  offer.timeout = config_.punch_timeout;
  offer.relay_delay = config_.relay_delay;
  host_.Punch(
      std::move(offer),
      [this, punch_id, self_name, target_peer](
          Result result, std::shared_ptr<PeerSession> session) {
        if (result == Result::Success) {
          PeerConnectedEvent event{session, punch_id, self_name, target_peer};
          if (event_callback_) {
            event_callback_(event);
          }
          return;
        }
        FireFailed(PeerLocatorPhase::Punch, result, target_peer);
      });
}

void PeerLocator::FireFailed(PeerLocatorPhase phase, Result reason,
                             const std::string& target_peer) {
  PeerLocatorFailedEvent event{phase, reason, target_peer};
  if (event_callback_) {
    event_callback_(event);
  }
}

}  // namespace p2p
}  // namespace znet

//
//    Copyright 2026 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include "znet/p2p/tcp/locator.h"

#include "znet/p2p/internal/gather.h"
#include "znet/p2p/internal/link_handler.h"
#include "znet/p2p/punch.h"
#include "znet/p2p/tcp/punch.h"

namespace znet {
namespace p2p {
namespace tcp {

PeerLocator::PeerLocator(const PeerLocatorConfig& config)
    : config_(config),
      client_(ClientConfig{config.server_address, config.server_port,
                           std::chrono::seconds(10), ConnectionType::TCP,
                           {}}) {
  client_.SetEventCallback(ZNET_BIND_FN(OnEvent));
}

PeerLocator::~PeerLocator() {
  Disconnect();
  // The worker sleeps on cv_; RequestStop alone never wakes it and ~Task
  // would deadlock on the join. The empty lock pairs with the predicate
  // check so the wakeup cannot fall between check and block.
  task_.RequestStop();
  { std::lock_guard<std::mutex> lock(mutex_); }
  cv_.notify_all();
  task_.Wait();
}

Result PeerLocator::Connect() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (is_running_) {
      return Result::AlreadyConnected;
    }
    is_running_ = true;
    wake_ = false;
    peer_name_.clear();
    session_ = nullptr;
    target_candidates_.clear();
    punch_id_ = kInvalidPunchId;
  }

  Result result;
  if (ZNET_UNLIKELY((result = client_.Bind()) != Result::Success)) ZNET_UNLIKELY_ATTR {
    std::lock_guard<std::mutex> lock(mutex_);
    is_running_ = false;
    return result;
  }
  if (ZNET_UNLIKELY((result = client_.Connect()) != Result::Success)) ZNET_UNLIKELY_ATTR {
    std::lock_guard<std::mutex> lock(mutex_);
    is_running_ = false;
    return result;
  }

  // started only after the connect took: a failed Connect() leaves no worker
  // parked on cv_ and the locator free to try again. A disconnect racing this
  // is not lost, since wake_ is a flag the predicate rechecks, not a signal.
  task_.Run([this]() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this]() { return wake_ || task_.IsStopRequested(); });
    is_running_ = false;
    if (task_.IsStopRequested()) {
      lock.unlock();
      PeerLocatorCloseEvent event;
      if (event_callback_) {
        event_callback_(event);
      }
      return;
    }
    // copy what the punch needs and let go of the mutex: the callbacks other
    // threads run take it, and a punch can last seconds
    //
    // bind the exact interface and port the link used: the punch reuses
    // that socket's NAT mapping, and a wildcard bind would collide with any
    // unrelated connection whose TIME_WAIT holds the same port on another
    // interface
    auto bind_endpoint = client_.local_address();
    auto candidates = target_candidates_;
    const uint64_t punch_id = punch_id_;
    const std::string self_name = peer_name_;
    const std::string target_name = target_peer_name_;
    const ConnectionType connection_type = connection_type_;
    session_ = nullptr;
    lock.unlock();
    if (bind_endpoint && !candidates.empty() &&
        connection_type != ConnectionType::TCP) {
      // the welcome already refused this; a backstop against a broker that
      // changes its mind
      FireFailed(PeerLocatorPhase::Punch, Result::InvalidBackend, target_name);
    } else if (bind_endpoint && !candidates.empty()) {
      // the punch binds the very port the link used, so that socket has to
      // be fully released, not just shut down: its descriptor closes with
      // the last reference to the session
      client_.Wait();
      client_.ReleaseSession();
      Result punch_result;
      std::shared_ptr<PeerSession> session = PunchSync(
          bind_endpoint, candidates,
          IsInitiator(punch_id, self_name, target_name), config_.punch_timeout,
          &punch_result);
      if (punch_result == Result::Success) {
        PeerConnectedEvent event{session, punch_id, self_name, target_name};
        if (event_callback_) {
          event_callback_(event);
        }
        return;
      }
      ZNET_LOG_ERROR("Punch failed with reason: {}", GetResultString(punch_result));
      FireFailed(PeerLocatorPhase::Punch, punch_result, target_name);
      // falls through to the close event: no session is coming
    }
    PeerLocatorCloseEvent event;
    if (event_callback_) {
      event_callback_(event);
    }
  });

  ZNET_LOG_INFO("Rendezvous link bound to {} and connected to {}",
                client_.local_address()->readable(),
                client_.server_address()->readable());
  return result;
}

Result PeerLocator::Disconnect() {
  return client_.Disconnect();
}

Result PeerLocator::AskPeer(std::string peer_name) {
  std::shared_ptr<PeerSession> session;
  {
    // written by the link's thread; AskPeer may come from any
    std::lock_guard<std::mutex> lock(mutex_);
    session = session_;
  }
  if (!session || !session->IsAlive()) {
    return Result::NotConnected;
  }
  auto pk = std::make_shared<ConnectPeerPacket>();
  pk->target_peer_ = peer_name;
  session->SendPacket(pk);
  return Result::Success;
}

void PeerLocator::Wait() {
  client_.Wait();
  task_.Wait();
}

std::string PeerLocator::peer_name() const {
  // written by the link's thread; readable from any
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
    session_ = session;
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
    wake_ = true;
  }
  cv_.notify_all();
  return false;
}

bool PeerLocator::OnConnectionFailedEvent(ClientConnectionFailedEvent& event) {
  (void)event;
  // the link died before it was ready; without this the worker would wait
  // for a disconnect event that is never coming
  FireFailed(PeerLocatorPhase::Link, Result::CannotConnect, "");
  {
    std::lock_guard<std::mutex> lock(mutex_);
    wake_ = true;
  }
  cv_.notify_all();
  return false;
}

void PeerLocator::OnWelcome(const WelcomePacket& welcome) {
  // both are known here, so neither is worth an ask first
  if (welcome.protocol_version_ != kRendezvousProtocolVersion) {
    ZNET_LOG_ERROR("Rendezvous speaks protocol {}, this build {}",
                   welcome.protocol_version_, kRendezvousProtocolVersion);
    FireFailed(PeerLocatorPhase::Link, Result::IncompatibleVersion, "");
    client_.Disconnect();
    return;
  }
  if (welcome.connection_type_ != ConnectionType::TCP) {
    ZNET_LOG_ERROR(
        "tcp::PeerLocator punches TCP only; run the rendezvous with punch "
        "type tcp, or use p2p::PeerLocator for ZDT.");
    FireFailed(PeerLocatorPhase::Link, Result::InvalidBackend, "");
    client_.Disconnect();
    return;
  }
  std::shared_ptr<PeerSession> session;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    peer_name_ = welcome.peer_name_;
    session = session_;
  }
  // the punch reuses the link's socket, so its port is the punch port and
  // every local address at that port is a host candidate. No reflector: a
  // TCP socket cannot probe one, and the rendezvous's own observation of the
  // link is the mapping the punch will reuse anyway.
  auto local = client_.local_address();
  auto gathering = std::make_shared<GatheringPacket>();
  gathering->punch_port_ = local ? local->port() : 0;
  if (local) {
    gathering->candidates_ = internal::LocalCandidates(local->port());
  }
  if (session) {
    session->SendPacket(gathering);
  }
  PeerLocatorReadyEvent event{welcome.peer_name_, welcome.endpoint_};
  if (event_callback_) {
    event_callback_(event);
  }
}

void PeerLocator::OnPeerNotFound(const std::string& target_peer) {
  ZNET_LOG_INFO("Rendezvous does not know a peer named {}.", target_peer);
  // the link stays up; the application may ask again
  FireFailed(PeerLocatorPhase::Exchange, Result::PeerNotFound, target_peer);
}

void PeerLocator::OnRepunchRequest(const std::string& from_peer) {
  ZNET_LOG_INFO("Rendezvous: {} wants to re-punch; re-asking it", from_peer);
  AskPeer(from_peer);
}

void PeerLocator::OnPunchOffer(const PunchOfferPacket& pk) {
  ZNET_LOG_INFO("Received a punch offer for {} with {} candidates",
                pk.target_peer_, pk.candidates_.size());
  {
    // the worker reads these under the same mutex once it wakes
    std::lock_guard<std::mutex> lock(mutex_);
    target_candidates_.clear();
    for (const auto& candidate : pk.candidates_) {
      if (candidate.type != CandidateType::Relayed) {
        target_candidates_.push_back(candidate.address);
      }
    }
    punch_id_ = pk.punch_id_;
    target_peer_name_ = pk.target_peer_;
    connection_type_ = pk.connection_type_;
  }
  CloseOptions options;
  options.Set<NoLingerKey>(true);
  client_.Disconnect(options);
}

void PeerLocator::FireFailed(PeerLocatorPhase phase, Result reason,
                             const std::string& target_peer) {
  PeerLocatorFailedEvent event{phase, reason, target_peer};
  if (event_callback_) {
    event_callback_(event);
  }
}

}  // namespace tcp
}  // namespace p2p
}  // namespace znet

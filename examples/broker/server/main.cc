//
//    Copyright 2026 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

//
// A broker of your own with the relay embedded, the shape a game's master
// server takes. Peers connect over TCP and say hello with what they
// gathered; every two of them are paired: one relay pairing is allocated
// and each side gets an offer holding the other's candidates plus the
// relayed one, the same token on both. Nothing else passes through here;
// the punch and the session run peer to peer, or through the relay.
//

#include "znet/codec.h"
#include "znet/init.h"
#include "znet/p2p/traversal_server.h"
#include "znet/packet_handler.h"
#include "znet/peer_session.h"
#include "znet/server.h"
#include "znet/server_events.h"
#include "znet/signal_handler.h"

#include "cxxopts.h"
#include "packets.h"

#include <mutex>
#include <random>

struct Waiting {
  std::string name;
  std::vector<p2p::Candidate> candidates;
  std::shared_ptr<PeerSession> session;
};

std::unique_ptr<p2p::TraversalServer> traversal_;
std::shared_ptr<InetAddress> traversal_endpoint_;  // as peers reach it
std::mutex mutex_;
std::unique_ptr<Waiting> waiting_;  // the peer without a partner yet
std::mt19937_64 punch_ids_{std::random_device{}()};

std::shared_ptr<OfferPacket> OfferFor(const Waiting& other, uint64_t punch_id,
                                      const p2p::Candidate* relayed) {
  auto offer = std::make_shared<OfferPacket>();
  offer->peer_name = other.name;
  offer->punch_id = punch_id;
  // as gathered: reflexive first, then host; the relayed one last
  offer->candidates = other.candidates;
  if (relayed != nullptr) {
    offer->candidates.push_back(*relayed);
  }
  return offer;
}

void Pair(const Waiting& x, const Waiting& y) {
  // one pairing for the two; a broker that cannot allocate still offers the
  // direct candidates and lets the punch try its luck
  p2p::Candidate relayed;
  const p2p::Candidate* relayed_ptr = nullptr;
  p2p::Relay::Allocation allocation;
  if (traversal_ && traversal_->relay()->Allocate(allocation) == Result::Success) {
    relayed.type = p2p::CandidateType::Relayed;
    relayed.address = traversal_endpoint_;
    relayed.relay_token = allocation.token;
    relayed_ptr = &relayed;
  }
  // both sides derive the tiebreak from this id, so it must be the same
  const uint64_t punch_id = punch_ids_();
  x.session->SendPacket(OfferFor(y, punch_id, relayed_ptr));
  y.session->SendPacket(OfferFor(x, punch_id, relayed_ptr));
  ZNET_LOG_INFO("Paired {} with {} (punch id {}, relay {})", x.name, y.name,
                punch_id, relayed_ptr ? "yes" : "no");
  // the pairing frees itself once the peers stop using it, or never bind it;
  // a broker that learns the game ended earlier calls
  // traversal_->relay()->Free(allocation.channel)
}

class BrokerHandler : public PacketHandler<BrokerHandler, HelloPacket> {
 public:
  explicit BrokerHandler(std::shared_ptr<PeerSession> session)
      : session_(std::move(session)) {}

  void OnPacket(const HelloPacket& hello) {
    ZNET_LOG_INFO("{} is here with {} candidates", hello.name,
                  hello.candidates.size());
    std::lock_guard<std::mutex> lock(mutex_);
    Waiting me{hello.name, hello.candidates, session_};
    if (!waiting_) {
      waiting_.reset(new Waiting(std::move(me)));
      return;
    }
    std::unique_ptr<Waiting> other = std::move(waiting_);
    Pair(*other, me);
  }

 private:
  std::shared_ptr<PeerSession> session_;
};

bool OnConnected(IncomingClientConnectedEvent& event) {
  event.session()->SetCodec(MakeBrokerCodec());
  event.session()->SetHandler(std::make_shared<BrokerHandler>(event.session()));
  return false;
}

bool OnDisconnected(IncomingClientDisconnectedEvent& event) {
  // a peer that left while waiting is not paired with the next arrival
  std::lock_guard<std::mutex> lock(mutex_);
  if (waiting_ && waiting_->session == event.session()) {
    waiting_.reset();
  }
  return false;
}

void OnEvent(Event& event) {
  EventDispatcher dispatcher{event};
  dispatcher.Dispatch<IncomingClientConnectedEvent>(
      ZNET_BIND_GLOBAL_FN(OnConnected));
  dispatcher.Dispatch<IncomingClientDisconnectedEvent>(
      ZNET_BIND_GLOBAL_FN(OnDisconnected));
}

int main(int argc, char* argv[]) {
  cxxopts::Options opts("broker-server",
                        "pairs peers two at a time and runs the relay they "
                        "fall back to");
  opts.add_options()
      ("t,target", "Host to listen on",
       cxxopts::value<std::string>()->default_value("0.0.0.0"))
      ("p,port", "Broker port",
       cxxopts::value<uint16_t>()->default_value("5100"))
      ("relay-host", "Host peers reach the traversal server at",
       cxxopts::value<std::string>()->default_value("127.0.0.1"))
      ("relay-port",
       "The traversal server's one UDP port (reflect + relay); 0 runs without",
       cxxopts::value<uint16_t>()->default_value("5102"))
      ("h,help", "Print usage");
  auto parsed = opts.parse(argc, argv);
  if (parsed["help"].as<bool>()) {
    std::cout << opts.help() << "\n";
    return 0;
  }

  Result result;
  if ((result = Init()) != Result::Success) {
    ZNET_LOG_ERROR("Failed to initialize znet: {}", GetResultString(result));
    return 1;
  }

  const uint16_t relay_port = parsed["relay-port"].as<uint16_t>();
  if (relay_port != 0) {
    // the same port answers reflections, so peers gather against it too
    p2p::TraversalServerConfig config;
    config.bind_address = parsed["target"].as<std::string>();
    config.port = relay_port;
    traversal_.reset(new p2p::TraversalServer(config));
    if ((result = traversal_->Start()) != Result::Success) {
      ZNET_LOG_ERROR("Traversal server failed to start: {}",
                     GetResultString(result));
      return 1;
    }
    traversal_endpoint_ =
        InetAddress::from(parsed["relay-host"].as<std::string>(), relay_port);
  }

  ServerConfig config{parsed["target"].as<std::string>(),
                      parsed["port"].as<uint16_t>(), std::chrono::seconds(10),
                      ConnectionType::TCP};
  Server server{config};
  RegisterSignalHandler(
      [&server](Signal) -> bool {
        server.Stop();
        return server.shutdown_complete();
      },
      kSignalInterrupt);
  server.SetEventCallback(ZNET_BIND_GLOBAL_FN(OnEvent));
  if ((result = server.Bind()) != Result::Success ||
      (result = server.Listen()) != Result::Success) {
    ZNET_LOG_ERROR("Broker failed to start: {}", GetResultString(result));
    return 1;
  }
  server.Wait();
  if (traversal_) {
    traversal_->Stop();
  }
  Cleanup();
  return 0;
}

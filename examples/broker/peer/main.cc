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
// A peer of the broker next door: gather against its relay, say hello,
// punch on the offer that comes back, and swap a note with whoever it
// named. Run two of these with different names against one broker-server.
//

#include "znet/client.h"
#include "znet/client_events.h"
#include "znet/codec.h"
#include "znet/init.h"
#include "znet/p2p/host.h"
#include "znet/p2p/punch.h"
#include "znet/packet_handler.h"
#include "znet/peer_session.h"

#include "cxxopts.h"
#include "packets.h"

#include <atomic>
#include <future>
#include <mutex>
#include <thread>

std::string name_;
std::unique_ptr<p2p::Host> host_;
std::vector<p2p::Candidate> gathered_;
std::mutex mutex_;
std::shared_ptr<PeerSession> peer_;  // the punched session, once ready
std::atomic<bool> settled_{false};   // the punch resolved either way
std::atomic<bool> note_received_{false};

class NoteHandler : public PacketHandler<NoteHandler, NotePacket> {
 public:
  void OnPacket(const NotePacket& note) {
    ZNET_LOG_INFO("{} got: {}", name_, note.text);
    note_received_ = true;
  }
};

// The offer is the broker's word on whom to punch; the tiebreak comes from
// its punch id and the two names.
class OfferHandler : public PacketHandler<OfferHandler, OfferPacket> {
 public:
  void OnPacket(const OfferPacket& pk) {
    ZNET_LOG_INFO("Offered {} with {} candidates", pk.peer_name,
                  pk.candidates.size());
    p2p::PunchOffer offer;
    offer.candidates = pk.candidates;
    offer.punch_id = pk.punch_id;
    offer.is_initiator = p2p::IsInitiator(pk.punch_id, name_, pk.peer_name);
    host_->Punch(std::move(offer), [](Result result,
                                       std::shared_ptr<PeerSession> session) {
      // on the host's thread, with a session whose handshake is done
      if (result != Result::Success) {
        ZNET_LOG_ERROR("Punch failed: {}", GetResultString(result));
        settled_ = true;
        return;
      }
      session->SetCodec(MakePeerCodec());
      session->SetHandler(std::make_shared<NoteHandler>());
      ZNET_LOG_INFO("Connected; peer address {}",
                    session->remote_address()->readable());
      auto note = std::make_shared<NotePacket>();
      note->text = "hello from " + name_;
      session->SendPacket(note);
      std::lock_guard<std::mutex> lock(mutex_);
      peer_ = session;
      settled_ = true;
    });
  }
};

bool OnConnected(ClientConnectedToServerEvent& event) {
  event.session()->SetCodec(MakeBrokerCodec());
  event.session()->SetHandler(std::make_shared<OfferHandler>());
  auto hello = std::make_shared<HelloPacket>();
  hello->name = name_;
  hello->candidates = gathered_;
  event.session()->SendPacket(hello);
  return false;
}

bool OnConnectionFailed(ClientConnectionFailedEvent&) {
  ZNET_LOG_ERROR("Could not reach the broker");
  settled_ = true;
  return false;
}

void OnEvent(Event& event) {
  EventDispatcher dispatcher{event};
  dispatcher.Dispatch<ClientConnectedToServerEvent>(
      ZNET_BIND_GLOBAL_FN(OnConnected));
  dispatcher.Dispatch<ClientConnectionFailedEvent>(
      ZNET_BIND_GLOBAL_FN(OnConnectionFailed));
}

int main(int argc, char* argv[]) {
  cxxopts::Options opts("broker-peer",
                        "gathers, registers with broker-server and punches "
                        "whoever it is paired with");
  opts.add_options()
      ("n,name", "This peer's name", cxxopts::value<std::string>())
      ("b,broker", "Broker host",
       cxxopts::value<std::string>()->default_value("127.0.0.1"))
      ("p,port", "Broker port",
       cxxopts::value<uint16_t>()->default_value("5100"))
      ("relay-port", "The broker's relay port, gathered against; 0 for none",
       cxxopts::value<uint16_t>()->default_value("5102"))
      ("h,help", "Print usage");
  auto parsed = opts.parse(argc, argv);
  if (parsed["help"].as<bool>() || parsed.count("name") == 0) {
    std::cout << opts.help() << "\n";
    return parsed["help"].as<bool>() ? 0 : 1;
  }
  name_ = parsed["name"].as<std::string>();
  const std::string broker = parsed["broker"].as<std::string>();
  const uint16_t relay_port = parsed["relay-port"].as<uint16_t>();

  Result result;
  if ((result = Init()) != Result::Success) {
    ZNET_LOG_ERROR("Failed to initialize znet: {}", GetResultString(result));
    return 1;
  }

  // Step 1: the punch socket, and what it can be reached at. The relay is
  // the reflector; without one only the host candidates come back.
  p2p::HostConfig host_config;
  host_.reset(new p2p::Host(host_config));
  if ((result = host_->Start()) != Result::Success) {
    ZNET_LOG_ERROR("Host failed to start: {}", GetResultString(result));
    return 1;
  }
  std::vector<std::shared_ptr<InetAddress>> reflectors;
  if (relay_port != 0) {
    reflectors.push_back(InetAddress::from(broker, relay_port));
  }
  std::promise<std::vector<p2p::Candidate>> gathering;
  host_->Gather(reflectors, std::chrono::seconds(2),
                [&](Result gather_result, std::vector<p2p::Candidate> found) {
                  if (gather_result != Result::Success) {
                    ZNET_LOG_WARN("Gather: {}", GetResultString(gather_result));
                  }
                  gathering.set_value(std::move(found));
                });
  gathered_ = gathering.get_future().get();
  for (const auto& candidate : gathered_) {
    ZNET_LOG_INFO("Reachable at {} ({})", candidate.address->readable(),
                  p2p::GetCandidateTypeString(candidate.type));
  }

  // Step 2: hand the gathering to the broker and wait for its offer. The
  // link only has to live until the offer arrives.
  ClientConfig config{broker, parsed["port"].as<uint16_t>(),
                      std::chrono::seconds(10), ConnectionType::TCP};
  Client client{config};
  client.SetEventCallback(ZNET_BIND_GLOBAL_FN(OnEvent));
  if ((result = client.Bind()) != Result::Success ||
      (result = client.Connect()) != Result::Success) {
    ZNET_LOG_ERROR("Could not connect to the broker: {}", GetResultString(result));
    return 1;
  }

  // Step 3 happens in OfferHandler; wait for it to resolve, then for the
  // other side's note
  while (!settled_) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  client.Disconnect();
  std::shared_ptr<PeerSession> peer;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    peer = peer_;
  }
  if (!peer) {
    host_->Stop();
    return 1;
  }
  for (int i = 0; i < 100 && !note_received_; i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  peer->Close();
  host_->Stop();
  Cleanup();
  return note_received_ ? 0 : 1;
}

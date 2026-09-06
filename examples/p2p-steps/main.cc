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
// The three steps by hand, between two agents in one process: both gather
// against a relay, the candidates are swapped in memory (the exchange a
// broker would carry), and both punch. --force-relay leaves the direct
// candidates out, so the relay carries the session the way it does for two
// peers that cannot reach each other.
//

#include "znet/codec.h"
#include "znet/init.h"
#include "znet/p2p/agent.h"
#include "znet/p2p/punch.h"
#include "znet/p2p/relay_server.h"
#include "znet/packet_handler.h"
#include "znet/peer_session.h"

#include "cxxopts.h"

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

using namespace znet;

enum PacketType : PacketId {
  kPacketNote,
};

class NotePacket : public Packet {
 public:
  NotePacket() : Packet(kPacketNote) {}

  std::string text;
};

class NoteSerializer : public PacketSerializer<NotePacket> {
 public:
  std::shared_ptr<Buffer> SerializeTyped(
      std::shared_ptr<NotePacket> packet,
      std::shared_ptr<Buffer> buffer) override {
    buffer->WriteString(packet->text);
    return buffer;
  }

  std::shared_ptr<NotePacket> DeserializeTyped(
      std::shared_ptr<Buffer> buffer) override {
    auto packet = std::make_shared<NotePacket>();
    packet->text = buffer->ReadString();
    return packet;
  }
};

std::atomic<int> notes_received_{0};

class NoteHandler : public PacketHandler<NoteHandler, NotePacket> {
 public:
  explicit NoteHandler(std::string name) : name_(std::move(name)) {}

  void OnPacket(const NotePacket& note) {
    ZNET_LOG_INFO("{} got: {}", name_, note.text);
    notes_received_++;
  }

 private:
  std::string name_;
};

// Step 1, gather. It resolves on the agent's thread; this waits for it.
std::vector<p2p::Candidate> GatherSync(p2p::Agent& agent, const std::string& name,
                                       std::shared_ptr<InetAddress> reflector) {
  std::promise<std::vector<p2p::Candidate>> promise;
  auto future = promise.get_future();
  agent.Gather({std::move(reflector)}, std::chrono::seconds(2),
              [&](p2p::Agent::GatherResult result) {
                if (result.result != Result::Success) {
                  ZNET_LOG_WARN("{}: gather: {}", name,
                                GetResultString(result.result));
                }
                if (result.nat_type != p2p::NatType::Unknown) {
                  ZNET_LOG_INFO("{}: NAT looks {}", name,
                                p2p::GetNatTypeString(result.nat_type));
                }
                promise.set_value(std::move(result.candidates));
              });
  std::vector<p2p::Candidate> candidates = future.get();
  for (const auto& candidate : candidates) {
    ZNET_LOG_INFO("{} is reachable at {} ({})", name,
                  candidate.address->readable(),
                  p2p::GetCandidateTypeString(candidate.type));
  }
  return candidates;
}

// Step 2, the exchange: what a broker hands one side. The peer's candidates
// as gathered, reflexive first, plus the relayed one both sides share.
p2p::PunchOffer OfferFor(std::vector<p2p::Candidate> peer_candidates,
                         const p2p::Candidate& relayed, uint64_t punch_id,
                         bool is_initiator, bool force_relay) {
  p2p::PunchOffer offer;
  if (!force_relay) {
    offer.candidates = std::move(peer_candidates);
  }
  offer.candidates.push_back(relayed);
  offer.punch_id = punch_id;
  offer.is_initiator = is_initiator;
  // the direct candidates get this long to themselves; on --force-relay
  // there are none, so this is simply how long the punch waits
  offer.relay_delay = std::chrono::milliseconds(500);
  return offer;
}

// Step 3, the punch. The callback fires on the agent's thread with a session
// whose handshake is done, so the codec and handler belong right there.
std::future<std::shared_ptr<PeerSession>> StartPunch(
    p2p::Agent& agent, const std::string& name, p2p::PunchOffer offer,
    std::shared_ptr<Codec> codec) {
  auto promise = std::make_shared<std::promise<std::shared_ptr<PeerSession>>>();
  agent.Punch(std::move(offer), [promise, name, codec](
                                   Result result,
                                   std::shared_ptr<PeerSession> session) {
    if (result != Result::Success) {
      ZNET_LOG_ERROR("{}: punch failed: {}", name, GetResultString(result));
      promise->set_value(nullptr);
      return;
    }
    session->SetCodec(codec);
    session->SetHandler(std::make_shared<NoteHandler>(name));
    ZNET_LOG_INFO("{}: connected, peer address {}", name,
                  session->remote_address()->readable());
    promise->set_value(session);
  });
  return promise->get_future();
}

void SendNote(const std::shared_ptr<PeerSession>& session,
              const std::string& text) {
  auto note = std::make_shared<NotePacket>();
  note->text = text;
  session->SendPacket(note);
}

int main(int argc, char* argv[]) {
  cxxopts::Options opts("p2p-steps",
                        "gather, exchange and punch between two agents in one "
                        "process, through a relay when told to");
  opts.add_options()
      ("force-relay", "Offer only the relayed candidate, so the relay carries it")
      ("h,help", "Print usage");
  auto parsed = opts.parse(argc, argv);
  if (parsed["help"].as<bool>()) {
    std::cout << opts.help() << "\n";
    return 0;
  }
  const bool force_relay = parsed["force-relay"].as<bool>();

  Result result;
  if ((result = Init()) != Result::Success) {
    ZNET_LOG_ERROR("Failed to initialize znet: {}", GetResultString(result));
    return 1;
  }

  // The relay doubles as the reflector both agents gather against. Port 0
  // takes an ephemeral one; a deployment fixes it and opens it for UDP.
  p2p::RelayServerConfig relay_config;
  relay_config.bind_address = "127.0.0.1";
  relay_config.port = 0;
  p2p::RelayServer relay{relay_config};
  if ((result = relay.Start()) != Result::Success) {
    ZNET_LOG_ERROR("Relay failed to start: {}", GetResultString(result));
    return 1;
  }

  // One agent per peer: the socket every punch and session of that peer
  // runs on. Two in one process only because this is an example.
  p2p::AgentConfig agent_config;
  agent_config.bind_address = "127.0.0.1";
  p2p::Agent a{agent_config};
  p2p::Agent b{agent_config};
  if (a.Start() != Result::Success || b.Start() != Result::Success) {
    ZNET_LOG_ERROR("An agent failed to start");
    return 1;
  }

  std::vector<p2p::Candidate> a_candidates = GatherSync(a, "a", relay.address());
  std::vector<p2p::Candidate> b_candidates = GatherSync(b, "b", relay.address());

  // A broker allocates one pairing for the two and hands both the same
  // token; the relay's address plus that token is the Relayed candidate.
  p2p::RelayServer::Allocation allocation;
  if ((result = relay.Allocate(allocation)) != Result::Success) {
    ZNET_LOG_ERROR("No relay pairing: {}", GetResultString(result));
    return 1;
  }
  p2p::Candidate relayed;
  relayed.type = p2p::CandidateType::Relayed;
  relayed.address = relay.address();
  relayed.relay_token = allocation.token;

  // The broker also picks the punch id; IsInitiator turns it into the
  // tiebreak, so exactly one side initiates.
  const uint64_t punch_id = 7;
  const bool a_initiates = p2p::IsInitiator(punch_id, "a", "b");

  auto codec = std::make_shared<Codec>();
  codec->Add(kPacketNote, std::make_unique<NoteSerializer>());
  auto a_future = StartPunch(
      a, "a", OfferFor(b_candidates, relayed, punch_id, a_initiates, force_relay),
      codec);
  auto b_future = StartPunch(
      b, "b", OfferFor(a_candidates, relayed, punch_id, !a_initiates, force_relay),
      codec);
  std::shared_ptr<PeerSession> a_session = a_future.get();
  std::shared_ptr<PeerSession> b_session = b_future.get();
  if (!a_session || !b_session) {
    return 1;
  }

  // Both sessions are ready and driven by their agents; traffic is end to
  // end encrypted whether or not the relay is in the path.
  SendNote(a_session, "hello from a");
  SendNote(b_session, "hello from b");
  for (int i = 0; i < 300 && notes_received_ < 2; i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  const p2p::RelayMetrics metrics = relay.metrics();
  ZNET_LOG_INFO("relay forwarded {} datagrams ({} bytes)",
                metrics.datagrams_relayed, metrics.bytes_relayed);

  a_session->Close();
  b_session->Close();
  a.Stop();
  b.Stop();
  // a broker that knows the pairing is over frees it; otherwise its timers do
  relay.Free(allocation.channel);
  relay.Stop();
  Cleanup();
  return notes_received_ == 2 ? 0 : 1;
}

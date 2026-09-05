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
// Test doubles the P2P suites share: a locator plus everything its events
// reported, collected thread-safely.
//

#ifndef ZNET_TESTS_P2P_PROBES_H_
#define ZNET_TESTS_P2P_PROBES_H_

#include "znet/codec.h"
#include "znet/p2p/locator.h"
#include "znet/p2p/tcp/locator.h"
#include "znet/packet.h"
#include "znet/packet_handler.h"
#include "znet/packet_serializer.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace znet {
namespace test {

// the one packet punched sessions speak in the tests
enum NotePacketType : PacketId { kPacketNote = 1 };

class NotePacket : public Packet {
 public:
  NotePacket() : Packet(kPacketNote) {}
  std::string text;
};

class NoteSerializer : public PacketSerializer<NotePacket> {
 public:
  std::shared_ptr<Buffer> SerializeTyped(std::shared_ptr<NotePacket> packet,
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

inline std::shared_ptr<Codec> MakeNoteCodec() {
  auto codec = std::make_shared<Codec>();
  codec->Add(kPacketNote, std::make_unique<NoteSerializer>());
  return codec;
}

class NoteCollector : public PacketHandler<NoteCollector, NotePacket> {
 public:
  void OnPacket(std::shared_ptr<NotePacket> packet) {
    std::lock_guard<std::mutex> lock(mutex);
    notes.push_back(packet->text);
    count++;
  }
  std::mutex mutex;
  std::vector<std::string> notes;
  std::atomic<int> count{0};
};

template <typename Pred>
bool WaitUntil(Pred pred, int ms) {
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (!pred() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return pred();
}

// one Host::Punch outcome, collected thread-safely off the host's thread
struct PunchOutcome {
  std::mutex mutex;
  Result result = Result::Failure;
  std::shared_ptr<PeerSession> session;
  std::atomic<bool> done{false};

  p2p::Host::PunchCallback Callback() {
    return [this](Result r, std::shared_ptr<PeerSession> s) {
      {
        std::lock_guard<std::mutex> lock(mutex);
        result = r;
        session = std::move(s);
      }
      done = true;
    };
  }

  std::shared_ptr<PeerSession> Session() {
    std::lock_guard<std::mutex> lock(mutex);
    return session;
  }
};

// one Host::Gather outcome, the same way
struct GatherOutcome {
  std::mutex mutex;
  Result result = Result::Failure;
  std::vector<p2p::Candidate> candidates;
  p2p::NatType nat_type = p2p::NatType::Unknown;
  std::atomic<bool> done{false};

  p2p::Host::GatherCallback Callback() {
    return [this](p2p::Host::GatherResult r) {
      {
        std::lock_guard<std::mutex> lock(mutex);
        result = r.result;
        candidates = std::move(r.candidates);
        nat_type = r.nat_type;
      }
      done = true;
    };
  }

  std::vector<p2p::Candidate> Candidates() {
    std::lock_guard<std::mutex> lock(mutex);
    return candidates;
  }

  int CountOf(p2p::CandidateType type) {
    int count = 0;
    for (const auto& candidate : Candidates()) {
      count += candidate.type == type ? 1 : 0;
    }
    return count;
  }
};

// one locator plus everything its events reported, collected thread-safely.
// Events fire on the link's thread (failed, close), and on the host tick for
// the Host-based locator (ready, connected) or a worker for the TCP one.
template <typename Locator, typename Config>
struct LocatorProbeOf {
  Locator locator;
  std::mutex mutex;
  std::string name;
  std::shared_ptr<InetAddress> endpoint;
  std::vector<std::shared_ptr<PeerSession>> sessions;
  p2p::PeerLocatorPhase failed_phase{};
  Result failed_reason{};
  std::string failed_target;
  std::atomic<int> connected{0};
  std::atomic<bool> ready{false};
  // readiness sampled inside PeerConnectedEvent, which is where a caller would
  // install its codec and handler
  std::atomic<bool> ready_at_event{false};
  std::atomic<bool> failed{false};
  std::atomic<bool> closed{false};

  explicit LocatorProbeOf(const Config& config) : locator(config) {
    locator.SetEventCallback([this](Event& event) {
      EventDispatcher dispatcher{event};
      dispatcher.Dispatch<p2p::PeerLocatorReadyEvent>(
          [this](p2p::PeerLocatorReadyEvent& ev) {
            {
              std::lock_guard<std::mutex> lock(mutex);
              name = ev.peer_name();
              endpoint = ev.endpoint();
            }
            ready = true;
            return false;
          });
      dispatcher.Dispatch<p2p::PeerConnectedEvent>(
          [this](p2p::PeerConnectedEvent& ev) {
            {
              std::lock_guard<std::mutex> lock(mutex);
              sessions.push_back(ev.session());
            }
            ready_at_event = ev.session() && ev.session()->IsReady();
            connected++;
            return false;
          });
      dispatcher.Dispatch<p2p::PeerLocatorFailedEvent>(
          [this](p2p::PeerLocatorFailedEvent& ev) {
            {
              std::lock_guard<std::mutex> lock(mutex);
              failed_phase = ev.phase();
              failed_reason = ev.reason();
              failed_target = ev.target_peer();
            }
            failed = true;
            return false;
          });
      dispatcher.Dispatch<p2p::PeerLocatorCloseEvent>(
          [this](p2p::PeerLocatorCloseEvent&) {
            closed = true;
            return false;
          });
    });
  }

  std::string Name() {
    std::lock_guard<std::mutex> lock(mutex);
    return name;
  }

  std::shared_ptr<InetAddress> Endpoint() {
    std::lock_guard<std::mutex> lock(mutex);
    return endpoint;
  }

  // the first punched session, or null
  std::shared_ptr<PeerSession> Session() {
    std::lock_guard<std::mutex> lock(mutex);
    return sessions.empty() ? nullptr : sessions.front();
  }

  bool AllSessionsReady(int expected) {
    std::lock_guard<std::mutex> lock(mutex);
    if (static_cast<int>(sessions.size()) != expected) {
      return false;
    }
    for (const auto& session : sessions) {
      if (!session->IsReady()) {
        return false;
      }
    }
    return true;
  }
};

// the ZDT locator over a Host, punching from a loopback socket
struct HostLocatorProbe
    : LocatorProbeOf<p2p::PeerLocator, p2p::PeerLocatorConfig> {
  static p2p::PeerLocatorConfig ConfigFor(PortNumber rendezvous_port) {
    p2p::PeerLocatorConfig config;
    config.server_address = "127.0.0.1";
    config.server_port = rendezvous_port;
    config.host.bind_address = "127.0.0.1";
    return config;
  }
  explicit HostLocatorProbe(PortNumber rendezvous_port)
      : LocatorProbeOf(ConfigFor(rendezvous_port)) {}
};

// the one-shot TCP locator
struct TCPLocatorProbe
    : LocatorProbeOf<p2p::tcp::PeerLocator, p2p::tcp::PeerLocatorConfig> {
  static p2p::tcp::PeerLocatorConfig ConfigFor(PortNumber rendezvous_port) {
    p2p::tcp::PeerLocatorConfig config;
    config.server_address = "127.0.0.1";
    config.server_port = rendezvous_port;
    return config;
  }
  explicit TCPLocatorProbe(PortNumber rendezvous_port)
      : LocatorProbeOf(ConfigFor(rendezvous_port)) {}
};

}  // namespace test
}  // namespace znet

#endif  // ZNET_TESTS_P2P_PROBES_H_

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
// Locator tests: the tcp::PeerLocator lifecycle (the destructor has to wake
// the worker out of its condition wait and join it; a regression here is a
// hang, so lifecycle cases run on a helper thread and fail on a deadline
// instead of deadlocking the suite), the whole rendezvous-and-punch flow end
// to end against an in-process RendezvousServer on loopback with both
// locators, the exchange itself spoken by hand, and the wire format.
//

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "p2p_probes.h"
#include "znet/client.h"
#include "znet/client_events.h"
#include "znet/detail/socket_ops.h"
#include "znet/init.h"
#include "znet/p2p/locator.h"
#include "znet/p2p/rendezvous_server.h"
#include "znet/p2p/tcp/locator.h"
#include "znet/p2p/tcp/punch.h"
#include "znet/packet_handler.h"

using namespace znet;
using znet::test::AgentLocatorProbe;
using znet::test::TCPLocatorProbe;

namespace {

// Runs fn on its own thread; returns false if it did not finish in time.
// The thread is leaked on timeout, which is fine for a failing test.
bool RunWithDeadline(std::function<void()> fn, std::chrono::seconds deadline) {
  auto done = std::make_shared<std::promise<void>>();
  std::future<void> fut = done->get_future();
  std::thread worker([done, fn]() {
    fn();
    done->set_value();
  });
  if (fut.wait_for(deadline) == std::future_status::ready) {
    worker.join();
    return true;
  }
  worker.detach();
  return false;
}

bool WaitFor(const std::atomic<bool>& flag, int ms) {
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (!flag.load() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return flag.load();
}

template <typename Pred>
bool WaitUntil(Pred pred, int ms) {
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (!pred() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return pred();
}

p2p::RendezvousServerConfig LoopbackRendezvous(ConnectionType punch_type) {
  p2p::RendezvousServerConfig config;
  config.bind_address = "127.0.0.1";
  config.bind_port = 0;
  config.punch_connection_type = punch_type;
  return config;
}

// a traversal server on loopback too, advertised as "the rendezvous host" so
// the locators exercise the substitution
void EnableLoopbackTraversal(p2p::RendezvousServerConfig& config) {
  config.traversal_enabled = true;
  config.traversal.bind_address = "127.0.0.1";
  config.traversal.port = 0;
}

// waits until the probe reports either outcome; true means connected
template <typename Probe>
bool WaitForPunchOutcome(Probe& probe, int ms) {
  return WaitUntil([&]() { return probe.connected.load() > 0 || probe.failed.load(); }, ms) &&
         probe.connected.load() > 0;
}

// two probes on one rendezvous: connect, wait for the names, ask for each
// other, and wait for both punches to resolve. True when both connected.
template <typename Probe>
bool PairUp(Probe& a, Probe& b) {
  EXPECT_EQ(a.locator.Connect(), Result::Success);
  EXPECT_EQ(b.locator.Connect(), Result::Success);
  EXPECT_TRUE(WaitFor(a.ready, 5000)) << "a never got a peer name";
  EXPECT_TRUE(WaitFor(b.ready, 5000)) << "b never got a peer name";
  EXPECT_EQ(a.locator.AskPeer(b.Name()), Result::Success);
  EXPECT_EQ(b.locator.AskPeer(a.Name()), Result::Success);
  const bool a_ok = WaitForPunchOutcome(a, 15000);
  const bool b_ok = WaitForPunchOutcome(b, 15000);
  EXPECT_TRUE(a.connected.load() || a.failed.load())
      << "a's punch neither completed nor failed";
  EXPECT_TRUE(b.connected.load() || b.failed.load())
      << "b's punch neither completed nor failed";
  return a_ok && b_ok;
}

}  // namespace

// --- tcp::PeerLocator lifecycle ---------------------------------------------

TEST(TCPPeerLocator, DestructorJoinsWorkerWithoutConnect) {
  ASSERT_EQ(znet::Init(), znet::Result::Success);
  bool finished = RunWithDeadline(
      []() {
        p2p::tcp::PeerLocatorConfig config;
        config.server_address = "127.0.0.1";
        config.server_port = 1;
        p2p::tcp::PeerLocator locator{config};
      },
      std::chrono::seconds(10));
  EXPECT_TRUE(finished) << "destructor hung with no worker started";
}

TEST(TCPPeerLocator, DestructorJoinsWorkerAfterFailedConnect) {
  ASSERT_EQ(znet::Init(), znet::Result::Success);
  bool finished = RunWithDeadline(
      []() {
        p2p::tcp::PeerLocatorConfig config;
        // Nothing listens on port 1; loopback refuses immediately.
        config.server_address = "127.0.0.1";
        config.server_port = 1;
        p2p::tcp::PeerLocator locator{config};
        znet::Result result = locator.Connect();
        EXPECT_NE(result, znet::Result::Success);
      },
      std::chrono::seconds(10));
  EXPECT_TRUE(finished) << "destructor hung joining the worker thread";
}

TEST(TCPPeerLocator, ConnectCanBeTriedAgainAfterAFailure) {
  ASSERT_EQ(Init(), Result::Success);
  p2p::tcp::PeerLocatorConfig config;
  config.server_address = "127.0.0.1";
  config.server_port = 1;
  p2p::tcp::PeerLocator locator{config};
  EXPECT_NE(locator.Connect(), Result::Success);
  // a failed attempt leaves nothing running, so a later Connect() is not
  // refused as AlreadyConnected
  EXPECT_NE(locator.Connect(), Result::AlreadyConnected);
}

// --- End to end over an in-process rendezvous ---------------------------------

// A TCP punch must rebind the link's exact port, and on a busy machine (CI
// runners especially) an unrelated connection's TIME_WAIT can hold that port
// for its full 60s. A clean failure is retryable: fresh links land on fresh
// ports, which is what an application would do too. A hang, or failing every
// attempt, is still a bug.
void RunTCPPunchEndToEnd(
    std::function<void(TCPLocatorProbe&, TCPLocatorProbe&)> check) {
  ASSERT_EQ(Init(), Result::Success);
  p2p::RendezvousServer rendezvous{LoopbackRendezvous(ConnectionType::TCP)};
  ASSERT_EQ(rendezvous.Start(), Result::Success);
  const PortNumber port = rendezvous.bind_address()->port();
  ASSERT_NE(port, 0);
  constexpr int kAttempts = 3;
  for (int attempt = 0; attempt < kAttempts; attempt++) {
    TCPLocatorProbe a{port};
    TCPLocatorProbe b{port};
    if (!PairUp(a, b)) {
      continue;
    }
    check(a, b);
    rendezvous.Stop();
    return;
  }
  rendezvous.Stop();
  FAIL() << "the punch failed on every attempt";
}

TEST(TCPPeerLocatorEndToEnd, PunchesOverTCP) {
  RunTCPPunchEndToEnd([](TCPLocatorProbe& a, TCPLocatorProbe& b) {
    // the punched sessions drive themselves; the post-punch handshake has to
    // settle on both ends
    EXPECT_TRUE(WaitUntil(
        [&]() { return a.Session()->IsReady() && b.Session()->IsReady(); },
        10000));
  });
}

// A connected event is where a caller installs its codec and handler, and the
// encryption layer owns both until the handshake finishes. So the session the
// event carries has to be ready, or anything installed is discarded and the
// connection stalls.
TEST(TCPPeerLocatorEndToEnd, ConnectedEventIsReady) {
  RunTCPPunchEndToEnd([](TCPLocatorProbe& a, TCPLocatorProbe& b) {
    EXPECT_TRUE(a.ready_at_event.load())
        << "a's connected event handed over a session still handshaking";
    EXPECT_TRUE(b.ready_at_event.load())
        << "b's connected event handed over a session still handshaking";
  });
}

// the one-shot locator is TCP only; a rendezvous brokering ZDT says so in
// its welcome and has to be met with a clean failure right there
TEST(TCPPeerLocatorEndToEnd, AZDTRendezvousFailsAtTheWelcome) {
  ASSERT_EQ(Init(), Result::Success);
  p2p::RendezvousServer rendezvous{LoopbackRendezvous(ConnectionType::ZDT)};
  ASSERT_EQ(rendezvous.Start(), Result::Success);
  TCPLocatorProbe a{rendezvous.bind_address()->port()};
  ASSERT_EQ(a.locator.Connect(), Result::Success);
  ASSERT_TRUE(WaitFor(a.failed, 5000));
  {
    std::lock_guard<std::mutex> lock(a.mutex);
    EXPECT_EQ(a.failed_phase, p2p::PeerLocatorPhase::Link);
    EXPECT_EQ(a.failed_reason, Result::InvalidBackend);
  }
  EXPECT_FALSE(a.ready.load()) << "nothing to gather for";
  EXPECT_TRUE(WaitFor(a.closed, 5000));
  rendezvous.Stop();
}

TEST(TCPPeerLocatorEndToEnd, AskingForAnUnknownPeerFailsTheExchange) {
  ASSERT_EQ(Init(), Result::Success);
  p2p::RendezvousServer rendezvous{LoopbackRendezvous(ConnectionType::TCP)};
  ASSERT_EQ(rendezvous.Start(), Result::Success);
  const PortNumber port = rendezvous.bind_address()->port();

  TCPLocatorProbe a{port};
  ASSERT_EQ(a.locator.Connect(), Result::Success);
  ASSERT_TRUE(WaitFor(a.ready, 5000));
  ASSERT_EQ(a.locator.AskPeer("no-such-peer"), Result::Success);
  ASSERT_TRUE(WaitFor(a.failed, 5000))
      << "the rendezvous must answer an unknown name, not stay silent";
  {
    std::lock_guard<std::mutex> lock(a.mutex);
    EXPECT_EQ(a.failed_phase, p2p::PeerLocatorPhase::Exchange);
    EXPECT_EQ(a.failed_reason, Result::PeerNotFound);
    EXPECT_EQ(a.failed_target, "no-such-peer");
  }
  EXPECT_EQ(a.connected.load(), 0);
  // the link survives the miss, so asking again stays possible
  a.locator.Disconnect();
  rendezvous.Stop();
}

// the same flow over ZDT, through the Agent-based locator
void RunPunchEndToEnd(
    p2p::RendezvousServerConfig config,
    std::function<void(p2p::RendezvousServer&, AgentLocatorProbe&,
                       AgentLocatorProbe&)> check) {
  ASSERT_EQ(Init(), Result::Success);
  p2p::RendezvousServer rendezvous{config};
  ASSERT_EQ(rendezvous.Start(), Result::Success);
  const PortNumber port = rendezvous.bind_address()->port();

  AgentLocatorProbe a{port};
  AgentLocatorProbe b{port};
  ASSERT_TRUE(PairUp(a, b)) << "the punch failed";
  check(rendezvous, a, b);
  a.locator.Disconnect();
  b.locator.Disconnect();
  rendezvous.Stop();
}

TEST(PeerLocatorEndToEnd, PunchesOverZDT) {
  RunPunchEndToEnd(LoopbackRendezvous(ConnectionType::ZDT),
                   [](p2p::RendezvousServer&, AgentLocatorProbe& a,
                      AgentLocatorProbe& b) {
                     EXPECT_TRUE(WaitUntil(
                         [&]() {
                           return a.Session()->IsReady() &&
                                  b.Session()->IsReady();
                         },
                         10000));
                   });
}

TEST(PeerLocatorEndToEnd, ConnectedEventIsReady) {
  RunPunchEndToEnd(LoopbackRendezvous(ConnectionType::ZDT),
                   [](p2p::RendezvousServer&, AgentLocatorProbe& a,
                      AgentLocatorProbe& b) {
                     EXPECT_TRUE(a.ready_at_event.load());
                     EXPECT_TRUE(b.ready_at_event.load());
                   });
}

// Relocate re-gathers and re-asks the connected peer; the broker nudges that
// peer to re-ask back, so the pair re-punches and both see a second connect.
// On loopback the address does not actually change, so the re-punch resolves to
// the live session, but the whole relocate -> nudge -> re-ask -> re-pair cycle
// still runs end to end.
TEST(PeerLocatorEndToEnd, RelocateRepunchesTheConnectedPeer) {
  RunPunchEndToEnd(
      LoopbackRendezvous(ConnectionType::ZDT),
      [](p2p::RendezvousServer&, AgentLocatorProbe& a, AgentLocatorProbe& b) {
        const int a_before = a.connected.load();
        const int b_before = b.connected.load();
        ASSERT_EQ(a.locator.Relocate(), Result::Success);
        EXPECT_TRUE(WaitUntil(
            [&]() {
              return a.connected.load() > a_before &&
                     b.connected.load() > b_before;
            },
            20000))
            << "the relocate did not re-punch the pair";
      });
}

#ifndef ZNET_TARGET_WIN
// std::clock() is process CPU time on POSIX, which is exactly the claim under
// test; on Windows it is wall time and the test would be meaningless.
TEST(PeerLocatorEndToEnd, IdlePunchedSessionsDoNotSpin) {
  RunPunchEndToEnd(
      LoopbackRendezvous(ConnectionType::ZDT),
      [](p2p::RendezvousServer&, AgentLocatorProbe&, AgentLocatorProbe&) {
        // two idle agents, one session each, for one wall second. Spinning
        // ticks would cost ~two CPU seconds; dozing ones cost almost nothing.
        const std::clock_t cpu_before = std::clock();
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const double cpu_seconds =
            static_cast<double>(std::clock() - cpu_before) / CLOCKS_PER_SEC;
        EXPECT_LT(cpu_seconds, 0.5)
            << "idle punched sessions must doze, not spin a core";
      });
}
#endif  // ZNET_TARGET_WIN

TEST(PeerLocatorEndToEnd, ATCPRendezvousFailsAtTheWelcome) {
  ASSERT_EQ(Init(), Result::Success);
  p2p::RendezvousServer rendezvous{LoopbackRendezvous(ConnectionType::TCP)};
  ASSERT_EQ(rendezvous.Start(), Result::Success);
  AgentLocatorProbe a{rendezvous.bind_address()->port()};
  ASSERT_EQ(a.locator.Connect(), Result::Success);
  ASSERT_TRUE(WaitFor(a.failed, 5000));
  {
    std::lock_guard<std::mutex> lock(a.mutex);
    EXPECT_EQ(a.failed_phase, p2p::PeerLocatorPhase::Link);
    EXPECT_EQ(a.failed_reason, Result::InvalidBackend);
  }
  EXPECT_FALSE(a.ready.load()) << "nothing to gather for";
  EXPECT_TRUE(WaitFor(a.closed, 5000));
  a.locator.Disconnect();
  rendezvous.Stop();
}

// a rendezvous from another protocol generation is refused at the welcome
// rather than waited on forever
TEST(PeerLocatorEndToEnd, AForeignProtocolVersionFailsAtTheWelcome) {
  ASSERT_EQ(Init(), Result::Success);
  ServerConfig config{};
  config.bind_address = "127.0.0.1";
  config.bind_port = 0;
  config.connection_type = ConnectionType::TCP;
  Server impostor{config};
  impostor.SetEventCallback([](Event& event) {
    EventDispatcher dispatcher{event};
    dispatcher.Dispatch<IncomingClientConnectedEvent>(
        [](IncomingClientConnectedEvent& ev) {
          ev.session()->SetCodec(p2p::BuildRendezvousCodec());
          auto welcome = std::make_shared<p2p::WelcomePacket>();
          welcome->protocol_version_ = p2p::kRendezvousProtocolVersion + 1;
          welcome->peer_name_ = "stranger";
          welcome->endpoint_ = ev.session()->remote_address();
          ev.session()->SendPacket(welcome);
          return false;
        });
  });
  ASSERT_EQ(impostor.Bind(), Result::Success);
  ASSERT_EQ(impostor.Listen(), Result::Success);

  AgentLocatorProbe a{impostor.bind_address()->port()};
  ASSERT_EQ(a.locator.Connect(), Result::Success);
  ASSERT_TRUE(WaitFor(a.failed, 5000));
  {
    std::lock_guard<std::mutex> lock(a.mutex);
    EXPECT_EQ(a.failed_phase, p2p::PeerLocatorPhase::Link);
    EXPECT_EQ(a.failed_reason, Result::IncompatibleVersion);
  }
  a.locator.Disconnect();
  impostor.Stop();
}

TEST(PeerLocatorEndToEnd, AskingForAnUnknownPeerFailsTheExchange) {
  ASSERT_EQ(Init(), Result::Success);
  p2p::RendezvousServer rendezvous{LoopbackRendezvous(ConnectionType::ZDT)};
  ASSERT_EQ(rendezvous.Start(), Result::Success);
  AgentLocatorProbe a{rendezvous.bind_address()->port()};
  ASSERT_EQ(a.locator.Connect(), Result::Success);
  ASSERT_TRUE(WaitFor(a.ready, 5000));
  ASSERT_EQ(a.locator.AskPeer("no-such-peer"), Result::Success);
  ASSERT_TRUE(WaitFor(a.failed, 5000));
  {
    std::lock_guard<std::mutex> lock(a.mutex);
    EXPECT_EQ(a.failed_phase, p2p::PeerLocatorPhase::Exchange);
    EXPECT_EQ(a.failed_reason, Result::PeerNotFound);
  }
  a.locator.Disconnect();
  rendezvous.Stop();
}

TEST(PeerLocatorEndToEnd, AskingBeforeTheLinkIsRefused) {
  ASSERT_EQ(Init(), Result::Success);
  AgentLocatorProbe a{1};
  EXPECT_EQ(a.locator.AskPeer("anyone"), Result::NotConnected);
}

// --- With a relay ---------------------------------------------------------------

TEST(PeerLocatorWithRelay, ReadyReportsTheReflexiveEndpoint) {
  p2p::RendezvousServerConfig config = LoopbackRendezvous(ConnectionType::ZDT);
  EnableLoopbackTraversal(config);
  RunPunchEndToEnd(config, [](p2p::RendezvousServer&, AgentLocatorProbe& a,
                              AgentLocatorProbe& b) {
    // the reflector on the traversal server's port told each socket its
    // mapping, which on loopback is the socket itself
    ASSERT_TRUE(a.Endpoint());
    EXPECT_EQ(a.Endpoint()->readable(),
              "127.0.0.1:" + std::to_string(a.locator.agent().punch_port()));
    EXPECT_EQ(b.Endpoint()->readable(),
              "127.0.0.1:" + std::to_string(b.locator.agent().punch_port()));
  });
}

TEST(PeerLocatorWithRelay, PunchesDirectAndTheRelayIsFreed) {
  p2p::RendezvousServerConfig config = LoopbackRendezvous(ConnectionType::ZDT);
  EnableLoopbackTraversal(config);
  config.traversal.relay.idle_timeout = std::chrono::milliseconds(500);
  RunPunchEndToEnd(config, [](p2p::RendezvousServer& rendezvous,
                              AgentLocatorProbe& a, AgentLocatorProbe& b) {
    // both were offered the relay and bound it, the direct path won, and
    // the unused allocation goes back once it idles
    EXPECT_NE(a.Session()->remote_address()->readable(),
              b.Session()->remote_address()->readable());
    ASSERT_NE(rendezvous.traversal(), nullptr);
    EXPECT_TRUE(WaitUntil(
        [&]() { return rendezvous.traversal()->relay()->metrics().binds_accepted == 2u; },
        3000));
    EXPECT_TRUE(WaitUntil(
        [&]() { return rendezvous.traversal()->relay()->allocation_count() == 0; }, 5000))
        << "a relay nobody uses must be freed";
  });
}

// --- The exchange, spoken by hand ----------------------------------------------

namespace {

// speaks the rendezvous protocol by hand, so it can misbehave in ways a
// locator never would
struct RawClient {
  Client client;
  std::mutex mutex;
  std::shared_ptr<PeerSession> session;
  std::atomic<bool> connected{false};
  std::string name;
  std::shared_ptr<InetAddress> observed;
  std::vector<std::shared_ptr<InetAddress>> reflectors;
  ConnectionType punch_type = ConnectionType::ZDT;
  std::atomic<int> welcome_count{0};
  std::vector<p2p::Candidate> offered;
  uint64_t punch_id = 0;
  std::atomic<int> offer_count{0};

  explicit RawClient(PortNumber port, int timeout_seconds = 5)
      : client(ClientConfig{"127.0.0.1", port,
                            std::chrono::seconds(timeout_seconds),
                            ConnectionType::TCP, {}}) {
    client.SetEventCallback([this](Event& event) {
      EventDispatcher dispatcher{event};
      dispatcher.Dispatch<ClientConnectedToServerEvent>(
          [this](ClientConnectedToServerEvent& ev) {
            auto handler = std::make_shared<CallbackPacketHandler>();
            handler->AddRef<p2p::WelcomePacket>(
                [this](const p2p::WelcomePacket& pk) {
                  {
                    std::lock_guard<std::mutex> lock(mutex);
                    name = pk.peer_name_;
                    observed = pk.endpoint_;
                    reflectors = pk.reflectors_;
                    punch_type = pk.connection_type_;
                  }
                  welcome_count++;
                });
            handler->AddRef<p2p::PunchOfferPacket>(
                [this](const p2p::PunchOfferPacket& pk) {
                  {
                    std::lock_guard<std::mutex> lock(mutex);
                    offered = pk.candidates_;
                    punch_id = pk.punch_id_;
                  }
                  offer_count++;
                });
            auto s = ev.session();
            s->SetCodec(p2p::BuildRendezvousCodec());
            s->SetHandler(handler);
            {
              std::lock_guard<std::mutex> lock(mutex);
              session = s;
            }
            connected = true;
            return false;
          });
    });
  }

  void Connect() {
    ASSERT_EQ(client.Bind(), Result::Success);
    ASSERT_EQ(client.Connect(), Result::Success);
    ASSERT_TRUE(WaitFor(connected, 5000));
    ASSERT_TRUE(WaitUntil([&]() { return welcome_count.load() >= 1; }, 5000));
  }

  std::shared_ptr<PeerSession> Session() {
    std::lock_guard<std::mutex> lock(mutex);
    return session;
  }

  std::string Name() {
    std::lock_guard<std::mutex> lock(mutex);
    return name;
  }

  void Gather(PortNumber punch_port, std::vector<p2p::Candidate> candidates) {
    auto pk = std::make_shared<p2p::GatheringPacket>();
    pk->punch_port_ = punch_port;
    pk->candidates_ = std::move(candidates);
    Session()->SendPacket(pk);
  }

  void Ask(const std::string& target) {
    auto ask = std::make_shared<p2p::ConnectPeerPacket>();
    ask->target_peer_ = target;
    Session()->SendPacket(ask);
  }

  std::vector<p2p::Candidate> Offered() {
    std::lock_guard<std::mutex> lock(mutex);
    return offered;
  }
};

p2p::Candidate Cand(p2p::CandidateType type, const std::string& host,
                    PortNumber port, uint64_t token = 0) {
  p2p::Candidate candidate;
  candidate.type = type;
  candidate.address = InetAddress::from(host, port);
  candidate.relay_token = token;
  return candidate;
}

std::vector<std::string> Readable(const std::vector<p2p::Candidate>& list) {
  std::vector<std::string> out;
  for (const auto& candidate : list) {
    out.push_back(p2p::GetCandidateTypeString(candidate.type) + " " +
                  candidate.address->readable());
  }
  return out;
}

// both ask for each other and both offers arrive
void Exchange(RawClient& a, RawClient& b) {
  a.Ask(b.Name());
  b.Ask(a.Name());
  ASSERT_TRUE(WaitUntil([&]() { return a.offer_count.load() >= 1; }, 5000));
  ASSERT_TRUE(WaitUntil([&]() { return b.offer_count.load() >= 1; }, 5000));
}

}  // namespace

TEST(RendezvousExchange, CandidatesReachTheMatchReflexiveFirst) {
  ASSERT_EQ(Init(), Result::Success);
  p2p::RendezvousServer rendezvous{LoopbackRendezvous(ConnectionType::ZDT)};
  ASSERT_EQ(rendezvous.Start(), Result::Success);
  RawClient a{rendezvous.bind_address()->port()};
  RawClient b{rendezvous.bind_address()->port()};
  a.Connect();
  b.Connect();
  EXPECT_TRUE(a.reflectors.empty()) << "no relay, no reflector";

  // a multi-homed peer cannot know which of its networks the match shares,
  // so every host candidate has to survive, after the reflexive one and
  // without any that merely repeats it
  a.Gather(4444, {Cand(p2p::CandidateType::Host, "10.0.0.1", 4444),
                  Cand(p2p::CandidateType::Reflexive, "198.51.100.7", 4444),
                  Cand(p2p::CandidateType::Host, "192.168.5.5", 4444),
                  Cand(p2p::CandidateType::Host, "198.51.100.7", 4444)});
  b.Gather(5555, {Cand(p2p::CandidateType::Reflexive, "198.51.100.8", 5555),
                  Cand(p2p::CandidateType::Host, "10.0.0.2", 5555)});
  Exchange(a, b);
  EXPECT_EQ(Readable(a.Offered()),
            (std::vector<std::string>{"Reflexive 198.51.100.8:5555",
                                      "Host 10.0.0.2:5555"}));
  EXPECT_EQ(Readable(b.Offered()),
            (std::vector<std::string>{"Reflexive 198.51.100.7:4444",
                                      "Host 10.0.0.1:4444",
                                      "Host 192.168.5.5:4444"}));
  EXPECT_EQ(a.punch_id, b.punch_id);
  a.client.Disconnect();
  b.client.Disconnect();
  rendezvous.Stop();
}

TEST(RendezvousExchange, SynthesizesAReflexiveFromTheObservedAddress) {
  ASSERT_EQ(Init(), Result::Success);
  p2p::RendezvousServer rendezvous{LoopbackRendezvous(ConnectionType::ZDT)};
  ASSERT_EQ(rendezvous.Start(), Result::Success);
  RawClient a{rendezvous.bind_address()->port()};
  RawClient b{rendezvous.bind_address()->port()};
  a.Connect();
  b.Connect();
  // no reflector answered a, so the match gets the address the rendezvous
  // saw a at, paired with a's punch port
  a.Gather(4444, {Cand(p2p::CandidateType::Host, "10.0.0.1", 4444)});
  b.Gather(5555, {});
  Exchange(a, b);
  EXPECT_EQ(Readable(b.Offered()),
            (std::vector<std::string>{"Reflexive 127.0.0.1:4444",
                                      "Host 10.0.0.1:4444"}));
  EXPECT_EQ(Readable(a.Offered()),
            (std::vector<std::string>{"Reflexive 127.0.0.1:5555"}));
  a.client.Disconnect();
  b.client.Disconnect();
  rendezvous.Stop();
}

TEST(RendezvousExchange, ARelayedClaimIsDropped) {
  ASSERT_EQ(Init(), Result::Success);
  p2p::RendezvousServer rendezvous{LoopbackRendezvous(ConnectionType::ZDT)};
  ASSERT_EQ(rendezvous.Start(), Result::Success);
  RawClient a{rendezvous.bind_address()->port()};
  RawClient b{rendezvous.bind_address()->port()};
  a.Connect();
  b.Connect();
  // only the server hands out relays; a client claiming one is lying
  a.Gather(4444, {Cand(p2p::CandidateType::Relayed, "203.0.113.9", 1, 42),
                  Cand(p2p::CandidateType::Host, "10.0.0.1", 4444)});
  b.Gather(5555, {});
  Exchange(a, b);
  EXPECT_EQ(Readable(b.Offered()),
            (std::vector<std::string>{"Reflexive 127.0.0.1:4444",
                                      "Host 10.0.0.1:4444"}));
  a.client.Disconnect();
  b.client.Disconnect();
  rendezvous.Stop();
}

TEST(RendezvousExchange, TheLatestGatheringWins) {
  ASSERT_EQ(Init(), Result::Success);
  p2p::RendezvousServer rendezvous{LoopbackRendezvous(ConnectionType::ZDT)};
  ASSERT_EQ(rendezvous.Start(), Result::Success);
  RawClient a{rendezvous.bind_address()->port()};
  RawClient b{rendezvous.bind_address()->port()};
  a.Connect();
  b.Connect();
  a.Gather(4444, {Cand(p2p::CandidateType::Host, "10.0.0.1", 4444)});
  a.Gather(4445, {Cand(p2p::CandidateType::Host, "10.0.0.9", 4445)});
  b.Gather(5555, {});
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  Exchange(a, b);
  EXPECT_EQ(Readable(b.Offered()),
            (std::vector<std::string>{"Reflexive 127.0.0.1:4445",
                                      "Host 10.0.0.9:4445"}));
  a.client.Disconnect();
  b.client.Disconnect();
  rendezvous.Stop();
}

TEST(RendezvousExchange, ARelayIsOfferedToBothSides) {
  ASSERT_EQ(Init(), Result::Success);
  p2p::RendezvousServerConfig config = LoopbackRendezvous(ConnectionType::ZDT);
  EnableLoopbackTraversal(config);
  p2p::RendezvousServer rendezvous{config};
  ASSERT_EQ(rendezvous.Start(), Result::Success);
  ASSERT_NE(rendezvous.traversal(), nullptr);
  RawClient a{rendezvous.bind_address()->port()};
  RawClient b{rendezvous.bind_address()->port()};
  a.Connect();
  b.Connect();
  // the welcome names the reflector on the rendezvous host
  ASSERT_EQ(a.reflectors.size(), 1u);
  EXPECT_TRUE(p2p::IsUnspecifiedHost(*a.reflectors[0]));
  EXPECT_EQ(a.reflectors[0]->port(),
            rendezvous.traversal()->address()->port());

  a.Gather(4444, {});
  b.Gather(5555, {});
  Exchange(a, b);
  const auto to_a = a.Offered();
  const auto to_b = b.Offered();
  ASSERT_EQ(to_a.size(), 2u);
  ASSERT_EQ(to_b.size(), 2u);
  // the same port and token for both, last in the list, on this host
  EXPECT_EQ(to_a.back().type, p2p::CandidateType::Relayed);
  EXPECT_EQ(to_b.back().type, p2p::CandidateType::Relayed);
  EXPECT_TRUE(p2p::IsUnspecifiedHost(*to_a.back().address));
  EXPECT_EQ(to_a.back().address->port(), to_b.back().address->port());
  EXPECT_EQ(to_a.back().relay_token, to_b.back().relay_token);
  EXPECT_NE(to_a.back().relay_token, 0u);
  EXPECT_EQ(rendezvous.traversal()->relay()->allocation_count(), 1u);
  a.client.Disconnect();
  b.client.Disconnect();
  rendezvous.Stop();
}

TEST(RendezvousExchange, AClientIsNeverItsOwnMatch) {
  ASSERT_EQ(Init(), Result::Success);
  p2p::RendezvousServerConfig config = LoopbackRendezvous(ConnectionType::ZDT);
  EnableLoopbackTraversal(config);
  p2p::RendezvousServer rendezvous{config};
  ASSERT_EQ(rendezvous.Start(), Result::Success);
  RawClient a{rendezvous.bind_address()->port()};
  a.Connect();
  a.Gather(4444, {});
  // asking for itself must not pair it with itself
  a.Ask(a.Name());
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  EXPECT_EQ(a.offer_count.load(), 0);
  EXPECT_EQ(rendezvous.traversal()->relay()->allocation_count(), 0u);
  a.client.Disconnect();
  rendezvous.Stop();
}

TEST(RendezvousExchange, ATraversalHostThatDoesNotResolveRefusesToStart) {
  ASSERT_EQ(Init(), Result::Success);
  p2p::RendezvousServerConfig config = LoopbackRendezvous(ConnectionType::ZDT);
  EnableLoopbackTraversal(config);
  config.traversal_host = "no-such-host.invalid";
  p2p::RendezvousServer rendezvous{config};
  EXPECT_EQ(rendezvous.Start(), Result::InvalidAddress)
      << "a bad traversal host is a configuration error, not a crash later";
}

TEST(RendezvousExchange, NoRelayForATCPPunch) {
  ASSERT_EQ(Init(), Result::Success);
  p2p::RendezvousServerConfig config = LoopbackRendezvous(ConnectionType::TCP);
  EnableLoopbackTraversal(config);
  p2p::RendezvousServer rendezvous{config};
  ASSERT_EQ(rendezvous.Start(), Result::Success);
  RawClient a{rendezvous.bind_address()->port()};
  RawClient b{rendezvous.bind_address()->port()};
  a.Connect();
  b.Connect();
  a.Gather(4444, {});
  b.Gather(5555, {});
  Exchange(a, b);
  EXPECT_EQ(a.Offered().size(), 1u) << "a TCP punch cannot use a UDP relay";
  EXPECT_EQ(rendezvous.traversal()->relay()->allocation_count(), 0u);
  a.client.Disconnect();
  b.client.Disconnect();
  rendezvous.Stop();
}

// --- The wire format --------------------------------------------------------------

TEST(RendezvousProtocol, GatheringRoundTripsEveryCandidate) {
  auto packet = std::make_shared<p2p::GatheringPacket>();
  packet->punch_port_ = 7000;
  packet->candidates_.push_back(Cand(p2p::CandidateType::Reflexive, "198.51.100.7", 7000));
  packet->candidates_.push_back(Cand(p2p::CandidateType::Host, "10.0.0.1", 7000));
  packet->candidates_.push_back(Cand(p2p::CandidateType::Host, "127.0.0.1", 7000));

  p2p::GatheringSerializer serializer;
  auto buffer = std::make_shared<Buffer>(Endianness::BigEndian);
  serializer.SerializeTyped(packet, buffer);
  auto back = serializer.DeserializeTyped(buffer);

  ASSERT_NE(back, nullptr);
  EXPECT_EQ(back->punch_port_, 7000);
  EXPECT_EQ(Readable(back->candidates_),
            (std::vector<std::string>{"Reflexive 198.51.100.7:7000",
                                      "Host 10.0.0.1:7000",
                                      "Host 127.0.0.1:7000"}));
}

TEST(RendezvousProtocol, OfferRoundTripsARelayedToken) {
  auto packet = std::make_shared<p2p::PunchOfferPacket>();
  packet->target_peer_ = "peer";
  packet->punch_id_ = 0x1122334455667788ULL;
  packet->connection_type_ = ConnectionType::ZDT;
  packet->candidates_.push_back(Cand(p2p::CandidateType::Reflexive, "198.51.100.7", 7000));
  packet->candidates_.push_back(Cand(p2p::CandidateType::Relayed, "0.0.0.0", 30001, 0xabcdefULL));

  p2p::PunchOfferSerializer serializer;
  auto buffer = std::make_shared<Buffer>(Endianness::BigEndian);
  serializer.SerializeTyped(packet, buffer);
  auto back = serializer.DeserializeTyped(buffer);

  ASSERT_NE(back, nullptr);
  EXPECT_EQ(back->target_peer_, "peer");
  EXPECT_EQ(back->punch_id_, 0x1122334455667788ULL);
  EXPECT_EQ(back->connection_type_, ConnectionType::ZDT);
  ASSERT_EQ(back->candidates_.size(), 2u);
  EXPECT_EQ(back->candidates_[0].relay_token, 0u);
  EXPECT_EQ(back->candidates_[1].type, p2p::CandidateType::Relayed);
  EXPECT_EQ(back->candidates_[1].relay_token, 0xabcdefULL);
  EXPECT_TRUE(p2p::IsUnspecifiedHost(*back->candidates_[1].address));
  EXPECT_EQ(back->candidates_[1].address->port(), 30001);
}

TEST(RendezvousProtocol, AnOversizedCandidateListIsRefused) {
  // a count past the cap must cost one refused frame, not an allocation
  auto buffer = std::make_shared<Buffer>(Endianness::BigEndian);
  buffer->WriteInt<uint16_t>(7000);
  buffer->WriteInt<uint8_t>(static_cast<uint8_t>(p2p::kMaxCandidates + 1));

  p2p::GatheringSerializer serializer;
  EXPECT_EQ(serializer.DeserializeTyped(buffer), nullptr);
}

TEST(RendezvousProtocol, ATruncatedAddressIsRefused) {
  // a short address body reads as 0.0.0.0:0, which no candidate ever is
  auto buffer = std::make_shared<Buffer>(Endianness::BigEndian);
  buffer->WriteInt<uint16_t>(7000);
  buffer->WriteInt<uint8_t>(1);
  buffer->WriteInt<uint8_t>(static_cast<uint8_t>(p2p::CandidateType::Host));
  buffer->WriteInt<uint8_t>(4);  // an IPv4 address, then nothing

  p2p::GatheringSerializer serializer;
  EXPECT_EQ(serializer.DeserializeTyped(buffer), nullptr);
}

TEST(RendezvousProtocol, AnUnknownCandidateTypeIsRefused) {
  auto buffer = std::make_shared<Buffer>(Endianness::BigEndian);
  buffer->WriteInt<uint16_t>(7000);
  buffer->WriteInt<uint8_t>(1);
  buffer->WriteInt<uint8_t>(9);  // no such type
  buffer->WriteInetAddress(*InetAddress::from("10.0.0.1", 7000));

  p2p::GatheringSerializer serializer;
  EXPECT_EQ(serializer.DeserializeTyped(buffer), nullptr);
}

// --- Rendezvous protections ----------------------------------------------------

TEST(RendezvousProtection, RequestSpamGetsTheClientDisconnected) {
  ASSERT_EQ(Init(), Result::Success);
  p2p::RendezvousServerConfig config = LoopbackRendezvous(ConnectionType::ZDT);
  config.max_requests_per_window = 3;
  p2p::RendezvousServer rendezvous{config};
  ASSERT_EQ(rendezvous.Start(), Result::Success);

  RawClient c{rendezvous.bind_address()->port()};
  c.Connect();
  for (int i = 0; i < 10; i++) {
    c.Ask("whoever");
  }
  EXPECT_TRUE(WaitUntil([&]() { return !c.Session()->IsAlive(); }, 5000))
      << "the rendezvous must drop a client that spams it";
  c.client.Disconnect();
  rendezvous.Stop();
}

TEST(RendezvousProtection, LinkHonorsServerOptions) {
  ASSERT_EQ(Init(), Result::Success);
  p2p::RendezvousServerConfig config = LoopbackRendezvous(ConnectionType::ZDT);
  config.options.denylist.push_back(CIDRBlock::Parse("127.0.0.0/8"));
  p2p::RendezvousServer rendezvous{config};
  ASSERT_EQ(rendezvous.Start(), Result::Success);

  RawClient c{rendezvous.bind_address()->port(), /*timeout_seconds=*/1};
  ASSERT_EQ(c.client.Bind(), Result::Success);
  (void)c.client.Connect();
  EXPECT_FALSE(WaitFor(c.connected, 1500))
      << "the rendezvous listener must honor its admission rules";
  rendezvous.Stop();
}

// --- TCP candidate racing --------------------------------------------------------

namespace {

PortNumber FreePortLocal(int socket_type) {
  SocketHandle probe = socket(AF_INET, socket_type, 0);
  PortNumber port = 0;
  auto any = InetAddress::from("127.0.0.1", 0);
  if (bind(probe, any->handle_ptr(), any->addr_size()) == 0) {
    sockaddr_storage addr{};
    socklen_t len = static_cast<socklen_t>(sizeof(addr));
    if (getsockname(probe, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
      auto bound = InetAddress::from(reinterpret_cast<sockaddr*>(&addr));
      if (bound) {
        port = bound->port();
      }
    }
  }
  CloseSocket(probe);
  return port;
}

// both peers punch with `dead_count` dead candidates ahead of the live one.
// The punch must still find the peer inside the budget, and it must not get
// slower just because a multi-homed peer offered more addresses: a candidate
// nobody answers has to cost a socket, not a share of the budget. The ZDT
// counterpart is P2PAgent.ManyDeadCandidatesDoNotSlowThePunch.
void RunTCPCandidateRace(PortNumber port_a, PortNumber port_b,
                         int dead_count = 1) {
  std::shared_ptr<InetAddress> local_a = InetAddress::from("127.0.0.1", port_a);
  std::shared_ptr<InetAddress> local_b = InetAddress::from("127.0.0.1", port_b);
  std::vector<std::shared_ptr<InetAddress>> to_b;
  std::vector<std::shared_ptr<InetAddress>> to_a;
  for (int i = 0; i < dead_count; i++) {
    // TEST-NET-1 space: routable nowhere, so the candidate just goes dark
    std::shared_ptr<InetAddress> dead =
        InetAddress::from("203.0.113." + std::to_string(1 + i), 9);
    to_b.push_back(dead);
    to_a.push_back(dead);
  }
  to_b.push_back(local_b);
  to_a.push_back(local_a);

  Result result_a = Result::Failure;
  Result result_b = Result::Failure;
  std::shared_ptr<PeerSession> session_a;
  std::shared_ptr<PeerSession> session_b;
  const std::chrono::milliseconds budget(10000);
  std::thread thread_a([&]() {
    session_a = p2p::tcp::PunchSync(local_a, to_b, /*is_initiator=*/true,
                                    budget, &result_a);
  });
  std::thread thread_b([&]() {
    session_b = p2p::tcp::PunchSync(local_b, to_a, /*is_initiator=*/false,
                                    budget, &result_b);
  });
  thread_a.join();
  thread_b.join();
  EXPECT_EQ(result_a, Result::Success) << "initiator punch failed";
  EXPECT_EQ(result_b, Result::Success) << "responder punch failed";
  ASSERT_TRUE(session_a);
  ASSERT_TRUE(session_b);
  EXPECT_TRUE(session_a->IsAlive());
  EXPECT_TRUE(session_b->IsAlive());
}

}  // namespace

TEST(PunchCandidates, TCPRacesPastADeadCandidate) {
  ASSERT_EQ(Init(), Result::Success);
  RunTCPCandidateRace(FreePortLocal(SOCK_STREAM), FreePortLocal(SOCK_STREAM));
}

// A multi-homed peer offers every address it has, and most of them are dead to
// anyone not on that network. The race must not care how many are dead.
TEST(PunchCandidates, TCPIsUnhurtByManyDeadCandidates) {
  ASSERT_EQ(Init(), Result::Success);
  RunTCPCandidateRace(FreePortLocal(SOCK_STREAM), FreePortLocal(SOCK_STREAM),
                      /*dead_count=*/7);
}

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
// The shared-socket P2P host: asynchronous punches, several peers on one
// socket, and the full mesh flow through an in-process rendezvous.
//

#include "p2p_probes.h"
#include "znet/init.h"
#include "znet/p2p/host.h"
#include "znet/p2p/locator.h"
#include "znet/p2p/punch.h"
#include "znet/p2p/rendezvous_server.h"
#include "znet/packet.h"
#include "znet/packet_handler.h"
#include "znet/packet_serializer.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace znet;

namespace {

using znet::test::PunchOutcome;
using znet::test::WaitUntil;

std::shared_ptr<InetAddress> HostAddr(const p2p::Host& host) {
  return InetAddress::from("127.0.0.1", host.punch_port());
}

// an offer naming the peer at every address given, as a broker would
p2p::PunchOffer Offer(std::vector<std::shared_ptr<InetAddress>> addresses,
                      uint64_t punch_id, bool is_initiator,
                      std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
  p2p::PunchOffer offer;
  for (auto& address : addresses) {
    p2p::Candidate candidate;
    candidate.type = p2p::CandidateType::Reflexive;
    candidate.address = std::move(address);
    offer.candidates.push_back(std::move(candidate));
  }
  offer.punch_id = punch_id;
  offer.is_initiator = is_initiator;
  offer.timeout = timeout;
  return offer;
}

using znet::test::MakeNoteCodec;
using znet::test::NoteCollector;
using znet::test::NotePacket;

bool BothReady(PunchOutcome& a, PunchOutcome& b) {
  auto sa = a.Session();
  auto sb = b.Session();
  return sa && sb && sa->IsReady() && sb->IsReady();
}

}  // namespace

TEST(P2PHost, PunchesAsynchronouslyAndExchangesMessages) {
  ASSERT_EQ(Init(), Result::Success);
  p2p::HostConfig config;
  config.bind_address = "127.0.0.1";
  p2p::Host a{config};
  p2p::Host b{config};
  ASSERT_EQ(a.Start(), Result::Success);
  ASSERT_EQ(b.Start(), Result::Success);

  // a dead first candidate on each side; the race has to find the real one
  std::shared_ptr<InetAddress> dead = InetAddress::from("203.0.113.1", 9);
  PunchOutcome at_a;
  PunchOutcome at_b;
  a.Punch(Offer({dead, HostAddr(b)}, 7, /*is_initiator=*/true),
               at_a.Callback());
  b.Punch(Offer({dead, HostAddr(a)}, 7, /*is_initiator=*/false),
               at_b.Callback());

  ASSERT_TRUE(WaitUntil([&]() { return at_a.done.load() && at_b.done.load(); },
                        12000));
  ASSERT_EQ(at_a.result, Result::Success);
  ASSERT_EQ(at_b.result, Result::Success);
  ASSERT_TRUE(WaitUntil([&]() { return BothReady(at_a, at_b); }, 10000))
      << "the encrypted handshake must complete over the shared sockets";

  // install the app codec and talk
  auto collector = std::make_shared<NoteCollector>();
  at_a.Session()->SetCodec(MakeNoteCodec());
  at_b.Session()->SetCodec(MakeNoteCodec());
  at_b.Session()->SetHandler(collector);

  auto note = std::make_shared<NotePacket>();
  note->text = "hello over the punched socket";
  ASSERT_EQ(at_a.Session()->SendPacket(note), Result::Success);
  ASSERT_TRUE(WaitUntil([&]() { return collector->count.load() == 1; }, 5000));
  {
    std::lock_guard<std::mutex> lock(collector->mutex);
    EXPECT_EQ(collector->notes[0], "hello over the punched socket");
  }
  a.Stop();
  b.Stop();
}

// the responder may speak from inside its ready callback, and that lands on
// the initiator in the very pass that made it ready, before its own callback
// installed anything: the transport keeps it until then
TEST(P2PHost, WhatThePeerSendsFirstWaitsForTheHandler) {
  ASSERT_EQ(Init(), Result::Success);
  p2p::HostConfig config;
  config.bind_address = "127.0.0.1";
  p2p::Host a{config};
  p2p::Host b{config};
  ASSERT_EQ(a.Start(), Result::Success);
  ASSERT_EQ(b.Start(), Result::Success);

  PunchOutcome at_a;
  PunchOutcome at_b;
  a.Punch(Offer({HostAddr(b)}, 9, /*is_initiator=*/true), at_a.Callback());
  b.Punch(Offer({HostAddr(a)}, 9, /*is_initiator=*/false),
          [&](Result result, std::shared_ptr<PeerSession> session) {
            if (result == Result::Success) {
              session->SetCodec(MakeNoteCodec());
              auto note = std::make_shared<NotePacket>();
              note->text = "first";
              session->SendPacket(note);
            }
            at_b.Callback()(result, std::move(session));
          });
  ASSERT_TRUE(WaitUntil([&]() { return at_a.done.load() && at_b.done.load(); },
                        12000));
  ASSERT_EQ(at_a.result, Result::Success);
  ASSERT_EQ(at_b.result, Result::Success);

  // a takes its time; the note must not be lost to that
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  auto collector = std::make_shared<NoteCollector>();
  at_a.Session()->SetCodec(MakeNoteCodec());
  at_a.Session()->SetHandler(collector);
  ASSERT_TRUE(WaitUntil([&]() { return collector->count.load() == 1; }, 5000));
  {
    std::lock_guard<std::mutex> lock(collector->mutex);
    EXPECT_EQ(collector->notes[0], "first");
  }
  a.Stop();
  b.Stop();
}

TEST(P2PHost, ThreePeersShareOneSocketEach) {
  ASSERT_EQ(Init(), Result::Success);
  p2p::HostConfig config;
  config.bind_address = "127.0.0.1";
  p2p::Host a{config};
  p2p::Host b{config};
  p2p::Host c{config};
  ASSERT_EQ(a.Start(), Result::Success);
  ASSERT_EQ(b.Start(), Result::Success);
  ASSERT_EQ(c.Start(), Result::Success);

  // two concurrent punches from a's single socket, the thing a mesh is
  PunchOutcome a_to_b;
  PunchOutcome a_to_c;
  PunchOutcome b_to_a;
  PunchOutcome c_to_a;
  a.Punch(Offer({HostAddr(b)}, 1, true), a_to_b.Callback());
  a.Punch(Offer({HostAddr(c)}, 2, true), a_to_c.Callback());
  b.Punch(Offer({HostAddr(a)}, 1, false), b_to_a.Callback());
  c.Punch(Offer({HostAddr(a)}, 2, false), c_to_a.Callback());

  ASSERT_TRUE(WaitUntil(
      [&]() {
        return a_to_b.done.load() && a_to_c.done.load() &&
               b_to_a.done.load() && c_to_a.done.load();
      },
      12000));
  ASSERT_EQ(a_to_b.result, Result::Success);
  ASSERT_EQ(a_to_c.result, Result::Success);
  ASSERT_EQ(b_to_a.result, Result::Success);
  ASSERT_EQ(c_to_a.result, Result::Success);
  EXPECT_EQ(a.session_count(), 2u) << "both peers live on a's one socket";

  ASSERT_TRUE(WaitUntil([&]() { return BothReady(a_to_b, b_to_a); }, 10000));
  ASSERT_TRUE(WaitUntil([&]() { return BothReady(a_to_c, c_to_a); }, 10000));

  // b and c each send to a; a must keep the two streams apart
  auto collector = std::make_shared<NoteCollector>();
  a_to_b.Session()->SetCodec(MakeNoteCodec());
  a_to_b.Session()->SetHandler(collector);
  a_to_c.Session()->SetCodec(MakeNoteCodec());
  a_to_c.Session()->SetHandler(collector);
  b_to_a.Session()->SetCodec(MakeNoteCodec());
  c_to_a.Session()->SetCodec(MakeNoteCodec());

  auto from_b = std::make_shared<NotePacket>();
  from_b->text = "from b";
  auto from_c = std::make_shared<NotePacket>();
  from_c->text = "from c";
  ASSERT_EQ(b_to_a.Session()->SendPacket(from_b), Result::Success);
  ASSERT_EQ(c_to_a.Session()->SendPacket(from_c), Result::Success);
  ASSERT_TRUE(WaitUntil([&]() { return collector->count.load() == 2; }, 5000));
  {
    std::lock_guard<std::mutex> lock(collector->mutex);
    EXPECT_NE(collector->notes[0], collector->notes[1])
        << "one note from each peer, not a duplicate";
  }
  a.Stop();
  b.Stop();
  c.Stop();
}

// a callback handed to a host that is stopping must still be resolved:
// Stop() drains what was queued and refuses what comes after, with no gap
// in between for a request to fall through
TEST(P2PHost, StopResolvesEveryRequestOrRefusesIt) {
  ASSERT_EQ(Init(), Result::Success);
  p2p::HostConfig config;
  config.bind_address = "127.0.0.1";
  p2p::Host host{config};
  ASSERT_EQ(host.Start(), Result::Success);
  std::atomic<int> resolved{0};
  std::atomic<bool> stop{false};
  std::thread caller([&]() {
    std::shared_ptr<InetAddress> dead = InetAddress::from("203.0.113.1", 9);
    while (!stop.load()) {
      host.Punch(Offer({dead}, 4, true, std::chrono::seconds(30)),
                      [&](Result, std::shared_ptr<PeerSession>) { resolved++; });
      host.Gather({dead}, std::chrono::seconds(30),
                  [&](Result, std::vector<p2p::Candidate>) { resolved++; });
    }
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  host.Stop();
  stop = true;
  caller.join();
  // every request the caller made either resolved before Stop() returned or
  // was refused on the spot afterwards; count them against what was issued
  // by issuing two more, which must be refused synchronously
  const int before = resolved.load();
  host.Punch(Offer({InetAddress::from("203.0.113.1", 9)}, 5, true),
                  [&](Result r, std::shared_ptr<PeerSession>) {
                    EXPECT_EQ(r, Result::AlreadyStopped);
                    resolved++;
                  });
  host.Gather({}, std::chrono::seconds(1),
              [&](Result r, std::vector<p2p::Candidate>) {
                EXPECT_EQ(r, Result::AlreadyStopped);
                resolved++;
              });
  EXPECT_EQ(resolved.load(), before + 2);
}

TEST(P2PHost, PunchTowardNothingTimesOut) {
  ASSERT_EQ(Init(), Result::Success);
  p2p::HostConfig config;
  config.bind_address = "127.0.0.1";
  p2p::Host host{config};
  ASSERT_EQ(host.Start(), Result::Success);

  PunchOutcome outcome;
  std::shared_ptr<InetAddress> dead = InetAddress::from("203.0.113.1", 9);
  host.Punch(Offer({dead}, 3, true, std::chrono::milliseconds(300)),
                  outcome.Callback());
  ASSERT_TRUE(WaitUntil([&]() { return outcome.done.load(); }, 5000));
  EXPECT_EQ(outcome.result, Result::Timeout);
  EXPECT_FALSE(outcome.Session());
  host.Stop();
}

// A multi-homed peer offers every address it has, and most of them are dead
// to anyone not on that network. Every candidate is raced at once, so a punch
// must not get slower the more of them it is given.
TEST(P2PHost, ManyDeadCandidatesDoNotSlowThePunch) {
  ASSERT_EQ(Init(), Result::Success);
  p2p::HostConfig config;
  config.bind_address = "127.0.0.1";
  p2p::Host a{config};
  p2p::Host b{config};
  ASSERT_EQ(a.Start(), Result::Success);
  ASSERT_EQ(b.Start(), Result::Success);

  std::vector<std::shared_ptr<InetAddress>> to_b;
  std::vector<std::shared_ptr<InetAddress>> to_a;
  for (int i = 0; i < 7; i++) {
    // TEST-NET-1 space: routable nowhere, so the candidate just goes dark
    std::shared_ptr<InetAddress> dead =
        InetAddress::from("203.0.113." + std::to_string(1 + i), 9);
    to_b.push_back(dead);
    to_a.push_back(dead);
  }
  to_b.push_back(HostAddr(b));
  to_a.push_back(HostAddr(a));

  PunchOutcome at_a;
  PunchOutcome at_b;
  const auto started = std::chrono::steady_clock::now();
  a.Punch(Offer(to_b, 9, true), at_a.Callback());
  b.Punch(Offer(to_a, 9, false), at_b.Callback());
  ASSERT_TRUE(WaitUntil([&]() { return at_a.done.load() && at_b.done.load(); },
                        12000));
  EXPECT_EQ(at_a.result, Result::Success);
  EXPECT_EQ(at_b.result, Result::Success);
  EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(5))
      << "dead candidates must cost nothing but a datagram each";
  a.Stop();
  b.Stop();
}

// --- The mesh through the rendezvous ------------------------------------------

using znet::test::HostLocatorProbe;

TEST(PeerLocatorEndToEnd, ThreePlayersFormAMesh) {
  ASSERT_EQ(Init(), Result::Success);
  p2p::RendezvousServerConfig config;
  config.bind_address = "127.0.0.1";
  config.bind_port = 0;
  config.punch_connection_type = ConnectionType::ZDT;
  p2p::RendezvousServer relay{config};
  ASSERT_EQ(relay.Start(), Result::Success);
  const PortNumber port = relay.bind_address()->port();

  HostLocatorProbe a{port};
  HostLocatorProbe b{port};
  HostLocatorProbe c{port};
  ASSERT_EQ(a.locator.Connect(), Result::Success);
  ASSERT_EQ(b.locator.Connect(), Result::Success);
  ASSERT_EQ(c.locator.Connect(), Result::Success);
  ASSERT_TRUE(WaitUntil(
      [&]() { return a.ready.load() && b.ready.load() && c.ready.load(); },
      5000));

  // everyone asks everyone; the rendezvous pairs the mutual asks into punches
  ASSERT_EQ(a.locator.AskPeer(b.Name()), Result::Success);
  ASSERT_EQ(a.locator.AskPeer(c.Name()), Result::Success);
  ASSERT_EQ(b.locator.AskPeer(a.Name()), Result::Success);
  ASSERT_EQ(b.locator.AskPeer(c.Name()), Result::Success);
  ASSERT_EQ(c.locator.AskPeer(a.Name()), Result::Success);
  ASSERT_EQ(c.locator.AskPeer(b.Name()), Result::Success);

  ASSERT_TRUE(WaitUntil(
      [&]() {
        return a.connected.load() == 2 && b.connected.load() == 2 &&
               c.connected.load() == 2;
      },
      20000))
      << "every pair must punch: a=" << a.connected.load()
      << " b=" << b.connected.load() << " c=" << c.connected.load();

  EXPECT_TRUE(WaitUntil([&]() { return a.AllSessionsReady(2); }, 10000));
  EXPECT_TRUE(WaitUntil([&]() { return b.AllSessionsReady(2); }, 10000));
  EXPECT_TRUE(WaitUntil([&]() { return c.AllSessionsReady(2); }, 10000));
  EXPECT_EQ(a.locator.host().session_count(), 2u);

  a.locator.Disconnect();
  b.locator.Disconnect();
  c.locator.Disconnect();
  relay.Stop();
}

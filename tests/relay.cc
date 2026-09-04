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
// The relay server on its own, spoken to by raw UDP sockets: binding,
// forwarding by channel, what it refuses, when it frees a pairing, and the
// reflector on the same port.
//

#include "p2p_probes.h"
#include "znet/backends/zdt/zdt_net.h"
#include "znet/backends/zdt/zdt_wire.h"
#include "znet/init.h"
#include "znet/p2p/host.h"
#include "znet/p2p/internal/zdt_punch.h"
#include "znet/p2p/relay_server.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

using namespace znet;
using namespace znet::backends;
using znet::test::MakeNoteCodec;
using znet::test::NoteCollector;
using znet::test::NotePacket;
using znet::test::PunchOutcome;
using znet::test::WaitUntil;

namespace {

// one raw peer of the relay: a loopback UDP socket with a receive timeout
struct RawPeer {
  UDPSocket socket;
  std::shared_ptr<InetAddress> address;
  uint32_t channel = 0;  // learned by Bind; Send wraps in it once set

  RawPeer() {
    socket.Open(InetProtocolVersion::IPv4);
    socket.Bind(*InetAddress::from("127.0.0.1", 0));
    socket.SetReceiveTimeout(std::chrono::milliseconds(300));
    address = socket.local_address();
  }
  ~RawPeer() { socket.Close(); }

  void SendBare(const InetAddress& to, const Buffer& datagram) {
    socket.SendTo(to, datagram.data(), datagram.size());
  }

  // `bytes` as a relayed datagram, on this peer's channel
  void Send(const InetAddress& to, const std::string& bytes) {
    Buffer payload(Endianness::BigEndian);
    payload.Write(bytes.data(), bytes.size());
    p2p::internal::SendDatagram(socket, to, channel, payload);
  }

  // blocks up to the receive timeout; empty on nothing. A relayed datagram
  // comes back without its header
  std::string Receive() {
    uint8_t buffer[2048];
    size_t len = 0;
    std::shared_ptr<InetAddress> from;
    if (socket.RecvFrom(buffer, sizeof(buffer), len, from) !=
        RecvResult::Received) {
      return {};
    }
    uint32_t seen = 0;
    if (ReadRelayHeader(buffer, len, seen)) {
      EXPECT_EQ(seen, channel) << "forwarded on the channel that was bound";
      return std::string(reinterpret_cast<const char*>(buffer) +
                             kZDTRelayHeaderSize,
                         len - kZDTRelayHeaderSize);
    }
    return std::string(reinterpret_cast<const char*>(buffer), len);
  }

  // true when a RelayBound carrying `token` came back, naming the channel
  bool Bind(const InetAddress& relay, uint64_t token) {
    SendBare(relay, p2p::internal::BuildRelayBind(token));
    const std::string reply = Receive();
    if (reply.size() < kZDTOfflineHeaderSize + sizeof(uint64_t) +
                           sizeof(uint32_t)) {
      return false;
    }
    ZDTOfflineMsg id;
    const auto* data = reinterpret_cast<const uint8_t*>(reply.data());
    if (!PeekOfflineHeader(data, reply.size(), id) ||
        id != ZDTOfflineMsg::RelayBound ||
        ReadBigEndian64(data + kZDTOfflineHeaderSize) != token) {
      return false;
    }
    Buffer in(reply.data(), reply.size(), Endianness::BigEndian);
    in.SkipRead(kZDTOfflineHeaderSize + sizeof(uint64_t));
    channel = in.ReadInt<uint32_t>();
    return channel != 0;
  }
};

struct RelayFixture {
  p2p::RelayServerConfig config;
  std::unique_ptr<p2p::RelayServer> relay;

  RelayFixture() {
    config.bind_address = "127.0.0.1";
    config.port = 0;
  }

  void Start() {
    relay.reset(new p2p::RelayServer(config));
    ASSERT_EQ(relay->Start(), Result::Success);
  }

  const InetAddress& At() { return *relay->address(); }
};

}  // namespace

TEST(RelayServer, ForwardsBetweenTwoBoundPeers) {
  ASSERT_EQ(Init(), Result::Success);
  RelayFixture fx;
  fx.Start();
  p2p::RelayServer::Allocation allocation;
  ASSERT_EQ(fx.relay->Allocate(allocation), Result::Success);
  EXPECT_NE(allocation.channel, 0u);
  EXPECT_NE(allocation.token, 0u);

  RawPeer a;
  RawPeer b;
  ASSERT_TRUE(a.Bind(fx.At(), allocation.token));
  ASSERT_TRUE(b.Bind(fx.At(), allocation.token));
  EXPECT_EQ(a.channel, allocation.channel);
  EXPECT_EQ(b.channel, allocation.channel);

  // opaque bytes both ways; the relay must not care what they are
  a.Send(fx.At(), "from a");
  EXPECT_EQ(b.Receive(), "from a");
  b.Send(fx.At(), "from b");
  EXPECT_EQ(a.Receive(), "from b");

  const p2p::RelayMetrics metrics = fx.relay->metrics();
  EXPECT_EQ(metrics.allocations_active, 1u);
  EXPECT_EQ(metrics.binds_accepted, 2u);
  EXPECT_EQ(metrics.datagrams_relayed, 2u);
  EXPECT_EQ(metrics.bytes_relayed, 2 * (kZDTRelayHeaderSize + 6));
  fx.relay->Stop();
}

TEST(RelayServer, ABindIsAnsweredAgainButNeverForwarded) {
  ASSERT_EQ(Init(), Result::Success);
  RelayFixture fx;
  fx.Start();
  p2p::RelayServer::Allocation allocation;
  ASSERT_EQ(fx.relay->Allocate(allocation), Result::Success);

  RawPeer a;
  RawPeer b;
  ASSERT_TRUE(a.Bind(fx.At(), allocation.token));
  ASSERT_TRUE(b.Bind(fx.At(), allocation.token));
  // a lost RelayBound is covered by binding again
  ASSERT_TRUE(a.Bind(fx.At(), allocation.token));
  EXPECT_TRUE(b.Receive().empty()) << "a bind must never reach the peer";
  EXPECT_EQ(fx.relay->metrics().datagrams_relayed, 0u);
  fx.relay->Stop();
}

TEST(RelayServer, DropsWhatAnUnboundSourceSends) {
  ASSERT_EQ(Init(), Result::Success);
  RelayFixture fx;
  fx.Start();
  p2p::RelayServer::Allocation allocation;
  ASSERT_EQ(fx.relay->Allocate(allocation), Result::Success);

  RawPeer a;
  RawPeer b;
  RawPeer stranger;
  ASSERT_TRUE(a.Bind(fx.At(), allocation.token));
  ASSERT_TRUE(b.Bind(fx.At(), allocation.token));
  // knowing the channel is not the same as having bound it
  stranger.channel = allocation.channel;
  stranger.Send(fx.At(), "injected");
  EXPECT_TRUE(a.Receive().empty());
  EXPECT_TRUE(b.Receive().empty());
  EXPECT_TRUE(WaitUntil(
      [&]() { return fx.relay->metrics().datagrams_dropped >= 1; }, 1000));
  fx.relay->Stop();
}

TEST(RelayServer, DropsAnUnknownChannelAndABareDatagram) {
  ASSERT_EQ(Init(), Result::Success);
  RelayFixture fx;
  fx.Start();
  p2p::RelayServer::Allocation allocation;
  ASSERT_EQ(fx.relay->Allocate(allocation), Result::Success);

  RawPeer a;
  RawPeer b;
  ASSERT_TRUE(a.Bind(fx.At(), allocation.token));
  ASSERT_TRUE(b.Bind(fx.At(), allocation.token));
  // a channel nobody was given
  const uint32_t bound = a.channel;
  a.channel = bound + 1;
  a.Send(fx.At(), "nowhere");
  EXPECT_TRUE(b.Receive().empty());
  // and no header at all: not a bind, not a probe, so nothing
  a.channel = 0;
  a.Send(fx.At(), "bare");
  EXPECT_TRUE(b.Receive().empty());
  a.channel = bound;
  a.Send(fx.At(), "wrapped");
  EXPECT_EQ(b.Receive(), "wrapped");
  EXPECT_EQ(fx.relay->metrics().datagrams_dropped, 2u);
  EXPECT_EQ(fx.relay->metrics().datagrams_relayed, 1u);
  fx.relay->Stop();
}

TEST(RelayServer, HoldsTrafficUntilTheOtherSideBinds) {
  ASSERT_EQ(Init(), Result::Success);
  RelayFixture fx;
  fx.Start();
  p2p::RelayServer::Allocation allocation;
  ASSERT_EQ(fx.relay->Allocate(allocation), Result::Success);

  RawPeer a;
  RawPeer b;
  ASSERT_TRUE(a.Bind(fx.At(), allocation.token));
  a.Send(fx.At(), "early");  // nowhere to go yet
  ASSERT_TRUE(b.Bind(fx.At(), allocation.token));
  EXPECT_TRUE(b.Receive().empty()) << "nothing is queued for a late binder";
  a.Send(fx.At(), "late");
  EXPECT_EQ(b.Receive(), "late");
  EXPECT_EQ(fx.relay->metrics().datagrams_dropped, 1u);
  fx.relay->Stop();
}

TEST(RelayServer, RefusesAWrongTokenAndAThirdBinder) {
  ASSERT_EQ(Init(), Result::Success);
  RelayFixture fx;
  fx.Start();
  p2p::RelayServer::Allocation allocation;
  ASSERT_EQ(fx.relay->Allocate(allocation), Result::Success);

  RawPeer a;
  RawPeer b;
  RawPeer c;
  EXPECT_FALSE(c.Bind(fx.At(), allocation.token ^ 1u)) << "wrong token";
  ASSERT_TRUE(a.Bind(fx.At(), allocation.token));
  ASSERT_TRUE(b.Bind(fx.At(), allocation.token));
  EXPECT_FALSE(c.Bind(fx.At(), allocation.token)) << "both slots are taken";
  c.channel = allocation.channel;
  c.Send(fx.At(), "still not a peer");
  EXPECT_TRUE(a.Receive().empty());
  EXPECT_TRUE(b.Receive().empty());
  EXPECT_EQ(fx.relay->metrics().binds_refused, 2u);
  fx.relay->Stop();
}

TEST(RelayServer, KeepsPairingsOnOnePortApart) {
  ASSERT_EQ(Init(), Result::Success);
  RelayFixture fx;
  fx.Start();
  // three pairings, six peers, one port
  p2p::RelayServer::Allocation allocations[3];
  RawPeer a[3];
  RawPeer b[3];
  for (int i = 0; i < 3; i++) {
    ASSERT_EQ(fx.relay->Allocate(allocations[i]), Result::Success);
    ASSERT_TRUE(a[i].Bind(fx.At(), allocations[i].token));
    ASSERT_TRUE(b[i].Bind(fx.At(), allocations[i].token));
  }
  EXPECT_NE(allocations[0].channel, allocations[1].channel);
  EXPECT_NE(allocations[1].channel, allocations[2].channel);
  a[0].Send(fx.At(), "zero");
  a[2].Send(fx.At(), "two");
  EXPECT_EQ(b[0].Receive(), "zero");
  EXPECT_TRUE(b[1].Receive().empty());
  EXPECT_EQ(b[2].Receive(), "two");
  // a peer of one pairing cannot speak on another's channel
  a[0].channel = allocations[1].channel;
  a[0].Send(fx.At(), "crossing");
  EXPECT_TRUE(b[1].Receive().empty());
  EXPECT_EQ(fx.relay->allocation_count(), 3u);
  EXPECT_EQ(fx.relay->metrics().datagrams_dropped, 1u);
  fx.relay->Stop();
}

TEST(RelayServer, FreesAnAllocationNobodyBinds) {
  ASSERT_EQ(Init(), Result::Success);
  RelayFixture fx;
  fx.config.bind_timeout = std::chrono::milliseconds(200);
  fx.Start();
  p2p::RelayServer::Allocation allocation;
  ASSERT_EQ(fx.relay->Allocate(allocation), Result::Success);
  // one side alone does not keep it: the pairing never formed
  RawPeer a;
  ASSERT_TRUE(a.Bind(fx.At(), allocation.token));
  EXPECT_EQ(fx.relay->allocation_count(), 1u);
  EXPECT_TRUE(WaitUntil([&]() { return fx.relay->allocation_count() == 0; },
                        2000));
  EXPECT_EQ(fx.relay->metrics().allocations_expired, 1u);
  EXPECT_FALSE(a.Bind(fx.At(), allocation.token)) << "the token is gone";
  fx.relay->Stop();
}

TEST(RelayServer, FreesAnIdlePairing) {
  ASSERT_EQ(Init(), Result::Success);
  RelayFixture fx;
  fx.config.idle_timeout = std::chrono::milliseconds(300);
  fx.config.bind_timeout = std::chrono::milliseconds(5000);
  fx.Start();
  p2p::RelayServer::Allocation allocation;
  ASSERT_EQ(fx.relay->Allocate(allocation), Result::Success);

  RawPeer a;
  RawPeer b;
  ASSERT_TRUE(a.Bind(fx.At(), allocation.token));
  ASSERT_TRUE(b.Bind(fx.At(), allocation.token));
  // traffic keeps it alive
  for (int i = 0; i < 4; i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    a.Send(fx.At(), "tick");
    EXPECT_EQ(b.Receive(), "tick");
  }
  EXPECT_EQ(fx.relay->allocation_count(), 1u);
  // silence does not
  EXPECT_TRUE(WaitUntil([&]() { return fx.relay->allocation_count() == 0; },
                        2000));
  fx.relay->Stop();
}

TEST(RelayServer, HonorsTheAllocationCap) {
  ASSERT_EQ(Init(), Result::Success);
  RelayFixture fx;
  fx.config.max_allocations = 2;
  fx.Start();
  p2p::RelayServer::Allocation first;
  p2p::RelayServer::Allocation second;
  p2p::RelayServer::Allocation third;
  EXPECT_EQ(fx.relay->Allocate(first), Result::Success);
  EXPECT_EQ(fx.relay->Allocate(second), Result::Success);
  EXPECT_EQ(fx.relay->Allocate(third), Result::ServerFull);
  EXPECT_NE(first.channel, second.channel);
  EXPECT_NE(first.token, second.token);
  fx.relay->Stop();
}

TEST(RelayServer, FreeReleasesAnAllocationAtOnce) {
  ASSERT_EQ(Init(), Result::Success);
  RelayFixture fx;
  fx.Start();
  p2p::RelayServer::Allocation allocation;
  ASSERT_EQ(fx.relay->Allocate(allocation), Result::Success);
  RawPeer a;
  ASSERT_TRUE(a.Bind(fx.At(), allocation.token));
  // a broker that knows the pairing is over need not wait for the timers
  EXPECT_EQ(fx.relay->Free(allocation.channel), Result::Success);
  EXPECT_EQ(fx.relay->allocation_count(), 0u);
  EXPECT_FALSE(a.Bind(fx.At(), allocation.token)) << "the token is gone";
  EXPECT_EQ(fx.relay->Free(allocation.channel), Result::PeerNotFound);
  EXPECT_EQ(fx.relay->metrics().allocations_expired, 1u);
  fx.relay->Stop();
}

TEST(RelayServer, RefusesToAllocateBeforeStart) {
  ASSERT_EQ(Init(), Result::Success);
  p2p::RelayServerConfig config;
  p2p::RelayServer relay{config};
  p2p::RelayServer::Allocation allocation;
  EXPECT_EQ(relay.Allocate(allocation), Result::AlreadyStopped);
}

// --- The reflector ------------------------------------------------------------

namespace {

// the address a Reflected names, or null when `reply` is not one for `nonce`
std::shared_ptr<InetAddress> Reflected(const std::string& reply,
                                       uint64_t nonce) {
  Buffer in(reply.data(), reply.size(), Endianness::BigEndian);
  ZDTOfflineMsg id;
  if (!ReadOfflineHeader(in, id) || id != ZDTOfflineMsg::Reflected) {
    return nullptr;
  }
  if (in.ReadInt<uint64_t>() != nonce) {
    return nullptr;
  }
  return in.ReadInetAddress();
}

}  // namespace

TEST(RelayReflector, AnswersWithTheObservedAddress) {
  ASSERT_EQ(Init(), Result::Success);
  RelayFixture fx;
  fx.Start();
  ASSERT_TRUE(fx.relay->address());
  ASSERT_NE(fx.At().port(), 0);

  RawPeer a;
  const uint64_t nonce = 0x0123456789abcdefULL;
  a.SendBare(fx.At(), p2p::internal::BuildReflect(nonce));
  auto observed = Reflected(a.Receive(), nonce);
  ASSERT_TRUE(observed);
  EXPECT_EQ(observed->readable(), a.address->readable());
  EXPECT_EQ(fx.relay->metrics().probes_answered, 1u);
  fx.relay->Stop();
}

TEST(RelayReflector, IgnoresAShortProbe) {
  ASSERT_EQ(Init(), Result::Success);
  RelayFixture fx;
  fx.Start();

  // the header and nonce without the padding: an answer would be larger
  // than the question, which is the amplification the padding rules out
  RawPeer a;
  Buffer probe = p2p::internal::BuildOffline(ZDTOfflineMsg::Reflect);
  probe.WriteInt<uint64_t>(7);
  ASSERT_LT(probe.size(), kZDTReflectSize);
  a.SendBare(fx.At(), probe);
  EXPECT_TRUE(a.Receive().empty());
  EXPECT_EQ(fx.relay->metrics().probes_refused, 1u);
  EXPECT_EQ(fx.relay->metrics().probes_answered, 0u);
  fx.relay->Stop();
}

TEST(RelayReflector, ThrottlesOneSource) {
  ASSERT_EQ(Init(), Result::Success);
  RelayFixture fx;
  fx.config.max_probes_per_source = 3;
  fx.Start();

  RawPeer a;
  int answered = 0;
  for (uint64_t nonce = 1; nonce <= 6; nonce++) {
    a.SendBare(fx.At(), p2p::internal::BuildReflect(nonce));
    if (Reflected(a.Receive(), nonce)) {
      answered++;
    }
  }
  EXPECT_EQ(answered, 3);
  EXPECT_EQ(fx.relay->metrics().probes_refused, 3u);
  fx.relay->Stop();
}

TEST(RelayReflector, ProbesCannotAmplify) {
  // the padded request is at least as large as any answer it can draw, IPv6
  // included: header, nonce, and a 19-byte address
  Buffer probe = p2p::internal::BuildReflect(1);
  Buffer answer = p2p::internal::BuildReflected(
      1, *InetAddress::from("2001:db8::1", 65535));
  EXPECT_GE(probe.size(), answer.size());
  EXPECT_EQ(probe.size(), kZDTReflectSize);
}

// --- Punching through the relay -----------------------------------------------

namespace {

p2p::Candidate Direct(std::shared_ptr<InetAddress> address) {
  p2p::Candidate candidate;
  candidate.type = p2p::CandidateType::Reflexive;
  candidate.address = std::move(address);
  return candidate;
}

p2p::Candidate Relayed(std::shared_ptr<InetAddress> relay, uint64_t token) {
  p2p::Candidate candidate;
  candidate.type = p2p::CandidateType::Relayed;
  candidate.address = std::move(relay);
  candidate.relay_token = token;
  return candidate;
}

p2p::PunchOffer OfferOf(std::vector<p2p::Candidate> candidates,
                        uint64_t punch_id, bool is_initiator,
                        std::chrono::milliseconds timeout,
                        std::chrono::milliseconds relay_delay) {
  p2p::PunchOffer offer;
  offer.candidates = std::move(candidates);
  offer.punch_id = punch_id;
  offer.is_initiator = is_initiator;
  offer.timeout = timeout;
  offer.relay_delay = relay_delay;
  return offer;
}

std::shared_ptr<InetAddress> Dead() {
  // TEST-NET-1 space: routable nowhere, so the candidate just goes dark
  return InetAddress::from("203.0.113.1", 9);
}

std::shared_ptr<InetAddress> HostAddr(const p2p::Host& host) {
  return InetAddress::from("127.0.0.1", host.punch_port());
}

p2p::HostConfig LoopbackHost() {
  p2p::HostConfig config;
  config.bind_address = "127.0.0.1";
  return config;
}

// punches both hosts on their offers and waits for both to resolve
void PunchBoth(p2p::Host& a, p2p::PunchOffer to_b, PunchOutcome& at_a,
               p2p::Host& b, p2p::PunchOffer to_a, PunchOutcome& at_b) {
  a.Punch(std::move(to_b), at_a.Callback());
  b.Punch(std::move(to_a), at_b.Callback());
  ASSERT_TRUE(WaitUntil([&]() { return at_a.done.load() && at_b.done.load(); },
                        15000));
}

// sends one note over `from` and waits for it at `to`
void ExchangeNote(const std::shared_ptr<PeerSession>& from,
                  const std::shared_ptr<NoteCollector>& to,
                  const std::string& text) {
  auto note = std::make_shared<NotePacket>();
  note->text = text;
  ASSERT_EQ(from->SendPacket(note), Result::Success);
  ASSERT_TRUE(WaitUntil([&]() { return to->count.load() >= 1; }, 5000));
  std::lock_guard<std::mutex> lock(to->mutex);
  EXPECT_EQ(to->notes.back(), text);
}

}  // namespace

TEST(RelayedPunch, ConnectsThroughTheRelayWhenDirectFails) {
  ASSERT_EQ(Init(), Result::Success);
  RelayFixture fx;
  fx.Start();
  p2p::RelayServer::Allocation allocation;
  ASSERT_EQ(fx.relay->Allocate(allocation), Result::Success);
  p2p::Host a{LoopbackHost()};
  p2p::Host b{LoopbackHost()};
  ASSERT_EQ(a.Start(), Result::Success);
  ASSERT_EQ(b.Start(), Result::Success);

  // the direct candidates go nowhere; only the relay can carry this one
  const std::chrono::milliseconds budget(10000);
  const std::chrono::milliseconds relay_delay(200);
  PunchOutcome at_a;
  PunchOutcome at_b;
  const auto started = std::chrono::steady_clock::now();
  const auto relay = fx.relay->address();
  PunchBoth(a,
            OfferOf({Direct(Dead()), Relayed(relay, allocation.token)}, 21,
                    true, budget, relay_delay),
            at_a, b,
            OfferOf({Direct(Dead()), Relayed(relay, allocation.token)}, 21,
                    false, budget, relay_delay),
            at_b);
  ASSERT_EQ(at_a.result, Result::Success);
  ASSERT_EQ(at_b.result, Result::Success);
  EXPECT_LT(std::chrono::steady_clock::now() - started,
            std::chrono::seconds(5));
  EXPECT_EQ(at_a.Session()->remote_address()->readable(), relay->readable())
      << "a relayed session's peer address is the relay";
  EXPECT_EQ(at_b.Session()->remote_address()->readable(), relay->readable());

  // real traffic both ways, encrypted end to end through a relay that only
  // ever saw ciphertext
  auto to_a = std::make_shared<NoteCollector>();
  auto to_b = std::make_shared<NoteCollector>();
  at_a.Session()->SetCodec(MakeNoteCodec());
  at_a.Session()->SetHandler(to_a);
  at_b.Session()->SetCodec(MakeNoteCodec());
  at_b.Session()->SetHandler(to_b);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  ExchangeNote(at_a.Session(), to_b, "through the relay");
  ExchangeNote(at_b.Session(), to_a, "and back");
  const p2p::RelayMetrics metrics = fx.relay->metrics();
  EXPECT_EQ(metrics.binds_accepted, 2u);
  EXPECT_GT(metrics.datagrams_relayed, 4u);
  a.Stop();
  b.Stop();
  fx.relay->Stop();
}

// two relayed sessions on one host share the relay's address and are told
// apart by channel alone
TEST(RelayedPunch, OneHostCarriesTwoRelayedPeers) {
  ASSERT_EQ(Init(), Result::Success);
  RelayFixture fx;
  fx.Start();
  p2p::RelayServer::Allocation to_b;
  p2p::RelayServer::Allocation to_c;
  ASSERT_EQ(fx.relay->Allocate(to_b), Result::Success);
  ASSERT_EQ(fx.relay->Allocate(to_c), Result::Success);
  p2p::Host a{LoopbackHost()};
  p2p::Host b{LoopbackHost()};
  p2p::Host c{LoopbackHost()};
  ASSERT_EQ(a.Start(), Result::Success);
  ASSERT_EQ(b.Start(), Result::Success);
  ASSERT_EQ(c.Start(), Result::Success);

  const std::chrono::milliseconds budget(10000);
  const std::chrono::milliseconds relay_delay(0);
  const auto relay = fx.relay->address();
  PunchOutcome a_to_b;
  PunchOutcome b_to_a;
  PunchOutcome a_to_c;
  PunchOutcome c_to_a;
  a.Punch(OfferOf({Relayed(relay, to_b.token)}, 31, true, budget, relay_delay),
          a_to_b.Callback());
  b.Punch(OfferOf({Relayed(relay, to_b.token)}, 31, false, budget, relay_delay),
          b_to_a.Callback());
  a.Punch(OfferOf({Relayed(relay, to_c.token)}, 32, true, budget, relay_delay),
          a_to_c.Callback());
  c.Punch(OfferOf({Relayed(relay, to_c.token)}, 32, false, budget, relay_delay),
          c_to_a.Callback());
  ASSERT_TRUE(WaitUntil(
      [&]() {
        return a_to_b.done.load() && b_to_a.done.load() &&
               a_to_c.done.load() && c_to_a.done.load();
      },
      15000));
  ASSERT_EQ(a_to_b.result, Result::Success);
  ASSERT_EQ(b_to_a.result, Result::Success);
  ASSERT_EQ(a_to_c.result, Result::Success);
  ASSERT_EQ(c_to_a.result, Result::Success);
  EXPECT_NE(a_to_b.Session(), a_to_c.Session());
  EXPECT_EQ(a.session_count(), 2u);

  auto at_b = std::make_shared<NoteCollector>();
  auto at_c = std::make_shared<NoteCollector>();
  a_to_b.Session()->SetCodec(MakeNoteCodec());
  a_to_c.Session()->SetCodec(MakeNoteCodec());
  b_to_a.Session()->SetCodec(MakeNoteCodec());
  b_to_a.Session()->SetHandler(at_b);
  c_to_a.Session()->SetCodec(MakeNoteCodec());
  c_to_a.Session()->SetHandler(at_c);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  ExchangeNote(a_to_b.Session(), at_b, "for b");
  ExchangeNote(a_to_c.Session(), at_c, "for c");
  EXPECT_EQ(at_b->count.load(), 1) << "nothing meant for c reached b";
  EXPECT_EQ(at_c->count.load(), 1);
  a.Stop();
  b.Stop();
  c.Stop();
  fx.relay->Stop();
}

TEST(RelayedPunch, DirectWinsWhenItWorks) {
  ASSERT_EQ(Init(), Result::Success);
  RelayFixture fx;
  fx.Start();
  p2p::RelayServer::Allocation allocation;
  ASSERT_EQ(fx.relay->Allocate(allocation), Result::Success);
  p2p::Host a{LoopbackHost()};
  p2p::Host b{LoopbackHost()};
  ASSERT_EQ(a.Start(), Result::Success);
  ASSERT_EQ(b.Start(), Result::Success);

  // a relay that could carry it, but a direct path that answers first
  const std::chrono::milliseconds budget(10000);
  const std::chrono::milliseconds relay_delay(0);
  const auto relay = fx.relay->address();
  PunchOutcome at_a;
  PunchOutcome at_b;
  PunchBoth(a,
            OfferOf({Direct(HostAddr(b)), Relayed(relay, allocation.token)},
                    22, true, budget, relay_delay),
            at_a, b,
            OfferOf({Direct(HostAddr(a)), Relayed(relay, allocation.token)},
                    22, false, budget, relay_delay),
            at_b);
  ASSERT_EQ(at_a.result, Result::Success);
  ASSERT_EQ(at_b.result, Result::Success);
  EXPECT_EQ(at_a.Session()->remote_address()->readable(),
            HostAddr(b)->readable());
  EXPECT_EQ(at_b.Session()->remote_address()->readable(),
            HostAddr(a)->readable());
  // both bound the relay all the same, and nothing else went through it
  EXPECT_TRUE(WaitUntil([&]() { return fx.relay->metrics().binds_accepted == 2u; }, 2000));
  a.Stop();
  b.Stop();
  fx.relay->Stop();
}

TEST(RelayedPunch, ARelayAloneIsEnough) {
  ASSERT_EQ(Init(), Result::Success);
  RelayFixture fx;
  fx.Start();
  p2p::RelayServer::Allocation allocation;
  ASSERT_EQ(fx.relay->Allocate(allocation), Result::Success);
  p2p::Host a{LoopbackHost()};
  p2p::Host b{LoopbackHost()};
  ASSERT_EQ(a.Start(), Result::Success);
  ASSERT_EQ(b.Start(), Result::Success);

  const std::chrono::milliseconds budget(10000);
  const std::chrono::milliseconds relay_delay(100);
  const auto relay = fx.relay->address();
  PunchOutcome at_a;
  PunchOutcome at_b;
  PunchBoth(a, OfferOf({Relayed(relay, allocation.token)}, 23, true, budget, relay_delay), at_a,
            b, OfferOf({Relayed(relay, allocation.token)}, 23, false, budget, relay_delay), at_b);
  EXPECT_EQ(at_a.result, Result::Success);
  EXPECT_EQ(at_b.result, Result::Success);
  a.Stop();
  b.Stop();
  fx.relay->Stop();
}

TEST(RelayedPunch, AMissingTokenIsRefusedUpFront) {
  ASSERT_EQ(Init(), Result::Success);
  p2p::Host host{LoopbackHost()};
  ASSERT_EQ(host.Start(), Result::Success);
  PunchOutcome outcome;
  // a broker that forgot the token: the relay would never bind it, so the
  // punch says so now instead of timing out later
  host.Punch(OfferOf({Relayed(InetAddress::from("127.0.0.1", 40000), 0)}, 25,
                     true, std::chrono::seconds(5),
                     std::chrono::milliseconds(0)),
             outcome.Callback());
  ASSERT_TRUE(WaitUntil([&]() { return outcome.done.load(); }, 1000));
  EXPECT_EQ(outcome.result, Result::InvalidArgument);
  host.Stop();
}

TEST(RelayedPunch, AWrongTokenLeavesOnlyTheDirectPath) {
  ASSERT_EQ(Init(), Result::Success);
  RelayFixture fx;
  fx.Start();
  p2p::RelayServer::Allocation allocation;
  ASSERT_EQ(fx.relay->Allocate(allocation), Result::Success);
  p2p::Host a{LoopbackHost()};
  p2p::Host b{LoopbackHost()};
  ASSERT_EQ(a.Start(), Result::Success);
  ASSERT_EQ(b.Start(), Result::Success);

  const std::chrono::milliseconds budget(1500);
  const std::chrono::milliseconds relay_delay(100);
  const auto relay = fx.relay->address();
  PunchOutcome at_a;
  PunchOutcome at_b;
  PunchBoth(a, OfferOf({Direct(Dead()), Relayed(relay, allocation.token ^ 1u)}, 24, true, budget, relay_delay), at_a,
            b, OfferOf({Direct(Dead()), Relayed(relay, allocation.token ^ 1u)}, 24, false, budget, relay_delay), at_b);
  EXPECT_EQ(at_a.result, Result::Timeout);
  EXPECT_EQ(at_b.result, Result::Timeout);
  EXPECT_EQ(fx.relay->metrics().binds_accepted, 0u);
  EXPECT_GE(fx.relay->metrics().binds_refused, 2u);
  a.Stop();
  b.Stop();
  fx.relay->Stop();
}

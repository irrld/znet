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
// The gather step: host candidates off the interfaces, the reflexive one off
// a reflector's port, and a punch that runs on what was gathered.
//

#include "p2p_probes.h"
#include "znet/init.h"
#include "znet/p2p/agent.h"
#include "znet/p2p/internal/gather.h"
#include "znet/p2p/traversal_server.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using namespace znet;
using znet::test::GatherOutcome;
using znet::test::PunchOutcome;
using znet::test::WaitUntil;

namespace {

std::unique_ptr<p2p::TraversalServer> StartTraversal() {
  p2p::TraversalServerConfig config;
  config.bind_address = "127.0.0.1";
  config.port = 0;
  std::unique_ptr<p2p::TraversalServer> traversal(new p2p::TraversalServer(config));
  EXPECT_EQ(traversal->Start(), Result::Success);
  return traversal;
}

p2p::AgentConfig LoopbackAgent() {
  p2p::AgentConfig config;
  config.bind_address = "127.0.0.1";
  return config;
}

p2p::Candidate Reflexive(const char* host, PortNumber port) {
  p2p::Candidate candidate;
  candidate.type = p2p::CandidateType::Reflexive;
  candidate.address = InetAddress::from(host, port);
  return candidate;
}

}  // namespace

TEST(LocalCandidates, EveryAddressCarriesThePort) {
  ASSERT_EQ(Init(), Result::Success);
  const auto candidates = p2p::internal::LocalCandidates(4242);
  ASSERT_FALSE(candidates.empty());
  for (const auto& candidate : candidates) {
    EXPECT_EQ(candidate.type, p2p::CandidateType::Host);
    EXPECT_EQ(candidate.address->port(), 4242);
    EXPECT_EQ(candidate.relay_token, 0u);
  }
}

TEST(HostGather, LearnsThePublicMappingFromAReflector) {
  ASSERT_EQ(Init(), Result::Success);
  auto traversal = StartTraversal();
  p2p::Agent agent{LoopbackAgent()};
  ASSERT_EQ(agent.Start(), Result::Success);

  GatherOutcome outcome;
  agent.Gather({traversal->address()}, std::chrono::seconds(2),
              outcome.Callback());
  ASSERT_TRUE(WaitUntil([&]() { return outcome.done.load(); }, 5000));
  EXPECT_EQ(outcome.result, Result::Success);
  ASSERT_EQ(outcome.CountOf(p2p::CandidateType::Reflexive), 1);
  // on loopback the mapping is the socket itself, and it comes first
  const auto candidates = outcome.Candidates();
  EXPECT_EQ(candidates.front().type, p2p::CandidateType::Reflexive);
  EXPECT_EQ(candidates.front().address->readable(),
            "127.0.0.1:" + std::to_string(agent.punch_port()));
  agent.Stop();
  traversal->Stop();
}

TEST(HostGather, WithoutReflectorsReportsTheHostCandidates) {
  ASSERT_EQ(Init(), Result::Success);
  p2p::Agent agent{LoopbackAgent()};
  ASSERT_EQ(agent.Start(), Result::Success);

  GatherOutcome outcome;
  agent.Gather({}, std::chrono::seconds(2), outcome.Callback());
  ASSERT_TRUE(WaitUntil([&]() { return outcome.done.load(); }, 5000));
  EXPECT_EQ(outcome.result, Result::Success);
  EXPECT_EQ(outcome.CountOf(p2p::CandidateType::Reflexive), 0);
  EXPECT_GE(outcome.CountOf(p2p::CandidateType::Host), 1);
  for (const auto& candidate : outcome.Candidates()) {
    EXPECT_EQ(candidate.address->port(), agent.punch_port());
  }
  agent.Stop();
}

TEST(HostGather, ADeadReflectorTimesOutWithTheHostCandidates) {
  ASSERT_EQ(Init(), Result::Success);
  p2p::Agent agent{LoopbackAgent()};
  ASSERT_EQ(agent.Start(), Result::Success);

  GatherOutcome outcome;
  const auto started = std::chrono::steady_clock::now();
  agent.Gather({InetAddress::from("203.0.113.1", 9)},
              std::chrono::milliseconds(300), outcome.Callback());
  ASSERT_TRUE(WaitUntil([&]() { return outcome.done.load(); }, 5000));
  EXPECT_LT(std::chrono::steady_clock::now() - started,
            std::chrono::seconds(2));
  EXPECT_EQ(outcome.result, Result::Timeout);
  EXPECT_EQ(outcome.CountOf(p2p::CandidateType::Reflexive), 0);
  EXPECT_GE(outcome.CountOf(p2p::CandidateType::Host), 1)
      << "what was learned locally is still worth handing over";
  agent.Stop();
}

TEST(HostGather, StoppingTheHostFailsAPendingGather) {
  ASSERT_EQ(Init(), Result::Success);
  p2p::Agent agent{LoopbackAgent()};
  ASSERT_EQ(agent.Start(), Result::Success);
  GatherOutcome outcome;
  agent.Gather({InetAddress::from("203.0.113.1", 9)}, std::chrono::seconds(30),
              outcome.Callback());
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  agent.Stop();
  ASSERT_TRUE(outcome.done.load());
  EXPECT_EQ(outcome.result, Result::AlreadyStopped);
}

// the whole client-side flow without a broker: both sides gather off the
// same reflector, swap what they learned, and punch on it
TEST(HostGather, GatheredCandidatesCarryAPunch) {
  ASSERT_EQ(Init(), Result::Success);
  auto traversal = StartTraversal();
  p2p::Agent a{LoopbackAgent()};
  p2p::Agent b{LoopbackAgent()};
  ASSERT_EQ(a.Start(), Result::Success);
  ASSERT_EQ(b.Start(), Result::Success);

  GatherOutcome gathered_a;
  GatherOutcome gathered_b;
  a.Gather({traversal->address()}, std::chrono::seconds(2),
           gathered_a.Callback());
  b.Gather({traversal->address()}, std::chrono::seconds(2),
           gathered_b.Callback());
  ASSERT_TRUE(WaitUntil(
      [&]() { return gathered_a.done.load() && gathered_b.done.load(); },
      5000));
  ASSERT_EQ(gathered_a.result, Result::Success);
  ASSERT_EQ(gathered_b.result, Result::Success);

  // the exchange, by hand
  p2p::PunchOffer to_b;
  to_b.candidates = gathered_b.Candidates();
  to_b.punch_id = 11;
  to_b.is_initiator = true;
  p2p::PunchOffer to_a;
  to_a.candidates = gathered_a.Candidates();
  to_a.punch_id = 11;
  to_a.is_initiator = false;

  PunchOutcome at_a;
  PunchOutcome at_b;
  a.Punch(to_b, at_a.Callback());
  b.Punch(to_a, at_b.Callback());
  ASSERT_TRUE(WaitUntil([&]() { return at_a.done.load() && at_b.done.load(); },
                        12000));
  EXPECT_EQ(at_a.result, Result::Success);
  EXPECT_EQ(at_b.result, Result::Success);
  a.Stop();
  b.Stop();
  traversal->Stop();
}

TEST(HostGather, OneReflectorLeavesTheNatTypeUnknown) {
  ASSERT_EQ(Init(), Result::Success);
  auto traversal = StartTraversal();
  p2p::Agent agent{LoopbackAgent()};
  ASSERT_EQ(agent.Start(), Result::Success);

  GatherOutcome outcome;
  agent.Gather({traversal->address()}, std::chrono::seconds(2), outcome.Callback());
  ASSERT_TRUE(WaitUntil([&]() { return outcome.done.load(); }, 5000));
  EXPECT_EQ(outcome.result, Result::Success);
  // one reflector cannot classify the mapping
  EXPECT_EQ(outcome.nat_type, p2p::NatType::Unknown);
  agent.Stop();
  traversal->Stop();
}

// ClassifyNat is pure, so the verdicts a real gather cannot reach on loopback
// (no NAT sits between, and a second loopback host is not bindable everywhere)
// are covered here with fabricated reflections.
namespace {
p2p::internal::Reflection Report(const char* reflector, const char* mapping,
                                 PortNumber mapped_port) {
  p2p::internal::Reflection reflection;
  reflection.reflector = InetAddress::from(reflector, 3478);
  reflection.mapping = InetAddress::from(mapping, mapped_port);
  return reflection;
}
}  // namespace

TEST(ClassifyNat, DistinctReflectorsAgreeingIsEndpointIndependent) {
  // two reflectors on distinct hosts see the same public mapping
  EXPECT_EQ(p2p::internal::ClassifyNat(
                {Report("198.51.100.1", "203.0.113.7", 55000),
                 Report("198.51.100.2", "203.0.113.7", 55000)}),
            p2p::NatType::EndpointIndependent);
}

TEST(ClassifyNat, DistinctReflectorsDifferingIsAddressDependent) {
  // a fresh port per destination is a symmetric NAT
  EXPECT_EQ(p2p::internal::ClassifyNat(
                {Report("198.51.100.1", "203.0.113.7", 55000),
                 Report("198.51.100.2", "203.0.113.7", 55001)}),
            p2p::NatType::AddressDependent);
}

TEST(ClassifyNat, InsufficientReflectorsAreUnknown) {
  // one reflector cannot classify
  EXPECT_EQ(p2p::internal::ClassifyNat({Report("198.51.100.1", "203.0.113.7",
                                               55000)}),
            p2p::NatType::Unknown);
  // two on the same host cannot tell endpoint-independent from address-dependent
  EXPECT_EQ(p2p::internal::ClassifyNat(
                {Report("198.51.100.1", "203.0.113.7", 55000),
                 Report("198.51.100.1", "203.0.113.7", 55001)}),
            p2p::NatType::Unknown);
}

// PredictedPorts is pure, so the symmetric case that needs real hardware to
// gather can still be covered here.
TEST(PredictedPorts, ExtendsASequentialMapping) {
  // two mappings on one host, stride 3
  const auto out = p2p::internal::PredictedPorts(
      {Reflexive("203.0.113.7", 4000), Reflexive("203.0.113.7", 4003)}, 4);
  ASSERT_EQ(out.size(), 4u);
  EXPECT_EQ(out[0].address->port(), 4006);
  EXPECT_EQ(out[3].address->port(), 4015);
  for (const auto& c : out) {
    EXPECT_EQ(c.type, p2p::CandidateType::Reflexive);
    EXPECT_EQ(c.address->readable().substr(0, 9), "203.0.113");
  }
}

TEST(PredictedPorts, RefusesWhatItCannotExtend) {
  // a single mapping: nothing to measure a stride from
  EXPECT_TRUE(p2p::internal::PredictedPorts({Reflexive("203.0.113.7", 4000)}, 4)
                  .empty());
  // two different hosts: not one NAT's sequence
  EXPECT_TRUE(p2p::internal::PredictedPorts(
                  {Reflexive("203.0.113.7", 4000),
                   Reflexive("203.0.113.8", 4001)},
                  4)
                  .empty());
  // same port twice: stride 0
  EXPECT_TRUE(p2p::internal::PredictedPorts(
                  {Reflexive("203.0.113.7", 4000),
                   Reflexive("203.0.113.7", 4000)},
                  4)
                  .empty());
  // a gap too wide to be a sequential allocation
  EXPECT_TRUE(p2p::internal::PredictedPorts(
                  {Reflexive("203.0.113.7", 4000),
                   Reflexive("203.0.113.7", 40000)},
                  4)
                  .empty());
}

TEST(PredictedPorts, ClampsAtThePortCeiling) {
  // stride 5 from 65525: 65530 and 65535 fit, 65540 does not
  const auto out = p2p::internal::PredictedPorts(
      {Reflexive("203.0.113.7", 65520), Reflexive("203.0.113.7", 65525)}, 8);
  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(out[0].address->port(), 65530);
  EXPECT_EQ(out[1].address->port(), 65535);
}

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
// Fan-out: one application thread broadcasting to many sessions, the shape a
// game server has. Rewards the opposite arrangement from znet_bench's
// one-session pipeline.
//

#include "common/fanout.h"
#include "common/harness.h"
#include "common/znet_tuning.h"

#include "znet/client.h"
#include "znet/client_events.h"
#include "znet/codec.h"
#include "znet/init.h"
#include "znet/packet.h"
#include "znet/packet_handler.h"
#include "znet/server.h"
#include "znet/server_events.h"
#include "znet/version.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace znet;

namespace {

enum FanoutPacketType : PacketId { kPacketFanout = 1 };

class FanoutPacket : public Packet {
 public:
  FanoutPacket() : Packet(kPacketFanout) {}
  std::string payload;
  uint32_t seq = 0;
};

class FanoutSerializer : public PacketSerializer<FanoutPacket> {
 public:
  std::shared_ptr<Buffer> SerializeTyped(std::shared_ptr<FanoutPacket> packet,
                                         std::shared_ptr<Buffer> buffer) override {
    buffer->WriteInt<uint32_t>(packet->seq);
    buffer->WriteString(packet->payload);
    return buffer;
  }
  std::shared_ptr<FanoutPacket> DeserializeTyped(
      std::shared_ptr<Buffer> buffer) override {
    auto packet = std::make_shared<FanoutPacket>();
    packet->seq = buffer->ReadInt<uint32_t>();
    packet->payload = buffer->ReadString();
    return packet;
  }
};

std::shared_ptr<Codec> MakeCodec() {
  auto codec = std::make_shared<Codec>();
  codec->Add(kPacketFanout, std::make_unique<FanoutSerializer>());
  return codec;
}

// Every client funnels into one counter.
class CountingHandler : public PacketHandler<CountingHandler, FanoutPacket> {
 public:
  explicit CountingHandler(std::atomic_uint32_t* received) : received_(received) {}
  void OnPacket(std::shared_ptr<FanoutPacket>) {
    received_->fetch_add(1, std::memory_order_relaxed);
  }

 private:
  std::atomic_uint32_t* received_;
};

bench::FanoutResult RunFanout(const char* profile, ConnectionType type,
                              uint32_t client_count, uint32_t per_client,
                              size_t payload_bytes, bool secure) {
  const std::string payload = bench::MakePayload(payload_bytes);
  const char* transport = type == ConnectionType::TCP ? "TCP" : "ZDT";
  std::atomic_uint32_t received{0};
  std::atomic_uint32_t clients_ready{0};

  std::mutex sessions_mutex;
  std::vector<std::shared_ptr<PeerSession>> sessions;

  PortNumber port = bench::FreePort();
  ServerConfig server_config{"127.0.0.1", port, std::chrono::seconds(10), type};
  server_config.child_options.common.encryption = secure;
  server_config.child_options.common.compression =
      secure ? CompressionType::Default : CompressionType::None;
  bench::ApplyBenchQueueBounds(server_config.child_options);

  Server server{server_config};
  server.SetEventCallback([&](Event& event) {
    EventDispatcher dispatcher{event};
    dispatcher.Dispatch<IncomingClientConnectedEvent>(
        [&](IncomingClientConnectedEvent& ev) {
          ev.session()->SetCodec(MakeCodec());
          std::lock_guard<std::mutex> lock(sessions_mutex);
          sessions.push_back(ev.session());
          return false;
        });
  });
  if (server.Bind() != Result::Success ||
      server.Listen() != Result::Success) {
    std::printf("%-10s %-6s fanout     %ux%u  FAILED to bind/listen\n", profile,
                transport, client_count, per_client);
    return {};
  }

  std::vector<std::unique_ptr<Client>> clients;
  clients.reserve(client_count);
  for (uint32_t i = 0; i < client_count; i++) {
    ClientConfig client_config{"127.0.0.1", port, std::chrono::seconds(10), type};
    bench::ApplyBenchQueueBounds(client_config.options);
    auto client = std::unique_ptr<Client>(new Client{client_config});
    client->SetEventCallback([&](Event& event) {
      EventDispatcher dispatcher{event};
      dispatcher.Dispatch<ClientConnectedToServerEvent>(
          [&](ClientConnectedToServerEvent& ev) {
            ev.session()->SetCodec(MakeCodec());
            ev.session()->SetHandler(std::make_shared<CountingHandler>(&received));
            clients_ready.fetch_add(1);
            return false;
          });
    });
    client->Bind();
    client->Connect();
    clients.push_back(std::move(client));
  }

  auto teardown = [&]() {
    for (auto& client : clients) {
      client->Disconnect();
    }
    server.Stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  };

  auto connect_deadline = bench::Clock::now() + std::chrono::seconds(30);
  while (bench::Clock::now() < connect_deadline) {
    size_t have = 0;
    {
      std::lock_guard<std::mutex> lock(sessions_mutex);
      have = sessions.size();
    }
    if (have >= client_count && clients_ready.load() >= client_count) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  std::vector<std::shared_ptr<PeerSession>> targets;
  {
    std::lock_guard<std::mutex> lock(sessions_mutex);
    targets = sessions;
  }
  if (targets.size() < client_count) {
    std::printf("%-10s %-6s fanout     %ux%u  only %zu/%u sessions connected\n",
                profile, transport, client_count, per_client, targets.size(),
                client_count);
    teardown();
    return {};
  }

  // the counter is written by the worker threads; the driver reads its delta
  uint32_t last = 0;
  bench::FanoutResult out = bench::RunFanoutMeasured(
      client_count, per_client,
      [&](uint32_t round) {
        for (std::shared_ptr<PeerSession>& session : targets) {
          auto packet = std::make_shared<FanoutPacket>();
          packet->seq = round;
          packet->payload = payload;
          while (session->SendPacket(packet) != Result::Success) {
            if (!session->IsAlive()) {
              return false;
            }
            std::this_thread::yield();
          }
        }
        return true;
      },
      [&]() {
        const uint32_t now = received.load(std::memory_order_relaxed);
        const uint32_t delta = now - last;
        last = now;
        return delta;
      });
  teardown();
  return out;
}

void RunCase(const char* profile, ConnectionType type, uint32_t clients,
             uint32_t per_client, size_t payload, bool secure) {
  const char* transport = type == ConnectionType::TCP ? "TCP" : "ZDT";
  std::vector<bench::FanoutResult> reps;
  for (int rep = 0; rep < bench::Reps(); rep++) {
    bench::FanoutResult r =
        RunFanout(profile, type, clients, per_client, payload, secure);
    if (r.ok) {
      reps.push_back(r);
    }
  }
  bench::ReportFanout(profile, transport, clients, per_client, payload, reps);
}

}  // namespace

int main() {
  if (Init() != Result::Success) {
    std::fprintf(stderr, "failed to initialize znet\n");
    return 1;
  }
  std::printf("znet %s fan-out\n", ZNET_VERSION_STRING);
  bench::AnnounceRunSettings();
  std::fflush(stdout);

  for (const auto& c : bench::DefaultFanoutCases()) {
    RunCase("znet", ConnectionType::ZDT, c.clients, c.per_client, c.payload, true);
  }
  for (const auto& c : bench::DefaultFanoutCases()) {
    RunCase("znet-raw", ConnectionType::ZDT, c.clients, c.per_client, c.payload, false);
  }
  for (const auto& c : bench::DefaultFanoutCases()) {
    RunCase("znet", ConnectionType::TCP, c.clients, c.per_client, c.payload, true);
  }

  Cleanup();
  return 0;
}

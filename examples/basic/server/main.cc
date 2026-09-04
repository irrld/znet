//
//    Copyright 2023 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include "znet/packet_handler.h"
#include "znet/peer_session.h"
#include "znet/init.h"
#include "znet/server_events.h"
#include "znet/codec.h"
#include "znet/server.h"
#include "znet/signal_handler.h"

#include "packets.h"

#include <iostream>

using namespace znet;

class MyPacketHandler : public PacketHandler<MyPacketHandler, DemoPacket> {
 public:
  MyPacketHandler(std::shared_ptr<PeerSession> session) : session_(session) { }

  void OnPacket(std::shared_ptr<DemoPacket> p) {
    ZNET_LOG_INFO("Received demo_packet.");
    std::shared_ptr<DemoPacket> pk = std::make_shared<DemoPacket>();
    pk->text = "Got ya! Hello from server!";
    session_->SendPacket(pk);
  }

 private:
  std::shared_ptr<PeerSession> session_;
};

bool OnNewSessionEvent(IncomingClientConnectedEvent& event) {
  PeerSession& session = *event.session();

  // one codec per connection here; share one across sessions in practice
  std::shared_ptr<Codec> codec = std::make_shared<Codec>();
  codec->Add(kPacketDemo, std::make_unique<DemoSerializer>());
  session.SetCodec(codec);

  // the handler can be swapped later, e.g. one for login, one for play
  session.SetHandler(std::make_shared<MyPacketHandler>(event.session()));
  return false;
}

bool OnDisconnectSessionEvent(IncomingClientDisconnectedEvent& event) {
  return false;
}

void OnEvent(Event& event) {
  EventDispatcher dispatcher{event};
  dispatcher.Dispatch<IncomingClientConnectedEvent>(
      ZNET_BIND_GLOBAL_FN(OnNewSessionEvent));
  dispatcher.Dispatch<IncomingClientDisconnectedEvent>(
      ZNET_BIND_GLOBAL_FN(OnDisconnectSessionEvent));
}

int main() {
  Result result;

  if ((result = znet::Init()) != Result::Success) {
    ZNET_LOG_ERROR("Failed to initialize znet: {}", GetResultString(result));
    return 1;
  }

  // localhost here; a real application takes these from its own config.
  // On POSIX the address can also be a Unix socket path,
  // e.g. {"unix:/run/demo.sock", 0, ...} with ConnectionType::TCP.
  ServerConfig config{"localhost", 25000, std::chrono::seconds(10)};

  // A public server usually wants admission control; all of it lives in
  // config.options. See the wiki's Configuration Reference for the details.
  // config.options.max_connections = 1024;
  // config.options.max_attempts_per_source = 10;  // per 10 s window
  // config.options.denylist.push_back(CIDRBlock::Parse("203.0.113.0/24"));
  // Sessions ping when idle and drop after silence; both knobs are
  // per-session and default to 1 s pings and a 10 s timeout:
  // config.child_options.common.keepalive_interval = std::chrono::seconds(1);
  // config.child_options.common.idle_timeout = std::chrono::seconds(10);

  Server server{config};

  // optional: stop cleanly on Ctrl+C
  RegisterSignalHandler([&server](Signal sig) -> bool {
    server.Stop();
    return server.shutdown_complete();
  }, znet::kSignalInterrupt);

  server.SetEventCallback(ZNET_BIND_GLOBAL_FN(OnEvent));

  if ((result = server.Bind()) != Result::Success) {
    ZNET_LOG_ERROR("Failed to bind: {}", GetResultString(result));
    return 1;
  }

  // returns at once; the server runs on its own thread
  if ((result = server.Listen()) != Result::Success) {
    ZNET_LOG_ERROR("Failed to listen: {}", GetResultString(result));
    return 1;
  }

  // a real application would go on with its own work instead of waiting
  server.Wait();

  znet::Cleanup();
  return 0;
}
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
#include "znet/client_events.h"
#include "znet/codec.h"
#include "znet/client.h"

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

bool OnConnectEvent(ClientConnectedToServerEvent& event) {
  PeerSession& session = *event.session();

  // one codec per connection here; share one across sessions in practice
  std::shared_ptr<Codec> codec = std::make_shared<Codec>();
  codec->Add(kPacketDemo, std::make_unique<DemoSerializer>());
  session.SetCodec(codec);

  // the handler can be swapped later, e.g. one for login, one for play
  session.SetHandler(std::make_shared<MyPacketHandler>(event.session()));

  std::shared_ptr<DemoPacket> pk = std::make_shared<DemoPacket>();
  pk->text = "Hello from client!";
  event.session()->SendPacket(pk);
  return false;
}

// Called when the dial or its handshake fails: no session ever became ready.
// A session that connected and later ended fires
// ClientDisconnectedFromServerEvent instead; the two never overlap.
bool OnConnectionFailedEvent(ClientConnectionFailedEvent& event) {
  (void)event;
  ZNET_LOG_ERROR("Could not connect to the server.");
  // This is the place to schedule a retry or surface an error in your UI.
  return false;
}

void OnEvent(Event& event) {
  EventDispatcher dispatcher{event};
  dispatcher.Dispatch<ClientConnectedToServerEvent>(
      ZNET_BIND_GLOBAL_FN(OnConnectEvent));
  dispatcher.Dispatch<ClientConnectionFailedEvent>(
      ZNET_BIND_GLOBAL_FN(OnConnectionFailedEvent));
}

int main() {
  Result result;

  if ((result = znet::Init()) != Result::Success) {
    ZNET_LOG_ERROR("Failed to initialize znet: {}", GetResultString(result));
    return 1;
  }


  // localhost here; a real application takes these from its own config.
  ClientConfig config{"localhost", 25000};

  Client client{config};

  client.SetEventCallback(ZNET_BIND_GLOBAL_FN(OnEvent));

  if ((result = client.Bind()) != Result::Success) {
    ZNET_LOG_ERROR("Failed to bind: {}", GetResultString(result));
    return 1;
  }

  // returns at once; the client runs on its own thread
  if ((result = client.Connect()) != Result::Success) {
    ZNET_LOG_ERROR("Failed to connect: {}", GetResultString(result));
    return 1;
  }

  // a real application would go on with its own work instead of waiting
  client.Wait();

  znet::Cleanup();
  return 0;
}

//
//    Copyright 2025 Metehan Gezer
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
#include "player.h"

#include <iostream>

using namespace znet;

Codecs codecs_;
std::unique_ptr<Player> player;

struct PlayingPacketHandler
    : public PacketHandler<PlayingPacketHandler, MovePacket> {

  PlayingPacketHandler(std::shared_ptr<PeerSession> session)
      : session_(session) {}

  void OnPacket(const TeleportPacket& pk) {
    player->pos_ = pk.pos;
  }

 private:
  std::shared_ptr<PeerSession> session_;
};

struct LoginPacketHandler
    : public PacketHandler<LoginPacketHandler,
                           StartGamePacket> {
 public:
  LoginPacketHandler(std::shared_ptr<PeerSession> session)
      : session_(session) {}

  void OnPacket(const StartGamePacket& pk) {
    // Here, the pk.spawn_pos_ is available and sent by the server.
    ZNET_LOG_INFO("Game start! LevelName: {}, GameMode: {}, SpawnPos: {}", pk.level_name_, pk.game_mode_, pk.spawn_pos_.to_string());
    session_->SendPacket(std::make_shared<ClientReadyPacket>());
    session_->SetHandler(std::make_shared<PlayingPacketHandler>(session_));
  }

 private:
  std::shared_ptr<PeerSession> session_;
};


bool OnConnectEvent(ClientConnectedToServerEvent& event) {
  PeerSession& session = *event.session();

  session.SetCodec(codecs_.codec_v2);

  session.SetHandler(std::make_shared<LoginPacketHandler>(event.session()));

  std::shared_ptr<NetworkSettingsPacket> pk = std::make_shared<NetworkSettingsPacket>();
  pk->protocol_ = 2;
  event.session()->SendPacket(pk);
  return false;
}

void OnEvent(Event& event) {
  EventDispatcher dispatcher{event};
  dispatcher.Dispatch<ClientConnectedToServerEvent>(
      ZNET_BIND_GLOBAL_FN(OnConnectEvent));
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

//
//    Copyright 2025 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//


#include "znet/client.h"
#include "znet/codec.h"
#include "znet/init.h"
#include "znet/packet_handler.h"
#include "znet/peer_session.h"
#include "znet/server_events.h"
#include "znet/server.h"
#include "znet/signal_handler.h"

#include "packets.h"

#include <algorithm>

using namespace znet;

Codecs codecs_;

class Player {
 public:
  Player() {}

 public:
  int protocol_ = 0;
  Vec3 pos_;
};

struct PlayingPacketHandler
    : public PacketHandler<PlayingPacketHandler, MovePacket> {

  PlayingPacketHandler(std::shared_ptr<PeerSession> session, std::shared_ptr<Player> player)
        : session_(session), player_(player) {}

  void OnPacket(const MovePacket& pk) {
    player_->pos_ += pk.delta;
  }

 private:
  std::shared_ptr<PeerSession> session_;
  std::shared_ptr<Player> player_;
};

struct LoginPacketHandler
    : public PacketHandler<LoginPacketHandler, NetworkSettingsPacket,
                           ClientReadyPacket> {
 public:
  LoginPacketHandler(std::shared_ptr<PeerSession> session)
      : session_(session) {}

  void OnPacket(const NetworkSettingsPacket& pk) {
    std::shared_ptr<Player> player = session_->user_pointer<Player>();
    if (player) {
      player->protocol_ = pk.protocol_;
      ZNET_LOG_INFO("Player protocol set to: {}", player->protocol_);
    } else {
      ZNET_LOG_ERROR("User object is not a Player type for session!");
    }

    if (pk.protocol_ == 1) {
      session_->SetCodec(codecs_.codec_v1);
    } else if (pk.protocol_ == 2) {
      session_->SetCodec(codecs_.codec_v2);
    } else {
      ZNET_LOG_ERROR("Invalid protocol version: {}", pk.protocol_);
      session_->Close();
      return;
    }

    auto start_game = std::make_shared<StartGamePacket>();
    start_game->spawn_pos_ = {0, 60, 0};
    start_game->game_mode_ = 0;
    start_game->level_name_ = "test_world";
    session_->SendPacket(start_game);
  }

  void OnPacket(const ClientReadyPacket& pk) {
    ZNET_LOG_INFO("Client ready {}!", session_->id());
    session_->SetHandler(std::make_shared<PlayingPacketHandler>(session_,
                                                                session_->user_pointer<Player>()));
    // After this, session_ is invalid.
  }

 private:
  std::shared_ptr<PeerSession> session_;
};


std::vector<std::shared_ptr<Player>> active_players_;

bool OnNewSessionEvent(IncomingClientConnectedEvent& event) {
  PeerSession& session = *event.session();

  session.SetCodec(codecs_.codec_latest); // Initial codec
  session.SetHandler(std::make_shared<LoginPacketHandler>(event.session())); // initial handler
  std::shared_ptr<Player> player = std::make_shared<Player>();
  session.SetUserPointer(player);
  active_players_.push_back(std::move(player));
  return true;
}

bool OnDisconnectSessionEvent(znet::IncomingClientDisconnectedEvent& event) {
  znet::PeerSession& session = *event.session();
  std::shared_ptr<Player> user_ptr = session.user_pointer<Player>();
  if (!user_ptr) {
    return false;
  }
  auto it = std::find(active_players_.begin(), active_players_.end(), user_ptr);
  if (it != active_players_.end()) {
    ZNET_LOG_INFO("Player disconnected. Removing.");
    std::iter_swap(it, active_players_.end() - 1);
    active_players_.pop_back();
  }
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
  ServerConfig config{"localhost", 25000};

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

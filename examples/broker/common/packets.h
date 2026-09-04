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
// What the broker and its peers say to each other, and what the punched
// peers say once they meet. Candidate lists ride on znet's own serializer,
// so a gathering looks the same here as it does to RendezvousServer.
//

#pragma once

#include "znet/p2p/rendezvous.h"
#include "znet/packet.h"

using namespace znet;

enum PacketType : PacketId {
  kPacketHello,  // peer -> broker: my name and what I gathered
  kPacketOffer,  // broker -> peer: whom to punch and how
  kPacketNote,   // peer <-> peer, over the punched session
};

class HelloPacket : public Packet {
 public:
  HelloPacket() : Packet(kPacketHello) {}

  std::string name;
  std::vector<p2p::Candidate> candidates;
};

class OfferPacket : public Packet {
 public:
  OfferPacket() : Packet(kPacketOffer) {}

  std::string peer_name;
  uint64_t punch_id = 0;
  std::vector<p2p::Candidate> candidates;
};

class NotePacket : public Packet {
 public:
  NotePacket() : Packet(kPacketNote) {}

  std::string text;
};

class HelloSerializer : public PacketSerializer<HelloPacket> {
 public:
  std::shared_ptr<Buffer> SerializeTyped(
      std::shared_ptr<HelloPacket> packet,
      std::shared_ptr<Buffer> buffer) override {
    buffer->WriteString(packet->name);
    p2p::detail::WriteCandidates(buffer, packet->candidates);
    return buffer;
  }

  std::shared_ptr<HelloPacket> DeserializeTyped(
      std::shared_ptr<Buffer> buffer) override {
    auto packet = std::make_shared<HelloPacket>();
    packet->name = buffer->ReadString();
    if (!p2p::detail::ReadCandidates(buffer, packet->candidates)) {
      return nullptr;
    }
    return packet;
  }
};

class OfferSerializer : public PacketSerializer<OfferPacket> {
 public:
  std::shared_ptr<Buffer> SerializeTyped(
      std::shared_ptr<OfferPacket> packet,
      std::shared_ptr<Buffer> buffer) override {
    buffer->WriteString(packet->peer_name);
    buffer->WriteInt<uint64_t>(packet->punch_id);
    p2p::detail::WriteCandidates(buffer, packet->candidates);
    return buffer;
  }

  std::shared_ptr<OfferPacket> DeserializeTyped(
      std::shared_ptr<Buffer> buffer) override {
    auto packet = std::make_shared<OfferPacket>();
    packet->peer_name = buffer->ReadString();
    packet->punch_id = buffer->ReadInt<uint64_t>();
    if (!p2p::detail::ReadCandidates(buffer, packet->candidates)) {
      return nullptr;
    }
    return packet;
  }
};

class NoteSerializer : public PacketSerializer<NotePacket> {
 public:
  std::shared_ptr<Buffer> SerializeTyped(
      std::shared_ptr<NotePacket> packet,
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

inline std::shared_ptr<Codec> MakeBrokerCodec() {
  auto codec = std::make_shared<Codec>();
  codec->Add(kPacketHello, std::make_unique<HelloSerializer>());
  codec->Add(kPacketOffer, std::make_unique<OfferSerializer>());
  return codec;
}

inline std::shared_ptr<Codec> MakePeerCodec() {
  auto codec = std::make_shared<Codec>();
  codec->Add(kPacketNote, std::make_unique<NoteSerializer>());
  return codec;
}

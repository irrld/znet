//
//    Copyright 2026 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//
// API stability: experimental (see the wiki, API Stability)

#ifndef ZNET_P2P_RENDEZVOUS_H_
#define ZNET_P2P_RENDEZVOUS_H_

#include "znet/p2p/punch.h"
#include "znet/server.h"

namespace znet {
namespace p2p {

// Rendezvous flow (C is a client, S the server):
//
// C connects, and S names it at once:
// WelcomePacket      S -> C  protocol version, peer name, the endpoint S
//                            observed, the reflectors to gather from, the
//                            punch transport
// GatheringPacket    C -> S  punch port and candidates (Agent::Gather). May be
//                            repeated; the latest replaces the rest
// ConnectPeerPacket  C -> S  asks for a peer by name. An unknown name is
//                            answered with PeerNotFoundPacket and not queued:
//                            if the target shows up later, ask again
// PunchOfferPacket   S -> C  to both, once each asked for the other: the
//                            other's candidates, reflexive then host, plus a
//                            relayed one when the server runs a relay
// RepunchRequestPacket S -> C  a former partner re-asked after its address
//                            changed; re-gather and re-ask it so the pair can
//                            re-punch. See PeerLocator::Relocate.
//
// A reflector or relayed candidate whose host is unspecified (0.0.0.0) lives
// on the rendezvous host itself; the client substitutes the address it
// reached the server at. See IsUnspecifiedHost.

// bumped whenever the packets below change shape; the welcome carries it and
// a client that disagrees fails at once instead of waiting for a name that
// never comes
ZNET_INLINE_CONSTEXPR uint8_t kRendezvousProtocolVersion = 2;

// wire ids of the rendezvous protocol; plain PacketIds, usable directly with
// Codec::Add()
ZNET_INLINE_CONSTEXPR PacketId kPacketWelcome = 0;
ZNET_INLINE_CONSTEXPR PacketId kPacketGathering = 1;
ZNET_INLINE_CONSTEXPR PacketId kPacketConnectPeer = 2;
ZNET_INLINE_CONSTEXPR PacketId kPacketPunchOffer = 3;
ZNET_INLINE_CONSTEXPR PacketId kPacketPeerNotFound = 4;
ZNET_INLINE_CONSTEXPR PacketId kPacketRepunchRequest = 5;

// Cap on the reflectors a welcome may name
ZNET_INLINE_CONSTEXPR size_t kMaxReflectors = 8;

/** @brief Whether the address names no host (0.0.0.0 or ::), which on the
 *         wire means "the rendezvous host itself". */
inline bool IsUnspecifiedHost(const InetAddress& address) {
  const std::string key = address.host_key();
  if (key.empty()) {
    return false;
  }
  for (const char byte : key) {
    if (byte != 0) {
      return false;
    }
  }
  return true;
}

namespace detail {

inline void WriteEndpoints(
    const std::shared_ptr<Buffer>& buffer,
    const std::vector<std::shared_ptr<InetAddress>>& endpoints, size_t cap) {
  size_t count = endpoints.size();
  if (count > cap) {
    count = cap;
  }
  buffer->WriteInt<uint8_t>(static_cast<uint8_t>(count));
  for (size_t i = 0; i < count; i++) {
    buffer->WriteInetAddress(*endpoints[i]);
  }
}

// Returns false on a corrupt address, so the caller can refuse the frame. A
// truncated address reads back as port 0, which no endpoint here ever has,
// so that is refused too.
inline bool ReadEndpoints(const std::shared_ptr<Buffer>& buffer,
                          std::vector<std::shared_ptr<InetAddress>>& out,
                          size_t cap) {
  const uint8_t count = buffer->ReadInt<uint8_t>();
  if (count > cap) {
    return false;
  }
  for (uint8_t i = 0; i < count; i++) {
    auto address = buffer->ReadInetAddress();
    if (!address || address->port() == 0) {
      return false;
    }
    out.push_back(std::move(address));
  }
  return true;
}

// type(1), address, and for a Relayed one its token(8); at most
// kMaxCandidates of them. Public so a broker of your own can carry the same
// type on its own packets.
inline void WriteCandidates(const std::shared_ptr<Buffer>& buffer,
                            const std::vector<Candidate>& candidates) {
  size_t count = candidates.size();
  if (count > kMaxCandidates) {
    count = kMaxCandidates;
  }
  buffer->WriteInt<uint8_t>(static_cast<uint8_t>(count));
  for (size_t i = 0; i < count; i++) {
    const Candidate& candidate = candidates[i];
    buffer->WriteInt<uint8_t>(static_cast<uint8_t>(candidate.type));
    buffer->WriteInetAddress(*candidate.address);
    if (candidate.type == CandidateType::Relayed) {
      buffer->WriteInt<uint64_t>(candidate.relay_token);
    }
  }
}

// Returns false on an oversized list, an unknown type or a corrupt address,
// so the caller can refuse the frame.
inline bool ReadCandidates(const std::shared_ptr<Buffer>& buffer,
                           std::vector<Candidate>& out) {
  const uint8_t count = buffer->ReadInt<uint8_t>();
  if (count > kMaxCandidates) {
    return false;
  }
  for (uint8_t i = 0; i < count; i++) {
    Candidate candidate;
    const uint8_t raw_type = buffer->ReadInt<uint8_t>();
    if (raw_type > static_cast<uint8_t>(CandidateType::Relayed)) {
      return false;
    }
    candidate.type = static_cast<CandidateType>(raw_type);
    candidate.address = buffer->ReadInetAddress();
    if (!candidate.address || candidate.address->port() == 0) {
      return false;  // corrupt, or truncated into 0.0.0.0:0
    }
    if (candidate.type == CandidateType::Relayed) {
      candidate.relay_token = buffer->ReadInt<uint64_t>();
    }
    out.push_back(std::move(candidate));
  }
  return true;
}

inline void WriteConnectionType(const std::shared_ptr<Buffer>& buffer,
                                ConnectionType type) {
  buffer->WriteInt<uint8_t>(static_cast<uint8_t>(type));
}

// the type dispatches a punch; an unknown one is not worth guessing about
inline bool ReadConnectionType(const std::shared_ptr<Buffer>& buffer,
                               ConnectionType& out) {
  const uint8_t raw_type = buffer->ReadInt<uint8_t>();
  if (raw_type == static_cast<uint8_t>(ConnectionType::TCP)) {
    out = ConnectionType::TCP;
    return true;
  }
  if (raw_type == static_cast<uint8_t>(ConnectionType::ZDT)) {
    out = ConnectionType::ZDT;
    return true;
  }
  return false;
}

}  // namespace detail

class WelcomePacket : public Packet {
 public:
  WelcomePacket() : Packet(kPacketWelcome) {}

  /** @brief kRendezvousProtocolVersion of the server. */
  uint8_t protocol_version_ = kRendezvousProtocolVersion;
  std::string peer_name_;
  /** @brief The client as the server observed it, over the rendezvous link. */
  std::shared_ptr<InetAddress> endpoint_;
  /** @brief Where to gather the reflexive candidate from; empty when the
   *         server offers no reflector. */
  std::vector<std::shared_ptr<InetAddress>> reflectors_;
  /** @brief The server picks the punch transport, so both peers agree. */
  ConnectionType connection_type_ = ConnectionType::ZDT;
};

class GatheringPacket : public Packet {
 public:
  GatheringPacket() : Packet(kPacketGathering) {}

  /** @brief The port this client punches from. When no reflexive candidate
   *         is reported, the server pairs it with the address it observed. */
  PortNumber punch_port_ = 0;
  /** @brief Host and Reflexive only; a Relayed claim is dropped. */
  std::vector<Candidate> candidates_;
};

class ConnectPeerPacket : public Packet {
 public:
  ConnectPeerPacket() : Packet(kPacketConnectPeer) {}

  std::string target_peer_;
};

class PunchOfferPacket : public Packet {
 public:
  PunchOfferPacket() : Packet(kPacketPunchOffer) {}

  std::string target_peer_;
  uint64_t punch_id_ = 0;
  ConnectionType connection_type_ = ConnectionType::ZDT;
  /** @brief The peer's candidates, reflexive first, then host, then the
   *         relayed one if any. */
  std::vector<Candidate> candidates_;
};

class PeerNotFoundPacket : public Packet {
 public:
  PeerNotFoundPacket() : Packet(kPacketPeerNotFound) {}

  std::string target_peer_;
};

/** @brief A former partner re-asked for this client after moving; the client
 *         re-gathers and re-asks it so the two can re-punch. */
class RepunchRequestPacket : public Packet {
 public:
  RepunchRequestPacket() : Packet(kPacketRepunchRequest) {}

  std::string from_peer_;
};

class WelcomeSerializer : public PacketSerializer<WelcomePacket> {
 public:
  std::shared_ptr<Buffer> SerializeTyped(std::shared_ptr<WelcomePacket> packet, std::shared_ptr<Buffer> buffer) override {
    buffer->WriteInt<uint8_t>(packet->protocol_version_);
    buffer->WriteString(packet->peer_name_);
    buffer->WriteInetAddress(*packet->endpoint_);
    detail::WriteEndpoints(buffer, packet->reflectors_, kMaxReflectors);
    detail::WriteConnectionType(buffer, packet->connection_type_);
    return buffer;
  }

  std::shared_ptr<WelcomePacket> DeserializeTyped(std::shared_ptr<Buffer> buffer) override {
    auto packet = std::make_shared<WelcomePacket>();
    packet->protocol_version_ = buffer->ReadInt<uint8_t>();
    packet->peer_name_ = buffer->ReadString();
    packet->endpoint_ = buffer->ReadInetAddress();
    if (!packet->endpoint_) {
      return nullptr;  // corrupt address; refuse the frame, not the process
    }
    if (!detail::ReadEndpoints(buffer, packet->reflectors_, kMaxReflectors)) {
      return nullptr;
    }
    if (!detail::ReadConnectionType(buffer, packet->connection_type_)) {
      return nullptr;
    }
    return packet;
  }
};

class GatheringSerializer : public PacketSerializer<GatheringPacket> {
 public:
  std::shared_ptr<Buffer> SerializeTyped(std::shared_ptr<GatheringPacket> packet, std::shared_ptr<Buffer> buffer) override {
    buffer->WriteInt<uint16_t>(static_cast<uint16_t>(packet->punch_port_));
    detail::WriteCandidates(buffer, packet->candidates_);
    return buffer;
  }

  std::shared_ptr<GatheringPacket> DeserializeTyped(std::shared_ptr<Buffer> buffer) override {
    auto packet = std::make_shared<GatheringPacket>();
    packet->punch_port_ = buffer->ReadInt<uint16_t>();
    if (!detail::ReadCandidates(buffer, packet->candidates_)) {
      return nullptr;
    }
    return packet;
  }
};

class ConnectPeerSerializer : public PacketSerializer<ConnectPeerPacket> {
 public:
  std::shared_ptr<Buffer> SerializeTyped(std::shared_ptr<ConnectPeerPacket> packet, std::shared_ptr<Buffer> buffer) override {
    buffer->WriteString(packet->target_peer_);
    return buffer;
  }

  std::shared_ptr<ConnectPeerPacket> DeserializeTyped(std::shared_ptr<Buffer> buffer) override {
    auto packet = std::make_shared<ConnectPeerPacket>();
    packet->target_peer_ = buffer->ReadString();
    return packet;
  }
};

class PunchOfferSerializer : public PacketSerializer<PunchOfferPacket> {
 public:
  std::shared_ptr<Buffer> SerializeTyped(std::shared_ptr<PunchOfferPacket> packet, std::shared_ptr<Buffer> buffer) override {
    buffer->WriteString(packet->target_peer_);
    buffer->WriteInt<uint64_t>(packet->punch_id_);
    detail::WriteConnectionType(buffer, packet->connection_type_);
    detail::WriteCandidates(buffer, packet->candidates_);
    return buffer;
  }

  std::shared_ptr<PunchOfferPacket> DeserializeTyped(std::shared_ptr<Buffer> buffer) override {
    auto packet = std::make_shared<PunchOfferPacket>();
    packet->target_peer_ = buffer->ReadString();
    packet->punch_id_ = buffer->ReadInt<uint64_t>();
    if (!detail::ReadConnectionType(buffer, packet->connection_type_)) {
      return nullptr;
    }
    if (!detail::ReadCandidates(buffer, packet->candidates_)) {
      return nullptr;
    }
    return packet;
  }
};

class PeerNotFoundSerializer : public PacketSerializer<PeerNotFoundPacket> {
 public:
  std::shared_ptr<Buffer> SerializeTyped(std::shared_ptr<PeerNotFoundPacket> packet, std::shared_ptr<Buffer> buffer) override {
    buffer->WriteString(packet->target_peer_);
    return buffer;
  }

  std::shared_ptr<PeerNotFoundPacket> DeserializeTyped(std::shared_ptr<Buffer> buffer) override {
    auto packet = std::make_shared<PeerNotFoundPacket>();
    packet->target_peer_ = buffer->ReadString();
    return packet;
  }
};

class RepunchRequestSerializer : public PacketSerializer<RepunchRequestPacket> {
 public:
  std::shared_ptr<Buffer> SerializeTyped(std::shared_ptr<RepunchRequestPacket> packet, std::shared_ptr<Buffer> buffer) override {
    buffer->WriteString(packet->from_peer_);
    return buffer;
  }

  std::shared_ptr<RepunchRequestPacket> DeserializeTyped(std::shared_ptr<Buffer> buffer) override {
    auto packet = std::make_shared<RepunchRequestPacket>();
    packet->from_peer_ = buffer->ReadString();
    return packet;
  }
};

inline std::shared_ptr<Codec> BuildRendezvousCodec() {
  std::shared_ptr<znet::Codec> codec = std::make_shared<znet::Codec>();
  codec->Add(kPacketWelcome, std::make_unique<WelcomeSerializer>());
  codec->Add(kPacketGathering, std::make_unique<GatheringSerializer>());
  codec->Add(kPacketConnectPeer, std::make_unique<ConnectPeerSerializer>());
  codec->Add(kPacketPunchOffer, std::make_unique<PunchOfferSerializer>());
  codec->Add(kPacketPeerNotFound, std::make_unique<PeerNotFoundSerializer>());
  codec->Add(kPacketRepunchRequest, std::make_unique<RepunchRequestSerializer>());
  return codec;
}

}  // namespace p2p
}  // namespace znet


#endif  // ZNET_P2P_RENDEZVOUS_H_

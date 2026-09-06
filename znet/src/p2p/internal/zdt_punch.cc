//
//    Copyright 2026 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include "znet/p2p/internal/zdt_punch.h"

#include "znet/logger.h"
#include "znet/p2p/internal/gather.h"

#include <algorithm>

namespace znet {
namespace p2p {
namespace internal {

using namespace backends;

namespace {

ZNET_INLINE_CONSTEXPR std::chrono::milliseconds kPunchInterval{50};
ZNET_INLINE_CONSTEXPR std::chrono::milliseconds kRequestInterval{100};
ZNET_INLINE_CONSTEXPR std::chrono::milliseconds kBindInterval{100};

}  // namespace

Buffer BuildOffline(ZDTOfflineMsg id) {
  Buffer buffer(Endianness::BigEndian);
  WriteOfflineHeader(buffer, id);
  return buffer;
}

Buffer BuildPunch() {
  return BuildOffline(ZDTOfflineMsg::Punch);
}

Buffer BuildRequest1(uint64_t local_guid, uint8_t capabilities) {
  Buffer request = BuildOffline(ZDTOfflineMsg::OpenConnectionRequest1);
  request.WriteInt<uint8_t>(kZDTProtocolVersion);
  request.WriteInt<uint64_t>(local_guid);
  request.WriteInt<uint8_t>(capabilities);
  return request;
}

Buffer BuildReply2(uint64_t local_guid, uint16_t mtu, uint8_t capabilities) {
  Buffer reply = BuildOffline(ZDTOfflineMsg::OpenConnectionReply2);
  reply.WriteInt<uint64_t>(local_guid);
  reply.WriteInt<uint16_t>(mtu);
  reply.WriteInt<uint8_t>(capabilities);
  return reply;
}

Buffer BuildIncompatibleVersion(uint64_t local_guid) {
  Buffer bad = BuildOffline(ZDTOfflineMsg::IncompatibleProtocolVersion);
  bad.WriteInt<uint8_t>(kZDTProtocolVersion);
  bad.WriteInt<uint64_t>(local_guid);
  return bad;
}

Buffer BuildRelayBind(uint64_t token) {
  Buffer bind = BuildOffline(ZDTOfflineMsg::RelayBind);
  bind.WriteInt<uint64_t>(token);
  return bind;
}

Buffer BuildRelayBound(uint64_t token, uint32_t channel) {
  Buffer bound = BuildOffline(ZDTOfflineMsg::RelayBound);
  bound.WriteInt<uint64_t>(token);
  bound.WriteInt<uint32_t>(channel);
  return bound;
}

Buffer BuildReflect(uint64_t nonce) {
  Buffer reflect = BuildOffline(ZDTOfflineMsg::Reflect);
  reflect.WriteInt<uint64_t>(nonce);
  // padded so the request is never smaller than the answer
  while (reflect.size() < kZDTReflectSize) {
    reflect.WriteInt<uint8_t>(0);
  }
  return reflect;
}

Buffer BuildReflected(uint64_t nonce, const InetAddress& observed) {
  Buffer reflected = BuildOffline(ZDTOfflineMsg::Reflected);
  reflected.WriteInt<uint64_t>(nonce);
  reflected.WriteInetAddress(observed);
  return reflected;
}

void SendDatagram(UDPSocket& socket, const InetAddress& address,
                  uint32_t channel, const Buffer& datagram) {
  if (channel == 0) {
    socket.SendTo(address, datagram.data(), datagram.size());
    return;
  }
  Buffer wrapped(Endianness::BigEndian);
  WriteRelayHeader(wrapped, channel);
  wrapped.Write(datagram.data(), datagram.size());
  socket.SendTo(address, wrapped.data(), wrapped.size());
}

ZDTPunch::ZDTPunch(PunchOffer offer, TimePoint now, bool local_migration)
    : offer_(std::move(offer)),
      local_migration_(local_migration),
      deadline_(now + offer_.timeout),
      relay_start_(now + offer_.relay_delay) {
  connection_.mtu = 1200;  // conservative; skips the ladder probe for P2P
  connection_.local_guid = GenerateGuid();
  for (const auto& candidate : offer_.candidates) {
    if (candidate.type == CandidateType::Relayed) {
      Relay relay;
      relay.candidate = candidate;
      relays_.push_back(std::move(relay));
    } else {
      direct_.push_back(candidate);
    }
  }
}

bool ZDTPunch::Owns(const InetAddress& from, uint32_t channel) const {
  if (channel == 0 && ContainsAddress(direct_, from)) {
    return true;
  }
  for (const auto& relay : relays_) {
    // bare datagrams from a relay are its RelayBound; wrapped ones are the
    // peer's, on the channel the bind handed out
    if (*relay.candidate.address == from &&
        (channel == 0 || channel == relay.channel)) {
      return true;
    }
  }
  return false;
}

bool ZDTPunch::RelayActive(const Relay& relay, TimePoint now) const {
  return relay.channel != 0 && now >= relay_start_;
}

void ZDTPunch::SendToActive(UDPSocket& socket, const Buffer& datagram,
                            TimePoint now) const {
  for (const auto& candidate : direct_) {
    socket.SendTo(*candidate.address, datagram.data(), datagram.size());
  }
  for (const auto& relay : relays_) {
    if (RelayActive(relay, now)) {
      SendDatagram(socket, *relay.candidate.address, relay.channel, datagram);
    }
  }
}

PunchOutcome ZDTPunch::Tick(UDPSocket& socket, TimePoint now) {
  PunchOutcome outcome;
  if (now >= deadline_) {
    outcome.state = PunchOutcome::State::Failed;
    outcome.reason = Result::Timeout;
    return outcome;
  }
  // bind the relays right away, so the path is ready the moment it is needed
  for (auto& relay : relays_) {
    if (relay.channel != 0 || now - relay.last_bind < kBindInterval) {
      continue;
    }
    Buffer bind = BuildRelayBind(relay.candidate.relay_token);
    socket.SendTo(*relay.candidate.address, bind.data(), bind.size());
    relay.last_bind = now;
    outcome.sent = true;
  }
  // keep the hole open from both sides, toward every candidate
  if (now - last_punch_ > kPunchInterval) {
    SendToActive(socket, BuildPunch(), now);
    last_punch_ = now;
    outcome.sent = true;
  }
  // the initiator also drives the handshake (Request1 doubles as a punch)
  if (offer_.is_initiator && now - last_request_ > kRequestInterval) {
    SendToActive(socket,
                 BuildRequest1(connection_.local_guid,
                               local_migration_ ? kZDTCapMigration : 0),
                 now);
    last_request_ = now;
    outcome.sent = true;
  }
  return outcome;
}

void ZDTPunch::OnRelayBound(const InetAddress& from, Buffer& in) {
  if (in.readable_bytes() < sizeof(uint64_t) + sizeof(uint32_t)) {
    return;
  }
  const uint64_t token = in.ReadInt<uint64_t>();
  const uint32_t channel = in.ReadInt<uint32_t>();
  if (channel == 0 || channel > kZDTMaxRelayChannel) {
    return;
  }
  for (auto& relay : relays_) {
    if (relay.candidate.relay_token == token &&
        *relay.candidate.address == from) {
      if (relay.channel == 0) {
        ZNET_LOG_INFO("ZDT punch {}: bound at relay {} on channel {}",
                      offer_.punch_id, from.readable(), channel);
      }
      relay.channel = channel;
    }
  }
}

PunchOutcome ZDTPunch::OnDatagram(UDPSocket& socket,
                                  const std::shared_ptr<InetAddress>& from,
                                  uint32_t channel, const uint8_t* data,
                                  size_t len) {
  PunchOutcome outcome;
  if (len == 0) {
    return outcome;
  }
  // online data means the initiator considers itself connected: the responder
  // adopts the source and hands the datagram to the new transport
  if ((data[0] & kFlagOnline) != 0) {
    if (offer_.is_initiator || !answered_) {
      // ahead of Reply2 on the initiator, or of any Request1 on the
      // responder: not this punch's traffic
      return outcome;
    }
    outcome.state = PunchOutcome::State::Completed;
    outcome.from = from;
    outcome.channel = channel;
    outcome.first_datagram = data;
    outcome.first_len = len;
    return outcome;
  }

  Buffer in(reinterpret_cast<const char*>(data), len, Endianness::BigEndian);
  ZDTOfflineMsg id;
  if (!ReadOfflineHeader(in, id) || id == ZDTOfflineMsg::Punch) {
    return outcome;  // stray or keepalive punch
  }
  if (id == ZDTOfflineMsg::RelayBound) {
    OnRelayBound(*from, in);
    return outcome;
  }

  if (!offer_.is_initiator && id == ZDTOfflineMsg::OpenConnectionRequest1) {
    const uint8_t version = in.ReadInt<uint8_t>();
    if (version != kZDTProtocolVersion) {
      SendDatagram(socket, *from, channel,
                   BuildIncompatibleVersion(connection_.local_guid));
      return outcome;
    }
    connection_.remote_guid = in.ReadInt<uint64_t>();
    const uint8_t peer_cap = in.ReadInt<uint8_t>();
    connection_.migration_enabled =
        local_migration_ && (peer_cap & kZDTCapMigration);
    answered_ = true;
    SendDatagram(socket, *from, channel,
                 BuildReply2(connection_.local_guid, connection_.mtu,
                             local_migration_ ? kZDTCapMigration : 0));
    // stays pending until online data confirms the peer connected, so a lost
    // Reply2 is simply re-answered on the next Request1
    return outcome;
  }
  if (offer_.is_initiator && id == ZDTOfflineMsg::OpenConnectionReply2) {
    connection_.remote_guid = in.ReadInt<uint64_t>();
    const uint16_t mtu = in.ReadInt<uint16_t>();
    if (mtu != 0) {
      connection_.mtu = std::min(connection_.mtu, mtu);
    }
    const uint8_t peer_cap = in.ReadInt<uint8_t>();
    connection_.migration_enabled =
        local_migration_ && (peer_cap & kZDTCapMigration);
    outcome.state = PunchOutcome::State::Completed;
    outcome.from = from;
    outcome.channel = channel;
    return outcome;
  }
  if (offer_.is_initiator && id == ZDTOfflineMsg::IncompatibleProtocolVersion) {
    outcome.state = PunchOutcome::State::Failed;
    outcome.reason = Result::IncompatibleVersion;
  }
  return outcome;
}

}  // namespace internal
}  // namespace p2p
}  // namespace znet

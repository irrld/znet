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
// ZDT wire format: constants, datagram and record headers, offline handshake
// messages, MTU arithmetic and the return-routability cookie. No connection
// state lives here; everything is a value type or a pure function.
//
// API stability: internal (see the wiki, API Stability)

#ifndef ZNET_BACKENDS_ZDT_ZDT_WIRE_H_
#define ZNET_BACKENDS_ZDT_ZDT_WIRE_H_

// the state-free wire layer: only the buffer it reads and writes, so nothing
// above it (sessions, backends) is dragged in underneath
#include "znet/buffer.h"
#include "znet/compat.h"
#include "znet/inet_addr.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace znet {
namespace backends {

/** @brief Protocol version, checked for strict equality during the handshake. */
ZNET_INLINE_CONSTEXPR uint8_t kZDTProtocolVersion = 4;

/**
 * @brief Prefix on offline (pre-connection) messages.
 *
 * Keeps unrelated UDP traffic on the port from being parsed as a handshake.
 */
ZNET_INLINE_CONSTEXPR std::array<uint8_t, 8> kZDTMagic = {'Z', 'N', 'E', 'T',
                                                     'Z', 'D', 'T', 0x01};

// online datagram flags (byte 0), connection-level only. bit 7 separates
// connected-state datagrams from offline handshake messages, which the demux
// keys on. Anything about an individual message lives in its record flags.
// bit 6 is reserved: with bit 7 it marks a relay-wrapped datagram (see
// kZDTRelayMarker), so an online datagram never sets it.
ZNET_INLINE_CONSTEXPR uint8_t kFlagFin = 1u << 0;     // graceful close
ZNET_INLINE_CONSTEXPR uint8_t kFlagPing = 1u << 1;    // keepalive probe
ZNET_INLINE_CONSTEXPR uint8_t kFlagPong = 1u << 2;    // keepalive reply
ZNET_INLINE_CONSTEXPR uint8_t kFlagHasCid = 1u << 3;  // 8-byte connection id present
ZNET_INLINE_CONSTEXPR uint8_t kFlagOnline = 1u << 7;  // online-datagram marker

// --- Relay channel header ----------------------------------------------------
// every datagram between a peer and a p2p::RelayServer, in both directions,
// carries marker(1) + channel(3) in front of the ZDT datagram, so one relay
// port can serve every pairing and the receiving host can tell relayed peers
// apart. the marker is bits 7 and 6, which no ZDT datagram ever has together,
// and no offline id reaches 0x80, so it is unambiguous on any socket.
ZNET_INLINE_CONSTEXPR uint8_t kZDTRelayMarker = 0xC0;
ZNET_INLINE_CONSTEXPR size_t kZDTRelayHeaderSize = 4;
ZNET_INLINE_CONSTEXPR uint32_t kZDTMaxRelayChannel = 0xFFFFFFu;

// per-record flags (byte 0 of each message record).
ZNET_INLINE_CONSTEXPR uint8_t kRecReliable = 1u << 0;  // retransmit until acked
ZNET_INLINE_CONSTEXPR uint8_t kRecOrdered = 1u << 1;   // ordering applies on channel
ZNET_INLINE_CONSTEXPR uint8_t kRecFragment = 1u << 2;  // frag_index/frag_count present

/** @brief Offline (handshake) message ids, all < 0x80 so they never set kFlagOnline. */
enum class ZDTOfflineMsg : uint8_t {
  OpenConnectionRequest1 = 0x01,
  OpenConnectionReply1 = 0x02,
  OpenConnectionRequest2 = 0x03,
  OpenConnectionReply2 = 0x04,
  IncompatibleProtocolVersion = 0x05,
  NoFreeConnections = 0x06,
  AlreadyConnected = 0x07,
  ConnectionBanned = 0x08,
  Punch = 0x09,  // P2P NAT hole-punch keepalive (not part of the client/server flow)
  // between a peer and a p2p::RelayServer; none of these appear in the
  // client/server flow either. See znet/p2p/relay_server.h.
  RelayBind = 0x0A,   // peer -> relay: token(8)
  RelayBound = 0x0B,  // relay -> peer: token(8), channel(4)
  Reflect = 0x0C,     // peer -> relay: nonce(8), padded to kZDTReflectSize
  Reflected = 0x0D,   // relay -> peer: nonce(8), observed address
  // connection migration: confirm a peer really moved to a new address
  // before the send target follows it. See enable_connection_migration.
  PathChallenge = 0x0E,  // to a new address: cid(8), epoch(4), cookie(16)
  PathResponse = 0x0F,   // echoed from it: cid(8), epoch(4), cookie(16)
};

// id(1) + magic
ZNET_INLINE_CONSTEXPR size_t kZDTOfflineHeaderSize = 1 + kZDTMagic.size();
// a Reflect is padded to this, and a Reflected is always smaller, so the
// reflector cannot be used to amplify toward a spoofed source. Shorter ones
// are ignored.
ZNET_INLINE_CONSTEXPR size_t kZDTReflectSize = 64;

// a datagram is one header followed by zero or more message records, so small
// messages share one instead of each paying for its own. zero records is a valid
// control datagram (bare ack, ping, pong or fin).
// flags(1) + packet_seq(2) + ack(2) + block_count(1), then 2 bytes per block.
// this is the fixed part; a kFlagHasCid datagram carries kZDTCidSize more
// right after the flags byte, and ReadZDTHeader checks the blocks separately.
ZNET_INLINE_CONSTEXPR size_t kZDTHeaderSize = 6;
ZNET_INLINE_CONSTEXPR size_t kZDTCidSize = 8;
// rec_flags, channel, message_seq, length
ZNET_INLINE_CONSTEXPR size_t kZDTRecordHeaderSize = 6;
// the above plus frag_index and frag_count
ZNET_INLINE_CONSTEXPR size_t kZDTFragRecordHeaderSize = 8;

// sequences go on the wire truncated to 16 bits but are tracked in full: a
// truncated value aliases every 65536 messages, which would let a late retransmit
// reconstruct onto a live message and corrupt ordering and map keys.
using WireSeq = uint16_t;
using SequenceId = uint64_t;

// rebuilds the full SequenceId from a truncated wire value, picking the
// candidate nearest `expected` (the standard TCP/QUIC reconstruction).
inline SequenceId ReconstructSeq(WireSeq truncated, SequenceId expected) {
  constexpr SequenceId kPeriod = SequenceId{1} << 16;
  SequenceId candidate = (expected & ~(kPeriod - 1)) | truncated;
  if (candidate + kPeriod / 2 < expected) {
    candidate += kPeriod;
  } else if (candidate >= expected + kPeriod / 2 && candidate >= kPeriod) {
    candidate -= kPeriod;
  }
  return candidate;
}

// how far back the receiver remembers which packet_seqs arrived. the encoder
// walks this to build ack blocks, so it bounds what one acknowledgment can
// describe and therefore how large the send window may usefully grow.
ZNET_INLINE_CONSTEXPR size_t kZDTAckHistoryBits = 1024;
ZNET_INLINE_CONSTEXPR size_t kZDTAckHistoryWords = kZDTAckHistoryBits / 64;

// ack blocks per datagram. each is a run of received packets followed by the
// run of missing ones just older, walking backwards from `ack`. a gap that does
// not fit is described by a later acknowledgment, so this bounds header size
// rather than what can eventually be reported.
ZNET_INLINE_CONSTEXPR size_t kZDTMaxAckBlocks = 24;
// each block is num_ack(1) + num_nack(1)
ZNET_INLINE_CONSTEXPR size_t kZDTAckBlockSize = 2;
// blocks a data datagram always has room for. kZDTHeaderSize covers only the
// fixed part, so records are packed against this larger figure and the ack
// encoder is capped at whatever is actually left. without the reserve a full
// datagram would carry no ack at all.
ZNET_INLINE_CONSTEXPR size_t kZDTAckBlocksReserved = 4;
ZNET_INLINE_CONSTEXPR size_t kZDTHeaderReserve =
    kZDTHeaderSize + kZDTCidSize + kZDTAckBlocksReserved * kZDTAckBlockSize;

// how long a base round trip measurement stays authoritative. the minimum is
// the queue-free path, so it has to be re-probed: a route change or a handover
// raises the floor permanently, and a minimum kept for the whole connection
// would read the new baseline as congestion and never open the window again.
ZNET_INLINE_CONSTEXPR int kZDTRttMinWindowMs = 10000;

// round trip inflation that counts as a queue building rather than jitter, and
// how hard to back off when it does: an ack that finds the queue, a timeout
// while the queue stands, and a timeout on a path that is merely lossy, which
// a delay-based controller treats as loss rather than congestion. Every
// reduction is one per loss epoch and never below kZDTMinWindow.
ZNET_INLINE_CONSTEXPR double kZDTQueueingRttRatio = 1.25;
ZNET_INLINE_CONSTEXPR double kZDTQueueingBackoff = 0.85;
ZNET_INLINE_CONSTEXPR double kZDTQueueingTimeoutBackoff = 0.5;
ZNET_INLINE_CONSTEXPR double kZDTLossTimeoutBackoff = 0.9;
ZNET_INLINE_CONSTEXPR double kZDTMinWindow = 2.0;
// the test reads the smallest sample of the last full window, not the
// smoothed value: a queue holds every round trip up, a busy endpoint only the
// odd one. The window is two round trips, and never shorter than this.
ZNET_INLINE_CONSTEXPR int kZDTQueueingWindowFloorMs = 10;

// tail-loss probe: how long the ack stream may stay silent with reliable data
// outstanding before the newest unacked message is resent once. A lost burst
// tail is invisible to the NAK path (nothing later arrives to expose the gap),
// so without the probe it always waits out the full RTO floor. The delay is
// max(2 * srtt, this floor), doubling per probe while the silence lasts.
ZNET_INLINE_CONSTEXPR int kZDTTailProbeFloorMs = 10;

ZNET_INLINE_CONSTEXPR int kZDTMaxDatagramsInFlight =
    static_cast<int>(kZDTAckHistoryBits);

// one run of received packets and the run of missing ones immediately older.
// blocks are ordered newest first, the first one ending at `ack`.
struct ZDTAckBlock {
  uint8_t num_ack = 0;   // consecutive received, ending at this block's head
  uint8_t num_nack = 0;  // consecutive missing, just older than those
};

struct ZDTHeader {
  uint8_t flags = kFlagOnline;
  // present only when flags carries kFlagHasCid: the sender's own guid, so
  // the receiver can route this datagram to the session after the source
  // address changed
  uint64_t cid = 0;
  uint16_t packet_seq = 0;  // connection-level, ++ per datagram (drives ack/RTT)
  uint16_t ack = 0;         // highest packet_seq seen from peer
  // run-length encoded picture of what arrived, walking back from `ack`. the
  // nack runs are the negative acknowledgment, so nothing caps the window at
  // what fits in a fixed-width bitfield.
  std::array<ZDTAckBlock, kZDTMaxAckBlocks> blocks{};
  uint8_t block_count = 0;
};

// one message, or one fragment of one, inside a datagram.
struct ZDTRecord {
  uint8_t flags = 0;         // kRec*
  uint8_t channel = 0;
  uint16_t message_seq = 0;  // per-channel message sequence
  uint8_t frag_index = 0;
  uint8_t frag_count = 1;
  uint16_t length = 0;  // payload bytes following this record header
};

// serializes `header` (big-endian) to the front of `buffer`.
void WriteZDTHeader(Buffer& buffer, const ZDTHeader& header);
// parses an online header from the front of `buffer`; returns false if the buffer
// is too short or does not carry the online marker. leaves the read cursor at the
// first record on success.
bool ReadZDTHeader(Buffer& buffer, ZDTHeader& out_header);

// record header only; the payload follows and is not copied.
void WriteZDTRecord(Buffer& buffer, const ZDTRecord& record);
// reads a record header and validates that `length` bytes actually follow.
// leaves the read cursor at the record's payload.
bool ReadZDTRecord(Buffer& buffer, ZDTRecord& out_record);

// bytes a record occupies on the wire, header plus payload.
inline size_t ZDTRecordSize(bool fragment, size_t payload_len) {
  return (fragment ? kZDTFragRecordHeaderSize : kZDTRecordHeaderSize) +
         payload_len;
}

// MTULadder rungs are *link* MTUs (1492 is PPPoE, 576 the IPv4 minimum
// reassembly size), but everything downstream of the handshake budgets
// against the UDP payload, which excludes the IP and UDP headers. Sending
// `rung` payload bytes puts `rung + overhead` on the wire, so on a 1500-byte
// path the probe would fail with EMSGSIZE and step the ladder down a rung it
// did not need to.
ZNET_INLINE_CONSTEXPR uint16_t kZDTIPv4Overhead = 28;  // 20 IPv4 + 8 UDP
ZNET_INLINE_CONSTEXPR uint16_t kZDTIPv6Overhead = 48;  // 40 IPv6 + 8 UDP

inline uint16_t ZDTDatagramOverhead(InetProtocolVersion ipv) {
  return ipv == InetProtocolVersion::IPv6 ? kZDTIPv6Overhead : kZDTIPv4Overhead;
}

/** @brief Largest UDP payload that still fits a link of `link_mtu` bytes. */
inline uint16_t ZDTPayloadForLinkMTU(uint16_t link_mtu,
                                     InetProtocolVersion ipv) {
  const uint16_t overhead = ZDTDatagramOverhead(ipv);
  return link_mtu > overhead ? static_cast<uint16_t>(link_mtu - overhead) : 0;
}

// writes an offline message id followed by kZDTMagic.
void WriteOfflineHeader(Buffer& buffer, ZDTOfflineMsg id);
// reads and validates an offline header (id < 0x80 and correct magic). on success
// the read cursor is left just past the magic and `out_id` holds the message id.
bool ReadOfflineHeader(Buffer& buffer, ZDTOfflineMsg& out_id);
// the same check on raw bytes, for a loop that must not build a Buffer per
// datagram. true when `data` carries a valid offline header.
bool PeekOfflineHeader(const uint8_t* data, size_t len, ZDTOfflineMsg& out_id);

// big-endian field reads off raw bytes, the same way Buffer would read them.
inline uint64_t ReadBigEndian64(const uint8_t* data) {
  uint64_t value = 0;
  for (int i = 0; i < 8; i++) {
    value = (value << 8) | data[i];
  }
  return value;
}

// the relay channel header, written in front of a datagram and read off raw
// bytes. ReadRelayHeader is true when `data` starts with one; the payload
// then begins kZDTRelayHeaderSize in.
inline void WriteRelayHeader(Buffer& buffer, uint32_t channel) {
  buffer.WriteInt<uint8_t>(kZDTRelayMarker);
  buffer.WriteInt<uint8_t>(static_cast<uint8_t>((channel >> 16) & 0xFFu));
  buffer.WriteInt<uint8_t>(static_cast<uint8_t>((channel >> 8) & 0xFFu));
  buffer.WriteInt<uint8_t>(static_cast<uint8_t>(channel & 0xFFu));
}
inline bool ReadRelayHeader(const uint8_t* data, size_t len,
                            uint32_t& out_channel) {
  if (len < kZDTRelayHeaderSize || data[0] != kZDTRelayMarker) {
    return false;
  }
  out_channel = (static_cast<uint32_t>(data[1]) << 16) |
                (static_cast<uint32_t>(data[2]) << 8) | data[3];
  return out_channel != 0;
}

// The connection id an online datagram carries, read straight off the bytes so
// the server can route it before parsing the rest. False when the flags lack
// kFlagHasCid or the datagram is too short to hold one.
inline bool PeekCid(const uint8_t* data, size_t len, uint64_t& out_cid) {
  if (len < 1 + kZDTCidSize || !(data[0] & kFlagHasCid)) {
    return false;
  }
  out_cid = ReadBigEndian64(data + 1);
  return true;
}

// --- Return-routability cookie ------------------------------------------------
// server issues HMAC(secret[epoch], addr, epoch) in Reply1 holding no state, and
// only allocates a session once the client echoes it back in Request2. Path
// validation reuses the same primitive, keyed on the new address and cid.
ZNET_INLINE_CONSTEXPR size_t kZDTCookieLen = 16;
using ZDTCookie = std::array<uint8_t, kZDTCookieLen>;

ZDTCookie ComputeCookie(const uint8_t* secret, size_t secret_len,
                        const std::string& peer_readable, uint32_t epoch);
// constant-time comparison (no early-out) to avoid timing side channels.
bool ConstTimeEqual(const ZDTCookie& a, const ZDTCookie& b);
// 64-bit random peer identifier (OpenSSL RAND_bytes).
uint64_t GenerateGuid();

// PathChallenge and PathResponse share a shape: the connection id being moved,
// the epoch its cookie was minted under, and a stateless return-routability
// cookie. The client echoes a challenge back verbatim; the server keeps no
// per-challenge state, recomputing the cookie to verify the response.
struct ZDTPathMessage {
  uint64_t cid = 0;
  uint32_t epoch = 0;
  ZDTCookie cookie{};
};

// Builds a PathChallenge or PathResponse datagram; `id` must be one of them.
Buffer WritePathMessage(ZDTOfflineMsg id, const ZDTPathMessage& msg);

// Reads the fields that follow an already-read offline header. False when fewer
// than all of them remain.
bool ReadPathMessage(Buffer& buffer, ZDTPathMessage& out);

// 16-bit sequence comparison that tolerates wraparound. Inline here rather than
// file-local, because both the ack encoder and the ack parser need them and
// they live in different translation units.
inline bool SeqGreater(uint16_t a, uint16_t b) {
  return static_cast<int16_t>(a - b) > 0;
}
inline bool SeqLess(uint16_t a, uint16_t b) {
  return static_cast<int16_t>(a - b) < 0;
}


}  // namespace backends
}  // namespace znet


#endif  // ZNET_BACKENDS_ZDT_ZDT_WIRE_H_

//
//    Copyright 2026 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//
// API stability: stable (see the wiki, API Stability)

#ifndef ZNET_CODEC_H_
#define ZNET_CODEC_H_

#include "znet/buffer.h"
#include "znet/compat.h"
#include "znet/packet.h"
#include "znet/packet_handler.h"
#include "znet/packet_serializer.h"

namespace znet {

/** @brief What Deserialize found in a buffer besides the packets it delivered. */
struct DecodeStats {
  /**
   * @brief Frames that could not be decoded.
   *
   * An unreadable header, a declared size the buffer cannot back, a serializer
   * refusing a frame, or one reading past its declared size. Unknown packet
   * ids are not counted: they skip cleanly and can be honest version skew.
   */
  uint32_t invalid_frames = 0;
  /**
   * @brief The rest of the buffer was dropped because the framing could no
   *        longer be trusted.
   */
  bool framing_lost = false;
};

/**
 * @brief Provides serialization and deserialization of packets.
 */
class Codec {
 public:
  Codec() = default;
  ~Codec() = default;

  /**
   * @brief Decodes every frame in `buffer` with the serializer its id names
   *        and hands each packet to `handler`. A frame with no serializer, a
   *        failed deserialize or a size mismatch is logged and skipped.
   *
   * @param dump_on_failure Log a bounded hex dump of the buffer at the first
   *        frame in it that fails to decode.
   * @return What failed to decode, for the caller to count; the codec itself
   *         is shared between sessions and keeps no per-peer state.
   */
  DecodeStats Deserialize(std::shared_ptr<Buffer> buffer,
                          PacketHandlerBase& handler,
                          bool dump_on_failure = false);

  /**
   * @brief Frames one packet: its id, its size and the serializer's bytes.
   *
   * @param headroom Bytes to leave in front of the payload, for stages that
   *        prepend a header afterwards. See Buffer::ReserveHeadroom.
   * @return A shared pointer to the resulting serialized buffer.
   *         Returns nullptr if no serializer is found for the packet.
   */
  std::shared_ptr<Buffer> Serialize(std::shared_ptr<Packet> packet,
                                    size_t headroom = 0);

  /**
   * @brief Registers a packet serializer for a specific packet type.
   *
   * @param id Unique identifier for the packet type.
   * @param serializer Serializer instance to handle the packet type.
   */
  void Add(PacketId id, std::unique_ptr<PacketSerializerBase> serializer);

 private:
  std::unordered_map<PacketId, std::unique_ptr<PacketSerializerBase>> serializers_;
};

}



#endif  // ZNET_CODEC_H_

//
//    Copyright 2023 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#pragma once

namespace znet {

// Max buffer size defines how big send and receive of each data can be.
#ifndef ZNET_MAX_BUFFER_SIZE
#define ZNET_MAX_BUFFER_SIZE 4096 //16384
#endif

template <typename T>
using Weak = std::weak_ptr<T>;

template <typename T>
using Scope = std::unique_ptr<T>;

template <typename T, typename... Args>
constexpr Scope<T> CreateScope(Args&&... args) {
  return std::make_unique<T>(std::forward<Args>(args)...);
}

template <typename T>
using Ref = std::shared_ptr<T>;

template <typename T, typename... Args>
constexpr Ref<T> CreateRef(Args&&... args) {
  return std::make_shared<T>(std::forward<Args>(args)...);
}

using PacketId = uint64_t;

enum class Endianness { LittleEndian, BigEndian };

#if defined(__cpp_lib_endian) && (__cpp_lib_endian >= 201907L)
#include <bit>

constexpr Endianness GetSystemEndianness() {
  if constexpr (std::endian::native == std::endian::big) {
    return Endianness::BigEndian;
  } else {
    return Endianness::LittleEndian;
  }
}
#else
inline Endianness GetSystemEndianness() {
  union {
    uint32_t i;
    uint8_t c[4];
  } u = {0x01020304};
  return (u.c[0] == 0x01) ? Endianness::BigEndian : Endianness::LittleEndian;
}
#endif

enum class Result {
  Success,
  Failure,
  AlreadyStopped,
  AlreadyClosed,
  AlreadyDisconnected,
  CannotBind,
  InvalidAddress,
  InvalidRemoteAddress,
  CannotCreateSocket,
  CannotListen,
  AlreadyConnected,
  AlreadyListening
};

enum class ConnectionType {
  TCP,
  //UDPUnreliable,
  //RakNet,
  //ENet,
  //WebSocket,
};

}  // namespace znet
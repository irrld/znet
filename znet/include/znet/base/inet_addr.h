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

#include "znet/precompiled.h"
#include "znet/base/types.h"

namespace znet {

#if defined(TARGET_APPLE) || defined(TARGET_WEB) || defined(TARGET_LINUX)
using SocketHandle = int;
using PortNumber = in_port_t;
using IPv4Address = in_addr;
using IPv6Address = in6_addr;
#elif defined(TARGET_WIN)
using SocketHandle = SOCKET;
using PortNumber = USHORT;
using IPv4Address = IN_ADDR;
using IPv6Address = IN6_ADDR;
#endif

enum class InetProtocolVersion { IPv4, IPv6 };

std::string ResolveHostnameToIP(const std::string& hostname);

IPv4Address ParseIPv4(const std::string& ip_str);
IPv6Address ParseIPv6(const std::string& ip_str);

int GetDomainByInetProtocolVersion(InetProtocolVersion version);

bool IsIPv4(const std::string& ip);
bool IsIPv6(const std::string& ip);

class InetAddress {
 public:
  InetAddress(InetProtocolVersion ipv, std::string readable)
      : ipv_(ipv), readable_(std::move(readable)) {}
  virtual ~InetAddress() = default;

  operator bool() const { return is_valid(); }

  ZNET_NODISCARD virtual bool is_valid() const { return false; }

  ZNET_NODISCARD virtual const std::string& readable() const {
    return readable_;
  };

  ZNET_NODISCARD InetProtocolVersion ipv() const { return ipv_; }

  ZNET_NODISCARD virtual socklen_t addr_size() const = 0;

  ZNET_NODISCARD virtual sockaddr* handle_ptr() const = 0;

  ZNET_NODISCARD virtual PortNumber port() const = 0;

  static std::unique_ptr<InetAddress> from(const std::string& host, PortNumber port);
  static std::unique_ptr<InetAddress> from(sockaddr* addr);

 protected:
  InetProtocolVersion ipv_;
  std::string readable_;
};

class InetAddressIPv4 : public InetAddress {
 public:
  explicit InetAddressIPv4(PortNumber port);

  InetAddressIPv4(IPv4Address ip, PortNumber port);

  InetAddressIPv4(const std::string& str, PortNumber port);

  ZNET_NODISCARD bool is_valid() const override { return is_valid_; }

  ZNET_NODISCARD socklen_t addr_size() const override { return sizeof(addr); }

  ZNET_NODISCARD sockaddr* handle_ptr() const override {
    return (sockaddr*)&addr;
  }

  ZNET_NODISCARD PortNumber port() const override {
    return addr.sin_port;
  }

 private:
  sockaddr_in addr{};
  bool is_valid_;
};

class InetAddressIPv6 : public InetAddress {
 public:
  explicit InetAddressIPv6(PortNumber port);

  InetAddressIPv6(IPv6Address ip, PortNumber port);

  InetAddressIPv6(const std::string& str, PortNumber port);

  ZNET_NODISCARD bool is_valid() const override { return is_valid_; }

  ZNET_NODISCARD socklen_t addr_size() const override { return sizeof(addr); }

  ZNET_NODISCARD sockaddr* handle_ptr() const override {
    return (sockaddr*)&addr;
  }

  ZNET_NODISCARD PortNumber port() const override {
    return addr.sin6_port;
  }

 private:
  sockaddr_in6 addr{};
  bool is_valid_;
};

}  // namespace znet
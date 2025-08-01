
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
#include <ostream>
#include <sstream>
#include <iostream>
#include <strstream>
#include <iomanip>
#include <type_traits>

#define ZNET_BIND_FN(fn)                                    \
  [this](auto&&... args) -> decltype(auto) {                \
    return this->fn(std::forward<decltype(args)>(args)...); \
  }

#define ZNET_BIND_GLOBAL_FN(fn)                       \
  [](auto&&... args) -> decltype(auto) {              \
    return fn(std::forward<decltype(args)>(args)...); \
  }

template <class T>
std::string ToHex(const T& numValue, int width) {
  std::ostringstream stream;
  stream << "0x"
         << std::setfill('0') << std::setw(width)
         << std::hex << +numValue;
  return stream.str();
}

template <typename F, typename... Args>
struct is_invocable {
 private:
  template <typename U>
  static auto test(U*) -> decltype(std::declval<U>()(std::declval<Args>()...), std::true_type{});

  template <typename>
  static std::false_type test(...);

 public:
  static constexpr bool value = decltype(test<F>(nullptr))::value;
};

std::string GeneratePeerName();
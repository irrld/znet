
//
//    Copyright 2024 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#pragma once

#include "znet/precompiled.h"
#include "znet/types.h"

#include <thread>
#include <utility>
#include <stop_token>

namespace znet {

class Task {
 public:
  Task() = default;

  // Destructor automatically requests stop and joins (std::jthread feature)
  ~Task() = default;

  Task(const Task&) = delete;
  Task& operator=(const Task&) = delete;

  // Move operations allowed
  Task(Task&&) noexcept = default;
  Task& operator=(Task&&) noexcept = default;

  ZNET_NODISCARD bool IsRunning() const {
    return thread_.joinable();
  }

  // Run function without stop_token (backward compatible)
  void Run(std::function<void()> run) {
    thread_ = std::jthread([run = std::move(run)](std::stop_token) {
      run();
    });
  }

  // Run function with stop_token for cooperative cancellation
  void Run(std::function<void(std::stop_token)> run) {
    thread_ = std::jthread(std::move(run));
  }

  // Request cooperative cancellation
  void RequestStop() {
    thread_.request_stop();
  }

  // Wait for thread to complete (blocks until done)
  void Wait() {
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  // Get stop token for external use
  ZNET_NODISCARD std::stop_token GetStopToken() const {
    return thread_.get_stop_token();
  }

 private:
  std::jthread thread_;
};

}

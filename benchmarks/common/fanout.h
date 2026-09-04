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
// The fan-out measurement, shared so every library reports comparable rows:
// one server broadcasting 1 KiB to N sessions, the shape a game server has
// rather than the one-session pipeline the throughput pool measures. Setup
// and teardown stay library-specific; the timed phase and the row do not.
//

#ifndef ZNET_BENCH_FANOUT_H
#define ZNET_BENCH_FANOUT_H

#include "common/harness.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace bench {

struct FanoutResult {
  bool ok = false;
  uint32_t delivered = 0;
  double seconds = 0.0;
  double cpu_seconds = 0.0;
  bool timed_out = false;
};

inline double FanoutRate(const FanoutResult& r) {
  return r.seconds > 0 ? static_cast<double>(r.delivered) / r.seconds : 0.0;
}

// Runs one already-connected fan-out: `broadcast_round` puts one message on
// every session and returns false only on a fatal send failure; `pump`
// services the library and returns deliveries since the last call. Timing and
// process CPU cover the measured phase alone.
template <typename BroadcastRound, typename Pump>
FanoutResult RunFanoutMeasured(uint32_t client_count, uint32_t per_client,
                               BroadcastRound broadcast_round, Pump pump) {
  const uint32_t total = client_count * per_client;
  const auto deadline = Clock::now() + std::chrono::seconds(120);
  const double cpu_before = ProcessCpuSeconds();
  const auto started = Clock::now();

  uint32_t received = 0;
  for (uint32_t round = 0; round < per_client; round++) {
    if (!broadcast_round(round)) {
      break;
    }
    received += pump();
    if (Clock::now() > deadline) {
      break;
    }
  }
  while (received < total && Clock::now() < deadline) {
    received += pump();
  }

  FanoutResult out;
  out.ok = true;
  out.delivered = received;
  out.seconds = std::chrono::duration<double>(Clock::now() - started).count();
  out.cpu_seconds = ProcessCpuSeconds() - cpu_before;
  out.timed_out = out.delivered < total;
  return out;
}

// Median rep by msg/s, span and CPU appended; CSV gets every rep.
inline void ReportFanout(const char* library, const char* transport,
                         uint32_t client_count, uint32_t per_client,
                         size_t payload_bytes,
                         const std::vector<FanoutResult>& reps) {
  if (reps.empty()) {
    return;
  }
  char case_name[32];
  std::snprintf(case_name, sizeof(case_name), "%ux%u", client_count,
                per_client);

  for (size_t i = 0; i < reps.size(); i++) {
    CsvRow row;
    row.kind = "fanout";
    row.library = library;
    row.transport = transport;
    row.case_name = case_name;
    row.rep = static_cast<int>(i + 1);
    row.delivered = reps[i].delivered;
    row.seconds = reps[i].seconds;
    row.msg_per_s = FanoutRate(reps[i]);
    row.mib_per_s = reps[i].seconds > 0
                        ? (static_cast<double>(reps[i].delivered) *
                           static_cast<double>(payload_bytes)) /
                              reps[i].seconds / (1024.0 * 1024.0)
                        : 0.0;
    row.timed_out = reps[i].timed_out ? 1 : 0;
    if (reps[i].cpu_seconds > 0.0 && reps[i].delivered > 0) {
      row.cpu_us_per_msg = reps[i].cpu_seconds * 1e6 /
                           static_cast<double>(reps[i].delivered);
    }
    EmitCsv(row);
  }

  std::vector<FanoutResult> sorted = reps;
  std::sort(sorted.begin(), sorted.end(),
            [](const FanoutResult& a, const FanoutResult& b) {
              return FanoutRate(a) < FanoutRate(b);
            });
  const FanoutResult& mid = sorted[sorted.size() / 2];
  const double mib = mid.seconds > 0
                         ? (static_cast<double>(mid.delivered) *
                            static_cast<double>(payload_bytes)) /
                               (1024.0 * 1024.0) / mid.seconds
                         : 0.0;
  std::printf(
      "%-10s %-6s fanout     %4ux%-6u %8u msgs  %8.3f s  %10.0f msg/s  %8.1f MiB/s",
      library, transport, client_count, per_client, mid.delivered, mid.seconds,
      FanoutRate(mid), mib);
  if (mid.cpu_seconds > 0.0 && mid.delivered > 0) {
    std::printf("  %7.2f cpu-us/msg",
                mid.cpu_seconds * 1e6 / static_cast<double>(mid.delivered));
  }
  if (mid.timed_out) {
    std::printf("  TIMEOUT (%u/%u in 120 s)", mid.delivered,
                client_count * per_client);
  }
  if (reps.size() > 1) {
    std::printf("  [%zu reps: %.0f..%.0f msg/s]", reps.size(),
                FanoutRate(sorted.front()), FanoutRate(sorted.back()));
  }
  std::printf("\n");
  std::fflush(stdout);
}

// The three cases every fan-out bench runs: the same session counts, 1 KiB.
struct FanoutCase {
  uint32_t clients;
  uint32_t per_client;
  size_t payload;
};

inline const std::vector<FanoutCase>& DefaultFanoutCases() {
  static const std::vector<FanoutCase> cases = {
      {8, 4000, 1024},
      {32, 2000, 1024},
      {64, 1000, 1024},
  };
  return cases;
}

}  // namespace bench

#endif  // ZNET_BENCH_FANOUT_H

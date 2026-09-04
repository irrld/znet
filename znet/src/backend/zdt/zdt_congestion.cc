//
//    Copyright 2026 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include "znet/backends/zdt/zdt_congestion.h"

#include <algorithm>
#include <cmath>

namespace znet {
namespace backends {

void ZDTRttEstimator::OnSample(std::chrono::steady_clock::duration sample,
                               TimePoint now,
                               std::chrono::milliseconds rto_min,
                               std::chrono::milliseconds rto_max) {
  const double ms = std::chrono::duration<double, std::milli>(sample).count();
  if (ms < 0.0) {
    return;
  }
  if (!has_rtt_) {
    has_rtt_ = true;
    srtt_ms_ = ms;
    rttvar_ms_ = ms / 2.0;
    rtt_min_ms_ = ms;
    rtt_min_stamp_ = now;
    collecting_min_ms_ = ms;
    window_start_ = now;
  } else {
    const bool stale =
        now - rtt_min_stamp_ > std::chrono::milliseconds(kZDTRttMinWindowMs);
    if (ms < rtt_min_ms_ || rtt_min_ms_ <= 0.0 || stale) {
      rtt_min_ms_ = ms;
      rtt_min_stamp_ = now;
    }
    rttvar_ms_ = 0.75 * rttvar_ms_ + 0.25 * std::fabs(srtt_ms_ - ms);
    srtt_ms_ = 0.875 * srtt_ms_ + 0.125 * ms;
    // one sample cannot open or close the queueing verdict; a full window of
    // them does. Two round trips, floored so a loopback window holds more
    // than a handful of samples.
    const double window_ms =
        std::max(2.0 * srtt_ms_, static_cast<double>(kZDTQueueingWindowFloorMs));
    const auto window = std::chrono::duration<double, std::milli>(window_ms);
    if (now - window_start_ >= window) {
      window_min_ms_ = collecting_min_ms_;
      collecting_min_ms_ = ms;
      window_start_ = now;
    } else if (ms < collecting_min_ms_) {
      collecting_min_ms_ = ms;
    }
  }
  const auto rto = std::chrono::milliseconds(
      static_cast<long long>(srtt_ms_ + 4.0 * rttvar_ms_));
  rto_ = compat::Clamp(rto, rto_min, rto_max);
}

// leaving recovery on the first ack of something sent after the loss is what
// keeps one burst from reducing the window more than once
void ZDTCongestionController::OnAckArrived(WireSeq peer_ack) {
  if (in_loss_recovery_ && !SeqLess(peer_ack, loss_recovery_until_)) {
    in_loss_recovery_ = false;
  }
}

void ZDTCongestionController::Reduce(double factor, WireSeq next_seq) {
  if (in_loss_recovery_) {
    return;  // this epoch already paid
  }
  in_loss_recovery_ = true;
  loss_recovery_until_ = next_seq;
  // the window it had is where slow start hands over to additive growth: a
  // reduction probes the queue, and once the verdict clears the window comes
  // back in a few round trips rather than one datagram per round trip
  ssthresh_ = cwnd_;
  cwnd_ *= factor;
  if (cwnd_ < kZDTMinWindow) {
    cwnd_ = kZDTMinWindow;
  }
}

void ZDTCongestionController::OnAcked(int acked_datagrams,
                                      const ZDTRttEstimator& rtt,
                                      WireSeq next_seq, int cap) {
  if (acked_datagrams <= 0) {
    return;
  }
  // a standing queue: back off once, and do not grow while it stands
  if (rtt.IsQueueing() && cwnd_ > 2.0 * kZDTMinWindow) {
    Reduce(kZDTQueueingBackoff, next_seq);
    return;
  }
  if (cwnd_ < ssthresh_) {
    cwnd_ += static_cast<double>(acked_datagrams);
  } else {
    cwnd_ += static_cast<double>(acked_datagrams) / cwnd_;
  }
  const double cap_d = static_cast<double>(cap);
  if (cwnd_ > cap_d) {
    cwnd_ = cap_d;
  }
}

void ZDTCongestionController::OnRetransmitTimeout(const ZDTRttEstimator& rtt,
                                                  WireSeq next_seq) {
  Reduce(rtt.IsQueueing() ? kZDTQueueingTimeoutBackoff : kZDTLossTimeoutBackoff,
         next_seq);
}

}  // namespace backends
}  // namespace znet

//
//    Copyright 2026 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//
// API stability: internal (see the wiki, API Stability)

//
// ZDT's round-trip estimator and congestion window, as two plain state
// machines. Neither owns a socket, a clock or a connection: time arrives as a
// parameter and everything they need about the rest of the transport is passed
// in. That is what makes them testable, which matters here more than usual,
// because congestion control only misbehaves on links that are awkward to
// reproduce.
//

#ifndef ZNET_BACKENDS_ZDT_ZDT_CONGESTION_H_
#define ZNET_BACKENDS_ZDT_ZDT_CONGESTION_H_

#include "znet/backends/zdt/zdt_wire.h"
#include "znet/compat.h"
#include "znet/compat.h"
#include "znet/options.h"

#include <chrono>

namespace znet {
namespace backends {

/**
 * @brief Jacobson/Karels smoothed round trip for the retransmit timeout, and
 *        the two windowed minimums the congestion signal is read from: the
 *        floor, re-probed every ten seconds, and the smallest sample of the
 *        last two round trips, which is what gets compared against it.
 */
class ZDTRttEstimator {
 public:
  using TimePoint = std::chrono::steady_clock::time_point;

  /** @brief Seeds the retransmit timeout before any sample has arrived. */
  void Reset(std::chrono::milliseconds rto) { rto_ = rto; }

  /**
   * @brief Folds in one round-trip measurement.
   *
   * @param sample  measured round trip; negative samples are ignored.
   * @param now     used to age the windowed minimum.
   * @param rto_min lower clamp on the resulting retransmit timeout.
   * @param rto_max upper clamp.
   */
  void OnSample(std::chrono::steady_clock::duration sample, TimePoint now,
                std::chrono::milliseconds rto_min,
                std::chrono::milliseconds rto_max);

  /**
   * @brief Whether every round trip of the last full window sat far enough
   *        above the floor to read as a queue building. One late ack, held by
   *        a busy endpoint, does not; a standing queue does.
   *
   * Known limit: a purely relative test is wrong at the bottom of the range.
   * At the few microseconds of a loopback round trip the ratio is a margin
   * under a microsecond, so the ordinary cost of keeping data in flight reads
   * as a queue and the window sits low on a path that has none. An absolute
   * margin opens the window there but costs about 30% at 64 B and makes that
   * case bimodal, so it is not simply a better rule.
   *
   * Inline: evaluated per ack and per retransmit scan.
   */
  ZNET_NODISCARD bool IsQueueing() const {
    return has_rtt_ && rtt_min_ms_ > 0.0 && window_min_ms_ > 0.0 &&
           window_min_ms_ > rtt_min_ms_ * kZDTQueueingRttRatio;
  }

  ZNET_NODISCARD bool has_rtt() const { return has_rtt_; }
  ZNET_NODISCARD double srtt_ms() const { return srtt_ms_; }
  ZNET_NODISCARD double rtt_min_ms() const { return rtt_min_ms_; }
  ZNET_NODISCARD std::chrono::milliseconds rto() const { return rto_; }

 private:
  double srtt_ms_ = 0.0;
  double rttvar_ms_ = 0.0;
  // a windowed minimum, not a lifetime one. the whole controller is measured
  // against this, so a stale floor left by a route change would read the new
  // baseline as permanent queueing and pin the window at its lower bound.
  double rtt_min_ms_ = 0.0;
  TimePoint rtt_min_stamp_;
  // the smallest sample of the last full window, which is what IsQueueing()
  // reads, and the one being collected
  double window_min_ms_ = 0.0;
  double collecting_min_ms_ = 0.0;
  TimePoint window_start_;
  bool has_rtt_ = false;
  std::chrono::milliseconds rto_{200};
};

/**
 * @brief The congestion window, in datagrams.
 *
 * The congestion signal is queueing delay rather than loss. Reno-style halving
 * on every drop settles at a window of ~1.2/sqrt(loss), six datagrams at 5%,
 * which collapses throughput on a link that is lossy rather than congested. A
 * full queue raises the round trip; a corrupted radio frame does not.
 *
 * Loss events are grouped into epochs so that one burst costs one reduction
 * rather than one per datagram in it. The epoch is defined against the sender's
 * packet sequence, which is why the send-path counter is passed in.
 */
class ZDTCongestionController {
 public:
  /** @brief Ends the current loss epoch once the peer acknowledges something
   *  sent after it opened. */
  void OnAckArrived(WireSeq peer_ack);

  /**
   * @brief Grows the window for newly acknowledged datagrams, or backs off if
   *        the round trip says a queue is building.
   *
   * @param acked_datagrams how many datagrams this ack retired.
   * @param rtt             the queueing signal.
   * @param next_seq        the sender's next packet sequence, which bounds the
   *                        loss epoch this may open.
   * @param cap             ceiling on the window.
   */
  void OnAcked(int acked_datagrams, const ZDTRttEstimator& rtt,
               WireSeq next_seq, int cap);

  /**
   * @brief A retransmit scan found something timed out.
   *
   * Halves when the round trip says a queue stands; a plain lossy path gets
   * a small reduction, since a timeout there is usually still just loss.
   * Either way it is one reduction per loss epoch, like every other.
   */
  void OnRetransmitTimeout(const ZDTRttEstimator& rtt, WireSeq next_seq);

  /**
   * @brief Datagrams allowed in flight, clamped to `cap` and to a floor that
   *        always permits forward progress.
   *
   * Inline: evaluated per iteration of the flush loop.
   */
  ZNET_NODISCARD int Window(int cap) const {
    const int w = static_cast<int>(cwnd_);
    return w > cap ? cap : w;
  }

  ZNET_NODISCARD double cwnd() const { return cwnd_; }
  ZNET_NODISCARD bool in_loss_recovery() const { return in_loss_recovery_; }

 private:
  // one reduction per loss epoch, whoever reports the event; the epoch ends
  // when the peer acknowledges something sent after it opened. The window
  // never drops below kZDTMinWindow, and slow-starts back to what it had.
  void Reduce(double factor, WireSeq next_seq);

  double cwnd_ = 10.0;     // initial window, TCP's IW10
  double ssthresh_ = 1e9;  // no threshold until the first loss teaches one
  WireSeq loss_recovery_until_ = 0;
  bool in_loss_recovery_ = false;
};

}  // namespace backends
}  // namespace znet


#endif  // ZNET_BACKENDS_ZDT_ZDT_CONGESTION_H_

#pragma once

#include <cstdint>
#include <vector>
#include <deque>
#include "protocol.h"
#include "logger.h"

namespace PudicaAlgorithm
{
  struct PudicaConfig
  {
    double B_MIN = 0.2;
    double B_MAX = 50.0;
    double L_SEC = static_cast<double>(pudica_net::INTERVAL / 1e6); // frame interval, ~35fps (INTERVAL = 28571us)
    double GAMMA_P = 1.25;                                          // (sec:4.1) pacing
    double ALPHA = 0.85;                                            // (sec:4.1) threshold for MI vs AI-MD
    double GAMMA_MI = 0.3;                                          // (sec:4.1) discounting coefficient for MI
    double GAMMA_MD = 0.05;                                         // (sec:4.1) MD param for AI-MD
    double A_MIN = -1.0;                                            // (sec:4.2) lower bound capping A
    double A_MAX = GAMMA_MD;                                        // (sec:4.2) dynamic upper bound capping A
    double ZETA = 0.15;                                             // (sec:4.3) temporary fallback fraction
    double DRAIN_WIN = 0.200;                                       // (sec:4.3) queue-drain window (secs)
    uint64_t NEXT_DELAY_THRESH = 2 * pudica_net::INTERVAL;          // (sec:4.3) next delay allowed beyond the base RTT
    uint64_t TIMEOUT = 10 * pudica_net::INTERVAL;                   // time to wait for a frame to complete processing
  };

  enum class State
  {
    STEADY,
    FALLBACK,
    CONGESTED_WAIT,
    DRAINING
  };

  struct Sample  // one frame sample for the history window
  {
    double bur;  // corrected BUR of the kth frame
    double rate; // bitrate of the kth frame (Mbps)
    uint64_t ts; // timestamp recorded for history clearing
  };

  struct FrameAck
  {
    uint32_t fid;               // frame id
    double D;                   // one-way frame delay (sec)
    double Dmin;                // running minimum one way delay (sec)
    std::vector<double> probes; // (sec:4.1) computed probe delays
    double recv_rate;           // frame receiving rate (Mbps)
    double in_bytes;            // total bytes across all in-flight frames
    uint64_t now_microsecs;     // current time (microsecs, for periodic resets)
    uint32_t n_inflight;
    double sent_bitrate = 0.0;
    bool lossy = false;         // frame needed FEC or retransmission
    // LADR sec 3.2: payload bytes of packets that arrived on their first attempt,
    // and the receiver-clock span (us) between this frame's first and last such arrival.
    double frame_mbps = 0.0;    // actual encoded payload bitrate of this frame
    double ack_bytes = 0.0;
    uint64_t ack_first_us = 0, ack_last_us = 0;
    uint32_t epoch = 0;         // Controller::encoding_epoch() when this frame was encoded
    uint64_t wall_us = 0;       // wall clock (us) for flow-synchronized tau resets; 0 = use now_microsecs
    double base_rtt = 0.0;      // windowed minimum RTT (sec); 0 = unknown
  };

  double raw_BUR(double D_sec, double Dmin_sec, const PudicaConfig &cfg);
  double pacing_multiplier(double bur, const PudicaConfig &cfg);
  double corrected_BUR(double raw_BUR, const std::vector<double> &probe_delays, const PudicaConfig &cfg);
  double smoothed_BUR(const std::deque<Sample> &history, double current_rate);
  double next_bitrate(double current_rate, double bur_tilde, uint64_t frames, const PudicaConfig &cfg);

  // this struct will be the return type of Controller::on_frame_acked()
  struct control_output
  {
    bool valid;
    double bitrate;
    double pacing;
    double bur;
  };

  // Snapshot of controller internal state for inspection
  struct StateSnapshot
  {
    double current_bitrate;
    double current_pacing;
    double last_bur;
    State current_state;
    double saved_rate;
    uint32_t frames_up;
    uint32_t adj_epoch;
    size_t history_size;
  };

  class Controller
  {
  private:
    PudicaConfig cfg;

    std::deque<Sample> history;

    double current_bitrate;
    double current_pacing;
    double last_bur = 0.0;

    State current_state = State::STEADY;
    double saved_rate = 0.0;

    // MI / AI-MD scheduling
    uint64_t reset_slot = UINT64_MAX; // 5 s wall-clock slot of the last periodic tau reset
    uint32_t frames_up = 0;  // tau: frames since last reset / congestion
    // Every rate decision opens a new epoch; the next MI/AI-MD step waits for
    // feedback from a frame encoded in it (sec 4.2: postpone until feedback).
    uint32_t rate_epoch = 0, adj_epoch = 0;
    double onset_recv_sum = 0.0; // receive rates since the current congestion onset
    uint32_t onset_recv_n = 0;
    uint64_t last_loss_backoff = 0;

    // Shallow-buffer CUBIC-like rate ceiling
    double cubic_rate;               // ceiling imposed by shallow-buffer detection
    bool shallow_congestion = false; // currently in shallow-buffer mode
    uint64_t shallow_entered_at = 0; // timestamp when shallow mode started
    uint32_t congested_samples = 0, last_sample_id = 0;
    struct LossSample { double queue, frame_mbps, ack_bytes; uint64_t ack_first_us, ack_last_us; };
    std::deque<LossSample> loss_samples;
    bool fallback_pending = false;
    double cubic_B_agg = 0.0;
    double cubic_B_safe = 0.0;

    // Reactive rate limit

    Logger *lg_ = nullptr;

  public:
    Controller(const PudicaConfig &config = PudicaConfig())
        : cfg(config), current_bitrate(config.B_MIN), current_pacing(config.GAMMA_P), cubic_rate(config.B_MAX) {}

    double get_bitrate() const { return current_bitrate; }
    double get_pacing() const { return current_pacing; }
    double get_b_min() const { return cfg.B_MIN; }
    double get_b_max() const { return cfg.B_MAX; }
    const PudicaConfig &get_config() const { return cfg; }

    void set_logger(Logger *l) { lg_ = l; }
    StateSnapshot get_state_snapshot() const;
    void clear_history() { history.clear(); }
    void check_shallow_congestion(const FrameAck &fa, double bur_corr);
    double get_final_bitrate() const;
    double take_encoding_bitrate();
    uint32_t encoding_epoch() const { return rate_epoch; }
    void on_retrans_loss_detected();
    void force_drain();
    control_output on_frame_acked(const FrameAck &fa);
    // Halve once per loss event: expirations within min_gap_us of the last backoff are ignored.
    void on_frame_loss(uint64_t now_us = 0, uint64_t min_gap_us = 0);
    // age: time since the oldest undelivered frame started sending; base_rtt: windowed min RTT (us)
    control_output on_inflight_age(uint64_t age, uint64_t base_rtt = 0);
  };
}
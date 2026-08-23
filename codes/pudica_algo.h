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
    uint64_t NEXT_DELAY_THRESH = 2 * pudica_net::INTERVAL;          // after 2 frame duration, ignore the oldest in-flight frame
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
    uint32_t n_inflight;        // number of currently in-flight frames
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
    uint32_t adj_after;
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
    uint64_t last_reset = 0; // timestamp of last frames_up reset (microsecs)
    uint32_t frames_up = 0;  // frames since last reset / congestion
    uint32_t adj_after = 0;  // hold off MI/AI-MD until fid > adj_after

    // Shallow-buffer CUBIC-like rate ceiling
    double cubic_rate;               // ceiling imposed by shallow-buffer detection
    bool shallow_congestion = false; // currently in shallow-buffer mode
    uint64_t shallow_entered_at = 0; // timestamp when shallow mode started
    bool loss_triggered = false;     // set by on_frame_loss()
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
    control_output on_retrans_loss_detected();
    void force_drain();
    control_output on_frame_acked(const FrameAck &fa);
    void on_frame_loss();
    control_output on_inflight_age(uint64_t age);
  };
}
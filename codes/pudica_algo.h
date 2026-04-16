#pragma once

#include <cstdint>
#include <optional>
#include <vector>
#include <deque>

namespace PudicaAlgorithm
{
  constexpr double L_SEC = 16666.0 / 1e6;                                          // 60fps
  constexpr double GAMMA_P = 1.25;                                                 // (sec:4.1) pacing
  constexpr double ALPHA = 0.85;                                                   // (sec:4.1) threshold for MI vs AI-MD
  constexpr double GAMMA_MI = 0.3;                                                 // (sec:4.1) discounting coefficient for MI
  constexpr double GAMMA_MD = 0.05;                                                // (sec:4.1) MD param for AI-MD
  constexpr double B_MAX = 50.0;                                                   // (sec:4.1) maximum bitrate in Mbps
  constexpr double B_MIN = 0.2;                                                    // (sec:4.1) minimum bitrate in Mbps --- changed it from 1.0 to 0.2 for mahimahi inbuilt traces
  constexpr double A_MIN = -1.0;                                                   // (sec:4.2) lower bound capping A
  constexpr double A_MAX = GAMMA_MD;                                               // (sec:4.2) dynamic upper bound capping A; A = min(A, A_MAX*current_rate)
  constexpr double ZETA = 0.15;                                                    // (sec:4.3) temporary fallback fraction
  constexpr double DRAIN_WIN = 0.200;                                              // (sec:4.3) queue-drain window (secs)
  constexpr uint64_t NEXT_DELAY_THRESH = static_cast<uint64_t>(2.0 * L_SEC * 1e6); // after 2 frame duration, ignore the oldest in-flight frame

  struct Sample
  {
    double bur;  // corrected BUR of the kth frame
    double rate; // bitrate of the kth frame in Mbps
    uint64_t ts; // timestamp recorded --> for history clearing
  };

  struct FrameAck
  {
    uint32_t fid;               // frame id
    double D;                   // one-way frame delay (sec)
    double Dmin;                // running minimum one way delay (sec)
    std::vector<double> probes; // (sec:4.1) computed probe delays
    double recv_rate;           // frame receiving rate (Mbps)
    double in_bytes;            // total bytes across all in-flight frames
    uint32_t n_inflight;        // number of currently in-flight frames
    uint64_t now_microsecs;     // current time (microsecs, for periodic resets)
  };

  double raw_BUR(double D_sec, double Dmin_sec);
  double pacing_multiplier(double bur);
  double corrected_BUR(double raw_BUR, const std::vector<double> &probe_delays);
  double smoothed_BUR(const std::deque<Sample> &history, double current_rate);
  double next_bitrate(double current_rate, double bur_tilde, uint64_t frames);

  // this struct will be the return type of Controller::on_frame_acked()
  // returns the output (updated bitrate and pacing) after processing an acknowledged frame
  struct control_output
  {
    double bitrate;
    double pacing;
    double bur;
  };

  // this will orchestrate the sender calculation/management and run the entire pudica algorithm
  class Controller
  {
  private:
    std::deque<Sample> history;

    double current_bitrate = B_MIN;
    double current_pacing = GAMMA_P;
    double last_bur = 0.0;

    // congestion or draining
    int congested_frames = 0;
    bool draining = false;

    // bitrate fallback for one frame only
    bool restore_next = false; // revert on next on_frame_acked call
    double saved_rate = 0.0;   // rate before fallback was applied [0.0 means no fallback is pending]

    // MI / AI-MD scheduling
    uint64_t last_reset = 0; // µs timestamp of last frames_up reset
    uint32_t frames_up = 0;  // frames since last reset / congestion
    uint32_t adj_after = 0;  // hold off MI/AI-MD until fid > adj_after

  public:
    Controller() = default;
    double get_bitrate() const { return current_bitrate; }
    double get_pacing() const { return current_pacing; }

    // implement the entire logic and all the calculations in pudica_algo.cc
    // (return bitrate, pacing)
    control_output on_frame_acked(const FrameAck &fa);

    // returns the bitrate, pacing only when fallback criteria is triggered
    std::optional<control_output> on_inflight_age(uint64_t age); // age is in microsecs
  };
}
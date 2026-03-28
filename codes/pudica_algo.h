#pragma once

#include<vector>
#include<deque>
namespace PudicaAlgorithm
{
  constexpr double L_SEC = 16666.0 / 1'000'000.0; 
  constexpr double GAMMA_P = 1.25;  // pacing
  constexpr double ALPHA = 0.85;    // threshold for MI vs AI-MD
  constexpr double GAMMA_MI = 0.3;  // discounting coefficient for MI
  constexpr double GAMMA_MD = 0.05; // MD param for AI-MD
  constexpr double B_MAX = 50.0;    // maximum bitrate in Mbps
  constexpr double A_MAX = 0.20;    // A_max = A_MAX * B (section 4.2 for bounding A)

  struct HistorySample {
    double R_k; // corrected BUR of the kth frame
    double B_k; // bitrate of the kth frame in Mbps
  };

  double raw_BUR(double D_sec, double D_min_sec);
  double pacing_multiplier(double R);
  double corrected_BUR(double raw_BUR, const std::vector<double>& probe_delays_T);
  double smoothed_BUR(const std::deque<HistorySample> &history, double current_B);
  double calculate_next_bitrate(double current_B, double R_tilde, double frames);
}
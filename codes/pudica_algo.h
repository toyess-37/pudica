#pragma once

#include <vector>
#include <deque>
namespace PudicaAlgorithm
{
  constexpr double L_SEC = 16666.0 / 1'000'000.0;
  constexpr double GAMMA_P = 1.25;  // pacing
  constexpr double ALPHA = 0.85;    // threshold for MI vs AI-MD
  constexpr double GAMMA_MI = 0.3;  // discounting coefficient for MI
  constexpr double GAMMA_MD = 0.05; // MD param for AI-MD
  constexpr double B_MAX = 50.0;    // maximum bitrate in Mbps
  constexpr double B_MIN = 1.0;     // minimum bitrate in Mbps
  constexpr double A_MIN = -1.0;    // lower bound capping A (according to section 4.2 of paper)
  constexpr double A_MAX = 1.0;     // A_max = A_MAX * current_B; A_new = min(A_max, A_new)

  struct HistorySample
  {
    double R_k; // corrected BUR of the kth frame
    double B_k; // bitrate of the kth frame in Mbps
  };

  double raw_BUR(double D_sec, double D_min_sec);
  double pacing_multiplier(double R);
  double corrected_BUR(double raw_BUR, const std::vector<double> &probe_delays_T);
  double smoothed_BUR(const std::deque<HistorySample> &history, double current_B);
  double calculate_next_bitrate(double current_B, double R_tilde, double frames);
}
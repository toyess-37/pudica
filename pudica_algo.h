#pragma once

#include<vector>
namespace PudicaAlgorithm
{
  constexpr double L_SEC = 16.67 / 1000.0; 
  constexpr double GAMMA_P = 1.25;  // pacing
  constexpr double ALPHA = 0.85;    // threshold for MI vs AI-MD
  constexpr double GAMMA_MI = 0.3;  // discounting coefficient for MI
  constexpr double GAMMA_MD = 0.05; // MD param for AI-MD
  constexpr double B_MAX = 50.0;    // maximum bitrate in Mbps

  double raw_BUR(double D_sec, double D_min_sec);
  double pacing_multiplier(double R);
  double corrected_BUR(double raw_BUR, const std::vector<double>& probe_delays_T);
}
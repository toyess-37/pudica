#include "pudica_algo.h"
#include <algorithm>
#include <iostream>
#include <cmath>
#include <deque>

namespace PudicaAlgorithm
{
  double raw_BUR(double D_sec, double D_min_sec)
  {
    return (D_sec - D_min_sec) / L_SEC;
  }

  // p = gamma_p / min(R, 1)
  double pacing_multiplier(double R)
  {
    return GAMMA_P / std::min(R, 1.0);
  }

  // R_corrected = R + sum(T_i/L)
  double corrected_BUR(double raw_BUR, const std::vector<double> &probe_delays_T)
  {
    double R_corrected = raw_BUR;
    for (auto T_i : probe_delays_T)
      R_corrected += (T_i / L_SEC);
    return R_corrected;
  }

  double smoothed_BUR(const std::deque<HistorySample> &history, double current_B)
  {
    if (history.empty())
      return 0.0;

    double weighted_sum = 0.0;
    double weight_total = 0.0;

    // k represents recency (1 is oldest, N is newest)
    for (size_t i = 0; i < history.size(); i++)
    {
      double k = static_cast<double>(i + 1);
      const auto &sample = history[i];

      // weights
      double w_I = std::min(sample.R_k + 1.0, 2.0);
      double w_II = std::min(sample.B_k + 10.0, 50.0);
      double w_III = k + 20.0;

      double w_k = w_I * w_II * w_III;

      double rectified_R_k = sample.R_k * (current_B / sample.B_k);

      weighted_sum += w_k * rectified_R_k;
      weight_total += w_k;
    }

    return weighted_sum / weight_total;
  }

  // new bitrate using MI or AI-MD
  double calculate_next_bitrate(double current_B, double R_tilde, double frames)
  {
    double new_B = current_B;

    /*
      this does not take into consideration the "postponed until feedback" of the paper.
      it is handled inside the listener() thread of sender.cc
    */
    if (R_tilde <= ALPHA)
    {
      // MI: xi = gamma * (( (1+alpha)/2 - R_tilde ) / R_tilde)
      double xi = GAMMA_MI * (((ALPHA + 1.0) / 2.0 - R_tilde) / R_tilde);
      new_B = current_B * (1.0 + xi);
    }
    else if (R_tilde <= 1.0)
    {
      double safe_tau = std::min(frames / 60.0, 5.0); // simultaneous AI-MD
      // adaptive AI step (I)
      double I = (B_MAX + (std::pow(2.0, safe_tau) / std::max(std::log(current_B), B_MIN))) * (GAMMA_MD / 2.0);
      double A = I - (GAMMA_MD * current_B); // net step A

      // bounds on A (according to section 4.2 of the paper)
      double A_min = 0.0;
      double A_max = A_MAX * current_B;
      A = std::min(std::max(A_min, A), A_max);

      new_B = current_B + A;
    } // steps for R > 1.0 will be addressed in the sender loop

    return std::min(std::max(new_B, 1.0), static_cast<double>(B_MAX));
  }
}
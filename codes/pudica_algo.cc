#include <algorithm>
#include <iostream>
#include <cmath>
#include <deque>
#include "pudica_algo.h"

namespace PudicaAlgorithm
{
  double raw_BUR(double D_sec, double Dmin_sec)
  {
    return (D_sec - Dmin_sec) / L_SEC;
  }

  // rho = gamma_p / min(bur, 1)
  double pacing_multiplier(double bur)
  {
    return GAMMA_P / std::min(bur, 1.0);
  }

  // (sec:4.2) bur_corrected = raw_bur + sum(T_i/L)
  double corrected_BUR(double raw_BUR, const std::vector<double> &probe_delays)
  {
    double R_corrected = raw_BUR;
    for (auto T_i : probe_delays)
      R_corrected += (T_i / L_SEC);
    return R_corrected;
  }

  double smoothed_BUR(const std::deque<Sample> &history, double current_rate)
  {
    if (history.empty())
      return 0.0;

    double sum = 0.0;
    double weights = 0.0;

    // k represents recency (1 is oldest, N is newest)
    for (size_t i = 0; i < history.size(); i++)
    {
      double k = static_cast<double>(i + 1);
      const auto &sample = history[i];

      // weights
      double w_I = std::min(sample.bur + 1.0, 2.0);
      double w_II = std::min(sample.rate + 10.0, 50.0);
      double w_III = k + 20.0;

      double w_k = w_I * w_II * w_III;

      double rectified_bur_k = sample.bur * (current_rate / sample.rate);

      sum += w_k * rectified_bur_k;
      weights += w_k;
    }

    return sum / weights;
  }

  // new bitrate using MI or AI-MD
  // bur > 1 is handled in the controller (next function)
  double next_bitrate(double current_rate, double bur_tilde, uint64_t frames)
  {
    double new_rate = current_rate;

    if (bur_tilde <= ALPHA)
    {                                                              // MI
      double safe_r = std::max(bur_tilde, 0.01);                   // hardcoded lower bound of 0.01 for no divide by near 0 overshoots.
      double xi = GAMMA_MI * ((1.0 + ALPHA) / (2.0 * safe_r) - 1); // MI: xi = gamma_mi * (( (1+alpha)/2 - bur_tilde ) / bur_tilde)
      new_rate = current_rate * (1.0 + xi);
    }
    else if (bur_tilde <= 1.0)
    { // simultaneous AI-MD
      double tau = std::min(frames / 60.0, 5.0);
      double I = (B_MAX + (std::pow(2.0, tau) / std::max(std::log(current_rate), B_MIN))) * (GAMMA_MD / 2.0);
      double A = I - (GAMMA_MD * current_rate); // net step A
      // bounds on A (according to section 4.2 of the paper)
      A = std::min(std::max(A_MIN, A), A_MAX);

      new_rate = current_rate + A;
    }
    return std::min(std::max(new_rate, B_MIN), B_MAX);
  }

  control_output Controller::on_frame_acked(const FrameAck &fa)
  {
    double raw_bur = raw_BUR(fa.D, fa.Dmin);
    double bur_corr = corrected_BUR(raw_bur, fa.probes);
    current_pacing = pacing_multiplier(bur_corr);
    last_bur = bur_corr;

    if (restore_next)
    {
      current_bitrate = saved_rate;
      saved_rate = 0.0;
      restore_next = false;
    }

    history.push_back({bur_corr, current_bitrate});
    if (history.size() > 12)
      history.pop_front(); // reset after 12 frames (200ms on 60fps, but may be sad due to congestion)

    if (bur_corr > 1.0)
    {
      congested_frames++;
      frames_up = 0;

      if (draining)
      {
        // already draining. eat something; do nothing.
      }
      else if (congested_frames >= 3)
      {
        draining = true;
        restore_next = false; // cancel any pending one-frame revert
        saved_rate = 0.0;

        double drain_rate = (8.0 * fa.in_bytes) / (DRAIN_WIN * 1'000'000.0);
        double new_rate = ALPHA * fa.recv_rate - drain_rate;
        current_bitrate = std::max(new_rate, B_MIN);
        adj_after = fa.fid + fa.n_inflight;
      }
      else if (congested_frames == 1)
      {
        if (!restore_next)
        {
          saved_rate = current_bitrate;
          restore_next = true;
        }
        current_bitrate = std::max(current_bitrate * (1.0 - ZETA), B_MIN);
      }
    }
    else
    {
      if (draining)
      {
        // pudica calculates the current receiving_rate
        // and directly restores the bitrate = receiving_rate
        draining = false;
        congested_frames = 0;
        frames_up = 0;
        restore_next = false;
        saved_rate = 0.0;
        current_bitrate = std::min(std::max(fa.recv_rate, B_MIN), B_MAX);
        adj_after = fa.fid + fa.n_inflight;

        return {current_bitrate, current_pacing, last_bur};
      }
      congested_frames = 0;

      // every 5seconds, reset stuff for additive step
      if (last_reset == 0)
        last_reset = fa.now_microsecs;
      uint64_t time = fa.now_microsecs;
      if (time - last_reset >= 5'000'000ULL)
      {
        frames_up = 0;
        last_reset = time;
      }
      frames_up++;

      if (fa.fid > adj_after)
      {
        double smooth_bur = smoothed_BUR(history, current_bitrate);
        current_bitrate = next_bitrate(current_bitrate, smooth_bur, frames_up);
        adj_after = fa.fid + fa.n_inflight;
      }
    }
    return {current_bitrate, current_pacing, last_bur};
  }

  // to monitor the frames that have been sent to the network
  // but have not yet been acknowledged
  // when the delay > 2 frame intervals, do a fallback
  std::optional<control_output> Controller::on_inflight_age(uint64_t age)
  {
    if (age < NEXT_DELAY_THRESH)
      return std::nullopt;
    if (restore_next || draining)
      return std::nullopt;

    // else, one frame fallback
    saved_rate = current_bitrate;
    restore_next = true;
    current_bitrate = std::max(current_bitrate * (1.0 - ZETA), B_MIN);
    return control_output{current_bitrate, current_pacing, last_bur};
  }
}
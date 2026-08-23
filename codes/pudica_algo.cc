#include <algorithm>
#include <cassert>
#include <cmath>
#include <deque>
#include "logger.h"
#include "pudica_algo.h"

// PROBE LOSS BEHAVIOR:
// If fewer than 4 probes arrive, we treat it as frame loss (TIMEOUT).
// This is conservative: we cut bitrate by 50%.

namespace PudicaAlgorithm
{
  double raw_BUR(double D_sec, double Dmin_sec, const PudicaConfig &cfg)
  {
    return (D_sec - Dmin_sec) / cfg.L_SEC;
  }

  double pacing_multiplier(double bur, const PudicaConfig &cfg)
  {
    return cfg.GAMMA_P / std::min(std::max(bur, 0.01), 1.0);
  }

  double corrected_BUR(double raw_BUR, const std::vector<double> &probe_delays, const PudicaConfig &cfg)
  {
    double R_corrected = raw_BUR;
    for (auto T_i : probe_delays)
      R_corrected += (T_i / cfg.L_SEC);
    return R_corrected;
  }

  // smoothed BUR with weighted history
  // weight formula based on authors' reference implementation, not in the public paper text
  double smoothed_BUR(const std::deque<Sample> &history, double current_rate)
  {
    if (history.empty())
      return 0.0;

    double sum = 0.0;
    double weights = 0.0;

    for (size_t i = 0; i < history.size(); i++)
    {
      double k = (double)(i + 1);
      const auto &sample = history[i];

      double w_I = std::min(sample.bur + 1.0, 2.0);
      double w_II = std::min(sample.rate + 10.0, 50.0);
      double w_III = k + 20.0;

      double w_k = w_I * w_II * w_III;
      double safe_sample_rate = std::max(sample.rate, 0.01);
      double rectified_bur_k = sample.bur * (current_rate / safe_sample_rate);

      sum += w_k * rectified_bur_k;
      weights += w_k;
    }

    return sum / weights;
  }

  double next_bitrate(double current_rate, double bur_tilde, uint64_t frames, const PudicaConfig &cfg)
  {
    double new_rate = current_rate;

    if (bur_tilde <= cfg.ALPHA)
    {
      double safe_r = std::max(bur_tilde, 0.01);
      double xi = cfg.GAMMA_MI * ((1.0 + cfg.ALPHA) / (2.0 * safe_r) - 1);
      new_rate = current_rate * (1.0 + xi);
    }
    else if (bur_tilde <= 1.0)
    {
      double tau = std::min(frames / 60.0, 5.0);
      double I = (cfg.B_MAX + (std::pow(2.0, tau) / std::max(std::log(current_rate), cfg.B_MIN))) * (cfg.GAMMA_MD / 2.0);
      double A = I - (cfg.GAMMA_MD * current_rate);
      double A_max = cfg.A_MAX * current_rate;
      A = std::min(std::max(cfg.A_MIN, A), A_max);

      new_rate = current_rate + A;
    }
    return std::min(std::max(new_rate, cfg.B_MIN), cfg.B_MAX);
  }

  control_output Controller::on_frame_acked(const FrameAck &fa)
  {
    double raw_bur = raw_BUR(fa.D, fa.Dmin, cfg);
    double bur_corr = corrected_BUR(raw_bur, fa.probes, cfg);
    check_shallow_congestion(fa, bur_corr);
    current_pacing = pacing_multiplier(bur_corr, cfg);
    last_bur = bur_corr;

    if (bur_corr <= 1.0 && (current_state == State::FALLBACK || current_state == State::CONGESTED_WAIT))
    {
      if (lg_)
        lg_->log("FALLBACK_RESTORED", fa.fid, {{"restored_rate", std::to_string(saved_rate)}});
      current_bitrate = saved_rate;
      saved_rate = 0.0;
      current_state = State::STEADY;
    }

    history.push_back({bur_corr, current_bitrate, fa.now_microsecs});

    while (!history.empty() && (fa.now_microsecs - history.front().ts) > 200'000ULL)
    {
      history.pop_front();
    }

    if (bur_corr > 1.0)
    {
      frames_up = 0;

      switch (current_state)
      {
      case State::STEADY:
        saved_rate = current_bitrate;
        current_state = State::FALLBACK;
        current_bitrate = std::min(std::max(current_bitrate * (1.0 - cfg.ZETA), cfg.B_MIN), cfg.B_MAX);
        if (lg_)
          lg_->log("FALLBACK_SET", fa.fid, {{"saved_rate", std::to_string(saved_rate)}, {"new_rate", std::to_string(current_bitrate)}, {"trigger", "\"congested_1\""}});
        break;
      case State::FALLBACK:
        current_state = State::CONGESTED_WAIT;
        break;
      case State::CONGESTED_WAIT:
        current_state = State::DRAINING;
        saved_rate = 0.0;
        {
          double drain_rate = (8.0 * fa.in_bytes) / (cfg.DRAIN_WIN * 1e6);
          double new_rate = cfg.ALPHA * fa.recv_rate - drain_rate;
          current_bitrate = std::min(std::max(new_rate, cfg.B_MIN), cfg.B_MAX);
          adj_after = fa.fid + fa.n_inflight;
          if (lg_)
            lg_->log("DRAIN_START", fa.fid, {{"bur", std::to_string(bur_corr)}, {"drain_rate", std::to_string(drain_rate)}, {"new_bitrate", std::to_string(current_bitrate)}});
        }
        break;
      case State::DRAINING:
        // Already in DRAINING, do nothing. Just wait until bur_corr <= 1.0.
        break;
      }
    }
    else
    {
      if (current_state == State::DRAINING)
      {
        current_state = State::STEADY;
        frames_up = 0;
        saved_rate = 0.0;
        current_bitrate = std::min(std::max(fa.recv_rate, cfg.B_MIN), cfg.B_MAX);
        adj_after = fa.fid + fa.n_inflight;
        history.clear();

        if (lg_)
          lg_->log("DRAIN_END", fa.fid, {{"restored_bitrate", std::to_string(current_bitrate)}, {"recv_rate", std::to_string(fa.recv_rate)}});

        if (lg_)
          lg_->log("FRAME_ACKED", fa.fid, {{"bur", std::to_string(bur_corr)}, {"bitrate", std::to_string(get_final_bitrate())}, {"pacing", std::to_string(current_pacing)}, {"delay_ms", std::to_string((fa.D - fa.Dmin) * 1000.0)}, {"recv_rate", std::to_string(fa.recv_rate)}, {"n_inflight", std::to_string(fa.n_inflight)}, {"n_probes", std::to_string(fa.probes.size())}});

        return {true, get_final_bitrate(), current_pacing, last_bur};
      }

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
        current_bitrate = next_bitrate(current_bitrate, smooth_bur, frames_up, cfg);
        adj_after = fa.fid + fa.n_inflight;
      }
    }

    if (lg_)
      lg_->log("FRAME_ACKED", fa.fid, {{"bur", std::to_string(bur_corr)}, {"bitrate", std::to_string(get_final_bitrate())}, {"pacing", std::to_string(current_pacing)}, {"delay_ms", std::to_string((fa.D - fa.Dmin) * 1000.0)}, {"recv_rate", std::to_string(fa.recv_rate)}, {"n_inflight", std::to_string(fa.n_inflight)}, {"n_probes", std::to_string(fa.probes.size())}});

    return {true, get_final_bitrate(), current_pacing, last_bur};
  }

  void Controller::on_frame_loss()
  {
    current_state = State::DRAINING;
    saved_rate = 0.0;
    current_bitrate = std::max(current_bitrate * 0.5, cfg.B_MIN);
    history.clear();
    loss_triggered = true;

    if (lg_)
      lg_->log("FRAME_LOSS", {{"new_bitrate", std::to_string(current_bitrate)}});

    if (lg_)
      lg_->log("DRAIN_START", 0, {{"bur", "0.0"}, {"drain_rate", "0.0"}, {"new_bitrate", std::to_string(current_bitrate)}});
  }

  StateSnapshot Controller::get_state_snapshot() const
  {
    return {current_bitrate, current_pacing, last_bur,
            current_state, saved_rate,
            frames_up, adj_after, history.size()};
  }

  void Controller::force_drain()
  {
    current_state = State::DRAINING;
    saved_rate = 0.0;
  }

  control_output Controller::on_inflight_age(uint64_t age)
  {
    if (age < cfg.NEXT_DELAY_THRESH)
      return {false, 0.0, 0.0, 0.0};
    if (current_state != State::STEADY)
      return {false, 0.0, 0.0, 0.0};

    saved_rate = current_bitrate;
    current_state = State::FALLBACK;
    current_bitrate = std::max(current_bitrate * (1.0 - cfg.ZETA), cfg.B_MIN);
    if (lg_)
      lg_->log("FALLBACK_SET", 0, {{"saved_rate", std::to_string(saved_rate)}, {"new_rate", std::to_string(current_bitrate)}, {"trigger", "\"inflight_age\""}});
    return control_output{true, get_final_bitrate(), current_pacing, last_bur};
  }

  // shallow-buffer detection: cubic-like rate ceiling
  // this is an extension beyond the core NSDI'24 paper, inspired by shallow-buffer research
  void Controller::check_shallow_congestion(const FrameAck &fa, double bur_corr)
  {
    double queuing_delay = fa.D - fa.Dmin;
    bool low_queue = queuing_delay < cfg.L_SEC;
    bool rate_mismatch = fa.recv_rate < current_bitrate * 0.85;

    if (loss_triggered && low_queue && rate_mismatch && !shallow_congestion)
    {
      shallow_congestion = true;
      loss_triggered = false;
      shallow_entered_at = fa.now_microsecs;

      cubic_B_safe = std::max(0.8 * fa.recv_rate, cfg.B_MIN);
      cubic_B_agg = std::max(current_bitrate, cubic_B_safe);
      cubic_rate = cubic_B_safe;

      if (lg_)
        lg_->log("DRAIN_START", fa.fid, {{"bur", std::to_string(bur_corr)}, {"drain_rate", "0.0"}, {"new_bitrate", std::to_string(cubic_rate)}});
    }

    if (shallow_congestion)
    {
      double t_sec = (fa.now_microsecs - shallow_entered_at) / 1e6;
      double B_agg = cubic_B_agg;
      double B_safe = cubic_B_safe;

      double C = 0.05;
      double K = std::cbrt((B_agg - B_safe) / C);
      double candidate = C * pow(t_sec - K, 3) + B_agg;
      cubic_rate = std::min(std::max(candidate, B_safe), cfg.B_MAX);

      if (bur_corr <= cfg.ALPHA && cubic_rate >= current_bitrate * 0.95)
      {
        shallow_congestion = false;
        cubic_rate = cfg.B_MAX;
      }
    }
  }

  double Controller::get_final_bitrate() const
  {
    return std::min(current_bitrate, cubic_rate);
  }

  control_output Controller::on_retrans_loss_detected()
  {
    // feeds the shallow-buffer detector below (sec:3.2 of the LADR paper): packet
    // loss is the congestion signal a delay-based controller alone can't see
    loss_triggered = true;
    if (lg_)
      lg_->log("RETRANS_LOSS", {{"bitrate", std::to_string(current_bitrate)}});
    return control_output{true, get_final_bitrate(), current_pacing, last_bur};
  }
}
#include <algorithm>
#include <cmath>
#include <deque>
#include "logger.h"
#include "pudica_algo.h"


namespace PudicaAlgorithm
{
  double raw_BUR(double D_sec, double Dmin_sec, const PudicaConfig &cfg)
  {
    return std::max(0.0, (D_sec - Dmin_sec) / cfg.L_SEC);
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
  // weight formula from Pudica Appendix B
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
      // Frame counter is dimensionless. Cap the exponent before evaluating it;
      // beyond this point the additive step already reaches its upper bound.
      double tau = std::min<double>(frames, 50.0);
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
    if(fa.fid<=last_sample_id) return {false,get_final_bitrate(),current_pacing,last_bur};
    if(last_sample_id && fa.fid!=last_sample_id+1) {congested_samples=0;loss_samples.clear();}
    last_sample_id=fa.fid;
    double raw_bur = raw_BUR(fa.D, fa.Dmin, cfg);
    double bur_corr = corrected_BUR(raw_bur, fa.probes, cfg);
    check_shallow_congestion(fa, bur_corr);
    current_pacing = pacing_multiplier(bur_corr, cfg);
    last_bur = bur_corr;

    if (bur_corr <= 1.0 && (current_state == State::FALLBACK || current_state == State::CONGESTED_WAIT))
    {
      if (lg_)
        lg_->log("FALLBACK_RESTORED", fa.fid, {{"restored_rate", std::to_string(saved_rate)}});
      if(fallback_pending && saved_rate>0) current_bitrate = saved_rate;
      saved_rate = 0.0;
      fallback_pending = false;
      current_state = State::STEADY;
    }

    history.push_back({bur_corr, fa.frame_mbps>0?fa.frame_mbps:current_bitrate, fa.now_microsecs});

    while (!history.empty() && (fa.now_microsecs - history.front().ts) > 200'000ULL)
    {
      history.pop_front();
    }

    // sec 4.2: tau restarts when the smoothed BUR exceeds one, and every five
    // seconds on wall-clock boundaries so that competing flows reset together.
    const double smooth_bur = smoothed_BUR(history, current_bitrate);
    if (smooth_bur > 1.0)
      frames_up = 0;
    const uint64_t slot = (fa.wall_us ? fa.wall_us : fa.now_microsecs) / 5'000'000ULL;
    if (slot != reset_slot)
    {
      frames_up = 0;
      reset_slot = slot;
    }

    if (bur_corr > 1.0)
    {
      ++congested_samples;
      if(congested_samples==1){onset_recv_sum=0;onset_recv_n=0;}
      onset_recv_sum+=fa.recv_rate;++onset_recv_n;
      if(congested_samples==1 && current_state==State::STEADY) {
        saved_rate=current_bitrate;fallback_pending=true;current_state=State::FALLBACK;
        current_bitrate=std::clamp(current_bitrate*(1-cfg.ZETA),cfg.B_MIN,cfg.B_MAX);
        if(lg_)lg_->log("FALLBACK_SET",fa.fid,{{"saved_rate",std::to_string(saved_rate)},
          {"new_rate",std::to_string(current_bitrate)},{"trigger",Logger::quote("congested_frame")}});
      } else if(congested_samples==2 && current_state!=State::DRAINING) {
        current_state=State::CONGESTED_WAIT;
      } else if(congested_samples>=3 && current_state!=State::DRAINING) {
        current_state=State::DRAINING;saved_rate=0;fallback_pending=false;
        // Eq. 11: receiving_rate averaged from the onset of congestion.
        const double recv_rate=onset_recv_sum/onset_recv_n;
        // Only bytes beyond the path's bandwidth-delay product sit in the
        // bottleneck queue; the rest are in flight on an empty pipe and would
        // be "drained" anyway. Counting them collapses the rate on long paths.
        const double pipe_bytes=recv_rate*1e6/8*fa.base_rtt;
        const double queued_bytes=std::max(0.0,fa.in_bytes-pipe_bytes);
        double drain_rate=8*queued_bytes/(cfg.DRAIN_WIN*1e6);
        current_bitrate=std::clamp(cfg.ALPHA*recv_rate-drain_rate,cfg.B_MIN,cfg.B_MAX);
        adj_epoch=++rate_epoch;
        if(lg_)lg_->log("DRAIN_START",fa.fid,{{"bur",std::to_string(bur_corr)},
          {"drain_rate",std::to_string(drain_rate)},{"recv_rate",std::to_string(recv_rate)},
          {"queued_bytes",std::to_string(queued_bytes)},{"inflight_bytes",std::to_string(fa.in_bytes)},
          {"new_bitrate",std::to_string(current_bitrate)}});
      }
    }
    else
    {
      congested_samples=0;
      if (current_state == State::DRAINING)
      {
        current_state = State::STEADY;
        frames_up = 0;
        saved_rate = 0.0;
        current_bitrate = std::min(std::max(fa.recv_rate, cfg.B_MIN), cfg.B_MAX);
        adj_epoch = ++rate_epoch;
        history.clear();

        if (lg_)
          lg_->log("DRAIN_END", fa.fid, {{"restored_bitrate", std::to_string(current_bitrate)}, {"recv_rate", std::to_string(fa.recv_rate)}});
      }
      else
      {
        frames_up++;
        if (fa.epoch >= adj_epoch)
        {
          current_bitrate = next_bitrate(current_bitrate, smooth_bur, frames_up, cfg);
          adj_epoch = ++rate_epoch;
        }
      }
    }

    if (lg_)
      lg_->log("CONTROL_SAMPLE", fa.fid, {{"bur", std::to_string(bur_corr)}, {"bitrate", std::to_string(get_final_bitrate())}, {"sample_rate", std::to_string(fa.sent_bitrate)}, {"pacing", std::to_string(current_pacing)}, {"delay_ms", std::to_string((fa.D - fa.Dmin) * 1000.0)}, {"recv_rate", std::to_string(fa.recv_rate)}, {"n_inflight", std::to_string(fa.n_inflight)}, {"n_probes", std::to_string(fa.probes.size())}});

    return {true, get_final_bitrate(), current_pacing, last_bur};
  }

  void Controller::on_frame_loss(uint64_t now_us, uint64_t min_gap_us)
  {
    if (now_us && last_loss_backoff && now_us - last_loss_backoff < min_gap_us)
      return;
    last_loss_backoff = now_us;
    current_state = State::DRAINING;
    saved_rate = 0.0;
    fallback_pending = false;
    congested_samples = 0;
    frames_up = 0;
    current_bitrate = std::max(current_bitrate * 0.5, cfg.B_MIN);
    adj_epoch = ++rate_epoch;
    history.clear();
    loss_samples.clear();

    if (lg_)
      lg_->log("LOSS_BACKOFF", {{"new_bitrate", std::to_string(current_bitrate)}});

    if (lg_)
      lg_->log("DRAIN_START", 0, {{"bur", "0.0"}, {"drain_rate", "0.0"}, {"new_bitrate", std::to_string(current_bitrate)}});
  }

  StateSnapshot Controller::get_state_snapshot() const
  {
    return {current_bitrate, current_pacing, last_bur,
            current_state, saved_rate,
            frames_up, adj_epoch, history.size()};
  }

  void Controller::force_drain()
  {
    current_state = State::DRAINING;
    saved_rate = 0.0;
    fallback_pending = false;
    congested_samples = 0;
  }

  control_output Controller::on_inflight_age(uint64_t age, uint64_t base_rtt)
  {
    // A frame is only late once it exceeds the path RTT plus the allowance;
    // a fixed threshold fires on every frame when RTT > 2 frame intervals.
    if (age < base_rtt + cfg.NEXT_DELAY_THRESH)
      return {false, 0.0, 0.0, 0.0};
    if (current_state != State::STEADY)
      return {false, 0.0, 0.0, 0.0};

    saved_rate = current_bitrate;
    fallback_pending = true;
    current_state = State::FALLBACK;
    current_bitrate = std::max(current_bitrate * (1.0 - cfg.ZETA), cfg.B_MIN);
    if (lg_)
      lg_->log("FALLBACK_SET", 0, {{"saved_rate", std::to_string(saved_rate)}, {"new_rate", std::to_string(current_bitrate)}, {"trigger", "\"inflight_age\""}});
    return control_output{true, get_final_bitrate(), current_pacing, last_bur};
  }

  // A loss-based ceiling inspired by LADR; the transport uses RTT timers,
  // so this project does not implement the paper's RACK-TLP recovery.
  void Controller::check_shallow_congestion(const FrameAck &fa, double bur_corr)
  {
    double queuing_delay = fa.D - fa.Dmin;
    if(!fa.lossy)loss_samples.clear();
    else {
      loss_samples.push_back({queuing_delay,fa.frame_mbps>0?fa.frame_mbps:current_bitrate,
                              fa.ack_bytes,fa.ack_first_us,fa.ack_last_us});
      if(loss_samples.size()>3)loss_samples.pop_front();
    }
    // LADR sec 3.2: compare the frames' average bitrate with the rate at which the
    // network ACKed their non-lost packets. LADR spans first-to-last ACK across
    // the n frames; with per-frame pacing that span includes the idle agnostic
    // periods and skipped frames, pulling the rate down to roughly the bitrate
    // itself. Summing each frame's own ACK span keeps a clear margin: a paced,
    // uncongested frame is ACKed at about rho x bitrate, a congested one at the
    // bottleneck rate. A long-window average would just echo our sending rate.
    double average_rate=0,average_ack=0,acked=0,span=0;
    for(const auto& sample:loss_samples){
      average_rate+=sample.frame_mbps;
      if(sample.ack_last_us>sample.ack_first_us){
        acked+=sample.ack_bytes;span+=static_cast<double>(sample.ack_last_us-sample.ack_first_us);
      }
    }
    if(!loss_samples.empty()) average_rate/=static_cast<double>(loss_samples.size());
    // No measurable arrivals (e.g. all packets lost): LADR treats the ACK rate as zero.
    if(span>0) average_ack=8.0*acked/span;
    if(loss_samples.size()==3 && loss_samples.front().queue<cfg.L_SEC &&
       average_ack<average_rate)
    {
      shallow_congestion = true;

      shallow_entered_at = fa.now_microsecs;
      loss_samples.clear();

      cubic_B_safe = std::max(0.2 * average_ack, cfg.B_MIN);
      cubic_B_agg = std::max(average_rate, cubic_B_safe);
      cubic_rate = cubic_B_safe;

      if (lg_)
        lg_->log("LOSS_CEILING_SET", fa.fid, {{"bur", std::to_string(bur_corr)}, {"new_bitrate", std::to_string(cubic_rate)},
          {"ack_mbps", std::to_string(average_ack)}, {"frame_mbps", std::to_string(average_rate)}});
    }

    if (shallow_congestion)
    {
      double t_sec = (fa.now_microsecs - shallow_entered_at) / 1e6;
      double B_agg = cubic_B_agg;
      double B_safe = cubic_B_safe;

      double C = 1.0;
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

  double Controller::take_encoding_bitrate()
  {
    const double target=get_final_bitrate();
    if(fallback_pending) {
      fallback_pending=false;
      current_bitrate=std::max(saved_rate,cfg.B_MIN);
      saved_rate=0;
      if(lg_) lg_->log("FALLBACK_RESTORED", {{"restored_rate",std::to_string(current_bitrate)}, {"trigger",Logger::quote("next_encoding")}});
    }
    return target;
  }

  double Controller::get_final_bitrate() const
  {
    return std::min(current_bitrate, cubic_rate);
  }

  // LADR sec 3.3: a lost retransmission makes the reactive rate limit applicable
  // for the session; the limit itself lives in the sender's pacer.
  void Controller::on_retrans_loss_detected()
  {
    if (lg_)
      lg_->log("RETRANS_LOSS", {{"bitrate", std::to_string(current_bitrate)}});
  }
}
#include <iostream>
#include <vector>
#include <random>
#include <cassert>
#include <cmath>
#include "pudica_algo.h"

using namespace PudicaAlgorithm;

void test_bitrate_bounded()
{
  Controller ctrl;
  std::mt19937 gen(42);
  std::uniform_real_distribution<double> dist_delay(0.001, 0.050);
  std::uniform_real_distribution<double> dist_recv_rate(1.0, 100.0);
  std::uniform_int_distribution<uint32_t> dist_probes(0, 4);

  uint64_t now_us = 0;
  for (int i = 0; i < 1000; ++i)
  {
    now_us += pudica_net::INTERVAL;
    FrameAck fa{};
    fa.fid = i + 1;
    fa.Dmin = 0.010;
    fa.D = fa.Dmin + dist_delay(gen); // random delay
    fa.probes = std::vector<double>(dist_probes(gen), 0.0);
    fa.recv_rate = dist_recv_rate(gen);
    fa.in_bytes = 1500 * 10;
    fa.now_microsecs = now_us;
    fa.n_inflight = 5;

    auto out = ctrl.on_frame_acked(fa);
    assert(out.bitrate >= ctrl.get_b_min() && out.bitrate <= ctrl.get_b_max());
  }
  std::cout << "[PASS] test_bitrate_bounded\n";
}

void test_restore_next_single_shot()
{
  Controller ctrl;

  // First, trigger a fallback (e.g. by setting next delay large)
  auto fallback = ctrl.on_inflight_age(ctrl.get_config().NEXT_DELAY_THRESH + 100);
  assert(fallback.valid);

  auto ss1 = ctrl.get_state_snapshot();
  assert(ss1.current_state == State::FALLBACK);

  // Now send an ack with bur <= 1.0
  FrameAck fa{};
  fa.fid = 1;
  fa.Dmin = 0.010;
  fa.D = 0.010; // D == Dmin -> raw_bur == 0
  fa.probes = {0.0, 0.0, 0.0, 0.0};
  fa.recv_rate = 20.0;
  fa.in_bytes = 1500;
  fa.now_microsecs = 100000;
  fa.n_inflight = 1;

  ctrl.on_frame_acked(fa);
  auto ss2 = ctrl.get_state_snapshot();
  assert(ss2.current_state == State::STEADY);

  // Send another ack with bur <= 1.0
  fa.fid = 2;
  fa.now_microsecs = 200000;
  ctrl.on_frame_acked(fa);
  auto ss3 = ctrl.get_state_snapshot();
  assert(ss3.current_state == State::STEADY);

  std::cout << "[PASS] test_restore_next_single_shot\n";
}

void test_loss_triggers_drain()
{
  Controller ctrl;
  ctrl.on_frame_loss();
  auto ss = ctrl.get_state_snapshot();
  assert(ss.current_state == State::DRAINING);
  std::cout << "[PASS] test_loss_triggers_drain\n";
}

void test_drain_uses_recv_rate()
{
  Controller ctrl;

  double recv_rate = 15.0;

  // Trigger draining by consecutive congested frames
  // 1 -> FALLBACK, 2 -> CONGESTED_WAIT, 3 -> DRAINING
  uint64_t now_us = 100000;
  for (int i = 1; i <= 3; ++i)
  {
    now_us += pudica_net::INTERVAL;
    FrameAck fa{};
    fa.fid = i;
    fa.Dmin = 0.010;
    fa.D = 0.090; // Delay large enough to make bur > 1
    fa.probes = {0.0, 0.0, 0.0, 0.0};
    fa.recv_rate = recv_rate;
    fa.in_bytes = 15000;
    fa.now_microsecs = now_us;
    fa.n_inflight = 10;

    auto out = ctrl.on_frame_acked(fa);

    if (i == 3)
    {
      auto ss = ctrl.get_state_snapshot();
      assert(ss.current_state == State::DRAINING);
      assert(out.bitrate <= recv_rate);
    }
  }
  std::cout << "[PASS] test_drain_uses_recv_rate\n";
}

void test_fallback_only_one_encoded_frame() {
  PudicaConfig cfg;cfg.B_MIN=2;cfg.B_MAX=20;
  Controller controller(cfg);
  controller.on_frame_acked(FrameAck{1,.010,.010,{0,0,0,0},10,0,100000,0,2,false});
  double original=controller.get_bitrate();
  controller.on_inflight_age(cfg.NEXT_DELAY_THRESH+1);
  double reduced=controller.take_encoding_bitrate();
  assert(reduced<original);
  assert(controller.take_encoding_bitrate()==original);
  std::cout<<"[PASS] fallback consumed by one encoded frame\n";
}
void test_next_delay_is_rtt_relative()
{
  Controller ctrl;
  const uint64_t thresh = ctrl.get_config().NEXT_DELAY_THRESH;
  const uint64_t rtt = 120'000; // 120 ms path: every frame is older than 2 intervals
  assert(!ctrl.on_inflight_age(thresh + 1000, rtt).valid);
  assert(ctrl.get_state_snapshot().current_state == State::STEADY);
  assert(ctrl.on_inflight_age(rtt + thresh + 1, rtt).valid);
  assert(ctrl.get_state_snapshot().current_state == State::FALLBACK);
  std::cout << "[PASS] test_next_delay_is_rtt_relative\n";
}

// Three lossy frames at 10 Mbps (35714 B each). ack_us is the receiver-clock span
// over which each frame's first-try packets arrived.
static double run_lossy_frames(uint64_t ack_us, uint64_t gap_us)
{
  PudicaConfig cfg; cfg.B_MIN = 1; cfg.B_MAX = 50;
  Controller ctrl(cfg);
  const double bytes = 10e6 / 8 * pudica_net::INTERVAL / 1e6;
  uint64_t t = 1'000'000;
  for (uint32_t i = 1; i <= 3; ++i)
  {
    FrameAck fa{};
    fa.fid = i; fa.Dmin = .010; fa.D = .015; fa.probes = {0, 0, 0, 0};
    fa.recv_rate = 10; fa.now_microsecs = t; fa.n_inflight = 1; fa.sent_bitrate = 10;
    fa.lossy = true; fa.frame_mbps = 10; fa.ack_bytes = bytes * 0.98;
    fa.ack_first_us = t; fa.ack_last_us = t + ack_us;
    ctrl.on_frame_acked(fa);
    t += gap_us;
  }
  return ctrl.get_final_bitrate();
}

void test_random_loss_keeps_rate()
{
  // Paced uncongested link: each frame ACKed over ~23 ms, frames 28.6 ms apart.
  assert(run_lossy_frames(22'857, pudica_net::INTERVAL) > 5.0);
  std::cout << "[PASS] test_random_loss_keeps_rate\n";
}

void test_congestion_loss_sets_ceiling()
{
  // Network drains each frame over 2 intervals: ACK rate below the sending rate.
  assert(run_lossy_frames(2 * pudica_net::INTERVAL, 2 * pudica_net::INTERVAL) < 5.0);
  std::cout << "[PASS] test_congestion_loss_sets_ceiling\n";
}

static FrameAck clean_ack(uint32_t fid, uint64_t now, uint32_t epoch)
{
  FrameAck fa{};
  fa.fid = fid; fa.Dmin = .010; fa.D = .012; fa.probes = {0, 0, 0, 0};
  fa.recv_rate = 5; fa.now_microsecs = now; fa.n_inflight = 1; fa.frame_mbps = 1; fa.epoch = epoch;
  return fa;
}

void test_adjustment_waits_for_new_epoch()
{
  PudicaConfig cfg; cfg.B_MIN = 1; cfg.B_MAX = 50;
  Controller ctrl(cfg);
  ctrl.on_frame_acked(clean_ack(1, 1'000'000, ctrl.encoding_epoch()));
  const double after_first = ctrl.get_bitrate();
  assert(after_first > 1.0);
  // Frames still encoded at the old rate must not trigger another MI step,
  // however many of them were already queued or in flight.
  for (uint32_t fid = 2; fid < 10; ++fid)
    ctrl.on_frame_acked(clean_ack(fid, 1'000'000 + fid * 1000, 0));
  assert(ctrl.get_bitrate() == after_first);
  ctrl.on_frame_acked(clean_ack(10, 1'100'000, ctrl.encoding_epoch()));
  assert(ctrl.get_bitrate() > after_first);
  std::cout << "[PASS] test_adjustment_waits_for_new_epoch\n";
}

void test_frame_loss_backs_off_once_per_event()
{
  PudicaConfig cfg; cfg.B_MIN = .1;
  Controller ctrl(cfg);
  ctrl.on_frame_acked(clean_ack(1, 1'000'000, 0));
  const double start = ctrl.get_bitrate();
  ctrl.on_frame_loss(2'000'000, 100'000);
  ctrl.on_frame_loss(2'030'000, 100'000); // same event
  ctrl.on_frame_loss(2'060'000, 100'000);
  assert(std::abs(ctrl.get_bitrate() - std::max(start / 2, cfg.B_MIN)) < 1e-9);
  ctrl.on_frame_loss(2'200'000, 100'000); // a later event
  assert(ctrl.get_bitrate() < start / 2 || ctrl.get_bitrate() == cfg.B_MIN);
  std::cout << "[PASS] test_frame_loss_backs_off_once_per_event\n";
}

void test_drain_uses_rate_since_onset()
{
  PudicaConfig cfg; cfg.B_MIN = .1;
  Controller ctrl(cfg);
  const double rates[] = {30, 20, 10};
  for (uint32_t i = 0; i < 3; ++i)
  {
    FrameAck fa{};
    fa.fid = i + 1; fa.Dmin = .010; fa.D = .090; fa.probes = {0, 0, 0, 0};
    fa.recv_rate = rates[i]; fa.now_microsecs = 1'000'000 + i * 30'000; fa.n_inflight = 1;
    ctrl.on_frame_acked(fa);
  }
  // Eq. 11 with no in-flight bytes: alpha x mean(30, 20, 10), not alpha x 10.
  assert(std::abs(ctrl.get_bitrate() - cfg.ALPHA * 20) < 1e-9);
  std::cout << "[PASS] test_drain_uses_rate_since_onset\n";
}

static double drain_with(double in_bytes, double base_rtt)
{
  PudicaConfig cfg; cfg.B_MIN = .1;
  Controller ctrl(cfg);
  for (uint32_t i = 1; i <= 3; ++i)
  {
    FrameAck fa{};
    fa.fid = i; fa.Dmin = .010; fa.D = .090; fa.probes = {0, 0, 0, 0};
    fa.recv_rate = 8; fa.in_bytes = in_bytes; fa.base_rtt = base_rtt;
    fa.now_microsecs = 1'000'000 + i * 30'000; fa.n_inflight = 4;
    ctrl.on_frame_acked(fa);
  }
  return ctrl.get_bitrate();
}

void test_drain_ignores_bytes_in_the_pipe()
{
  // 8 Mbps over a 120 ms path keeps 120 kB in flight with an empty queue.
  const double pipe = 8e6 / 8 * 0.120;
  assert(std::abs(drain_with(pipe, 0.120) - 0.85 * 8) < 1e-9);
  // 50 kB beyond the pipe is queue: drain it within 200 ms (2 Mbps).
  assert(std::abs(drain_with(pipe + 50'000, 0.120) - (0.85 * 8 - 2.0)) < 1e-9);
  // Unknown RTT keeps the old behaviour: every in-flight byte counts.
  assert(drain_with(pipe, 0.0) < 3.0);
  std::cout << "[PASS] test_drain_ignores_bytes_in_the_pipe\n";
}

int main()
{
  test_drain_ignores_bytes_in_the_pipe();
  test_adjustment_waits_for_new_epoch();
  test_frame_loss_backs_off_once_per_event();
  test_drain_uses_rate_since_onset();
  test_random_loss_keeps_rate();
  test_congestion_loss_sets_ceiling();
  test_next_delay_is_rtt_relative();
  test_fallback_only_one_encoded_frame();
  test_bitrate_bounded();
  test_restore_next_single_shot();
  test_loss_triggers_drain();
  test_drain_uses_recv_rate();
  std::cout << "All tests passed.\n";
  return 0;
}

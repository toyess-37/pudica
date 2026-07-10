#include <iostream>
#include <vector>
#include <random>
#include <cassert>
#include "pudica_algo.h"

using namespace PudicaAlgorithm;

void test_bitrate_bounded()
{
  Controller ctrl;
  std::mt19937 gen(42);
  std::uniform_real_distribution<double> dist_delay(1.0, 50.0);
  std::uniform_real_distribution<double> dist_recv_rate(1.0, 100.0);
  std::uniform_int_distribution<uint32_t> dist_probes(0, 4);

  uint64_t now_us = 0;
  for (int i = 0; i < 1000; ++i)
  {
    now_us += 16666;
    FrameAck fa;
    fa.fid = i + 1;
    fa.Dmin = 10.0;
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
  FrameAck fa;
  fa.fid = 1;
  fa.Dmin = 10.0;
  fa.D = 10.0; // D == Dmin -> raw_bur == 0
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
    now_us += 16666;
    FrameAck fa;
    fa.fid = i;
    fa.Dmin = 10.0;
    fa.D = 30.0; // Delay large enough to make bur > 1
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

int main()
{
  test_bitrate_bounded();
  test_restore_next_single_shot();
  test_loss_triggers_drain();
  test_drain_uses_recv_rate();
  std::cout << "All tests passed.\n";
  return 0;
}

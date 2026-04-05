#include <iostream>
#include <cstring>
#include <cmath>
#include <map>
#include <vector>
#include <deque>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "protocol.h"
#include "pudica_algo.h"

using namespace std;
using namespace std::chrono;

uint64_t now_microsecs()
{
  return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

/*
  uses the idea of precise_sleep from:
  https://blog.bearcats.nl/accurate-sleep-function/
  (tl;dr - dynamically updates the guess of OS delay
         - sleeps for that much time only while also ensuring low cpu usage)
  [the blog post explains various sleep methods in detail]
*/
void precise_sleep(double microsecs)
{
  double seconds = microsecs / 1000000.0;

  static double estimate = 5e-3; // initial guess of OS wakeup delay --> 5ms
  static double mean = 5e-3;
  static double m2 = 0;
  static int64_t count = 1;
  while (seconds > estimate)
  {
    auto start = steady_clock::now();
    this_thread::sleep_for(milliseconds(1));
    auto end = steady_clock::now();
    double observed = (end - start).count() / 1e9;
    seconds -= observed;
    ++count;
    double delta = observed - mean;
    mean += delta / count;
    m2 += delta * (observed - mean);
    double stddev = sqrt(m2 / (count - 1));
    estimate = mean + stddev;
    estimate = max(1e-4, min(estimate, 5e-3));
  }
  auto start = steady_clock::now();
  auto spinNs = (int64_t)(seconds * 1e9);
  auto delay = nanoseconds(spinNs);
  while (steady_clock::now() - start < delay)
  {
    __asm__ volatile("pause" ::: "memory");
  }
}

class PudicaSender
{
private:
  int sock = -1;
  sockaddr_in dest{};
  socklen_t dest_len = sizeof(dest);

  atomic<bool> running{false};
  thread t_pacer;
  thread t_listener;

  mutex hist_mtx;
  deque<PudicaAlgorithm::HistorySample> hist; // past 200ms sliding window

  atomic<double> bitrate{PudicaAlgorithm::B_MIN};
  atomic<double> pace_p{1.25};
  atomic<int64_t> d_min{INT64_MAX};

  std::atomic<uint32_t> pacer_bytes[128]; // how many bytes is pacer() sending in each frame

  atomic<bool> in_drain_phase{false};
  atomic<double> pre_fallback_bitrate{0.0};

  uint32_t mi_adjustment_frame = 0; // frame_id when last MI increase was sent

  uint32_t frames_sent = 0;
  uint64_t last_frames_reset = 0; // to be used to track when frames_sent was reset; reset every 5s irrespective of congestion

  void pacer()
  {
    uint32_t fid = 1; // frame id
    while (running)
    {
      auto t_start = steady_clock::now();

      double rate = bitrate.load();
      double rho = pace_p.load();

      uint32_t f_bytes = (rate * 1000.0 * 125.0) / 60.0;
      uint32_t pkts = (f_bytes / LOAD_SZ) + 1;

      pacer_bytes[fid % 128].store(f_bytes, memory_order_relaxed);

      double sensible = INTERVAL / rho;
      double pkt_interval = sensible / pkts;

      uint8_t buf[LOAD_SZ];
      memset(buf + sizeof(PktHeader), 0, LOAD_SZ - sizeof(PktHeader));

      for (uint32_t id = 0; id < pkts && running; id++)
      {
        PktHeader hdr{};
        hdr.frame_id = fid;
        hdr.packet_id = id;
        hdr.send_time = now_microsecs();

        if (id == 0)
          hdr.flags |= IS_FIRST;
        if (id == pkts - 1)
          hdr.flags |= IS_LAST;

        memcpy(buf, &hdr, sizeof(PktHeader));
        sendto(sock, buf, sizeof(buf), 0, (sockaddr *)&dest, dest_len);

        precise_sleep(pkt_interval);
      }

      double agnostic = INTERVAL - sensible;
      double probe_interval = agnostic / (N_PROBE + 1);

      for (uint32_t i = 0; i < N_PROBE && running; i++)
      {
        precise_sleep(probe_interval);

        PktHeader probe_hdr{};
        probe_hdr.frame_id = fid;
        probe_hdr.packet_id = UINT32_MAX - i; // high id so that it does not get confused with data packets
        probe_hdr.flags = IS_PROBE;
        probe_hdr.send_time = now_microsecs();

        memcpy(buf, &probe_hdr, sizeof(PktHeader));
        int s = sendto(sock, &probe_hdr, sizeof(PktHeader), 0, (sockaddr *)&dest, dest_len);
        if (s < 0)
        {
          cerr << "[sender] send err: " << strerror(errno) << "\n";
          continue;
        }
      }

      fid++;
      this_thread::sleep_until(t_start + microseconds(INTERVAL));
    }
  }

  void listener()
  {
    uint8_t buf[MAX_BUF];
    int congested_frames = 0;

    struct FrameProgress
    {
      double frame_D_sec = 0;  // frame delay (in secs)
      uint64_t first_send = 0; // send time of first packet of frame (microsecs)
      uint64_t last_recv = 0;  // recv time of last packet of frame (microsecs)
      uint32_t bytes_sent = 0; // how many bytes the pacer sent for a particular frame
      bool has_first = false;
      bool has_last = false;
      bool is_done = false;
      vector<double> probes;
    };
    map<uint32_t, FrameProgress> inflight;

    while (running)
    {
      double rho = pace_p.load();
      double Dmin = d_min.load();

      ssize_t n = recvfrom(sock, buf, sizeof(buf), 0, nullptr, nullptr);
      if (n < static_cast<ssize_t>(sizeof(RecvACK)))
        continue;

      RecvACK *ack = reinterpret_cast<RecvACK *>(buf);
      uint32_t fid = ack->frame_id;

      if (fid > 5 && (ack->flags & IS_FIRST)) // reset only on frame boundary
        inflight.erase(inflight.begin(), inflight.lower_bound(fid - 5));

      if (inflight.find(fid) == inflight.end()) // update it when we first see the frame fid
        inflight[fid].bytes_sent = pacer_bytes[fid % 128].load(memory_order_relaxed);

      // one-way delay (microseconds) and Dmin
      int64_t owd = static_cast<int64_t>(ack->recv_time) - static_cast<int64_t>(ack->echoed_send);
      if (owd < Dmin)
      {
        d_min.store(owd);
        Dmin = owd;
      }

      double current_Dmin = Dmin / 1'000'000.0; // in secs
      double pkt_D = owd / 1'000'000.0;         // in sec

      if (ack->flags & IS_FIRST)
      {
        inflight[fid].first_send = ack->echoed_send;
        inflight[fid].has_first = true;
      }

      if (ack->flags & IS_LAST)
      {
        inflight[fid].last_recv = ack->recv_time;
        inflight[fid].has_last = true;
      }

      if (ack->flags & IS_PROBE)
      {
        double L_SEC = INTERVAL / 1'000'000.0;
        double T_bound = (1.0 - 1.0 / rho) * L_SEC / (N_PROBE + 1);
        double raw_T = max(0.0, pkt_D - current_Dmin);

        double H_i = numeric_limits<double>::max();
        if (inflight[fid].has_last && ack->recv_time >= inflight[fid].last_recv)
          H_i = (static_cast<int64_t>(ack->recv_time) - static_cast<int64_t>(inflight[fid].last_recv)) / 1'000'000.0;

        double t_i = min(min(raw_T, H_i), T_bound);
        inflight[fid].probes.push_back(t_i);
      }

      if (!inflight[fid].is_done && inflight[fid].has_first && inflight[fid].has_last && inflight[fid].probes.size() == N_PROBE)
      {
        inflight[fid].is_done = true;

        // D_us = recv_time(last) − send_time(first) [in microsecs]
        int64_t D_us = static_cast<int64_t>(inflight[fid].last_recv) - static_cast<int64_t>(inflight[fid].first_send);
        inflight[fid].frame_D_sec = D_us / 1'000'000.0;

        // calculate BUR
        double raw_r = PudicaAlgorithm::raw_BUR(inflight[fid].frame_D_sec, current_Dmin);
        double r_corr = PudicaAlgorithm::corrected_BUR(raw_r, inflight[fid].probes);

        // for wsl -- don't prioritize probe_delays as precise_sleep is still not precise
        // uncomment the following line for wsl --- 0.1 is empirical
        r_corr = (r_corr - raw_r)*0.1 + raw_r;

        double cur_rate = bitrate.load();
        pace_p.store(PudicaAlgorithm::pacing_multiplier(r_corr));

        cout << "BUR: " << r_corr
             << " bitrate: " << cur_rate
             << " delay: " << ((inflight[fid].frame_D_sec - current_Dmin) * 1000.0) << "\n";

        {
          lock_guard<mutex> lock(hist_mtx);
          hist.push_back({r_corr, cur_rate});
          if (hist.size() > 12)
            hist.pop_front();
        }

        // adaptive AI-MD bitrate adjustment
        if (r_corr > 1.0)
        {
          congested_frames++;
          frames_sent = 0; // reset frames when congestion
          if (in_drain_phase.load()) 
          {
            // do nothing; eat chocolate
          }
          else if (congested_frames >= 3)
          {
            in_drain_phase.store(true);
            pre_fallback_bitrate.store(0.0);

            // draining_rate: extra throughput needed to clear self-queued data within DRAIN_WINDOW.
            constexpr double DRAIN_WINDOW = 0.200; // 200ms then drain

            double inflight_bytes = 0;
            for (auto const &[id, progress] : inflight)
              inflight_bytes += progress.bytes_sent;

            double draining_rate = (8.0 * inflight_bytes) / (DRAIN_WINDOW * 1'000'000.0); // Mbps

            // B_new = ALPHA * receiving_rate − draining_rate (receiving_rate = ack->rate)
            double new_B = PudicaAlgorithm::ALPHA * ack->rate - draining_rate;
            bitrate.store(max(new_B, PudicaAlgorithm::B_MIN));

            mi_adjustment_frame = fid + static_cast<uint32_t>(inflight.size());
          }
          else if (congested_frames < 3)
          {
            if (congested_frames == 1)
            {
              pre_fallback_bitrate.store(cur_rate); // save for restore
              bitrate.store(cur_rate * 0.85);
            }
          }
        }
        else
        {
          double pre_fall_rate = pre_fallback_bitrate.load();
          if (pre_fall_rate > 0.0) // there was a fallback to 85%
          {
            cur_rate = pre_fall_rate;
            bitrate.store(cur_rate);
            pre_fallback_bitrate.store(0.0); // no pending fallback
          }

          // when previous one was draining phase, skip calculation on this frame and reset to normal
          // for fresh calculation
          if (in_drain_phase.load())
          {
            in_drain_phase.store(false);
            // restore bitrate to the current receiving_rate
            bitrate.store(min(max(ack->rate, PudicaAlgorithm::B_MIN), PudicaAlgorithm::B_MAX));

            congested_frames = 0;
            frames_sent = 0;
            // skip the normal MI/AI-MD update this frame
            mi_adjustment_frame = fid + static_cast<uint32_t>(inflight.size());
            inflight.erase(fid);
            continue;
          }
          congested_frames = 0;
          frames_sent++;
          uint64_t now = now_microsecs();
          if (last_frames_reset == 0)
            last_frames_reset = now;
          if (now - last_frames_reset >= 5'000'000ULL)
          {
            frames_sent = 0;
            last_frames_reset = now;
          }

          double r_tilde = PudicaAlgorithm::smoothed_BUR(hist, cur_rate);

          // if fid <= mi_adjustment_frame, we haven't received the latest feedback; so we'll skip it
          if (fid > mi_adjustment_frame)
          {
            if (r_tilde <= PudicaAlgorithm::ALPHA)
            {
              double safe_r = max(r_tilde, 1e-3); // make sure bur doesn't get near 0
              double xi = PudicaAlgorithm::GAMMA_MI * (((PudicaAlgorithm::ALPHA + 1.0) / 2.0 - safe_r) / safe_r);
              cur_rate = min(max(cur_rate * (1.0 + xi), PudicaAlgorithm::B_MIN), PudicaAlgorithm::B_MAX);
              bitrate.store(cur_rate);
            }
            else
            {
              cur_rate = PudicaAlgorithm::calculate_next_bitrate(cur_rate, r_tilde, frames_sent);
              bitrate.store(cur_rate);
            }
            mi_adjustment_frame = fid + static_cast<uint32_t>(inflight.size());
          }
        }
        inflight.erase(fid);
      }
    }
  }

public:
  PudicaSender(const string &ip, int port)
  {
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
      throw runtime_error("[sender] socket creation failed");

    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);

    if (inet_pton(AF_INET, ip.c_str(), &dest.sin_addr) <= 0)
      throw runtime_error("[sender] invalid ip address");
  }

  ~PudicaSender()
  {
    stop();
    if (sock >= 0)
      close(sock);
  }

  void start()
  {
    if (running)
      return;
    running = true;
    t_pacer = thread(&PudicaSender::pacer, this);
    t_listener = thread(&PudicaSender::listener, this);
  }

  void stop()
  {
    if (!running)
      return;
    running = false;
    if (t_pacer.joinable())
      t_pacer.join();
    if (t_listener.joinable())
      t_listener.join();
  }
};

int main(int argc, char *argv[])
{
  if (argc != 3)
  {
    cerr << "Usage: " << argv[0] << " <ip> <port>\n";
    return 1;
  }

  try
  {
    PudicaSender sender(argv[1], stoi(argv[2]));
    sender.start();

    cout << "[sender] sender running. auto-exiting soon.\n";
    precise_sleep(30'000'000.0);

    sender.stop();
    cout << "[sender] exited.\n";
  }
  catch (const exception &e)
  {
    cerr << "[sender] Error: " << e.what() << "\n";
    return 1;
  }

  return 0;
}
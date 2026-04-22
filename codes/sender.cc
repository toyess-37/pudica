#include <iostream>
#include <cstring>
#include <cmath>
#include <unordered_map>
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
  uses the idea (well, the exact code) of precise_sleep from:
  https://blog.bearcats.nl/accurate-sleep-function/
  (tl;dr - dynamically updates the guess of OS delay
         - sleeps for that much time only, while also ensuring low cpu usage)
  [the blog post explains various sleep methods in detail]
*/
void precise_sleep(double microsecs)
{
  double seconds = microsecs / 1000000.0;

  static double estimate = 5e-3;
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

struct Frame
{
  uint64_t created_at = 0; // local clock when pacer inserted
  uint64_t t0 = 0;         // echoed_send of IS_FIRST
  uint64_t t1_recv = 0;    // recv_time of IS_LAST
  vector<double> probes;   // Ti values
  uint32_t bytes_out = 0;
  bool got_first = false;
  bool got_last = false;
  bool done = false; // frame processed or not

  bool complete() const
  {
    return got_first && got_last && probes.size() == N_PROBE;
  }
};

class PudicaSender
{
private:
  int sock = -1;
  sockaddr_in dest{};
  socklen_t dest_len = sizeof(dest);

  atomic<bool> running{false};
  thread t_pacer, t_listener; // pacer and listener threads for sending packets and running the algorithm

  atomic<double> bitrate{PudicaAlgorithm::B_MIN};
  atomic<double> pacing{PudicaAlgorithm::GAMMA_P};

  unordered_map<uint32_t, Frame> table;
  mutex table_mtx;

  PudicaAlgorithm::Controller ctrl;

  uint32_t last_done_fid = 0;       // latest frame which is complete
  uint32_t oldest_inflight_fid = 1; // oldest frame in flight
  uint32_t running_inflight_frames = 0;
  uint32_t running_inflight_bytes = 0;

  void evaluate(uint32_t fid, Frame &fr, double recv_rate, int64_t min_d, uint64_t rtt_min)
  {
    double Dmin = min_d / 1e6;
    double D = ((int64_t)fr.t1_recv - (int64_t)fr.t0) / 1e6;

    PudicaAlgorithm::FrameAck fa{
        fid, D, Dmin, fr.probes, recv_rate,
        static_cast<double>(running_inflight_bytes),
        now_microsecs(),
        running_inflight_frames};

    auto out = ctrl.on_frame_acked(fa);

    bitrate.store(out.bitrate);
    pacing.store(out.pacing);
    fr.done = true;
    last_done_fid = max(last_done_fid, fid);
    double prop = rtt_min / 2000.0;     // propagation (sec to ms)
    double queue = (D - Dmin) * 1000.0; // queuing (microsec to ms)

    cout << "BUR: " << out.bur
         << " bitrate: " << out.bitrate
         << " delay: " << (prop + queue)
         << " frame id: " << fid
         << " probes: " << fr.probes.size() << "/" << N_PROBE << "\n";
  }

  void pacer()
  {
    for (uint32_t fid = 1; running; fid++)
    {
      auto t_start = steady_clock::now();

      double rate = bitrate.load();
      double rho = pacing.load();
      uint32_t f_bytes = static_cast<uint32_t>((rate * 1000.0 * 125.0) / 60.0); // 1000 / 8 = 125
      uint32_t pkts = f_bytes / LOAD_SZ + 1;

      {
        lock_guard<mutex> lk(table_mtx);
        auto &fr = table[fid];
        fr.bytes_out = f_bytes;
        fr.created_at = now_microsecs();

        running_inflight_frames++;
        running_inflight_bytes += f_bytes;
      }

      double sensible = INTERVAL / rho;
      double pkt_gap = sensible / pkts;
      double agnostic = INTERVAL - sensible;
      double probe_gap = agnostic / (N_PROBE + 1);

      uint8_t buf[LOAD_SZ + sizeof(PktHeader)];
      memset(buf + sizeof(PktHeader), 0, LOAD_SZ);

      for (uint32_t pid = 0; pid < pkts && running; pid++)
      {
        PktHeader hdr{};
        hdr.frame_id = fid;
        hdr.packet_id = pid;
        hdr.send_time = now_microsecs();
        if (pid == 0)
          hdr.flags |= IS_FIRST;
        if (pid == pkts - 1)
          hdr.flags |= IS_LAST;
        memcpy(buf, &hdr, sizeof(PktHeader));
        sendto(sock, buf, sizeof(buf), 0, (sockaddr *)&dest, dest_len);
        if (pid < pkts - 1)
          precise_sleep(pkt_gap);
      }

      for (uint32_t i = 0; i < N_PROBE && running; i++)
      {
        precise_sleep(probe_gap);
        PktHeader phdr{};
        phdr.frame_id = fid;
        phdr.packet_id = UINT32_MAX - i;
        phdr.flags = IS_PROBE;
        phdr.send_time = now_microsecs();
        memcpy(buf, &phdr, sizeof(PktHeader));
        sendto(sock, &phdr, sizeof(PktHeader), 0, (sockaddr *)&dest, dest_len);
      }

      this_thread::sleep_until(t_start + microseconds(INTERVAL));
    }
  }

  void listener()
  {
    uint8_t buf[MAX_BUF];
    struct OwdSample
    { // for resetting d_min after every 10s
      uint64_t ts;
      int64_t owd;
    };
    deque<OwdSample> owd_window;
    int64_t d_min = INT64_MAX;
    int64_t rtt_min = INT64_MAX;
    double recv_rate = 0.0;

    while (running)
    {
      ssize_t n = recvfrom(sock, buf, sizeof(buf), 0, nullptr, nullptr);
      if (n < static_cast<ssize_t>(sizeof(RecvACK)))
        continue;

      auto *ack = reinterpret_cast<RecvACK *>(buf);
      uint64_t now = now_microsecs();
      uint32_t fid = ack->frame_id;
      if (fid < oldest_inflight_fid)
        continue;
      recv_rate = ack->rate;

      int64_t rtt = static_cast<int64_t>(now - ack->echoed_send);
      int64_t cutoff = static_cast<int64_t>(now - 10'000'000ULL);
      int64_t owd = static_cast<int64_t>(ack->recv_time) - static_cast<int64_t>(ack->echoed_send);

      while (!owd_window.empty() && static_cast<int64_t>(owd_window.front().ts) < cutoff)
        owd_window.pop_front();
      while (!owd_window.empty() && owd_window.back().owd >= owd)
        owd_window.pop_back();
      owd_window.push_back({now, owd});

      rtt_min = min(rtt, rtt_min);
      d_min = owd_window.front().owd;

      double Dmin = d_min / 1e6; // in sec
      double D_pkt = owd / 1e6;  // in sec

      lock_guard<mutex> lk(table_mtx);

      // update the table entries
      {
        auto it = table.find(fid);
        if (it != table.end() && !it->second.done)
        {
          Frame &fr = it->second;

          if (ack->flags & IS_FIRST)
          {
            fr.t0 = ack->echoed_send;
            fr.got_first = true;
          }
          if (ack->flags & IS_LAST)
          {
            fr.t1_recv = ack->recv_time;
            fr.got_last = true;
          }
          if (ack->flags & IS_PROBE)
          {
            double rho = pacing.load();
            double T_bound = (1.0 - 1.0 / rho) * PudicaAlgorithm::L_SEC / (N_PROBE + 1);
            double raw_T = max(0.0, D_pkt - Dmin);
            double Hi = numeric_limits<double>::max();
            if (fr.got_last && ack->recv_time >= fr.t1_recv)
              Hi = (static_cast<int64_t>(ack->recv_time) - static_cast<int64_t>(fr.t1_recv)) / 1e6;
            fr.probes.push_back(min(min(raw_T, Hi), T_bound));
          }

          if (fr.complete())
          {
            evaluate(fid, fr, recv_rate, d_min, rtt_min);
            running_inflight_frames--;
            running_inflight_bytes -= fr.bytes_out;
            table.erase(it);
          }
        }
      }

      auto oldest_it = table.find(oldest_inflight_fid);
      if (oldest_it != table.end() && !oldest_it->second.done)
      {
        uint64_t age = now - oldest_it->second.created_at;

        auto fallback = ctrl.on_inflight_age(age);
        if (fallback.has_value())
        {
          bitrate.store(fallback->bitrate);
          pacing.store(fallback->pacing);
        }

        if (age > PudicaAlgorithm::TIMEOUT)
        {
          // we never got the full frame back. Network is dropping packets.
          ctrl.on_frame_loss();

          running_inflight_frames--;
          running_inflight_bytes -= oldest_it->second.bytes_out;
          table.erase(oldest_it);
          oldest_inflight_fid++;
        }
      }

      // keep checking the oldest_inflight_fid to be there in the table and not cleaned already
      while (table.find(oldest_inflight_fid) == table.end() && oldest_inflight_fid <= last_done_fid)
        oldest_inflight_fid++;
    }
  }

public:
  PudicaSender(const string &ip, int port)
  {
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
      throw runtime_error("[sender] ERROR: socket creation failed");
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &dest.sin_addr) <= 0)
      throw runtime_error("[sender] ERROR: invalid ip address");
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
  if (argc != 4)
  {
    cerr << "Usage: " << argv[0] << " <target_ip> <port> <duration_sec>\n";
    return 1;
  }
  try
  {
    PudicaSender sender(argv[1], stoi(argv[2]));
    sender.start();

    double duration = stod(argv[3]);
    precise_sleep(duration * 1e6);
    sender.stop();
  }
  catch (const exception &e)
  {
    cerr << "[sender] Error: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
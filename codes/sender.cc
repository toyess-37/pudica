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

class PudicaSender
{
private:
  int sock = -1;
  sockaddr_in dest{};
  socklen_t dest_len = sizeof(dest);

  atomic<bool> running{false};
  thread t_pacer, t_listener;

  mutex ctrl_mtx;
  PudicaAlgorithm::Controller ctrl;

  atomic<double> bitrate{PudicaAlgorithm::B_MIN};
  atomic<double> pacing{PudicaAlgorithm::GAMMA_P};

  atomic<int64_t> d_min{INT64_MAX};
  atomic<int64_t> rtt_min{INT64_MAX};
  atomic<uint32_t> pacer_bytes[128]; // bytes sent per frame slot

  void pacer()
  {
    uint32_t fid = 1;
    while (running)
    {
      auto t_start = steady_clock::now();

      double rate = bitrate.load();
      double rho = pacing.load();

      uint32_t f_bytes = static_cast<uint32_t>((rate * 1000.0 * 125.0) / 60.0);
      uint32_t pkts = f_bytes / LOAD_SZ + 1;
      pacer_bytes[fid % 128].store(f_bytes, memory_order_relaxed);

      double sensible = INTERVAL / rho;
      double pkt_gap = sensible / pkts;
      double agnostic = INTERVAL - sensible;
      double probe_gap = agnostic / (N_PROBE + 1);

      uint8_t buf[LOAD_SZ];
      memset(buf + sizeof(PktHeader), 0, LOAD_SZ - sizeof(PktHeader));

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
        if (sendto(sock, &phdr, sizeof(PktHeader), 0, (sockaddr *)&dest, dest_len) < 0)
          cerr << "[pacer] ERROR: " << strerror(errno) << "\n";
      }

      fid++;
      this_thread::sleep_until(t_start + microseconds(INTERVAL));
    }
  }

  struct Frame
  {
    uint64_t t0 = 0;        // send time of first packet (microsecs)
    uint64_t t1_recv = 0;   // recv time of last packet  (microsecs)
    uint32_t bytes_out = 0; // bytes sent by pacer
    vector<double> probes;  // Ti values
    bool got_first = false;
    bool got_last = false;
    bool done = false; // processed

    bool ready() const { return got_first && got_last && !probes.empty(); }
  };

  void listener()
  {
    uint8_t buf[MAX_BUF];
    map<uint32_t, Frame> inflight;

    while (running)
    {
      ssize_t n = recvfrom(sock, buf, sizeof(buf), 0, nullptr, nullptr);
      if (n < static_cast<ssize_t>(sizeof(RecvACK)))
        continue;

      auto *ack = reinterpret_cast<RecvACK *>(buf);
      uint32_t fid = ack->frame_id;

      int64_t rtt = static_cast<int64_t>(now_microsecs() - ack->echoed_send);
      rtt_min.store(min(rtt, rtt_min.load()));

      int64_t owd = static_cast<int64_t>(ack->recv_time) - static_cast<int64_t>(ack->echoed_send);
      if (owd < d_min.load())
        d_min.store(owd);

      double Dmin = d_min.load() / 1e6; // in sec
      double D_pkt = owd / 1e6;         // in sec

      if (!inflight.count(fid))
        inflight[fid].bytes_out = pacer_bytes[fid % 128].load(memory_order_relaxed);

      auto &fr = inflight[fid];
      if (fr.done)
        continue; // already processed

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

      if (fr.ready())
      {
        fr.done = true;

        double D = (static_cast<int64_t>(fr.t1_recv) - static_cast<int64_t>(fr.t0)) / 1e6;

        double in_bytes = 0;
        for (auto &[id, f] : inflight)
          in_bytes += f.bytes_out;

        PudicaAlgorithm::FrameAck fa{
            fid, D, Dmin, fr.probes,
            ack->rate, in_bytes,
            static_cast<uint32_t>(inflight.size()),
            now_microsecs()};

        PudicaAlgorithm::control_output out;
        {
          lock_guard<mutex> lk(ctrl_mtx);
          out = ctrl.on_frame_acked(fa);
        }
        bitrate.store(out.bitrate);
        pacing.store(out.pacing);

        double prop = rtt_min.load() / 2000.0; // propagation (sec)
        double queue = (D - Dmin) * 1000.0;    // queuing (ms)

        cout << "BUR: " << out.bur
             << " bitrate: " << out.bitrate
             << " delay: " << (prop + queue) << "\n";

        inflight.erase(fid);
      }

      // oldest inflight check and update
      if (!inflight.empty())
      {
        auto &oldest = inflight.begin()->second;
        if (oldest.t0 > 0)
        {
          uint64_t age = now_microsecs() - oldest.t0;
          optional<PudicaAlgorithm::control_output> fb;
          {
            lock_guard<mutex> lk(ctrl_mtx);
            fb = ctrl.on_inflight_age(age);
          }
          if (fb)
          {
            bitrate.store(fb->bitrate);
            pacing.store(fb->pacing);
          }
        }
      }

      if (fid > 5)
        inflight.erase(inflight.begin(), inflight.lower_bound(fid - 5));
    }
  }

public:
  PudicaSender(const string &ip, int port)
  {
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
      throw runtime_error("[sender] ERROR: socket creation failed.");
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
    cerr << "Usage: " << argv[0] << " <ip> <port> <duration_sec>\n";
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
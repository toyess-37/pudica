#include <iostream>
#include <cstring>
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

uint64_t now() {
  return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

void panic(const char *msg) {
  perror(msg);
  exit(1);
}

// struct PudicaState {
//   mutex history_mutex;
//   atomic<double> current_bitrate{50.0};     // in Mbps
//   atomic<double> pacing_multiplier_p{1.25}; // pacing multiplier p
//   atomic<double> fallback_rate_mbps{0.0};
//   atomic<int64_t> d_min{INT64_MAX};

//   uint32_t frames = 0;

//   // queue draining flag for BUR > 1 stages
//   atomic<bool> is_draining{false};

//   // Sliding window of past 200ms
//   deque<PudicaAlgorithm::HistorySample> history;
// };

class PudicaSender {
private:
  int sock = -1;
  sockaddr_in dest{};
  socklen_t dest_len = sizeof(dest);

  atomic<bool> running{false};
  thread t_pacer;
  thread t_listener;

  mutex hist_mtx;
  deque<PudicaAlgorithm::HistorySample> hist; // past 200ms sliding window

  atomic<double> bitrate{10.0};
  atomic<double> pace_p{1.25};
  atomic<int> d_min{INT64_MAX};

  uint32_t frames_sent = 0;

  void pacer() {
    uint32_t fid = 1; // frame id
    while(running) {
      auto t_start = steady_clock::now();

      double rate = bitrate.load();
      double p = pace_p.load();

      uint32_t f_bytes = (rate * 1000.0 * 125.0)/60.0;
      uint32_t pkts = (f_bytes/LOAD_SZ) + 1;

      double sensible = INTERVAL/p;
      double pkt_interval = sensible/pkts;

      for (uint32_t id = 0; id < pkts && running; id++) {
        PktHeader hdr{};
        hdr.frame_id = fid;
        hdr.packet_id = id;
        hdr.send_time = now();

        if (id==0) hdr.flags |= IS_FIRST;
        if (id==pkts-1) hdr.flags |= IS_LAST;

        uint8_t buf[LOAD_SZ] = {0};
        memcpy(buf, &hdr, sizeof(PktHeader));
        sendto(sock, buf, sizeof(buf), 0, (sockaddr*)&dest, dest_len);

        this_thread::sleep_for(microseconds(static_cast<int>(pkt_interval)));
      }

      double agnostic = INTERVAL - sensible;
      double probe_interval = agnostic/(N_PROBE + 1);

      for (int i=0; i<N_PROBE && running; i++) {
        this_thread::sleep_for(microseconds(static_cast<int>(probe_interval)));

        PktHeader probe_hdr{};
        probe_hdr.frame_id = fid;
        probe_hdr.packet_id = UINT32_MAX - i; // high id so that it does not get confused with data packets
        probe_hdr.flags = IS_PROBE;
        probe_hdr.send_time = now();

        int s = sendto(sock, &probe_hdr, sizeof(PktHeader), 0, (sockaddr*)&dest, dest_len);
        if (s<0) {
          cerr << "[sender] send err: " << strerror(errno) << "\n";
          continue;
        }
      }
      
      fid++;
      this_thread::sleep_until(t_start + microseconds(INTERVAL));
    }
  }

  void listener() {
    uint8_t buf[MAX_BUF];
    int congested_frames = 0;

    struct FrameProgress {
      double frame_D_sec = 0;
      bool has_last = false;
      vector<double> probes;
    };
    map<uint32_t, FrameProgress> inflight;

    while(running) {
      ssize_t n = recvfrom(sock, buf, sizeof(buf), 0, nullptr, nullptr);
      if (n < sizeof(RecvACK)) continue;

      RecvACK* ack = reinterpret_cast<RecvACK*>(buf);
      uint32_t fid = ack->frame_id;

      if (fid > 5) inflight.erase(inflight.begin(), inflight.lower_bound(fid - 5));

      // one-way delay (microseconds) and Dmin
      int64_t owd = static_cast<int64_t>(ack->recv_time) - static_cast<int64_t>(ack->echoed_send);
      if (owd < d_min.load()) d_min.store(owd);

      double current_Dmin = d_min.load() / 1000000.0; // in secs
      double pkt_D = owd / 1000000.0; // in sec

      if (ack->flags & IS_PROBE) {
        double T_bound = ((INTERVAL / 1000000.0) * 0.2) / (N_PROBE + 1);
        double raw_T = max(0.0, pkt_D - current_Dmin);
        double t_i = min(raw_T, T_bound);
        inflight[fid].probes.push_back(t_i);
      } else if (ack->flags & IS_LAST) {
        inflight[fid].frame_D_sec = pkt_D;
        inflight[fid].has_last = true;
      }

      if (inflight[fid].has_last && inflight[fid].probes.size() >= 1) {
        // calculate BUR
        double raw_r = PudicaAlgorithm::raw_BUR(inflight[fid].frame_D_sec, current_Dmin);
        double r_corr = PudicaAlgorithm::corrected_BUR(raw_r, inflight[fid].probes);
        cout << "BUR: " << r_corr<< " bitrate: " << bitrate.load() << "\n";

        pace_p.store(PudicaAlgorithm::pacing_multiplier(r_corr));

        {
          lock_guard<mutex> lock(hist_mtx);
          hist.push_back({r_corr, bitrate.load()});
          if (hist.size()>12) hist.pop_front();
        }

        // adaptive AI-MD bitrate adjustment
        if (r_corr > 1.0) {
          congested_frames++;
          if (congested_frames>=3) bitrate.store(ack->rate);
          else bitrate.store(bitrate.load()*0.85);
        } else {
          congested_frames = 0;
          double r_tilde = PudicaAlgorithm::smoothed_BUR(hist, bitrate.load());
          double next_rate = PudicaAlgorithm::calculate_next_bitrate(bitrate.load(), r_tilde, frames_sent);
          bitrate.store(next_rate);
        }

        inflight.erase(fid);
      }
    }
  }

public:
  PudicaSender(const string& ip, int port) {
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) throw runtime_error("[sender] socket creation failed");

    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);

    if (inet_pton(AF_INET, ip.c_str(), &dest.sin_addr) <= 0)
      throw runtime_error("[sender] invalid ip address");
  }

  ~PudicaSender() {
    stop();
    if (sock>=0) close(sock);
  }

  void start() {
    if (running) return;
    running = true;
    t_pacer = thread(&PudicaSender::pacer, this);
    t_listener = thread(&PudicaSender::listener, this);
  }

  void stop() {
    if (!running) return;
    running = false;
    if (t_pacer.joinable()) t_pacer.join();
    if (t_listener.joinable()) t_listener.join();
  }
};

int main(int argc, char* argv[]) {
  if (argc != 3) {
    cerr << "Usage: " << argv[0] << " <ip> <port>\n";
    return 1;
  }

  try {
    PudicaSender sender(argv[1], stoi(argv[2]));
    sender.start();

    cout << "[sender] sender running. auto-exiting soon.\n";
    this_thread::sleep_for(seconds(30));

    sender.stop();
    cout << "[sender] exited.\n";
  } catch (const exception& e) {
    cerr << "[sender] Error: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
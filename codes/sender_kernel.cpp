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

#include <linux/net_tstamp.h>
#include <time.h>

#include "protocol.h"
#include "pudica_algo.h"

using namespace std;
using namespace std::chrono;

void panic(const char *msg) {
  perror(msg);
  exit(1);
}

uint64_t now() {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return static_cast<uint64_t>(t.tv_nsec + 1000000000ULL*t.tv_sec);
}

// send a packet using SO_TXTIME --- https://lwn.net/Articles/748879/
void send_with_txtime(int sock, const sockaddr_in& dest, const uint8_t* buf, size_t len, uint64_t time_ns) {
  char control[CMSG_SPACE(sizeof(uint64_t))] = {0};
  struct msghdr msg = {0};
  struct iovec iov[1];

  iov[0].iov_base = const_cast<uint8_t*>(buf);
  iov[0].iov_len = len;

  msg.msg_iov = iov;
  msg.msg_iovlen = 1;
  msg.msg_name = (void*)&dest;
  msg.msg_namelen = sizeof(dest);
  msg.msg_control = control;
  msg.msg_controllen = sizeof(control);

  // attach the nanosecond timestamp
  struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_TXTIME;
  cmsg->cmsg_len = CMSG_LEN(sizeof(uint64_t));
  *((uint64_t *)CMSG_DATA(cmsg)) = time_ns;

  if (sendmsg(sock, &msg, 0) < 0) {
    cerr << "[sender] sendmsg err: " << strerror(errno) << "\n";
  }
}
struct PudicaState {
  mutex history_mutex;
  atomic<double> current_bitrate{50.0};     // in Mbps
  atomic<double> pacing_multiplier_p{1.25}; // pacing multiplier p
  atomic<double> fallback_rate_mbps{0.0};
  atomic<int64_t> d_min{INT64_MAX};

  uint32_t frames = 0;

  // queue draining flag for BUR > 1 stages
  atomic<bool> is_draining{false};

  // Sliding window of past 200ms
  deque<PudicaAlgorithm::HistorySample> history;
};
PudicaState global_state;

void pacer_thread(int sock, sockaddr_in client_addr) {
  socklen_t client_len = sizeof(client_addr);
  uint32_t current_frame_id = 1;

  while (true) {
    auto loop_start = steady_clock::now(); // this uses standard steady_clock
    uint64_t frame_start_time = now(); // this uses CLOCK_MONOTONIC

    double bitrate = global_state.current_bitrate.load(); // current bitrate
    double p = global_state.pacing_multiplier_p.load();

    // frame size in packets [ 10^6 / 8 = 125 * 10^3 ]
    uint32_t frame_bytes = (bitrate * 1000.0 * 125.0) / 60.0;
    uint32_t total_packets = (frame_bytes / LOAD_SZ) + 1;

    // sensible period = L/p
    uint64_t interval_ns = INTERVAL * 1000ULL;
    double sensible_period_ns = interval_ns / p;
    double packet_interval_ns = sensible_period_ns / total_packets;

    // send frames' packets
    for (uint32_t id = 0; id < total_packets; id++) {
      PktHeader header{};
      header.frame_id = current_frame_id;
      header.packet_id = id;

      uint64_t send_time = frame_start_time + id*packet_interval_ns;
      header.send_time = (send_time) / 1000; // receiver calculates in microseconds

      if (id == 0) header.flags |= IS_FIRST;
      if (id == total_packets - 1) header.flags |= IS_LAST;

      // dummy payload and send
      uint8_t buffer[LOAD_SZ] = {0};
      memcpy(buffer, &header, sizeof(PktHeader));
      
      send_with_txtime(sock, client_addr, buffer, sizeof(buffer), send_time);
    }

    // agnostic period probes
    double agnostic_period = interval_ns - sensible_period_ns;
    double probe_interval = agnostic_period/(N_PROBE+1);

    for (uint32_t i=0; i<N_PROBE; i++) {
      uint64_t probe_time = frame_start_time + sensible_period_ns + i*probe_interval;

      PktHeader probe_hdr{};
      probe_hdr.frame_id = current_frame_id;
      probe_hdr.packet_id = UINT32_MAX - i; // high id so that it does not get confused with data packets
      probe_hdr.flags = IS_PROBE;
      probe_hdr.send_time = probe_time / 1000;

      send_with_txtime(sock, client_addr, reinterpret_cast<uint8_t*>(&probe_hdr), sizeof(PktHeader), probe_time);
    }

    // sleep until the next 16.67ms
    current_frame_id++;
    this_thread::sleep_until(
      time_point<steady_clock>(loop_start + nanoseconds(interval_ns))
    );
  }
}

// to measure the Bandwidth Utilization Ratio
void listener_thread(int sock) {
  uint8_t buf[MAX_BUF];
  int congested_frames = 0;

  struct FrameProgress {
    double frame_D_sec = 0;
    bool has_last_packet = false;
    vector<double> probe_delays;
  };
  map<uint32_t, FrameProgress> in_flight_frames;

  while(true) {
    ssize_t n = recvfrom(sock, buf, sizeof(buf), 0, nullptr, nullptr);
    if (n < sizeof(RecvACK)) continue;

    RecvACK* ack = reinterpret_cast<RecvACK*>(buf);
    uint32_t fid = ack->frame_id;
    if (fid > 5) 
      in_flight_frames.erase(in_flight_frames.begin(), in_flight_frames.lower_bound(fid-5));

    // one-way delay (microseconds) and Dmin
    int64_t one_way_delay = static_cast<int64_t>(ack->recv_time) - static_cast<int64_t>(ack->echoed_send);
    if (one_way_delay < global_state.d_min.load())
      global_state.d_min.store(one_way_delay);

    double current_Dmin = global_state.d_min.load() / 1000000.0; // in secs
    double pkt_D = one_way_delay / 1000000.0; // in sec

    bool is_probe = (ack->flags & IS_PROBE);
    bool is_last  = (ack->flags & IS_LAST);
    
    if (is_probe) {
      double T_i = pkt_D - current_Dmin;
      if (T_i < 0.0) T_i = 0.0;
      in_flight_frames[fid].probe_delays.push_back(T_i);
    } else if (is_last) {
      in_flight_frames[fid].frame_D_sec = pkt_D;
      in_flight_frames[fid].has_last_packet = true;
    }

    if (in_flight_frames[fid].has_last_packet && in_flight_frames[fid].probe_delays.size() == N_PROBE) {
      // Calculate BUR
      double raw_R = PudicaAlgorithm::raw_BUR(in_flight_frames[fid].frame_D_sec, current_Dmin);
      double R_corrected = PudicaAlgorithm::corrected_BUR(raw_R, in_flight_frames[fid].probe_delays);
      cout << "BUR: " << R_corrected<< " bitrate: " << global_state.current_bitrate.load()<< endl;
      
      // Update Pacing Multiplier
      global_state.pacing_multiplier_p.store(PudicaAlgorithm::pacing_multiplier(R_corrected));

      {
        // Long-term AI-MD History Update
        lock_guard<mutex> lock(global_state.history_mutex);
        global_state.history.push_back({R_corrected, global_state.current_bitrate.load()});
        if (global_state.history.size() > 12) global_state.history.pop_front();
      }

      // Short-term Reactions
      if (R_corrected > 1.0) {
        congested_frames++;
        if (congested_frames >= 3) global_state.current_bitrate.store(ack->rate); // Active draining
        else global_state.current_bitrate.store(global_state.current_bitrate.load() * 0.85); // 15% fallback
      } else {
        congested_frames = 0;
        double R_tilde = PudicaAlgorithm::smoothed_BUR(global_state.history, global_state.current_bitrate.load());
        global_state.frames++;
        double next_B = PudicaAlgorithm::calculate_next_bitrate(global_state.current_bitrate.load(), R_tilde, global_state.frames);
        global_state.current_bitrate.store(next_B);
      }
      
      in_flight_frames.erase(fid);
    }
  }
}

int main(int argc, char* argv[]) {
  if (argc != 3) {
    cerr << "Usage: " << argv[0] << " <ip> <port>\n";
    return 1;
  }
  string ip = argv[1];
  int port = stoi(argv[2]);

  int sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) panic("[sender] socket fail");

  sock_txtime txt;
  txt.clockid = CLOCK_MONOTONIC;
  txt.flags = SOF_TXTIME_REPORT_ERRORS;
  if (setsockopt(sock, SOL_SOCKET, SO_TXTIME, &txt, sizeof(txt)) < 0)
    panic("[sender] SO_TXTIME failed");

  sockaddr_in dest{};
  dest.sin_family = AF_INET;
  dest.sin_port = htons(port);

  if (inet_pton(AF_INET, ip.c_str(), &dest.sin_addr) <= 0)
    panic("[sender] Invalid IP address format");

  thread t1(pacer_thread, sock, dest);
  thread t2(listener_thread, sock);

  t1.join();
  t2.join();

  return 0;
}
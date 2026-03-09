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

struct PudicaState {
  mutex history_mutex;
  atomic<double> current_bitrate{50.0};     // in Mbps
  atomic<double> pacing_multiplier_p{1.25}; // pacing multiplier p
  atomic<double> fallback_rate_mbps{0.0};
  atomic<uint64_t> d_min{UINT64_MAX};

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
    auto frame_start_time = steady_clock::now();

    double bitrate = global_state.current_bitrate.load(); // current bitrate
    double p = global_state.pacing_multiplier_p.load();

    // frame size in packets [ 10^6 / 8 = 125 * 10^3 ]
    uint32_t frame_bytes = (bitrate * 1000.0 * 125.0) / 60.0;
    uint32_t total_packets = (frame_bytes / LOAD_SZ) + 1;

    // sensible period = L/p
    double sensible_period = INTERVAL / p;
    double packet_interval = sensible_period / total_packets;

    // send frames' packets
    for (uint32_t id = 0; id < total_packets; id++) {
      PktHeader header{};
      header.frame_id = current_frame_id;
      header.packet_id = id;
      header.send_time = now();

      if (id == 0)
        header.flags |= IS_FIRST;
      if (id == total_packets - 1)
        header.flags |= IS_LAST;

      // dummy payload and send
      uint8_t buffer[LOAD_SZ] = {0};
      memcpy(buffer, &header, sizeof(PktHeader));
      sendto(sock, buffer, sizeof(buffer), 0, (sockaddr *)&client_addr, client_len);

      std::this_thread::sleep_for(microseconds(static_cast<int>(packet_interval)));
    }

    // agnostic period probes
    double agnostic_period = INTERVAL - sensible_period;
    double probe_interval = agnostic_period/5.0;

    for (uint32_t i=0; i<4; i++) {
      std::this_thread::sleep_for(microseconds(static_cast<int>(probe_interval)));

      PktHeader probe_hdr{};
      probe_hdr.frame_id = current_frame_id;
      probe_hdr.packet_id = UINT64_MAX - i; // high id so that it does not get confused with data packets
      probe_hdr.flags = IS_PROBE;
      probe_hdr.send_time = now();

      int s = sendto(sock, &probe_hdr, sizeof(PktHeader), 0, (sockaddr *)&client_addr, client_len);
      if (s<0) {
        cerr << "[sender] send err: " << strerror(errno) << "\n";
        continue;
      }
    }

    // sleep until the next 16.67ms
    current_frame_id++;
    std::this_thread::sleep_until(frame_start_time + microseconds(INTERVAL));
  }
}

// to measure the Bandwidth Utilization Ratio
void listener_thread(int sock) {
  uint8_t buf[MAX_BUF];
  int congested_frames = 0;

  struct FrameProgress {
    double frame_D_sec = 0;
    vector<double> probe_delays;
  };
  map<uint32_t, FrameProgress> in_flight_frames;

  while(true) {
    ssize_t n = recvfrom(sock, buf, sizeof(buf), 0, nullptr, nullptr);
    if (n < sizeof(RecvACK)) continue;

    RecvACK* ack = reinterpret_cast<RecvACK*>(buf);
    uint32_t fid = ack->frame_id;

    // one-way delay (microseconds) and Dmin
    uint64_t one_way_delay = ack->recv_time - ack->echoed_send;
    if (one_way_delay < global_state.d_min.load())
      global_state.d_min.store(one_way_delay);

    double current_Dmin = global_state.d_min.load() / 1000000.0; // in secs
    double pkt_D = one_way_delay / 1000000.0; // in sec

    bool is_probe = (ack->flags & IS_PROBE);
    bool is_last  = (ack->flags & IS_LAST);
    
    if (is_probe) {
      double T_i = pkt_D - current_Dmin;
      if (T_i > 0) in_flight_frames[fid].probe_delays.push_back(T_i);
    } else if (is_last) {
      in_flight_frames[fid].frame_D_sec = pkt_D;
    }

    if (in_flight_frames[fid].frame_D_sec > 0 && in_flight_frames[fid].probe_delays.size() == 4) {
      // Calculate BUR
      double raw_R = PudicaAlgorithm::raw_BUR(in_flight_frames[fid].frame_D_sec, current_Dmin);
      double R_corrected = PudicaAlgorithm::corrected_BUR(raw_R, in_flight_frames[fid].probe_delays);

      // Update Pacing Multiplier
      global_state.pacing_multiplier_p.store(PudicaAlgorithm::pacing_multiplier(R_corrected));

      // Short-term Reactions
      if (R_corrected > 1.0) {
        congested_frames++;
        if (congested_frames >= 3)
          global_state.current_bitrate.store(ack->rate); // Active draining
        else
          global_state.current_bitrate.store(global_state.current_bitrate.load() * 0.85); // 15% fallback
      } else congested_frames = 0;

      // Long-term AI-MD History Update
      lock_guard<mutex> lock(global_state.history_mutex);
      global_state.history.push_back({R_corrected, global_state.current_bitrate.load()});
      if (global_state.history.size() > 12) global_state.history.pop_front();

      double R_tilde = PudicaAlgorithm::smoothed_BUR(global_state.history, global_state.current_bitrate.load());
      global_state.frames++;
      double next_B = PudicaAlgorithm::calculate_next_bitrate(global_state.current_bitrate.load(), R_tilde, global_state.frames);
      
      global_state.current_bitrate.store(next_B);
      in_flight_frames.erase(fid);
    }
  }
}
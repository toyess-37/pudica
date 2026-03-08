#include <iostream>
#include <cstring>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <vector>
#include <deque>
#include <sys/socket.h>
#include <netinet/in.h>

#include "protocol.h"
#include "pudica_algo.h"

using namespace std;
using namespace std::chrono;

uint64_t now() {
  return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
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

void tx_loop(int sock, sockaddr_in client_addr) {
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
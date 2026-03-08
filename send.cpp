#include <iostream>
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
  atomic<double> current_bitrate{50.0};       // in Mbps 
  atomic<double> pacing_multiplier_p{1.25};   // pacing multiplier p  
  atomic<uint64_t> d_min{UINT64_MAX};       

  // queue draining flags for BUR > 1 stages
  atomic<bool> is_draining{false};
  atomic<double> fallback_rate_mbps{0.0};

  uint32_t frames = 0;

  // Sliding window of past 200ms
  mutex history_mutex;
  deque<PudicaAlgorithm::HistorySample> history; 
};

PudicaState g_state;


#include <iostream>
#include <cstring>
#include <cerrno>
#include <cstdlib>
#include <thread>
#include <vector>
#include <deque>
#include <mutex>
#include <cmath>
#include <atomic>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <chrono>
#include "protocol.h"

using namespace std;
using namespace std::chrono;

// time in nanoseconds
uint64_t now() {
  return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

// panic for error messages
void panic(const char* msg) {
  perror(msg);
  exit(1);
}

// current status of the pipe
struct State {
  atomic<double> d_min{1e9};
  atomic<uint64_t> last_dmin{0}; // last update of dmin
  atomic<double> bur{0.0};
  atomic<int> pacing_delay{50};
};

State st;

// to measure the Bandwidth Utilization ratio
void listen_thread(int fd) {
  char buf[2048];
  
  while(1) {
    uint64_t current_time = now();
    uint64_t last_update = st.last_dmin.load();

    if (current_time - last_update > 10000000000ULL) {
      st.d_min.store(1e9);
    }

    int n = recv(fd, buf, sizeof(buf), 0);

    if (n < 0) {
      cerr << "[sender] recv err: " << strerror(errno) << "\n";
      continue;
    }

    if (n > 0) {
      if (n < sizeof(Header)) {
        cerr << "[sender] small packet\n";
        continue;
      }

      Header* h = (Header*)buf;

      double delay =(double)(h->ts_recv - h->ts_sent);

      double cur = st.d_min.load();
      if (delay < cur) st.d_min.store(delay);

      double L = 16666666.0;
      double b = (delay - st.d_min.load()) / L;
      st.bur.store(b);
    }
  }
}

struct Pkt {
  Header h;
  char data[LOAD_SZ];
};

deque<Pkt> wheel;
mutex wheel_mx;

void pacer_thread(int fd, sockaddr_in dst) {
  auto tick = microseconds(1000);

  while(1) {
    wheel_mx.lock();
      
    while(!wheel.empty()) {
      Pkt p = wheel.front();
          
      wheel.pop_front();
      wheel_mx.unlock();

      int s = sendto(fd, &p.h, sizeof(p.h) + LOAD_SZ, 0, (sockaddr*)&dst, sizeof(dst));
      if (s<0) {
        cerr << "[sender] send err: " << strerror(errno) << "\n";
        continue;
      }
      wheel_mx.lock();
      int sleep_time = st.pacing_delay.load();
      if (sleep_time > 0) usleep(sleep_time);
    }
    wheel_mx.unlock();
      
    this_thread::sleep_for(tick);
  }
}

int main() {
  // socket
  int fd = socket(AF_INET, SOCK_DGRAM, 0);

  // error handling
  if (fd < 0) panic("[sender] socket fail");

  sockaddr_in dst{};
  dst.sin_family = AF_INET;
  dst.sin_port = htons(8080);
    
  int ret = inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr);
  if (ret <= 0) panic("[sender] inet_pton");

  // Launch Threads
  try {
    thread t1(listen_thread,fd);
    thread t2(pacer_thread,fd,dst);
    
    t1.detach();
    t2.detach();
  } catch (...) {
    panic("[sender] thread fail");
  }

  uint32_t frame_id = 0;
  double rate = 10.0;

  while(1) { // simulating 60 fps game
    auto t_start = steady_clock::now();

    double r = st.bur.load();

    // Logic of algorithm
    if (r <= 0.85) { rate *= 1.05; } 
    else if (r <= 1.0) { rate += 0.1; } 
    else { rate *= 0.7; }

    if(rate>50) rate=50;
    if(rate<1) rate=1;
    
    double m = 1.25 / min(r, 1.0);
    double bits = (rate * 1e6) * 0.01666;
    int bytes = (int)(bits / 8);
    int n_pkts = bytes/LOAD_SZ;
    if(n_pkts<1) n_pkts=1;

    double total_time = 16666.66; // microseconds
    int gap = (int)((total_time / n_pkts) / m);
    st.pacing_delay.store(gap);

    wheel_mx.lock();
    // send packets
    for(int i=0; i<n_pkts; i++) {     
      Pkt p;
      p.h.seq = i;
      p.h.frame_id = frame_id;
      p.h.ts_sent =
      now();
      
      wheel.
      push_back(p);
    }
    wheel_mx.unlock();
    frame_id++;

    auto t_end = steady_clock::now();
    auto elap = duration_cast<microseconds>(t_end-t_start).count();
    if (elap < INTERVAL) usleep(INTERVAL - elap);
  }

  return 0;
}
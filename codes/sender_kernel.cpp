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

void panic(const char *msg) {
  perror(msg);
  exit(1);
}

uint64_t now() {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return static_cast<uint64_t>(t.tv_nsec + 1000000000ULL*t.tv_sec);
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
  if (setsockopt(sock, SOL_SOCKET, SO_TXTIME, &txt, sizeof(txt)) < 0) {
    panic("[sender] SO_TXTIME failed");
  }

  sockaddr_in dest{};
  dest.sin_family = AF_INET;
  dest.sin_port = htons(port);

  if (inet_pton(AF_INET, ip.c_str(), &dest.sin_addr) <= 0) {
    panic("[sender] Invalid IP address format");
  }

  // thread t1(pacer_thread, sock, dest);
  // thread t2(listener_thread, sock);

  // t1.join();
  // t2.join();

  return 0;
}
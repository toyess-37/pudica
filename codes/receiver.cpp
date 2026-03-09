#include <iostream>
#include <cstring>
#include <chrono>
#include <vector>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "protocol.h"

using namespace std;
using namespace std::chrono;

// time in microseconds
uint64_t now() {
  return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

// panic for error
void panic(const char* msg) {
  perror(msg);
  exit(1);
}

// for calculating recv rate
auto last_calc = now();
uint32_t bytes_accumulated = 0;
double current_recv_rate = 0.0; // in Mbps

int main(int argc, char *argv[]) {
  if (argc != 2) {
    cerr << "Usage: " << argv[0] << " <port>\n";
    return 1;
  }
  int port = stoi(argv[1]);

  int sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) panic("[receiver] socket creation failed");
  
  sockaddr_in server_addr{};
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = INADDR_ANY;
  server_addr.sin_port = htons(port);

  if (bind(sock, (sockaddr*)&server_addr, sizeof(server_addr)) < 0)
    panic("[receiver] bind fail");
  
  cout << "Receiver on " << port << "\n";

  uint8_t buf[MAX_BUF];
  sockaddr_in client_addr{};
  socklen_t len = sizeof(client_addr);

  while(true) {
    ssize_t n = recvfrom(sock, buf, sizeof(buf), 0, (sockaddr*)&client_addr, &len);
    if (n<0) {
      cerr << "[receiver] recv error: " << strerror(errno) << "\n";
      continue;
    }

    auto recv_ts = now();
    bytes_accumulated += n;
    uint64_t elapsed_time = recv_ts - last_calc;

    if (elapsed_time >= 100000) { // after 100ms redo the rate calculation
      current_recv_rate = (8.0*bytes_accumulated)/elapsed_time;
      bytes_accumulated = 0;
      last_calc = recv_ts;
    }

    if (n < sizeof(PktHeader)) {
      cerr << "[receiver] small packet\n";
      continue;
    }

    PktHeader header;
    memcpy(&header, buf, sizeof(PktHeader));

    RecvACK ack{};
    ack.echoed_send = header.send_time;
    ack.recv_time = recv_ts;
    ack.frame_id = header.frame_id;
    ack.packet_id = header.packet_id;
    ack.rate = current_recv_rate;
    ack.flags = header.flags;

    int s = sendto(sock, &ack, sizeof(RecvACK), 0, (sockaddr*)&client_addr, len);
    if (s < 0) {
      cerr << "[receiver] send error: " << strerror(errno) << "\n";
      continue;
    };
  }
}
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

int main(int argc, char *argv[]) {
  int port = std::stoi(argv[1]);

  int sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) panic("[receiver] socket creation failed");
  
  sockaddr_in server_addr{};
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = INADDR_ANY;
  server_addr.sin_port = htons(port);

  if (bind(sock, (sockaddr*)&server_addr, sizeof(server_addr)) < 0)
    panic("[receiver] bind fail");
  
  cout << "Receiver on " << server_addr.sin_port << "\n";

  uint8_t buf[MAX_RECV_BUF];
  sockaddr_in client_addr{};
  socklen_t len = sizeof(client_addr);

  while(1) {
    int n = recvfrom(sock, buf, sizeof(buf), 0, (sockaddr*)&client_addr, &len);
    if (n<0) {
      cerr << "[receiver] recv error: " << strerror(errno) << "\n";
      continue;
    }

    auto recv_ts = now();

    if (n < sizeof(PktHeader)) {
      cerr << "[receiver] small packet\n";
      continue;
    }

    PktHeader* header;
    memcpy(&header, buf, sizeof(PktHeader));

    RecvACK ack{};
    ack.echoed_send = header->send_time;
    ack.recv_time = recv_ts;
    ack.frame_id = header->frame_id;
    ack.packet_id = header->packet_id;

    // TODO
    ack.rate = 0.0;

    int s = sendto(sock, &ack, sizeof(RecvACK), 0, (sockaddr*)&client_addr, len);
    if (s < 0) {
      cerr << "[receiver] send error: " << strerror(errno) << "\n";
      continue;
    };
  }
}
#include <iostream>
#include <cstring>
#include <chrono>
#include <vector>
#include <atomic>
#include <thread>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "protocol.h"

using namespace std;
using namespace std::chrono;

// time in microseconds
uint64_t now_microsecs() {
  return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

class PudicaReceiver {
private:
  int sock = -1;
  sockaddr_in server_addr{};
  atomic<bool> running{false};

  uint64_t last_calc;
  uint32_t bytes_acc = 0;
  double recv_rate = 0.0; // in Mbps

public:
  PudicaReceiver(int port) {
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock<0) throw runtime_error("[receiver] socket creation failed");

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, (sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
      close(sock);
      throw runtime_error("[receiver] bind failed (port might already be in use)");
    }

    last_calc = now_microsecs();
  }

  ~PudicaReceiver() {
    running = false;
    if (sock>=0) close(sock);
  }

  void run() {
    running = true;
    cout << "[receiver] Listening on port " << ntohs(server_addr.sin_port) << "\n";

    uint8_t buf[MAX_BUF];
    sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);

    while(running) {
      ssize_t n = recvfrom(sock, buf, sizeof(buf), 0, (sockaddr*)&client_addr, &client_len);
      if (n<0) {
        cerr << "[receiver] recv error: " << strerror(errno) << "\n";
        continue;
      }
      cout << "[receiver] got " << n << " bytes\n";

      uint64_t recv_ts = now_microsecs();

      if (n < static_cast<ssize_t>(sizeof(PktHeader))) {
        cerr << "[receiver] small packet dropped\n";
        continue;
      }
      
      bytes_acc += n;

      uint64_t elapsed = recv_ts - last_calc;
      // recalculate every 100ms
      if (elapsed >= 100000) { 
        recv_rate = (8.0 * bytes_acc) / elapsed;
        bytes_acc = 0;
        last_calc = recv_ts;
      }

      PktHeader* hdr = reinterpret_cast<PktHeader*>(buf);

      RecvACK ack{};
      ack.echoed_send = hdr->send_time;
      ack.recv_time = recv_ts;
      ack.rate = recv_rate;
      ack.frame_id = hdr->frame_id;
      ack.packet_id = hdr->packet_id;
      ack.flags = hdr->flags;

      int s = sendto(sock, &ack, sizeof(RecvACK), 0, (sockaddr*)&client_addr, client_len);
      if (s<0) cerr << "[receiver] send error: " << strerror(errno) << "\n";
    }
  }
};

int main(int argc, char *argv[]) {
  if (argc != 2) {
    cerr << "Usage: " << argv[0] << " <port>\n";
    return 1;
  }

  try {
    int port = stoi(argv[1]);
    PudicaReceiver receiver(port);
    receiver.run();
  } catch (const exception& e) {
    cerr << "[recv] Error: " << e.what() << "\n";
    return 1;
  }

  return 0;
}
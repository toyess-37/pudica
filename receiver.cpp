#include <iostream>
#include <cstring>
#include <cerrno>
#include <cstdlib>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <chrono>
#include "protocol.h"

using namespace std;
using namespace std::chrono;

// time in nanoseconds
uint64_t now() {
  return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

// panic for error
void panic(const char* msg) {
  perror(msg);
  exit(1);
}

int main() {
  // socket
  int fd = socket(AF_INET, SOCK_DGRAM, 0);

  // error handling
  if (fd < 0) panic("[receiver] socket fail");

  // bind addr
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(8080);

  if (bind(fd, (sockaddr*)&addr, sizeof(addr)) < 0)
    panic("bind fail");

  cout << "Receiver on " << addr.sin_port << "\n";

  // buffer
  char buf[2048];

  sockaddr_in cli{};
  socklen_t len = sizeof(cli);

  while(1) {
    // receive
    int n = recvfrom(fd, buf, 2048, 0, (sockaddr*)&cli, &len);

    if (n < 0) {
      cerr << "[receiver] recv error: " << strerror(errno) << "\n";
      continue;
    }

    if (n > 0) {
      if (n < sizeof(Header)) {
        cerr << "[receiver] small packet\n";
        continue;
      }

      Header *h = (Header *)buf;
      h->ts_recv = now();
      int s = sendto(fd, buf, n, 0, (sockaddr *)&cli, len);
      if (s < 0) {
        cerr << "[receiver] send error: " << strerror(errno) << "\n";
        continue;
      };
    }
  }

  return 0;
}
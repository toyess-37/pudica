#include<iostream>
#include<cstring>
#include<cstdlib>
#include<thread>
#include<atomic>
#include<sys/socket.h>
#include<arpa/inet.h>
#include<chrono>

using namespace std;
using namespace std::chrono;

uint64_t now() {
  return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

void panic(const char* msg) {
  perror(msg);
  exit(1);
}


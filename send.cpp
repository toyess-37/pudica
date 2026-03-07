#include <iostream>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "protocol.h"

using namespace std;
using namespace std::chrono;

uint64_t now()
{
  return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

void panic(const char *msg)
{
  perror(msg);
  exit(1);
}
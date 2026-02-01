#ifndef PROC_H
#define PROC_H

#include <cstdint>

struct __attribute__((packed))
Header {
  uint32_t seq; // sequence no.
  uint32_t frame_id; // frame id
  uint64_t ts_sent; // send time
  uint64_t ts_recv; // receive time
};

// 60 FPS
const int INTERVAL = 16666;

// MTU limit = 1500
// ip header + udp header = 28
// our header = 24
// remaining payload, for safety set at 1400
const int LOAD_SZ = 1400;

#endif
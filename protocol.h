#pragma once

#include<cstdint>

#pragma pack(push, 1)

constexpr uint8_t IS_FIRST = 0x01;
constexpr uint8_t IS_LAST  = 0x02;
constexpr uint8_t IS_PROBE = 0x04;

constexpr uint64_t INTERVAL = 16666; // 60 FPS

/*
  MTU limit = 1500
  ip header + udp header = 28
  our header = 24
  remaining payload, for safety set at 1400
*/
constexpr uint32_t LOAD_SZ = 1400;
constexpr uint32_t MAX_BUF = 2048;

struct PktHeader {
  uint64_t send_time;   // t_0
  uint32_t frame_id;    // frame no.
  uint32_t packet_id;   // seq no. of packet
  uint8_t  flags;       // IS_FIRST, IS_LAST, IS_PROBE
};

struct RecvACK {
  uint64_t echoed_send; // timestamp of sender sent back (save memory)
  uint64_t recv_time;
  double rate;

  uint32_t frame_id;
  uint32_t packet_id;
  uint8_t  flags; // first pkt, last pkt or probe pkt
};

#pragma pack(pop)
#pragma once

#include <cstdint>

namespace pudica_net
{
  enum PacketFlags : uint8_t
  {
    FIRST = 0x01,
    LAST = 0x02,
    PROBE = 0x04,
    FEC = 0x08 // XOR parity packet
  };

  constexpr uint64_t INTERVAL = 28571; // ~35 FPS
  constexpr uint32_t N_PROBE = 4;      // no. of probe packets

  /*
    MTU limit = 1500
    ip header + udp header = 28
    our header = 24
    remaining payload, for safety set at 1400
  */
  constexpr uint32_t LOAD_SZ = 1400;
  constexpr uint32_t MAX_BUF = 2048;

  // XOR FEC: 1 parity packet for every FEC_K data packets within a frame.
  // Receiver can recover any single lost packet within a group of FEC_K.
  constexpr uint32_t FEC_K = 4;

#pragma pack(push, 1)

  struct PktHeader
  {
    uint64_t send_time;
    uint32_t frame_id;
    uint32_t packet_id; // seq no. of packet (UINT32_MAX-i for probes (i=1..4), UINT32_MAX-8 for FEC parity)
    uint8_t flags;      // FIRST, or LAST, or PROBE, or FEC
    uint8_t retrans_seq; // ???: me
    uint8_t fec_group;  // ???: me, ai: which FEC group this packet belongs to (data: 0..FEC_K-1; parity: group index)
  };

  struct RecvACK
  {
    uint64_t echoed_send; // timestamp of sender sent back
    uint64_t recv_time;
    double rate; // current receive rate???

    uint32_t frame_id;
    uint32_t packet_id;
    uint8_t flags;
    uint8_t retrans_seq; // me: ???
  };

#pragma pack(pop)

  static_assert(sizeof(PktHeader) == 19, "PktHeader size mismatch; fec_group added");
  static_assert(sizeof(RecvACK) == 34, "RecvACK size mismatch");
}
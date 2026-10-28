#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace pudica_net {
constexpr uint32_t MAGIC = 0x50554432; // PUD2; rebuild both endpoints together.
constexpr uint8_t VERSION = 2;
constexpr uint32_t FPS = 35;
constexpr uint64_t INTERVAL = 1'000'000 / FPS;
constexpr uint32_t N_PROBE = 4;
constexpr uint32_t LOAD_SZ = 1200;
constexpr uint32_t FEC_K = 4;
constexpr uint32_t MAX_FRAME_BYTES = 4 * 1024 * 1024;
constexpr uint64_t FRAME_TIMEOUT = 10 * INTERVAL;
constexpr size_t HEADER_SIZE = 46;
constexpr size_t ACK_SIZE = 56;
constexpr size_t MAX_BUF = HEADER_SIZE + LOAD_SZ;
enum PacketFlags : uint8_t {
  FIRST=1, LAST=2, PROBE=4, FEC=8, RECOVERED=16, STREAM_INFO=32,
  FRAME_COMPLETE=64, KEYFRAME=128
};
// ACK direction only: receiver asks for an IDR after losing decoder state.
constexpr uint8_t KEY_REQUEST = STREAM_INFO;
struct PktHeader {
  uint8_t flags=0;
  uint16_t retrans_seq=0;
  uint64_t session=0, send_time=0, frame_time=0;
  uint32_t frame_id=0, packet_id=0, frame_bytes=0;
  uint16_t fec_group=0;
};
struct RecvACK {
  uint8_t flags=0;
  uint16_t retrans_seq=0;
  uint64_t session=0, echoed_send=0, recv_time=0, frame_recv_time=0, wire_bytes=0;
  uint32_t frame_id=0, packet_id=0;
};
struct StreamInfo { uint32_t width=0, height=0; };
inline uint32_t packet_count(uint32_t bytes) { return (bytes + LOAD_SZ - 1) / LOAD_SZ; }
inline uint32_t parity_count(uint32_t packets) { return packets / FEC_K + ((packets % FEC_K) > 1); }
// UDP payload bytes the receiver counts in wire_bytes for one frame:
// padded data and parity packets plus header-only probes.
inline uint64_t frame_udp_bytes(uint32_t frame_bytes) {
  const uint32_t packets = packet_count(frame_bytes);
  return uint64_t(packets + parity_count(packets)) * MAX_BUF + uint64_t(N_PROBE) * HEADER_SIZE;
}
// Explicit big-endian integers avoid packed native structs and native doubles.
inline void put(std::vector<uint8_t>& out, uint64_t value, size_t bytes) {
  for (size_t i=bytes; i>0; --i) out.push_back(static_cast<uint8_t>(value >> ((i-1)*8)));
}
inline uint64_t get(const uint8_t*& in, size_t bytes) {
  uint64_t value=0;
  while (bytes--) value=(value<<8)|*in++;
  return value;
}
inline std::vector<uint8_t> encode(const PktHeader& h) {
  std::vector<uint8_t> b; b.reserve(MAX_BUF);
  put(b,MAGIC,4); put(b,VERSION,1); put(b,h.flags,1); put(b,h.retrans_seq,2);
  put(b,h.session,8); put(b,h.send_time,8); put(b,h.frame_time,8);
  put(b,h.frame_id,4); put(b,h.packet_id,4); put(b,h.frame_bytes,4); put(b,h.fec_group,2);
  return b;
}
inline std::vector<uint8_t> encode(const RecvACK& a) {
  std::vector<uint8_t> b; b.reserve(ACK_SIZE);
  put(b,MAGIC,4); put(b,VERSION,1); put(b,a.flags,1); put(b,a.retrans_seq,2);
  put(b,a.session,8); put(b,a.echoed_send,8); put(b,a.recv_time,8);
  put(b,a.frame_recv_time,8); put(b,a.wire_bytes,8); put(b,a.frame_id,4); put(b,a.packet_id,4);
  return b;
}
inline bool decode(const uint8_t* b, size_t n, PktHeader& h) {
  if(n<HEADER_SIZE || get(b,4)!=MAGIC || get(b,1)!=VERSION) return false;
  h.flags=get(b,1); h.retrans_seq=get(b,2); h.session=get(b,8);
  h.send_time=get(b,8); h.frame_time=get(b,8); h.frame_id=get(b,4);
  h.packet_id=get(b,4); h.frame_bytes=get(b,4); h.fec_group=get(b,2);
  if(!h.session || h.send_time>INT64_MAX || h.frame_time>INT64_MAX) return false;
  if(h.flags & STREAM_INFO) return h.flags==STREAM_INFO && n==HEADER_SIZE+8;
  if(!h.frame_id || h.frame_id==UINT32_MAX || !h.send_time || !h.frame_time || !h.frame_bytes || h.frame_bytes>MAX_FRAME_BYTES) return false;
  const uint32_t count=packet_count(h.frame_bytes);
  if(h.flags & PROBE) return h.flags==PROBE && h.packet_id<N_PROBE && n==HEADER_SIZE;
  if(h.flags & FEC) return h.flags==FEC && h.fec_group<(count+FEC_K-1)/FEC_K &&
      n==MAX_BUF && count-h.fec_group*FEC_K>1;
  if(h.flags & ~(FIRST|LAST|KEYFRAME)) return false;
  return h.packet_id<count && h.fec_group==h.packet_id/FEC_K && n==MAX_BUF &&
      bool(h.flags&FIRST)==(h.packet_id==0) && bool(h.flags&LAST)==(h.packet_id+1==count);
}
inline bool decode(const uint8_t* b, size_t n, RecvACK& a) {
  if(n!=ACK_SIZE || get(b,4)!=MAGIC || get(b,1)!=VERSION) return false;
  a.flags=get(b,1); a.retrans_seq=get(b,2); a.session=get(b,8);
  a.echoed_send=get(b,8); a.recv_time=get(b,8); a.frame_recv_time=get(b,8);
  a.wire_bytes=get(b,8); a.frame_id=get(b,4); a.packet_id=get(b,4);
  if(a.flags==KEY_REQUEST) return a.session && a.frame_id && a.recv_time &&
      a.recv_time<=INT64_MAX && !a.echoed_send && !a.frame_recv_time && !a.retrans_seq;
  if(!a.session || !a.frame_id || !a.recv_time || a.recv_time>INT64_MAX ||
      a.echoed_send>INT64_MAX || (a.flags&(FEC|STREAM_INFO))) return false;
  if(a.flags&FRAME_COMPLETE) {
    if(!a.frame_recv_time || a.frame_recv_time>a.recv_time) return false;
  } else if(a.frame_recv_time) return false;
  if(a.flags&PROBE) return !(a.flags&~(PROBE|FRAME_COMPLETE)) &&
      a.packet_id<N_PROBE && !a.retrans_seq && a.echoed_send;
  if(a.flags&RECOVERED) return !(a.flags&~(RECOVERED|FRAME_COMPLETE)) &&
      !a.echoed_send && !a.retrans_seq;
  return !(a.flags&~(FIRST|LAST|KEYFRAME|FRAME_COMPLETE)) && a.echoed_send;
}
}

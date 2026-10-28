#pragma once
#include "protocol.h"
#include <algorithm>
#include <map>
#include <optional>

namespace pudica_net {
struct SentPacket {
  uint64_t sent_at=0, first_try_recv=0; // receiver clock; 0 unless arrived on attempt 0
  uint16_t attempt=0;
  bool acked=false, retry_queued=false;
};
struct SentFrame {
  uint32_t id=0;
  uint64_t created_at=0, first_sent=0, delivered_at=0, probes_sent_at=0, captured_at=0;
  uint32_t epoch=0;
  double target_mbps=0, probe_gap_sec=0;
  bool keyframe=false, all_sent=false, delivered=false, sampled=false, loss=false;
  std::vector<uint8_t> bytes;
  std::vector<SentPacket> packets;
  std::array<std::optional<RecvACK>,N_PROBE> probes;
  bool all_acked() const {
    return std::all_of(packets.begin(),packets.end(),[](const auto& p){return p.acked;});
  }
  size_t probe_count() const {
    return std::count_if(probes.begin(),probes.end(),[](const auto& p){return p.has_value();});
  }
  // Delivery and feedback completion have separate lifetimes.
  bool resolved() const { return delivered && sampled; }
  bool acknowledge(const RecvACK& a) {
    if(a.flags&RECOVERED) loss=true;
    if(a.flags&PROBE) {
      if(a.packet_id<N_PROBE && !probes[a.packet_id]) probes[a.packet_id]=a;
    } else if(a.packet_id<packets.size()) {
      auto& p=packets[a.packet_id];
      if(!(a.flags&RECOVERED) && a.retrans_seq==0 && !p.first_try_recv) p.first_try_recv=a.recv_time;
      packets[a.packet_id].acked=true;
      packets[a.packet_id].retry_queued=false;
    }
    if(a.flags&FRAME_COMPLETE) {
      if(!delivered) delivered_at=a.frame_recv_time;
      delivered=true;
      for(auto& p:packets) { p.acked=true; p.retry_queued=false; }
    }
    return !sampled && delivered && probe_count()==N_PROBE;
  }
};
struct ReceivedFrame {
  uint32_t size;
  uint64_t first_seen, frame_time, completed_at=0;
  bool keyframe;
  size_t received=0;
  std::vector<std::vector<uint8_t>> payloads;
  std::map<uint16_t,std::vector<uint8_t>> parity;
  ReceivedFrame(const PktHeader& h,uint64_t now)
      :size(h.frame_bytes),first_seen(now),frame_time(h.frame_time),
       keyframe(h.flags&KEYFRAME),payloads(packet_count(size)) {}
  bool matches(const PktHeader& h) const {return size==h.frame_bytes && frame_time==h.frame_time;}
  bool complete() const {return received==payloads.size();}
  bool add(uint32_t pid,const uint8_t* data) {
    if(pid>=payloads.size() || !payloads[pid].empty()) return false;
    payloads[pid].assign(data,data+LOAD_SZ); ++received; return true;
  }
  std::optional<uint32_t> recover(uint16_t group) {
    auto it=parity.find(group);
    if(it==parity.end()) return {};
    uint32_t start=group*FEC_K;
    uint32_t end=std::min<uint32_t>(start+FEC_K,payloads.size());
    uint32_t missing=0, pid=0;
    for(uint32_t i=start;i<end;++i) if(payloads[i].empty()){++missing;pid=i;}
    if(missing!=1) return {};
    auto data=it->second;
    for(uint32_t i=start;i<end;++i) if(i!=pid)
      for(uint32_t b=0;b<LOAD_SZ;++b) data[b]^=payloads[i][b];
    add(pid,data.data());
    parity.erase(it);
    return pid;
  }
  std::vector<uint8_t> assemble() const {
    if(!complete()) throw std::logic_error("assembling incomplete frame");
    std::vector<uint8_t> out; out.reserve(size);
    for(const auto& p:payloads)
      out.insert(out.end(),p.begin(),p.begin()+std::min<size_t>(p.size(),size-out.size()));
    return out;
  }
};
}

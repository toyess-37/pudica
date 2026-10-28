#include "transport.h"
#include <cassert>
#include <iostream>
using namespace pudica_net;
int main() {
  for(uint32_t count:{1u,2u,3u,4u,5u,9u}) {
    for(uint32_t missing=0;missing<count;++missing) {
      PktHeader header;header.frame_bytes=count*LOAD_SZ-17;header.session=7;header.frame_time=10;
      ReceivedFrame frame(header,20);
      std::vector<std::vector<uint8_t>> packets(count,std::vector<uint8_t>(LOAD_SZ));
      for(uint32_t i=0;i<count;++i) for(uint32_t b=0;b<LOAD_SZ;++b)packets[i][b]=static_cast<uint8_t>(i+b);
      uint32_t group=missing/FEC_K,start=group*FEC_K,end=std::min(count,start+FEC_K);
      if(end-start>1) {
        auto& parity=frame.parity[group];parity.assign(LOAD_SZ,0);
        for(uint32_t i=start;i<end;++i)for(uint32_t b=0;b<LOAD_SZ;++b)parity[b]^=packets[i][b];
      }
      // Parity arrives first; test both boundaries and every short final group.
      for(uint32_t i=0;i<count;++i) if(i!=missing){frame.add(i,packets[i].data());frame.recover(i/FEC_K);}
      if(end-start==1) assert(!frame.complete());
      else {assert(frame.complete());assert(frame.payloads[missing]==packets[missing]);}
      frame.add(missing,packets[missing].data());
      assert(frame.complete());assert(frame.assemble().size()==header.frame_bytes);
      size_t received=frame.received;frame.add(missing,packets[missing].data());assert(frame.received==received);
    }
  }
  SentFrame frame;frame.packets.resize(2);
  RecvACK ack;ack.packet_id=0;ack.flags=FIRST;ack.retrans_seq=0;
  frame.packets[0].attempt=2;frame.acknowledge(ack);assert(frame.packets[0].acked);
  ack.flags=LAST|FRAME_COMPLETE;ack.packet_id=1;ack.frame_recv_time=100;
  frame.acknowledge(ack);assert(frame.delivered&&!frame.resolved());
  for(uint32_t i=0;i<N_PROBE;++i){ack.flags=PROBE;ack.packet_id=i;frame.acknowledge(ack);frame.acknowledge(ack);}
  assert(frame.probe_count()==N_PROBE);frame.sampled=true;assert(frame.resolved());
  PktHeader header{FIRST|LAST,0,42,11,10,1,0,25,0};
  auto encoded=encode(header);encoded.resize(MAX_BUF,0);PktHeader decoded;
  assert(decode(encoded.data(),encoded.size(),decoded)&&decoded.session==42&&decoded.frame_bytes==25);
  assert(!decode(encoded.data(),HEADER_SIZE-1,decoded));
  encoded[4]=1;assert(!decode(encoded.data(),encoded.size(),decoded));
  ack.session=42;ack.frame_id=1;ack.echoed_send=11;ack.recv_time=120;ack.frame_recv_time=0;encoded=encode(ack);RecvACK decoded_ack;
  assert(encoded.size()==ACK_SIZE&&decode(encoded.data(),encoded.size(),decoded_ack));
  // Matches what the receiver adds to wire_bytes: padded data + parity + probes.
  assert(frame_udp_bytes(1)==MAX_BUF+N_PROBE*HEADER_SIZE);
  assert(frame_udp_bytes(4*LOAD_SZ)==5*MAX_BUF+N_PROBE*HEADER_SIZE);
  assert(frame_udp_bytes(5*LOAD_SZ)==6*MAX_BUF+N_PROBE*HEADER_SIZE);
  assert(frame_udp_bytes(6*LOAD_SZ)==8*MAX_BUF+N_PROBE*HEADER_SIZE);
  RecvACK request;request.flags=KEY_REQUEST;request.session=42;request.frame_id=9;request.recv_time=5;
  encoded=encode(request);assert(decode(encoded.data(),encoded.size(),decoded_ack)&&decoded_ack.flags==KEY_REQUEST);
  request.echoed_send=1;encoded=encode(request);assert(!decode(encoded.data(),encoded.size(),decoded_ack));
  std::cout<<"[PASS] transport framing, boundary FEC, parity-first, deduplication, late ACKs\n";
}

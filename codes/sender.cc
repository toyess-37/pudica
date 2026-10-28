#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <cmath>
#include <cerrno>
#include <csignal>
#include <getopt.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "transport.h"
#include "pudica_algo.h"
#include "../cloud_gaming/capture/capture.h"
#include "../cloud_gaming/capture/encoder.h"

using namespace pudica_net;
using namespace std::chrono;
static uint64_t now_us() {
  return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}
static volatile sig_atomic_t interrupted=0;
static void signal_stop(int){interrupted=1;}
struct EncodedFrame {
  std::vector<uint8_t> bytes;
  double target;
  bool keyframe;
  uint32_t epoch=0;      // controller rate epoch the frame was encoded in
  uint64_t captured_at=0;
};

class Sender {
  int sock=-1;
  uint64_t session;
  bool synthetic;
  std::atomic<bool> running{false}, producing{false}, failed{false}, force_keyframe{false};
  std::atomic<double> bitrate, pacing{1.25};
  std::thread capture_thread,pacer_thread,listener_thread,keyboard_thread;
  std::mutex mutex;
  std::condition_variable available;
  std::deque<EncodedFrame> frames;
  std::deque<std::pair<uint32_t,uint32_t>> retries;
  std::map<uint32_t,SentFrame> inflight;
  Logger logger;
  PudicaAlgorithm::Controller ctrl;
  uint32_t next_id=1;
  double srtt_us=0, rttvar_us=0, ack_rate=0;
  uint64_t retry_delay_us=100'000;
  int64_t min_owd=INT64_MAX;
  std::deque<std::pair<uint64_t,int64_t>> owd_samples;
  std::deque<std::pair<uint64_t,uint64_t>> wire_samples;
  bool retrans_loss_seen=false, limiting=false;
  uint32_t width=0,height=0;
  int64_t min_rtt=INT64_MAX;
  std::deque<std::pair<uint64_t,int64_t>> rtt_samples;
  steady_clock::time_point pace_next{}; // pacer thread only
  uint64_t last_key_request=0;

  void send_bytes(const std::vector<uint8_t>& b) {
    ssize_t n=::send(sock,b.data(),b.size(),0);
    // A connected UDP socket reports earlier ICMP port-unreachable here; the
    // receiver may simply not be up yet, so treat it as a lost datagram.
    if(n<0 && (errno==ECONNREFUSED || errno==ENOBUFS)) return;
    if(n!=static_cast<ssize_t>(b.size()))
      throw std::runtime_error(std::string("UDP send failed: ")+strerror(errno));
  }
  template<class Function> void guarded(Function f) {
    try { f(); }
    catch(const std::exception& e) {
      failed=true; running=false; producing=false; available.notify_all();
      std::cerr<<"[sender] "<<e.what()<<"\n";
      try {logger.log("ERROR",{{"message",Logger::quote(e.what())}});} catch(...) {}
    }
  }
  void store_output(const PudicaAlgorithm::control_output& out) {
    if(out.valid) {bitrate=out.bitrate;pacing=out.pacing;}
  }
  std::pair<uint32_t,uint64_t> outstanding() const {
    uint32_t count=0;uint64_t bytes=0;
    for(const auto& [id,f]:inflight) if(!f.delivered) {
      ++count;
      for(size_t i=0;i<f.packets.size();++i)
        if(!f.packets[i].acked) bytes+=std::min<size_t>(LOAD_SZ,f.bytes.size()-i*LOAD_SZ);
    }
    return {count,bytes};
  }
  size_t pending_retries() const {
    size_t n=0;
    for(const auto& [id,f]:inflight)
      for(const auto& p:f.packets) if(!p.acked && (p.attempt>0||p.retry_queued)) ++n;
    return n;
  }
  void probe_summary(const SentFrame& f) {
    logger.log("PROBE_SUMMARY",f.id,{{"expected",std::to_string(N_PROBE)},
      {"received",std::to_string(f.probe_count())}});
  }
  // Resolve feedback in frame order; each delivered frame yields one controller sample.
  void resolve(uint64_t now) {
    while(!inflight.empty()) {
      auto it=inflight.begin();auto& f=it->second;
      bool expired=now>=f.created_at && now-f.created_at>FRAME_TIMEOUT;
      // A lost probe ACK must not stall feedback for later frames: wait one
      // retransmission timeout after the last probe left, then sample anyway.
      bool probes_ready=f.probe_count()==N_PROBE || (f.probes_sent_at &&
          now>=f.probes_sent_at && now-f.probes_sent_at>retry_delay_us);
      if(!expired && !(f.delivered && probes_ready)) break;
      if(f.delivered && min_owd!=INT64_MAX) {
        std::vector<double> probes;
        for(const auto& p:f.probes) {
          // Missing probes count as fully queued (bounded by T_packet, Eq. 4).
          if(!p) {probes.push_back(f.probe_gap_sec);continue;}
          double queue=std::max(0.0,(static_cast<double>(p->recv_time)-
             static_cast<double>(p->echoed_send)-static_cast<double>(min_owd))/1e6);
          double since_last=p->recv_time>=f.delivered_at?(p->recv_time-f.delivered_at)/1e6:0;
          probes.push_back(std::min({queue,since_last,f.probe_gap_sec}));
        }
        auto [count,bytes]=outstanding();
        // ack_rate counts every UDP payload byte (padding, parity, probes); the
        // controller compares it with encoder bitrates, so convert to payload Mbps.
        const double payload_rate=ack_rate*static_cast<double>(f.bytes.size())/
            static_cast<double>(frame_udp_bytes(static_cast<uint32_t>(f.bytes.size())));
        PudicaAlgorithm::FrameAck fa{f.id,
          (static_cast<int64_t>(f.delivered_at)-static_cast<int64_t>(f.first_sent))/1e6,
          min_owd/1e6,probes,payload_rate,static_cast<double>(bytes),now,count};
        // B_k is what the encoder actually produced; targets can be far off
        // (static scenes, keyframes), and Eq. 6 rectifies by the real size.
        fa.frame_mbps=static_cast<double>(f.bytes.size())*8.0/static_cast<double>(INTERVAL);
        fa.sent_bitrate=fa.frame_mbps;
        fa.lossy=f.loss;
        fa.epoch=f.epoch;
        fa.base_rtt=min_rtt==INT64_MAX?0.0:static_cast<double>(min_rtt)/1e6;
        fa.wall_us=static_cast<uint64_t>(duration_cast<microseconds>(system_clock::now().time_since_epoch()).count());
        fa.ack_first_us=UINT64_MAX;
        for(size_t i=0;i<f.packets.size();++i) if(const uint64_t t=f.packets[i].first_try_recv) {
          fa.ack_bytes+=static_cast<double>(std::min<size_t>(LOAD_SZ,f.bytes.size()-i*LOAD_SZ));
          fa.ack_first_us=std::min(fa.ack_first_us,t);fa.ack_last_us=std::max(fa.ack_last_us,t);
        }
        if(!fa.ack_last_us) fa.ack_first_us=0;
        store_output(ctrl.on_frame_acked(fa));f.sampled=true;
      } else if(!f.delivered) {
        logger.log("FRAME_LOSS",f.id,{{"reason",Logger::quote("delivery_timeout")},
          {"bytes",std::to_string(f.bytes.size())}});
        // Frames that expire together are one loss event: back off once per RTT.
        ctrl.on_frame_loss(now,std::max<uint64_t>(static_cast<uint64_t>(srtt_us),2*INTERVAL));
        bitrate=ctrl.get_final_bitrate();pacing=ctrl.get_pacing();
      }
      probe_summary(f);
      inflight.erase(it);
    }
    const size_t pending=pending_retries();
    if(retrans_loss_seen && pending>=4) limiting=true;
    if(pending<2 && retries.empty()) limiting=false;
  }
  void timers(uint64_t now) {
    for(auto& [id,f]:inflight) {
      if(f.delivered || !f.all_sent) continue;
      for(uint32_t pid=0;pid<f.packets.size();++pid) {
        auto& p=f.packets[pid];
        if(p.acked||p.retry_queued||!p.sent_at||now<p.sent_at||
           now-p.sent_at<retry_delay_us||p.attempt>=8) continue;
        if(p.attempt>0 && !retrans_loss_seen) {
          retrans_loss_seen=true;ctrl.on_retrans_loss_detected();
        }
        p.retry_queued=true; f.loss=true; retries.emplace_back(id,pid);
      }
    }
    auto [count,bytes]=outstanding();
    (void)bytes;
    if(count) {
      auto it=std::find_if(inflight.begin(),inflight.end(),[](const auto& entry){return !entry.second.delivered;});
      store_output(ctrl.on_inflight_age(now-it->second.created_at,min_rtt==INT64_MAX?0:static_cast<uint64_t>(min_rtt)));
    }
    resolve(now);available.notify_all();
  }
  void capture() {
    std::unique_ptr<FrameCapture> screen;
    std::unique_ptr<VideoEncoder> encoder;
    if(!synthetic) {
      screen=std::make_unique<FrameCapture>();
      encoder=std::make_unique<VideoEncoder>(screen->GetWidth(),screen->GetHeight(),FPS,bitrate);
      std::lock_guard<std::mutex> lock(mutex);
      width=screen->GetWidth();height=screen->GetHeight();
    }
    auto deadline=steady_clock::now();
    while(producing && running) {
      deadline+=microseconds(INTERVAL);
      double target;
      bool skip;
      uint32_t epoch=0;
      const uint64_t captured_at=now_us();
      {
        std::lock_guard<std::mutex> lock(mutex);
        uint64_t queued=0;
        auto wire_bytes=[](size_t bytes) {
          const size_t packets=packet_count(bytes);
          const size_t parity=parity_count(static_cast<uint32_t>(packets));
          return (packets+parity)*(MAX_BUF+28)+N_PROBE*(HEADER_SIZE+28);
        };
        for(const auto& f:frames) queued+=wire_bytes(f.bytes.size());
        for(const auto& entry:inflight) {
          for(const auto& packet:entry.second.packets)
            if(!packet.sent_at) queued+=(MAX_BUF+28)*5/4;
        }
        double rate=limiting?std::max(ack_rate,ctrl.get_b_min()):bitrate.load();
        skip=queued*8.0/(rate*1e6)>2*INTERVAL/1e6 || frames.size()>=3;
        target=skip?ctrl.get_final_bitrate():ctrl.take_encoding_bitrate();bitrate=ctrl.get_final_bitrate();
        epoch=ctrl.encoding_epoch();
      }
      if(skip) {
        logger.log("FRAME_SKIPPED",{{"reason",Logger::quote("encoder_backpressure")}});
        std::this_thread::sleep_until(deadline);continue;
      }
      EncodedFrame encoded{{},target,synthetic,epoch,captured_at};
      if(synthetic) {
        size_t bytes=std::max<size_t>(1,static_cast<size_t>(target*1e6*INTERVAL/8e6));
        encoded.bytes.assign(bytes,0x5a);
      } else {
        encoder->SetBitrate(target);
        if(force_keyframe.exchange(false)) encoder->ForceKeyframe();
        auto packets=encoder->Encode(screen->CaptureFrame());
        encoded.keyframe=encoder->WasKeyframe();
        for(const auto& p:packets) encoded.bytes.insert(encoded.bytes.end(),p.begin(),p.end());
      }
      if(!encoded.bytes.empty()) {
        if(encoded.bytes.size()>MAX_FRAME_BYTES) throw std::runtime_error("encoded frame exceeds limit");
        std::lock_guard<std::mutex> lock(mutex);
        frames.push_back(std::move(encoded));available.notify_one();
      }
      std::this_thread::sleep_until(deadline);
      if(deadline<steady_clock::now()-microseconds(INTERVAL)) deadline=steady_clock::now();
    }
  }
  void pace_wire(size_t bytes, double mbps) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      if(limiting) mbps=std::min(mbps,std::max(ack_rate,ctrl.get_b_min()));
    }
    pace_delay(duration<double>(bytes*8.0/(std::max(mbps,0.01)*1e6)));
  }
  // Absolute deadlines: per-sleep overshoot is absorbed by the next wait instead
  // of accumulating across a frame. Idle time is not banked into a later burst.
  void pace_delay(duration<double> gap) {
    const auto now=steady_clock::now();
    if(pace_next<now-microseconds(500)) pace_next=now;
    pace_next+=duration_cast<steady_clock::duration>(gap);
    std::this_thread::sleep_until(pace_next);
  }
  bool retry_one() {
    std::vector<uint8_t> packet;
    double rate=0;
    {
      std::lock_guard<std::mutex> lock(mutex);
      while(!retries.empty()) {
        auto [fid,pid]=retries.front();retries.pop_front();
        auto it=inflight.find(fid);
        if(it==inflight.end()) continue;
        auto& f=it->second;auto& p=f.packets[pid];
        p.retry_queued=false;
        if(p.acked) continue;
        ++p.attempt;p.sent_at=now_us();
        PktHeader h{static_cast<uint8_t>((pid==0?FIRST:0)|
          (pid+1==f.packets.size()?LAST:0)|(f.keyframe?KEYFRAME:0)),
          p.attempt,session,p.sent_at,f.first_sent,fid,pid,
          static_cast<uint32_t>(f.bytes.size()),static_cast<uint16_t>(pid/FEC_K)};
        packet=encode(h);packet.resize(MAX_BUF,0);
        std::copy_n(f.bytes.begin()+pid*LOAD_SZ,
          std::min<size_t>(LOAD_SZ,f.bytes.size()-pid*LOAD_SZ),packet.begin()+HEADER_SIZE);
        rate=limiting?std::max(ack_rate,ctrl.get_b_min()):bitrate.load()*pacing.load();
        logger.log("PACKET_RETRANSMITTED",fid,{{"packet_id",std::to_string(pid)},
          {"attempt",std::to_string(p.attempt)}});
        break;
      }
    }
    if(packet.empty()) return false;
    send_bytes(packet);pace_wire(packet.size()+28,rate);return true;
  }
  void pacer() {
    uint64_t last_info=0;
    while(running) {
      while(running && retry_one()) {}
      uint32_t fid=0,count=0;
      uint64_t started=now_us();
      double target=0,rho=0,wire_rate=0;
      bool key=false;
      std::vector<uint8_t> bytes;
      {
        std::unique_lock<std::mutex> lock(mutex);
        if(!synthetic && width && started-last_info>1'000'000) {
          PktHeader info;info.flags=STREAM_INFO;info.session=session;
          auto b=encode(info);put(b,width,4);put(b,height,4);send_bytes(b);last_info=started;
        }
        if(frames.empty()) {
          available.wait_for(lock,milliseconds(5),[&]{return !running||!frames.empty()||!retries.empty();});
          continue;
        }
        auto queued=std::move(frames.front());frames.pop_front();
        bytes=std::move(queued.bytes);target=queued.target;key=queued.keyframe;
        const uint32_t epoch=queued.epoch;const uint64_t captured_at=queued.captured_at;
        if(next_id==UINT32_MAX) throw std::runtime_error("frame identifier exhausted");
        fid=next_id++;count=packet_count(bytes.size());
        rho=pacing;double sensible=INTERVAL/rho;
        SentFrame f;f.id=fid;f.created_at=started;f.first_sent=now_us();
        f.target_mbps=target;f.keyframe=key;f.bytes=std::move(bytes);f.packets.resize(count);
        f.epoch=epoch;f.captured_at=captured_at;
        f.probe_gap_sec=(INTERVAL-sensible)/(N_PROBE+1)/1e6;
        // Eq. 2: pace at rho x bitrate. A target-sized frame spans the sensible
        // period; an oversized keyframe takes proportionally longer instead of
        // bursting at a multiple of the link rate.
        const uint32_t nominal=packet_count(std::max<uint32_t>(1,static_cast<uint32_t>(target*INTERVAL/8)));
        wire_rate=(nominal+parity_count(nominal))*(MAX_BUF+28)*8.0/(sensible);
        if(limiting) wire_rate=std::min(wire_rate,std::max(ack_rate,ctrl.get_b_min()));
        inflight.emplace(fid,std::move(f));
        logger.log("FRAME_SENT",fid,{{"bytes",std::to_string(inflight.at(fid).bytes.size())},
          {"target_mbps",std::to_string(target)},{"frame_interval_us",std::to_string(INTERVAL)},{"sensible_us",std::to_string(sensible)},
          {"sender_queue_us",std::to_string(captured_at&&started>captured_at?started-captured_at:0)}});
      }
      std::array<uint8_t,LOAD_SZ> parity{};
      for(uint32_t pid=0;pid<count && running;++pid) {
        while(running && retry_one()) {}
        std::vector<uint8_t> packet;
        {
          std::lock_guard<std::mutex> lock(mutex);
          auto it=inflight.find(fid);if(it==inflight.end()) break;
          auto& f=it->second;
          PktHeader h{static_cast<uint8_t>((pid==0?FIRST:0)|(pid+1==count?LAST:0)|(key?KEYFRAME:0)),
            0,session,now_us(),f.first_sent,fid,pid,static_cast<uint32_t>(f.bytes.size()),
            static_cast<uint16_t>(pid/FEC_K)};
          f.packets[pid].sent_at=h.send_time;
          packet=encode(h);packet.resize(MAX_BUF,0);
          std::copy_n(f.bytes.begin()+pid*LOAD_SZ,
            std::min<size_t>(LOAD_SZ,f.bytes.size()-pid*LOAD_SZ),packet.begin()+HEADER_SIZE);
        }
        if(pid%FEC_K==0) parity.fill(0);
        for(uint32_t b=0;b<LOAD_SZ;++b) parity[b]^=packet[HEADER_SIZE+b];
        send_bytes(packet);pace_wire(packet.size()+28,wire_rate);
        if((pid%FEC_K==FEC_K-1||pid+1==count) && pid%FEC_K!=0) {
          PktHeader h;
          {
            std::lock_guard<std::mutex> lock(mutex);
            auto it=inflight.find(fid);if(it==inflight.end()) break;
            h={FEC,0,session,now_us(),it->second.first_sent,fid,0,
              static_cast<uint32_t>(it->second.bytes.size()),static_cast<uint16_t>(pid/FEC_K)};
          }
          auto b=encode(h);b.insert(b.end(),parity.begin(),parity.end());
          send_bytes(b);pace_wire(b.size()+28,wire_rate);
        }
      }
      {
        std::lock_guard<std::mutex> lock(mutex);
        auto it=inflight.find(fid);
        if(it!=inflight.end()) it->second.all_sent=true;
      }
      for(uint32_t i=0;i<N_PROBE && running;++i) {
        PktHeader h;double gap=0;
        {
          std::lock_guard<std::mutex> lock(mutex);
          auto it=inflight.find(fid);if(it==inflight.end()) break;
          gap=it->second.probe_gap_sec;
          h={PROBE,0,session,0,it->second.first_sent,fid,i,
            static_cast<uint32_t>(it->second.bytes.size()),0};
        }
        pace_delay(duration<double>(gap));h.send_time=now_us();
        auto probe=encode(h);send_bytes(probe);pace_wire(probe.size()+28,wire_rate);
      }
      {
        std::lock_guard<std::mutex> lock(mutex);
        auto it=inflight.find(fid);
        if(it!=inflight.end()) it->second.probes_sent_at=now_us();
      }
      // Hold the slot for probes' agnostic period, but start a waiting frame at
      // once: idling here would leave it queued unseen by the congestion signal.
      auto end=steady_clock::time_point(microseconds(started+INTERVAL));
      std::unique_lock<std::mutex> lock(mutex);
      available.wait_until(lock,end,[&]{return !running||!frames.empty()||!retries.empty();});
    }
  }
  void listener() {
    std::array<uint8_t,MAX_BUF+1> buf{};
    while(running) {
      ssize_t n=recv(sock,buf.data(),buf.size(),0);
      uint64_t now=now_us();
      std::lock_guard<std::mutex> lock(mutex);
      RecvACK ack;
      const bool valid_ack=n>0 && decode(buf.data(),static_cast<size_t>(n),ack) && ack.session==session;
      if(valid_ack && ack.flags==KEY_REQUEST) {
        // Receiver lost decoder state; serve at most one IDR per round trip.
        if(!synthetic && (!last_key_request || now-last_key_request>=std::max<uint64_t>(200'000,2*srtt_us))) {
          last_key_request=now;force_keyframe=true;
          logger.log("KEYFRAME_REQUESTED",ack.frame_id,{});
        }
      } else if(valid_ack) {
        auto it=inflight.find(ack.frame_id);
        if(it!=inflight.end()) {
          auto& f=it->second;
          bool valid=(ack.flags&PROBE)?ack.packet_id<N_PROBE:ack.packet_id<f.packets.size();
          if(valid && ack.echoed_send<=now) {
            if(ack.echoed_send && !(ack.flags&RECOVERED)) {
              double rtt=now-ack.echoed_send;
              if(srtt_us==0){srtt_us=rtt;rttvar_us=rtt/2;}
              else {rttvar_us=.75*rttvar_us+.25*std::abs(srtt_us-rtt);srtt_us=.875*srtt_us+.125*rtt;}
              retry_delay_us=std::max<uint64_t>(20'000,srtt_us+4*rttvar_us);
              const int64_t rtt_us=static_cast<int64_t>(rtt);
              while(!rtt_samples.empty() && now-rtt_samples.front().first>10'000'000) rtt_samples.pop_front();
              while(!rtt_samples.empty() && rtt_samples.back().second>=rtt_us) rtt_samples.pop_back();
              rtt_samples.emplace_back(now,rtt_us);min_rtt=rtt_samples.front().second;
              int64_t owd=static_cast<int64_t>(ack.recv_time)-static_cast<int64_t>(ack.echoed_send);
              while(!owd_samples.empty() && now-owd_samples.front().first>10'000'000) owd_samples.pop_front();
              while(!owd_samples.empty() && owd_samples.back().second>=owd) owd_samples.pop_back();
              owd_samples.emplace_back(now,owd);min_owd=owd_samples.front().second;
            }
            if(wire_samples.empty() || (ack.recv_time>wire_samples.back().first &&
                ack.wire_bytes>=wire_samples.back().second)) {
              wire_samples.emplace_back(ack.recv_time,ack.wire_bytes);
              while(wire_samples.size()>2 && ack.recv_time-wire_samples[1].first>200'000)
                wire_samples.pop_front();
              auto [ts,total]=wire_samples.front();
              if(ack.recv_time>ts) ack_rate=8.0*(ack.wire_bytes-total)/(ack.recv_time-ts);
            }
            bool was_delivered=f.delivered;
            f.acknowledge(ack);
            if(!was_delivered && f.delivered) {
              logger.log("FRAME_DELIVERED",f.id,{{"bytes",std::to_string(f.bytes.size())},
                {"target_mbps",std::to_string(f.target_mbps)},
                {"encoded_mbps",std::to_string(f.bytes.size()*8.0/INTERVAL)},
                {"delay_ms",std::to_string((now-f.first_sent)/1000.0)},
                {"capture_to_ack_ms",std::to_string(f.captured_at?(now-f.captured_at)/1000.0:0.0)},
                {"frame_interval_us",std::to_string(INTERVAL)},{"lossy",f.loss?"true":"false"}});
            }
          }
        }
      } else if(n<0 && errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR && errno!=ECONNREFUSED) {
        throw std::runtime_error(std::string("UDP receive failed: ")+strerror(errno));
      }
      timers(now);
    }
  }
  void keyboard() {
    while(running) {
      fd_set fds;FD_ZERO(&fds);FD_SET(STDIN_FILENO,&fds);timeval tv{0,100000};
      int ready=select(STDIN_FILENO+1,&fds,nullptr,nullptr,&tv);
      if(ready<=0) continue;
      char c;ssize_t n=read(STDIN_FILENO,&c,1);
      if(n==0) return;
      if(n<0){if(errno==EINTR)continue;return;}
      std::lock_guard<std::mutex> lock(mutex);
      if(c=='q'){running=false;producing=false;available.notify_all();}
      else if(c=='l'){ctrl.on_frame_loss();bitrate=ctrl.get_final_bitrate();}
      else if(c=='r') ctrl.clear_history();
      else if(c=='d') ctrl.force_drain();
      else if(c=='s') std::cerr<<"[state] bitrate="<<bitrate<<" inflight="<<inflight.size()<<"\n";
    }
  }
public:
  Sender(const std::string& ip,int port,double bmin,double bmax,bool synth,const std::string& log)
      :session((static_cast<uint64_t>(std::random_device{}())<<32)^now_us()),
       synthetic(synth),bitrate(bmin),ctrl(PudicaAlgorithm::PudicaConfig{bmin,bmax}) {
    if(!session) session=1;
    if(!log.empty()){logger.open(log);ctrl.set_logger(&logger);}
    sock=socket(AF_INET,SOCK_DGRAM,0);
    if(sock<0) throw std::runtime_error("socket creation failed");
    sockaddr_in dest{};dest.sin_family=AF_INET;dest.sin_port=htons(port);
    if(inet_pton(AF_INET,ip.c_str(),&dest.sin_addr)!=1 || connect(sock,
        reinterpret_cast<sockaddr*>(&dest),sizeof(dest))<0) {
      close(sock);sock=-1;throw std::runtime_error("invalid destination or UDP connect failed");
    }
    timeval tv{0,5000};
    if(setsockopt(sock,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv))<0) {
      close(sock);sock=-1;throw std::runtime_error("setting socket timeout failed");
    }
  }
  ~Sender(){try {stop();} catch(...) {} if(sock>=0)close(sock);}
  void run(double seconds) {
    running=true;producing=true;
    capture_thread=std::thread([&]{guarded([&]{capture();});});
    pacer_thread=std::thread([&]{guarded([&]{pacer();});});
    listener_thread=std::thread([&]{guarded([&]{listener();});});
    keyboard_thread=std::thread([&]{guarded([&]{keyboard();});});
    auto end=steady_clock::now()+duration<double>(seconds);
    while(running && !interrupted && steady_clock::now()<end) std::this_thread::sleep_for(milliseconds(5));
    if(interrupted) running=false;
    producing=false;
    if(capture_thread.joinable())capture_thread.join();
    // Allow the last sent frames to resolve before reporting the experiment.
    auto drain_end=steady_clock::now()+microseconds(FRAME_TIMEOUT+INTERVAL);
    while(running && steady_clock::now()<drain_end) {
      {std::lock_guard<std::mutex> lock(mutex);if(frames.empty()&&inflight.empty())break;}
      std::this_thread::sleep_for(milliseconds(5));
    }
    stop();
    if(failed) throw std::runtime_error("sender worker failed");
  }
  void stop() {
    running=false;producing=false;available.notify_all();
    for(auto* t:{&capture_thread,&pacer_thread,&listener_thread,&keyboard_thread})
      if(t->joinable())t->join();
    std::lock_guard<std::mutex> lock(mutex);
    for(const auto& [id,f]:inflight) {
      if(!f.delivered)logger.log("FRAME_LOSS",id,{{"reason",Logger::quote("shutdown")}});
      probe_summary(f);
    }
    inflight.clear();frames.clear();retries.clear();
  }
};
int main(int argc,char** argv) {
  try {
    std::string log;double bmin=.2,bmax=50;bool synthetic=false;
    option options[]={{"log",required_argument,nullptr,'l'},{"bmin",required_argument,nullptr,'m'},
      {"bmax",required_argument,nullptr,'M'},{"synthetic",no_argument,nullptr,'S'},{nullptr,0,nullptr,0}};
    int opt;
    while((opt=getopt_long(argc,argv,"l:m:M:S",options,nullptr))!=-1) {
      if(opt=='l')log=optarg;else if(opt=='m')bmin=std::stod(optarg);
      else if(opt=='M')bmax=std::stod(optarg);else if(opt=='S')synthetic=true;
      else throw std::runtime_error("unknown option");
    }
    if(argc-optind!=3)throw std::runtime_error(
      "Usage: sender [--log path] [--bmin Mbps] [--bmax Mbps] [--synthetic] <IPv4> <port> <seconds>");
    int port=std::stoi(argv[optind+1]);double seconds=std::stod(argv[optind+2]);
    if(port<1||port>65535||!std::isfinite(seconds)||seconds<=0||
       !std::isfinite(bmin)||!std::isfinite(bmax)||bmin<=0||bmax<bmin||bmax>1000)
      throw std::runtime_error("invalid port, duration or bitrate bounds");
    std::signal(SIGINT,signal_stop);std::signal(SIGTERM,signal_stop);
    Sender sender(argv[optind],port,bmin,bmax,synthetic,log);sender.run(seconds);return 0;
  } catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
}

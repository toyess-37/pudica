#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <ctime>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "transport.h"
#include "../cloud_gaming/playback/decoder.h"
#include "../cloud_gaming/playback/renderer.h"
using namespace pudica_net;
static volatile sig_atomic_t interrupted=0;
static void signal_stop(int){interrupted=1;}
static uint64_t now_us(){return std::chrono::duration_cast<std::chrono::microseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count();}
// Arrival time on the steady clock. The kernel stamps packets on CLOCK_REALTIME
// when they reach the socket; subtracting the time they then waited (while this
// thread decoded or rendered) keeps that wait out of the one-way delay and BUR.
static uint64_t arrival_us(const msghdr& message) {
    const uint64_t steady=now_us();
    for(cmsghdr* c=CMSG_FIRSTHDR(&message);c;c=CMSG_NXTHDR(const_cast<msghdr*>(&message),c)) {
        if(c->cmsg_level!=SOL_SOCKET || c->cmsg_type!=SCM_TIMESTAMPNS) continue;
        timespec stamp{},real{};std::memcpy(&stamp,CMSG_DATA(c),sizeof(stamp));
        clock_gettime(CLOCK_REALTIME,&real);
        const int64_t waited=(int64_t(real.tv_sec)-stamp.tv_sec)*1'000'000+(real.tv_nsec-stamp.tv_nsec)/1000;
        if(waited>=0 && waited<1'000'000 && uint64_t(waited)<steady) return steady-uint64_t(waited);
    }
    return steady;
}
struct Entry {
    ReceivedFrame frame;
    bool played=false;
    Entry(const PktHeader& h,uint64_t now):frame(h,now){}
};
int main(int argc,char** argv) {
    int sock=-1;
    try {
        bool headless=false,decode_only=false;
        if(argc<2||argc>3)throw std::runtime_error("Usage: receiver <port> [--headless|--decode-only]");
        int port=std::stoi(argv[1]);
        if(port<1||port>65535)throw std::runtime_error("invalid port");
        if(argc==3){headless=std::string(argv[2])=="--headless";decode_only=std::string(argv[2])=="--decode-only";
            if(!headless&&!decode_only)throw std::runtime_error("unknown receiver option");}
        sock=socket(AF_INET,SOCK_DGRAM,0);
        if(sock<0)throw std::runtime_error("socket failed");
        timeval timeout{0,10000};
        if(setsockopt(sock,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout))<0)throw std::runtime_error("socket timeout failed");
        sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(port);address.sin_addr.s_addr=INADDR_ANY;
        const int on=1;
        if(setsockopt(sock,SOL_SOCKET,SO_TIMESTAMPNS,&on,sizeof(on))<0)throw std::runtime_error("socket timestamps failed");
        if(bind(sock,reinterpret_cast<sockaddr*>(&address),sizeof(address))<0)throw std::runtime_error("bind failed");
        std::signal(SIGINT,signal_stop);std::signal(SIGTERM,signal_stop);
        std::map<uint32_t,Entry> frames;
        std::set<uint64_t> retired;
        uint64_t session=0,wire_bytes=0,delivered=0,decoded=0;
        uint32_t next=1,width=0,height=0;
        sockaddr_in peer{};
        bool need_key=true;
        uint64_t last_key_request=0;
        std::unique_ptr<VideoDecoder> decoder;
        std::unique_ptr<VideoRenderer> renderer;
        std::array<uint8_t,MAX_BUF+1> buffer{};
        std::cout<<"[receiver] listening on "<<port<<std::endl;
        while(!interrupted) {
            sockaddr_in client{};
            iovec vector{buffer.data(),buffer.size()};
            alignas(cmsghdr) char control[CMSG_SPACE(sizeof(timespec))];
            msghdr message{};message.msg_name=&client;message.msg_namelen=sizeof(client);
            message.msg_iov=&vector;message.msg_iovlen=1;message.msg_control=control;message.msg_controllen=sizeof(control);
            ssize_t n=recvmsg(sock,&message,0);
            uint64_t now=n>0?arrival_us(message):now_us();
            if(n<0&&errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR)throw std::runtime_error("receive failed");
            PktHeader h;
            if(n>0&&decode(buffer.data(),n,h)&&!retired.count(h.session)) {
                bool same_peer=client.sin_addr.s_addr==peer.sin_addr.s_addr&&client.sin_port==peer.sin_port;
                bool starts=(h.flags==STREAM_INFO)||(h.frame_id==1);
                if(!session || (h.session!=session&&starts)) {
                    if(session)retired.insert(session);
                    if(retired.size()>64)throw std::runtime_error("too many session restarts; restart receiver");
                    session=h.session;peer=client;wire_bytes=0;frames.clear();next=1;need_key=true;
                    decoder.reset();renderer.reset();width=height=0;same_peer=true;
                }
                if(same_peer&&h.session==session) {
                    wire_bytes+=n; // UDP payload bytes actually received; never count reconstructed data.
                    if(h.flags==STREAM_INFO) {
                        const uint8_t* data=buffer.data()+HEADER_SIZE;
                        uint32_t w=get(data,4),v=get(data,4);
                        if(w>=2&&v>=2&&w<=8192&&v<=8192&&!((w|v)&1)) {
                            if(!headless&&(w!=width||v!=height)) {
                                renderer.reset();decoder.reset();
                                decoder=std::make_unique<VideoDecoder>(w,v);
                                if(!decode_only)renderer=std::make_unique<VideoRenderer>(w,v);
                                need_key=true;
                            }
                            width=w;height=v;
                        }
                    } else {
                        auto it=frames.find(h.frame_id);
                        if(it==frames.end()&&h.frame_id>=next&&h.frame_id-next<256&&frames.size()<256) {
                            size_t allocated=0;
                            for(const auto& entry:frames)allocated+=entry.second.frame.size;
                            if(allocated+h.frame_bytes<=16*1024*1024)
                                it=frames.try_emplace(h.frame_id,h,now).first;
                        }
                        if(it!=frames.end()&&it->second.frame.matches(h)) {
                            auto& f=it->second.frame;
                            std::optional<uint32_t> recovered;
                            if(h.flags==FEC) {
                                f.parity.try_emplace(h.fec_group,buffer.begin()+HEADER_SIZE,buffer.begin()+n);
                                recovered=f.recover(h.fec_group);
                            } else if(!(h.flags&PROBE)) {
                                f.keyframe|=(h.flags&KEYFRAME)!=0;
                                f.add(h.packet_id,buffer.data()+HEADER_SIZE);
                                recovered=f.recover(h.packet_id/FEC_K);
                            }
                            if(f.complete()&&!f.completed_at){f.completed_at=now;++delivered;}
                            auto acknowledge=[&](uint32_t pid,uint8_t flags,uint64_t echo,uint16_t attempt) {
                                RecvACK a; a.flags=flags|(f.complete()?FRAME_COMPLETE:0);a.retrans_seq=attempt;
                                a.session=session;a.echoed_send=echo;a.recv_time=now;a.frame_recv_time=f.completed_at;
                                a.wire_bytes=wire_bytes;a.frame_id=h.frame_id;a.packet_id=pid;
                                auto b=encode(a);
                                if(sendto(sock,b.data(),b.size(),0,reinterpret_cast<sockaddr*>(&peer),sizeof(peer))!=static_cast<ssize_t>(b.size()))
                                    throw std::runtime_error("ACK send failed");
                            };
                            if(recovered)acknowledge(*recovered,RECOVERED,0,0);
                            if(h.flags!=FEC)acknowledge(h.packet_id,h.flags,h.send_time,h.retrans_seq);
                        }
                    }
                }
            }
            // Ask for an IDR while waiting on one, at most every 200 ms.
            auto request_key=[&](uint32_t fid) {
                if(headless||!session||(last_key_request&&now-last_key_request<200'000))return;
                last_key_request=now;
                RecvACK a;a.flags=KEY_REQUEST;a.session=session;a.recv_time=now;a.frame_id=fid?fid:1;
                auto b=encode(a);
                if(sendto(sock,b.data(),b.size(),0,reinterpret_cast<sockaddr*>(&peer),sizeof(peer))!=static_cast<ssize_t>(b.size()))
                    throw std::runtime_error("keyframe request send failed");
            };
            // Ordered delivery continues on receive timeouts, including an entirely missing frame.
            while(!frames.empty()&&(headless||decoder)) {
                auto it=frames.find(next);
                if(it==frames.end()) {
                    it=frames.lower_bound(next);
                    if(it==frames.end()||now-it->second.frame.first_seen<FRAME_TIMEOUT)break;
                    next=it->first;need_key=true;
                }
                auto& entry=it->second;
                if(!entry.frame.complete()) {
                    if(now-entry.frame.first_seen<FRAME_TIMEOUT)break;
                    ++next;need_key=true;entry.played=true;continue;
                }
                if(!entry.played) {
                    if(headless)++decoded;
                    else if(!need_key||entry.frame.keyframe) {
                        if(need_key){decoder->Reset();need_key=false;}
                        auto bytes=entry.frame.assemble();int pitch=0;
                        try {
                            const uint8_t* image=decoder->Decode(bytes.data(),bytes.size(),pitch);
                            if(image){++decoded;if(renderer)renderer->Render(image,pitch);}
                        } catch(const std::runtime_error& e) {
                            // A corrupt frame poisons references, not the session.
                            std::cerr<<"[receiver] frame "<<it->first<<": "<<e.what()<<"; waiting for keyframe\n";
                            need_key=true;request_key(it->first);
                        }
                    } else request_key(it->first);
                    entry.played=true;
                }
                ++next;
            }
            for(auto it=frames.begin();it!=frames.end();) {
                if(now-it->second.frame.first_seen>2*FRAME_TIMEOUT) {
                    if(!it->second.played) {next=std::max(next,it->first+1);need_key=true;}
                    it=frames.erase(it);
                }
                else ++it;
            }
            if(renderer&&!renderer->PollEvents())break;
        }
        close(sock);
        std::cout<<"[receiver] delivered="<<delivered<<" decoded="<<decoded<<std::endl;
        return 0;
    } catch(const std::exception& e){if(sock>=0)close(sock);std::cerr<<"[receiver] "<<e.what()<<'\n';return 1;}
}

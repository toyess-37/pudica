#include <iostream>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <chrono>
#include <unordered_map>
#include <vector>
#include <cstdint>
#include <signal.h>
#include <algorithm>

#include "protocol.h"
#include "../cloud_gaming/playback/decoder.h"
#include "../cloud_gaming/playback/renderer.h"
#include <deque>
#include <mutex>
#include <condition_variable>
#include <thread>

using namespace std;
using namespace std::chrono;
using namespace pudica_net;

uint64_t now_microsecs()
{
  return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

// frame tracking state for dynamic group size
struct FrameState {
  uint32_t pkts = 0;
  uint32_t max_pid = 0;
  bool is_complete = false;
  std::unordered_map<uint32_t, std::vector<uint8_t>> payloads;
};
static unordered_map<uint32_t, FrameState> frame_states;

std::deque<std::vector<uint8_t>> frame_queue;
std::mutex queue_mtx;
std::condition_variable queue_cv;

// capture resolution, learned from the sender's STREAM_INFO packets instead of being hardcoded
std::mutex info_mtx;
std::condition_variable info_cv;
bool got_stream_info = false;
uint32_t stream_width = 0;
uint32_t stream_height = 0;

void check_frame_complete(uint32_t fid) {
    auto &fs = frame_states[fid];
    if (fs.is_complete) return;
    if (fs.pkts > 0 && fs.payloads.size() == fs.pkts) {
        fs.is_complete = true;
        std::vector<uint8_t> full_frame;
        full_frame.reserve(fs.pkts * LOAD_SZ);
        for (uint32_t i = 0; i < fs.pkts; i++) {
            full_frame.insert(full_frame.end(), fs.payloads[i].begin(), fs.payloads[i].end());
        }
        std::lock_guard<std::mutex> lk(queue_mtx);
        frame_queue.push_back(std::move(full_frame));
        queue_cv.notify_one();
    }
}

// fec group state: stores received payloads so we can xor-recover a missing one
struct FecGroup
{
  vector<vector<uint8_t>> payloads;
  int received = 0;
  int missing_idx = -1;
  bool got_parity = false;
  uint8_t parity[LOAD_SZ];

  FecGroup() : payloads(FEC_K) {}
};

// map (frame_id, group) -> FecGroup
static unordered_map<uint64_t, FecGroup> fec_state;

static uint64_t fec_key(uint32_t fid, uint8_t group)
{
  return ((uint64_t)(fid) << 8) | group;
}

static void cleanup_old_fec(uint32_t cur_fid)
{
  if (cur_fid < 5) return;
  uint32_t cutoff = cur_fid - 5;
  for (auto it = fec_state.begin(); it != fec_state.end();)
  {
    uint32_t fid = (uint32_t)(it->first >> 8);
    if (fid < cutoff)
      it = fec_state.erase(it);
    else
      ++it;
  }
  for (auto it = frame_states.begin(); it != frame_states.end();)
  {
    if (it->first < cutoff)
      it = frame_states.erase(it);
    else
      ++it;
  }
}

// try to recover a missing data packet using xor parity
static bool try_recover(FecGroup &g, int grp_size, uint8_t *out)
{
  if (!g.got_parity) return false;
  int missing = 0;
  int miss_idx = -1;
  for (int i = 0; i < grp_size; i++)
  {
    if (g.payloads[i].empty())
    {
      missing++;
      miss_idx = i;
    }
  }
  if (missing != 1) return false; // can only fix single loss
  g.missing_idx = miss_idx;
  memcpy(out, g.parity, LOAD_SZ);
  for (int i = 0; i < grp_size; i++)
  {
    if (i == miss_idx) continue;
    for (int b = 0; b < (int)(LOAD_SZ); b++)
      out[b] ^= g.payloads[i][b];
  }
  return true;
}

volatile bool running = true;
void handle_sigint(int)
{
  running = false;
}

void playback_loop() {
    try {
        uint32_t w, h;
        {
            std::unique_lock<std::mutex> lk(info_mtx);
            info_cv.wait(lk, [] { return got_stream_info || !running; });
            if (!running) return;
            w = stream_width;
            h = stream_height;
        }

        VideoDecoder decoder(w, h);
        VideoRenderer renderer(w, h);

        while (running) {
            std::vector<uint8_t> frame_buf;
            {
                std::unique_lock<std::mutex> lk(queue_mtx);
                if (queue_cv.wait_for(lk, std::chrono::milliseconds(100), []{ return !frame_queue.empty(); })) {
                    frame_buf = std::move(frame_queue.front());
                    frame_queue.pop_front();
                }
            }
            
            if (!frame_buf.empty()) {
                int pitch;
                const uint8_t* bgra = decoder.Decode(frame_buf.data(), frame_buf.size(), pitch);
                if (bgra) {
                    renderer.Render(bgra, pitch);
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[receiver] Playback loop error: " << e.what() << "\n";
        running = false;
    }
}

int main(int argc, char *argv[])
{
  if (argc != 2)
  {
    cerr << "Usage: " << argv[0] << " <port>\n";
    return 1;
  }

  int port = stoi(argv[1]);

  int sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0)
  {
    cerr << "[receiver] socket failed\n";
    return 1;
  }

  int opt = 1;
  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct timeval tv{0, 200000}; // 200ms timeout, so shutdown notices `running` promptly
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(port);

  if (bind(sock, (sockaddr *)&addr, sizeof(addr)) < 0)
  {
    cerr << "[receiver] bind failed\n";
    return 1;
  }

  cout << "[receiver] listening on port " << port << "...\n";

  signal(SIGINT, handle_sigint);
  
  std::thread t_playback(playback_loop);

  double recv_rate = 0.0;
  uint64_t bytes_acc = 0;
  uint64_t last_calc = now_microsecs();

  uint8_t buf[MAX_BUF];
  sockaddr_in client_addr{};
  socklen_t client_len = sizeof(client_addr);

  while (running)
  {
    ssize_t n = recvfrom(sock, buf, sizeof(buf), 0, (sockaddr *)&client_addr, &client_len);
    if (n < (ssize_t)(sizeof(PktHeader)))
      continue;

    uint64_t now = now_microsecs();
    bytes_acc += n;

    uint64_t elapsed = now - last_calc;
    if (elapsed >= 100000)
    {
      recv_rate = (8.0 * bytes_acc) / elapsed; // Mbps
      bytes_acc = 0;
      last_calc = now;
    }

    auto *hdr = (PktHeader *)(buf);
    uint8_t flags = hdr->flags;
    uint32_t fid = hdr->frame_id;

    // strip bad flag combos (shouldn't happen but just in case)
    // actually data packets never have PROBE set, skip this check

    if (flags & PacketFlags::STREAM_INFO)
    {
      if (n >= (ssize_t)(sizeof(PktHeader) + sizeof(StreamInfo)))
      {
        auto *info = (StreamInfo *)(buf + sizeof(PktHeader));
        std::lock_guard<std::mutex> lk(info_mtx);
        if (!got_stream_info)
        {
          stream_width = info->width;
          stream_height = info->height;
          got_stream_info = true;
          info_cv.notify_one();
        }
      }
      continue;
    }

    bool is_fec = flags & PacketFlags::FEC;

    if (is_fec)
    {
      // parity packet: store and try recovery
      if (n >= (ssize_t)(sizeof(PktHeader) + LOAD_SZ))
      {
        uint8_t grp = hdr->fec_group;
        auto &g = fec_state[fec_key(fid, grp)];
        if (!g.got_parity)
        {
          g.got_parity = true;
          memcpy(g.parity, buf + sizeof(PktHeader), LOAD_SZ);
        }
        
        uint32_t grp_size = FEC_K;
        auto it = frame_states.find(fid);
        if (it != frame_states.end()) {
            if (it->second.pkts > 0) {
                uint32_t pkts = it->second.pkts;
                if (grp == (pkts - 1) / FEC_K) {
                    grp_size = pkts - grp * FEC_K;
                }
            } else {
                uint32_t max_pid = it->second.max_pid;
                if (grp == max_pid / FEC_K) {
                    grp_size = (max_pid % FEC_K) + 2;
                }
            }
        }
        grp_size = min(grp_size, FEC_K); // FecGroup::payloads is only ever sized FEC_K

        uint8_t recovered[LOAD_SZ];
        if (try_recover(g, grp_size, recovered))
        {
          bytes_acc += LOAD_SZ;
          auto &fs = frame_states[fid];
          uint32_t recovered_pid = grp * FEC_K + g.missing_idx;
          fs.payloads[recovered_pid].assign(recovered, recovered + LOAD_SZ);
          g.payloads[g.missing_idx] = fs.payloads[recovered_pid];
          g.received++;
          check_frame_complete(fid);

          // tell the sender this packet doesn't need retransmitting; it never
          // saw this pid arrive, so it would otherwise assume it was lost
          RecvACK rack{};
          rack.echoed_send = 0;
          rack.recv_time = now;
          rack.rate = recv_rate;
          rack.frame_id = fid;
          rack.packet_id = recovered_pid;
          rack.flags = PacketFlags::RECOVERED;
          rack.retrans_seq = 0;
          sendto(sock, &rack, sizeof(RecvACK), 0, (sockaddr *)&client_addr, client_len);
        }
        cleanup_old_fec(fid);
      }
      continue;
    }

    // data or probe packet — buffer payload for fec if it's a data pkt
    bool is_data = !(flags & PacketFlags::PROBE);
    if (is_data && n >= (ssize_t)(sizeof(PktHeader) + LOAD_SZ))
    {
      uint32_t pid = hdr->packet_id;
      
      auto &fs = frame_states[fid];
      fs.max_pid = max(fs.max_pid, pid);
      if (flags & PacketFlags::LAST) {
        fs.pkts = pid + 1;
      }

      if (fs.payloads.find(pid) == fs.payloads.end()) {
          fs.payloads[pid].assign(buf + sizeof(PktHeader), buf + sizeof(PktHeader) + LOAD_SZ);
          uint8_t grp = (uint8_t)(pid / FEC_K);
          uint8_t pos = (uint8_t)(pid % FEC_K);
          auto &g = fec_state[fec_key(fid, grp)];
          if (g.payloads[pos].empty()) {
            g.payloads[pos] = fs.payloads[pid];
            g.received++;
          }
          check_frame_complete(fid);
      }
    }

    // ACK every data/probe packet (not just FIRST/LAST/PROBE) so the sender can
    // tell which individual packets are missing and retransmit them
    bool need_ack = !is_fec;

    if (need_ack)
    {
      RecvACK ack{};
      ack.echoed_send = hdr->send_time;
      ack.recv_time = now;
      ack.rate = recv_rate;
      ack.frame_id = hdr->frame_id;
      ack.packet_id = hdr->packet_id;
      ack.flags = hdr->flags;
      ack.retrans_seq = hdr->retrans_seq;

      int s = sendto(sock, &ack, sizeof(RecvACK), 0, (sockaddr *)&client_addr, client_len);
      if (s < 0)
        cerr << "[receiver] send error: " << strerror(errno) << "\n";
    }
  }

  close(sock);
  info_cv.notify_all(); // in case playback_loop is still waiting for the first STREAM_INFO packet
  if (t_playback.joinable()) t_playback.join();
  return 0;
}
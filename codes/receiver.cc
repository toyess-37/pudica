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
};
static unordered_map<uint32_t, FrameState> frame_states;

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

        uint8_t recovered[LOAD_SZ];
        if (try_recover(g, grp_size, recovered))
        {
          bytes_acc += LOAD_SZ;
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

      uint8_t grp = (uint8_t)(pid / FEC_K);
      uint8_t pos = (uint8_t)(pid % FEC_K);
      auto &g = fec_state[fec_key(fid, grp)];
      if (g.payloads[pos].empty())
      {
        g.payloads[pos].assign(buf + sizeof(PktHeader), buf + sizeof(PktHeader) + LOAD_SZ);
        g.received++;
      }
    }

    // ACK: send for FIRST, LAST, and PROBE packets so sender can track frame completion
    bool need_ack = (flags & PacketFlags::FIRST) || (flags & PacketFlags::LAST) || (flags & PacketFlags::PROBE);

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
  return 0;
}
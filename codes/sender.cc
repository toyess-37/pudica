#include <iostream>
#include <cstring>
#include <cmath>
#include <cassert>
#include <unordered_map>
#include <vector>
#include <deque>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <getopt.h>
#include "logger.h"
#include "protocol.h"
#include "pudica_algo.h"
#include "../cloud_gaming/capture/capture.h"
#include "../cloud_gaming/capture/encoder.h"

using namespace std;
using namespace std::chrono;
using namespace pudica_net;

uint64_t now_microsecs()
{
  return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

// from https://blog.bearcats.nl/accurate-sleep-function/
void precise_sleep(double microsecs)
{
  double seconds = microsecs / 1000000.0;

  thread_local double estimate = 5e-3;
  thread_local double mean = 5e-3;
  thread_local double m2 = 0;
  thread_local int64_t count = 1;
  while (seconds > estimate)
  {
    auto start = steady_clock::now();
    this_thread::sleep_for(milliseconds(1));
    auto end = steady_clock::now();
    double observed = (end - start).count() / 1e9;
    seconds -= observed;
    ++count;
    double delta = observed - mean;
    mean += delta / count;
    m2 += delta * (observed - mean);
    double stddev = sqrt(m2 / (count - 1));
    estimate = mean + stddev;
    estimate = max(1e-4, min(estimate, 5e-3));
  }
  auto start = steady_clock::now();
  auto spinNs = (int64_t)(seconds * 1e9);
  auto delay = nanoseconds(spinNs);
  while (steady_clock::now() - start < delay)
  {
    __asm__ volatile("pause" ::: "memory");
  }
}

struct Frame
{
  uint64_t created_at = 0;
  uint64_t t0 = 0;
  uint64_t t1_recv = 0;
  vector<double> probes;
  uint32_t bytes_out = 0;
  bool got_first = false;
  bool got_last = false;
  bool done = false;
  uint8_t last_echoed_retrans_seq = 0;

  // retransmission bookkeeping (LADR sec:3.3/3.4)
  vector<uint8_t> raw_bytes;      // original encoded bytes, kept around in case a packet needs resending
  uint32_t pkts = 0;              // data packet count for this frame
  vector<uint8_t> acked;          // per packet_id ack bitmap, sized to pkts
  vector<uint64_t> pkt_send_time; // per packet_id last-sent timestamp, sized to pkts
  bool all_sent = false;          // sender has pushed every data/FEC packet for this frame at least once
  uint8_t retrans_round = 0;
  bool needed_retrans = false;

  bool complete() const
  {
    return got_first && got_last && probes.size() == pudica_net::N_PROBE;
  }
};

class InflightWindow
{
private:
  unordered_map<uint32_t, Frame> table;
  mutex mtx;

public:
  void push_frame(uint32_t fid, uint32_t f_bytes, uint32_t pkts, const vector<uint8_t> &raw)
  {
    lock_guard<mutex> lk(mtx);
    Frame fr;
    fr.bytes_out = f_bytes;
    fr.created_at = now_microsecs();
    fr.pkts = pkts;
    fr.acked.assign(pkts, 0);
    fr.pkt_send_time.assign(pkts, 0);
    fr.raw_bytes = raw;
    table[fid] = std::move(fr);
  }

  void record_sent(uint32_t fid, uint32_t pid, uint64_t ts)
  {
    lock_guard<mutex> lk(mtx);
    auto it = table.find(fid);
    if (it == table.end() || pid >= it->second.pkt_send_time.size())
      return;
    it->second.pkt_send_time[pid] = ts;
  }

  void mark_all_sent(uint32_t fid)
  {
    lock_guard<mutex> lk(mtx);
    auto it = table.find(fid);
    if (it != table.end())
      it->second.all_sent = true;
  }

  // FEC-recovered packets never reach the sender as a normal ack; the receiver
  // tells us about them separately so we don't retransmit something it already has
  void mark_recovered(uint32_t fid, uint32_t pid)
  {
    lock_guard<mutex> lk(mtx);
    auto it = table.find(fid);
    if (it != table.end() && pid < it->second.acked.size())
      it->second.acked[pid] = 1;
  }

  bool acknowledge_packet(const RecvACK *ack, double D_pkt, double Dmin, double T_bound, bool &retrans_loss, Frame &out_fr)
  {
    lock_guard<mutex> lk(mtx);
    auto it = table.find(ack->frame_id);
    if (it == table.end())
      return false;

    Frame &fr = it->second;

    if (ack->packet_id < fr.pkts)
      fr.acked[ack->packet_id] = 1;

    // Pudica's own BUR bookkeeping is already finalized for this frame; only the
    // ack bitmap above (used for retransmission) still needs updating past this point.
    if (fr.done)
      return false;

    if (ack->flags & PacketFlags::FIRST)
    {
      fr.t0 = ack->echoed_send;
      fr.got_first = true;
    }
    if (ack->flags & PacketFlags::LAST)
    {
      fr.t1_recv = ack->recv_time;
      fr.got_last = true;
    }
    if (ack->flags & PacketFlags::PROBE)
    {
      double raw_T = max(0.0, D_pkt - Dmin);
      double Hi = 1e18;
      if (fr.got_last && ack->recv_time >= fr.t1_recv)
        Hi = ((int64_t)(ack->recv_time) - (int64_t)(fr.t1_recv)) / 1e6;
      fr.probes.push_back(min(min(raw_T, Hi), T_bound));
    }

    retrans_loss = false;
    if (ack->retrans_seq > 0)
    {
      if (fr.last_echoed_retrans_seq > 0 && ack->retrans_seq > fr.last_echoed_retrans_seq + 1)
      {
        retrans_loss = true;
      }
      fr.last_echoed_retrans_seq = ack->retrans_seq;
    }

    if (fr.complete())
    {
      fr.done = true;
      out_fr = fr;
      return true;
    }
    return false;
  }

  // RACK-lite: any packet_id sent more than reo_wnd_us ago and still unacked is
  // declared lost. Only runs once every packet in the frame has been sent at least
  // once, and only after previously-flagged pids get a fresh grace period.
  bool collect_missing(uint32_t fid, uint64_t now, uint64_t reo_wnd_us, uint8_t &out_round,
                        vector<uint32_t> &out_pids, vector<uint8_t> &out_bytes)
  {
    lock_guard<mutex> lk(mtx);
    auto it = table.find(fid);
    if (it == table.end())
      return false;

    Frame &fr = it->second;
    if (!fr.all_sent)
      return false;

    for (uint32_t pid = 0; pid < fr.pkts; pid++)
    {
      if (!fr.acked[pid] && fr.pkt_send_time[pid] != 0 && (now - fr.pkt_send_time[pid]) > reo_wnd_us)
        out_pids.push_back(pid);
    }
    if (out_pids.empty())
      return false;

    fr.retrans_round++;
    fr.needed_retrans = true;
    out_round = fr.retrans_round;
    out_bytes = fr.raw_bytes;
    for (auto pid : out_pids)
      fr.pkt_send_time[pid] = now; // grace period before this pid can be flagged missing again
    return true;
  }

  bool get_unacked(uint32_t fid, Frame &out_fr)
  {
    lock_guard<mutex> lk(mtx);
    auto it = table.find(fid);
    if (it != table.end() && !it->second.done)
    {
      out_fr = it->second;
      return true;
    }
    return false;
  }

  void erase_frame(uint32_t fid)
  {
    lock_guard<mutex> lk(mtx);
    table.erase(fid);
  }

  // true once Pudica has evaluated this frame (or it was never known to begin with);
  // used to advance oldest_inflight_fid past frames that only have straggling
  // retransmissions left, which the "next delay" fallback signal doesn't care about
  bool is_done(uint32_t fid)
  {
    lock_guard<mutex> lk(mtx);
    auto it = table.find(fid);
    return it == table.end() || it->second.done;
  }

  // drop table entries once every packet is finally acked, or the frame has been
  // around long enough that retrying further isn't worth it
  void reap(uint64_t now, uint64_t timeout_us)
  {
    lock_guard<mutex> lk(mtx);
    for (auto it = table.begin(); it != table.end();)
    {
      Frame &fr = it->second;
      bool all_acked = true;
      for (auto a : fr.acked)
      {
        if (!a)
        {
          all_acked = false;
          break;
        }
      }
      if (all_acked || (now - fr.created_at) > timeout_us)
        it = table.erase(it);
      else
        ++it;
    }
  }
};

class UdpSocket
{
private:
  int sock = -1;

public:
  UdpSocket(const string &ip, int port)
  {
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
      throw runtime_error("[sender] ERROR: socket creation failed");
    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &dest.sin_addr) <= 0)
    {
      close(sock);
      throw runtime_error("[sender] ERROR: invalid ip address");
    }
    if (connect(sock, (struct sockaddr *)&dest, sizeof(dest)) < 0)
    {
      close(sock);
      throw runtime_error("[sender] ERROR: socket connect failed");
    }
    struct timeval tv{0, 200000}; // 200ms timeout
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  }
  ~UdpSocket()
  {
    if (sock >= 0)
      close(sock);
  }
  int fd() const { return sock; }
  void send(const void *buf, size_t len)
  {
    ::send(sock, buf, len, 0);
  }
};

class PudicaSender
{
private:
  UdpSocket socket;

  atomic<bool> running{false};
  thread t_pacer, t_listener, t_keyboard, t_capture;

  std::deque<std::vector<uint8_t>> frame_queue;
  std::mutex queue_mtx;

  atomic<double> bitrate{0.2};
  atomic<double> pacing{PudicaAlgorithm::PudicaConfig().GAMMA_P};

  InflightWindow window;
  mutex ctrl_mtx; // protects ctrl, n_retrans_pending, and running_inflight fields

  Logger logger;
  PudicaAlgorithm::Controller ctrl;

  uint32_t last_done_fid = 0;
  uint32_t oldest_inflight_fid = 1;
  uint32_t running_inflight_frames = 0;
  uint64_t running_inflight_bytes = 0;

  uint32_t n_retrans_pending = 0;
  atomic<bool> reactive_rate_limit{false};
  uint32_t consecutive_lossy_frames = 0;

  void capture_loop()
  {
    try
    {
      FrameCapture capture(nullptr);
      VideoEncoder encoder(capture.GetWidth(), capture.GetHeight(), 35, bitrate.load());

      PktHeader ihdr{};
      ihdr.flags = PacketFlags::STREAM_INFO;
      StreamInfo info{(uint32_t)(capture.GetWidth()), (uint32_t)(capture.GetHeight())};
      uint8_t ibuf[sizeof(PktHeader) + sizeof(StreamInfo)];
      memcpy(ibuf, &ihdr, sizeof(PktHeader));
      memcpy(ibuf + sizeof(PktHeader), &info, sizeof(StreamInfo));

      uint64_t last_info_send = 0;

      while (running)
      {
        // resend periodically in case the receiver starts listening after we do
        uint64_t now = now_microsecs();
        if (now - last_info_send > 1'000'000ULL)
        {
          socket.send(ibuf, sizeof(ibuf));
          last_info_send = now;
        }

        encoder.SetBitrate(bitrate.load());
        const uint8_t *bgra_data = capture.CaptureFrame();
        auto packets = encoder.Encode(bgra_data);

        std::vector<uint8_t> frame_buf;
        for (const auto &pkt : packets)
        {
          frame_buf.insert(frame_buf.end(), pkt.begin(), pkt.end());
        }

        if (!frame_buf.empty())
        {
          lock_guard<mutex> lk(queue_mtx);

          // reactive rate limit (LADR sec:3.3): if the backlog would take more than
          // two frame intervals to drain at the current bitrate, skip this frame
          // rather than let unsent data pile up behind pending retransmissions
          uint64_t queued_bytes = 0;
          for (auto &f : frame_queue)
            queued_bytes += f.size();
          double bytes_per_sec = bitrate.load() * 1e6 / 8.0;
          double drain_secs = bytes_per_sec > 0 ? (queued_bytes / bytes_per_sec) : 0.0;

          if (drain_secs * 1e6 <= 2.0 * pudica_net::INTERVAL)
            frame_queue.push_back(std::move(frame_buf));
        }
      }
    }
    catch (const std::exception &e)
    {
      cerr << "[sender] Capture loop error: " << e.what() << "\n";
    }
  }

  void evaluate(uint32_t fid, Frame &fr, double recv_rate, int64_t min_d)
  {
    double Dmin = min_d / 1e6;
    double D = ((int64_t)fr.t1_recv - (int64_t)fr.t0) / 1e6;

    lock_guard<mutex> lk(ctrl_mtx);
    PudicaAlgorithm::FrameAck fa{
        fid, D, Dmin, fr.probes, recv_rate,
        (double)(running_inflight_bytes),
        now_microsecs(),
        running_inflight_frames};

    auto out = ctrl.on_frame_acked(fa);
    if (!out.valid)
      return;

    bitrate.store(out.bitrate);
    pacing.store(out.pacing);
    last_done_fid = max(last_done_fid, fid);
  }

  void pacer()
  {

    for (uint32_t fid = 1; running; fid++)
    {
      std::vector<uint8_t> current_frame;
      while (running)
      {
        {
          lock_guard<mutex> lk(queue_mtx);
          if (!frame_queue.empty())
          {
            current_frame = std::move(frame_queue.front());
            frame_queue.pop_front();
            break;
          }
        }
        this_thread::sleep_for(milliseconds(1));
      }
      if (!running)
        break;

      auto t_start = steady_clock::now();

      double rate = bitrate.load();
      double rho = pacing.load();
      uint32_t f_bytes = current_frame.size();
      if (f_bytes == 0)
        f_bytes = 1; // Prevent div by zero
      uint32_t pkts = f_bytes / pudica_net::LOAD_SZ;
      if (f_bytes % pudica_net::LOAD_SZ != 0)
        pkts++;

      window.push_frame(fid, f_bytes, pkts, current_frame);

      {
        lock_guard<mutex> lk(ctrl_mtx);
        running_inflight_frames++;
        running_inflight_bytes += f_bytes;
      }

      double effective_pacing = rho;
      if (reactive_rate_limit.load())
      {
        // LADR sec:3.3 pacing rate limit: stop probing for bandwidth while packets
        // are still being recovered, so we don't add to the congestion that caused the loss
        effective_pacing = std::min(effective_pacing, 1.0);
      }

      double sensible = pudica_net::INTERVAL / effective_pacing;
      double pkt_gap = sensible / pkts;
      double agnostic = pudica_net::INTERVAL - sensible;
      double probe_gap = agnostic / (pudica_net::N_PROBE + 1);

      uint8_t buf[pudica_net::LOAD_SZ + sizeof(PktHeader)];

      // xor parity buffer for fec, reset each group
      uint8_t xor_buf[pudica_net::LOAD_SZ];

      for (uint32_t pid = 0; pid < pkts && running; pid++)
      {
        PktHeader hdr{};
        hdr.frame_id = fid;
        hdr.packet_id = pid;
        hdr.send_time = now_microsecs();
        hdr.retrans_seq = 0;
        hdr.fec_group = 0;
        if (pid == 0)
          hdr.flags |= PacketFlags::FIRST;
        if (pid == pkts - 1)
          hdr.flags |= PacketFlags::LAST;

        // data packets never have PROBE set, so no flag conflict possible

        memcpy(buf, &hdr, sizeof(PktHeader));

        uint32_t offset = pid * pudica_net::LOAD_SZ;
        uint32_t to_copy = pudica_net::LOAD_SZ;
        if (offset + to_copy > f_bytes)
        {
          to_copy = f_bytes - offset;
        }
        memcpy(buf + sizeof(PktHeader), current_frame.data() + offset, to_copy);
        if (to_copy < pudica_net::LOAD_SZ)
        {
          memset(buf + sizeof(PktHeader) + to_copy, 0, pudica_net::LOAD_SZ - to_copy);
        }

        socket.send(buf, sizeof(buf));
        window.record_sent(fid, pid, hdr.send_time);

        // xor fec: build parity for each group of FEC_K packets
        uint32_t grp = pid / pudica_net::FEC_K;
        uint8_t *payload = buf + sizeof(PktHeader);
        if (pid % pudica_net::FEC_K == 0)
        {
          // first pkt in group, just copy
          memcpy(xor_buf, payload, pudica_net::LOAD_SZ);
        }
        else
        {
          for (uint32_t b = 0; b < pudica_net::LOAD_SZ; b++)
            xor_buf[b] ^= payload[b];
        }

        // send parity pkt at end of each group (if group has >1 pkt)
        bool last_in_grp = (pid % pudica_net::FEC_K == pudica_net::FEC_K - 1) || (pid == pkts - 1);
        bool grp_multi = (pid % pudica_net::FEC_K != 0);
        if (last_in_grp && grp_multi)
        {
          uint8_t parity_buf[pudica_net::LOAD_SZ + sizeof(PktHeader)];
          PktHeader fhdr{};
          fhdr.frame_id = fid;
          fhdr.packet_id = UINT32_MAX - 8 - grp;
          fhdr.flags = PacketFlags::FEC;
          fhdr.retrans_seq = 0;
          fhdr.fec_group = (uint8_t)(grp);
          fhdr.send_time = now_microsecs();
          memcpy(parity_buf, &fhdr, sizeof(PktHeader));
          memcpy(parity_buf + sizeof(PktHeader), xor_buf, pudica_net::LOAD_SZ);
          socket.send(parity_buf, sizeof(parity_buf));
        }

        if (pid < pkts - 1)
          precise_sleep(pkt_gap);
      }

      window.mark_all_sent(fid);

      for (uint32_t i = 0; i < pudica_net::N_PROBE && running; i++)
      {
        precise_sleep(probe_gap);

        PktHeader phdr{};
        phdr.frame_id = fid;
        phdr.packet_id = UINT32_MAX - i;
        phdr.flags = PacketFlags::PROBE;
        phdr.retrans_seq = 0;
        phdr.fec_group = 0;
        phdr.send_time = now_microsecs();
        socket.send(&phdr, sizeof(PktHeader));
      }

      this_thread::sleep_until(t_start + microseconds(pudica_net::INTERVAL));
    }
  }

  void listener()
  {
    uint8_t buf[pudica_net::MAX_BUF];
    struct OwdSample
    {
      uint64_t ts;
      int64_t owd;
    };
    deque<OwdSample> owd_window;
    int64_t d_min = INT64_MAX;
    int64_t rtt_min = INT64_MAX;
    double recv_rate = 0.0;

    while (running)
    {
      ssize_t n = recv(socket.fd(), buf, sizeof(buf), 0);
      if (n < (ssize_t)(sizeof(RecvACK)))
        continue;

      auto *ack = (RecvACK *)(buf);
      uint64_t now = now_microsecs();
      uint32_t fid = ack->frame_id;
      if (fid < oldest_inflight_fid)
        continue;

      if (ack->flags & PacketFlags::RECOVERED)
      {
        // receiver rebuilt this packet from FEC parity; it never actually arrived
        // as a normal packet, so there's no OWD/RTT signal here, just an ack bit
        window.mark_recovered(fid, ack->packet_id);
        continue;
      }

      recv_rate = ack->rate;

      int64_t rtt = (int64_t)(now - ack->echoed_send);
      int64_t cutoff = (int64_t)(now - 10'000'000ULL);
      int64_t owd = (int64_t)(ack->recv_time) - (int64_t)(ack->echoed_send);

      while (!owd_window.empty() && (int64_t)(owd_window.front().ts) < cutoff)
        owd_window.pop_front();
      while (!owd_window.empty() && owd_window.back().owd >= owd)
        owd_window.pop_back();
      owd_window.push_back({now, owd});

      rtt_min = min(rtt, rtt_min);
      d_min = owd_window.front().owd;

      double Dmin = d_min / 1e6;
      double D_pkt = owd / 1e6;

      double rho = pacing.load();
      double T_bound = (1.0 - 1.0 / rho) * ctrl.get_config().L_SEC / (pudica_net::N_PROBE + 1);

      bool retrans_loss = false;
      Frame completed_fr;
      bool completed = window.acknowledge_packet(ack, D_pkt, Dmin, T_bound, retrans_loss, completed_fr);

      if (retrans_loss || ack->retrans_seq > 0)
      {
        lock_guard<mutex> lk(ctrl_mtx);
        if (retrans_loss)
        {
          auto r_out = ctrl.on_retrans_loss_detected();
          if (r_out.valid)
          {
            bitrate.store(r_out.bitrate);
            pacing.store(r_out.pacing);
          }
        }
        if (ack->retrans_seq > 0)
        {
          if (n_retrans_pending > 0)
            n_retrans_pending--;
          if (n_retrans_pending < 2)
            reactive_rate_limit.store(false);
        }
      }

      if (completed)
      {
        evaluate(fid, completed_fr, recv_rate, d_min);
        {
          lock_guard<mutex> lk(ctrl_mtx);
          if (running_inflight_frames > 0)
            running_inflight_frames--;
          if (running_inflight_bytes >= completed_fr.bytes_out)
          {
            running_inflight_bytes -= completed_fr.bytes_out;
          }
          else
          {
            logger.log("ERROR", fid, {{"msg", "\"Underflow in running_inflight_bytes on frame ack\""}});
            assert(false && "Underflow in running_inflight_bytes on frame ack (root cause: missing byte tracking)");
          }
        }
        // table entry stays until every data packet is acked or it ages out (see
        // window.reap below) -- a Pudica-complete frame may still be missing packets
        if (!completed_fr.needed_retrans)
          consecutive_lossy_frames = 0;
      }

      {
        uint64_t reo_wnd = 8000; // 8ms default reordering window
        if (rtt_min != INT64_MAX)
          reo_wnd = max<uint64_t>(rtt_min / 2, 4000);

        uint8_t round;
        vector<uint32_t> missing;
        vector<uint8_t> raw;
        if (window.collect_missing(fid, now, reo_wnd, round, missing, raw))
        {
          uint32_t pkts = raw.size() / pudica_net::LOAD_SZ + (raw.size() % pudica_net::LOAD_SZ ? 1 : 0);
          for (uint32_t pid : missing)
          {
            PktHeader hdr{};
            hdr.frame_id = fid;
            hdr.packet_id = pid;
            hdr.send_time = now_microsecs();
            hdr.retrans_seq = round;
            hdr.fec_group = 0;
            if (pid == 0)
              hdr.flags |= PacketFlags::FIRST;
            if (pid == pkts - 1)
              hdr.flags |= PacketFlags::LAST;

            uint8_t rbuf[pudica_net::LOAD_SZ + sizeof(PktHeader)];
            memcpy(rbuf, &hdr, sizeof(PktHeader));
            uint32_t offset = pid * pudica_net::LOAD_SZ;
            uint32_t to_copy = min<uint32_t>(pudica_net::LOAD_SZ, raw.size() > offset ? raw.size() - offset : 0);
            memcpy(rbuf + sizeof(PktHeader), raw.data() + offset, to_copy);
            if (to_copy < pudica_net::LOAD_SZ)
              memset(rbuf + sizeof(PktHeader) + to_copy, 0, pudica_net::LOAD_SZ - to_copy);

            socket.send(rbuf, sizeof(rbuf));
            window.record_sent(fid, pid, hdr.send_time);
          }

          lock_guard<mutex> lk(ctrl_mtx);
          n_retrans_pending += (uint32_t)(missing.size());
          if (n_retrans_pending >= 4)
            reactive_rate_limit.store(true);

          // LADR sec:3.3 requires 3 consecutive lossy frames before treating this
          // as a genuine congestion signal, to smooth out one-off transient losses
          consecutive_lossy_frames++;
          if (consecutive_lossy_frames >= 3)
          {
            auto r_out = ctrl.on_retrans_loss_detected();
            if (r_out.valid)
            {
              bitrate.store(r_out.bitrate);
              pacing.store(r_out.pacing);
            }
          }
        }
      }

      Frame oldest_fr;
      if (window.get_unacked(oldest_inflight_fid, oldest_fr))
      {
        uint64_t age = now - oldest_fr.created_at;

        {
          lock_guard<mutex> lk(ctrl_mtx);
          auto fallback = ctrl.on_inflight_age(age);
          if (fallback.valid)
          {
            logger.log("INFLIGHT_AGE", oldest_inflight_fid, {{"age_microsecs", std::to_string(age)}});
            bitrate.store(fallback.bitrate);
            pacing.store(fallback.pacing);
          }

          if (age > ctrl.get_config().TIMEOUT)
          {
            logger.log("FRAME_LOSS", oldest_inflight_fid, {{"age_microsecs", std::to_string(age)}});
            ctrl.on_frame_loss();

            // a frame that timed out entirely is stale by the time we'd notice --
            // retransmitting it is pointless for real-time video, so we just drop it

            bitrate.store(ctrl.get_bitrate());
            pacing.store(ctrl.get_pacing());

            running_inflight_frames--;
            running_inflight_bytes -= oldest_fr.bytes_out;
            if (running_inflight_frames == (uint32_t)(-1))
            {
              logger.log("ERROR", oldest_inflight_fid, {{"msg", "\"Underflow in running_inflight_frames\""}});
              assert(false && "Underflow in running_inflight_frames (root cause: missing frame tracking)");
            }
            if (running_inflight_bytes > 1ULL << 60)
            {
              logger.log("ERROR", oldest_inflight_fid, {{"msg", "\"Underflow in running_inflight_bytes\""}});
              assert(false && "Underflow in running_inflight_bytes (root cause: missing byte tracking)");
            }

            window.erase_frame(oldest_inflight_fid);
            oldest_inflight_fid++;
          }
        }
      }

      while (window.is_done(oldest_inflight_fid) && oldest_inflight_fid <= last_done_fid)
        oldest_inflight_fid++;

      window.reap(now, ctrl.get_config().TIMEOUT);
    }
  }

  void keyboard()
  {
    while (running)
    {
      struct timeval tv{0, 100000};
      fd_set fds;
      FD_ZERO(&fds);
      FD_SET(STDIN_FILENO, &fds);
      int ret = select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv);
      if (ret <= 0)
        continue;

      char c = '\0';
      if (read(STDIN_FILENO, &c, 1) != 1)
        continue;

      lock_guard<mutex> lk(ctrl_mtx);
      switch (c)
      {
      case 's':
      {
        auto ss = ctrl.get_state_snapshot();
        cerr << "[state] bitrate=" << ss.current_bitrate
             << " pacing=" << ss.current_pacing
             << " bur=" << ss.last_bur
             << " state=" << (int)(ss.current_state)
             << " history=" << ss.history_size
             << " frames_up=" << ss.frames_up
             << " adj_after=" << ss.adj_after << "\n";
        break;
      }
      case 'l':
        cerr << "[keyboard] forcing frame loss\n";
        ctrl.on_frame_loss();
        bitrate.store(ctrl.get_bitrate());
        pacing.store(ctrl.get_pacing());
        break;
      case 'd':
        cerr << "[keyboard] forcing drain\n";
        ctrl.force_drain();
        break;
      case 'q':
        cerr << "[keyboard] quit\n";
        running = false;
        break;
      case 'r':
        cerr << "[keyboard] clearing BUR history\n";
        ctrl.clear_history();
        break;
      default:
        break;
      }
    }
  }

public:
  PudicaSender(const string &ip, int port, double min_b = 0.2, double max_b = 50.0)
      : socket(ip, port), bitrate(min_b), ctrl(PudicaAlgorithm::PudicaConfig{min_b, max_b})
  {
  }

  void open_logger(const std::string &path)
  {
    logger.open(path);
    ctrl.set_logger(&logger);
  }

  ~PudicaSender()
  {
    stop();
  }

  void start()
  {
    if (running)
      return;
    running = true;
    t_capture = thread(&PudicaSender::capture_loop, this);
    t_pacer = thread(&PudicaSender::pacer, this);
    t_listener = thread(&PudicaSender::listener, this);
    t_keyboard = thread(&PudicaSender::keyboard, this);
  }

  void stop()
  {
    if (!running)
      return;

    running = false;
    if (t_capture.joinable())
      t_capture.join();
    if (t_pacer.joinable())
      t_pacer.join();
    if (t_listener.joinable())
      t_listener.join();
    if (t_keyboard.joinable())
      t_keyboard.join();
  }
};

int main(int argc, char *argv[])
{
  string log_path;
  double bmin = 0.2;
  double bmax = 50.0;

  static struct option long_options[] = {
      {"log", required_argument, 0, 'l'},
      {"bmin", required_argument, 0, 'm'},
      {"bmax", required_argument, 0, 'M'},
      {0, 0, 0, 0}};

  int opt;
  while ((opt = getopt_long(argc, argv, "l:m:M:", long_options, nullptr)) != -1)
  {
    switch (opt)
    {
    case 'l':
      log_path = optarg;
      break;
    case 'm':
      bmin = stod(optarg);
      break;
    case 'M':
      bmax = stod(optarg);
      break;
    default:
      cerr << "Usage: " << argv[0] << " [options] <target_ip> <port> <duration_sec>\n"
           << "Options:\n"
           << "  --log <path>   Event JSONL trace output\n"
           << "  --bmin <Mbps>  Minimum bitrate (default 0.2)\n"
           << "  --bmax <Mbps>  Maximum bitrate (default 50.0)\n";
      return 1;
    }
  }

  if (optind + 3 > argc)
  {
    cerr << "Usage: " << argv[0] << " [options] <target_ip> <port> <duration_sec>\n";
    return 1;
  }

  string target_ip = argv[optind];
  int port = stoi(argv[optind + 1]);
  double duration = stod(argv[optind + 2]);

  try
  {
    PudicaSender sender(target_ip, port, bmin, bmax);
    if (!log_path.empty())
      sender.open_logger(log_path);
    sender.start();

    precise_sleep(duration * 1e6);
    sender.stop();
  }
  catch (const exception &e)
  {
    cerr << "[sender] Error: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
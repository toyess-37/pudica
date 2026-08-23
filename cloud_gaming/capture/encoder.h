// Phase 1: encoder.h
// Depends on: None (standalone for Phase 1)
// Purpose: Wraps libavcodec to encode raw BGRA frames to H.264 NAL units at 35 FPS.

#ifndef CLOUD_GAMING_ENCODER_H
#define CLOUD_GAMING_ENCODER_H

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <stdexcept>
#include <vector>
#include <iostream>
#include <cstdlib>
#include <chrono>

class VideoEncoder {
public:
    // initial_mbps seeds the encoder's rate-control target. Passing 0 (the old
    // default) puts libx264 into constant-quality mode, which ignores whatever
    // bitrate the congestion controller decides on -- always pass the sender's
    // current Pudica bitrate here so the encoder starts in bitrate-targeting
    // mode from the first frame.
    VideoEncoder(int width, int height, int fps = 35, double initial_mbps = 2.0)
        : width(width), height(height), fps(fps) {
        sws_ctx = sws_getContext(width, height, AV_PIX_FMT_BGRA,
                                 width, height, AV_PIX_FMT_YUV420P,
                                 SWS_BILINEAR, NULL, NULL, NULL);
        if (!sws_ctx) {
            throw std::runtime_error("Could not initialize sws context");
        }

        open_codec(initial_mbps);
    }

    ~VideoEncoder() {
        if (sws_ctx) sws_freeContext(sws_ctx);
        if (frame) av_frame_free(&frame);
        if (pkt) av_packet_free(&pkt);
        if (ctx) avcodec_free_context(&ctx);
    }

    // Re-targets the encoder to a new bitrate (Mbps), e.g. every time Pudica's
    // controller output changes. libavcodec does not guarantee that changing
    // AVCodecContext::bit_rate after avcodec_open2() takes effect on every
    // codec/version, so the reliable way to retarget is to close and reopen
    // the codec with the new bit_rate. Reopening forces a fresh I-frame and
    // discards no meaningful internal state here, since max_b_frames=0 and
    // the zerolatency tune already disable frame reordering/lookahead.
    // Two guards keep this cheap: skip if the change is small (<15%), and
    // skip if we reopened less than half a second ago, so this can be called
    // every encoded frame without actually reopening every frame.
    void SetBitrate(double mbps) {
        int64_t bps = (int64_t)(mbps * 1e6);
        if (bps <= 0) return;

        if (current_bitrate_bps > 0) {
            int64_t delta = std::llabs(bps - current_bitrate_bps);
            if (delta * 100 < current_bitrate_bps * 15) return;
        }

        uint64_t now = now_ms();
        if (now - last_reconfig_ms < 500) return;

        reopen(bps);
        last_reconfig_ms = now;
    }

    // Encodes a BGRA frame and returns a vector of H.264 NAL units (can be multiple packets)
    std::vector<std::vector<uint8_t>> Encode(const uint8_t* bgra_data) {
        std::vector<std::vector<uint8_t>> encoded_packets;
        int ret;

        if (bgra_data) {
            const int in_linesize[1] = { 4 * width }; // BGRA is 4 bytes per pixel
            const uint8_t* in_data[1] = { bgra_data };
            
            sws_scale(sws_ctx, in_data, in_linesize, 0, height, frame->data, frame->linesize);

            frame->pts = frame_count++;
            ret = avcodec_send_frame(ctx, frame);
        } else {
            // Flush
            ret = avcodec_send_frame(ctx, nullptr);
        }

        if (ret < 0) {
            throw std::runtime_error("Error sending frame for encoding");
        }

        while (ret >= 0) {
            ret = avcodec_receive_packet(ctx, pkt);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            } else if (ret < 0) {
                throw std::runtime_error("Error during encoding");
            }

            std::vector<uint8_t> data(pkt->data, pkt->data + pkt->size);
            encoded_packets.push_back(std::move(data));
            av_packet_unref(pkt);
        }

        return encoded_packets;
    }

private:
    // Allocates and opens a fresh AVCodecContext targeting bit_rate = mbps,
    // plus a matching AVFrame/AVPacket. Used both by the constructor and by
    // reopen() when SetBitrate() decides a new target is worth applying.
    void open_codec(double mbps) {
        const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_H264);
        if (!codec) {
            throw std::runtime_error("H.264 encoder not found");
        }

        ctx = avcodec_alloc_context3(codec);
        if (!ctx) {
            throw std::runtime_error("Could not allocate video codec context");
        }

        ctx->width = width;
        ctx->height = height;
        ctx->time_base = {1, fps};
        ctx->framerate = {fps, 1};
        ctx->pix_fmt = AV_PIX_FMT_YUV420P;
        ctx->gop_size = fps * 2; // I-frame every 2 seconds
        ctx->max_b_frames = 0; // Zero latency

        // Setting bit_rate (plus a matched rc_max_rate/rc_buffer_size, i.e. a
        // roughly one-second VBV buffer) puts libx264 into bitrate-targeting
        // mode instead of its default constant-quality mode, so the encoder
        // actually respects what Pudica's congestion controller decided the
        // network can carry.
        int64_t bps = (int64_t)(mbps * 1e6);
        if (bps <= 0) bps = 200'000; // fall back to 0.2 Mbps rather than CRF mode
        ctx->bit_rate = bps;
        ctx->rc_max_rate = bps;
        ctx->rc_buffer_size = bps;
        current_bitrate_bps = bps;

        // Important: optimize for real-time capture
        av_opt_set(ctx->priv_data, "preset", "ultrafast", 0);
        av_opt_set(ctx->priv_data, "tune", "zerolatency", 0);

        if (avcodec_open2(ctx, codec, NULL) < 0) {
            throw std::runtime_error("Could not open codec");
        }

        frame = av_frame_alloc();
        if (!frame) {
            throw std::runtime_error("Could not allocate video frame");
        }
        frame->format = ctx->pix_fmt;
        frame->width  = ctx->width;
        frame->height = ctx->height;

        if (av_frame_get_buffer(frame, 32) < 0) {
            throw std::runtime_error("Could not allocate the video frame data");
        }

        pkt = av_packet_alloc();
        if (!pkt) {
            throw std::runtime_error("Could not allocate packet");
        }
    }

    // Tears down the current codec/frame/packet and opens a new one at bps.
    // sws_ctx is untouched: color conversion doesn't depend on bitrate.
    void reopen(int64_t bps) {
        if (frame) { av_frame_free(&frame); frame = nullptr; }
        if (pkt) { av_packet_free(&pkt); pkt = nullptr; }
        if (ctx) { avcodec_free_context(&ctx); ctx = nullptr; }
        open_codec(bps / 1e6);
    }

    static uint64_t now_ms() {
        using namespace std::chrono;
        return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    }

    int width;
    int height;
    int fps;
    int64_t frame_count = 0;
    int64_t current_bitrate_bps = 0;
    uint64_t last_reconfig_ms = 0;

    AVCodecContext* ctx = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* pkt = nullptr;
    SwsContext* sws_ctx = nullptr;
};

#endif // CLOUD_GAMING_ENCODER_H
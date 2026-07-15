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

class VideoEncoder {
public:
    VideoEncoder(int width, int height, int fps = 35) : width(width), height(height), fps(fps) {
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

        sws_ctx = sws_getContext(width, height, AV_PIX_FMT_BGRA,
                                 width, height, AV_PIX_FMT_YUV420P,
                                 SWS_BILINEAR, NULL, NULL, NULL);
        if (!sws_ctx) {
            throw std::runtime_error("Could not initialize sws context");
        }
    }

    ~VideoEncoder() {
        if (sws_ctx) sws_freeContext(sws_ctx);
        if (ctx) avcodec_free_context(&ctx);
        if (frame) av_frame_free(&frame);
        if (pkt) av_packet_free(&pkt);
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
    int width;
    int height;
    int fps;
    int64_t frame_count = 0;

    AVCodecContext* ctx = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* pkt = nullptr;
    SwsContext* sws_ctx = nullptr;
};

#endif // CLOUD_GAMING_ENCODER_H
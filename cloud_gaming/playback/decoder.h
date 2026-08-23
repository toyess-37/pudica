#pragma once

#include <vector>
#include <stdexcept>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

class VideoDecoder {
    AVCodecContext* codec_ctx = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* frame_bgra = nullptr;
    AVPacket* pkt = nullptr;
    SwsContext* sws_ctx = nullptr;
    int width, height;

public:
    VideoDecoder(int w, int h) : width(w), height(h) {
        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (!codec) throw std::runtime_error("H264 decoder not found");
        
        codec_ctx = avcodec_alloc_context3(codec);
        codec_ctx->width = width;
        codec_ctx->height = height;
        codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
        codec_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
        codec_ctx->flags2 |= AV_CODEC_FLAG2_FAST;
        codec_ctx->thread_count = 1; 

        if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
            avcodec_free_context(&codec_ctx);
            throw std::runtime_error("Failed to open codec");
        }

        frame = av_frame_alloc();
        frame_bgra = av_frame_alloc();
        pkt = av_packet_alloc();
        if (!frame || !frame_bgra || !pkt) {
            throw std::runtime_error("Failed to allocate decoder frame/packet");
        }

        int num_bytes = av_image_get_buffer_size(AV_PIX_FMT_BGRA, width, height, 1);
        uint8_t* buffer = (uint8_t*)av_malloc(num_bytes * sizeof(uint8_t));
        if (!buffer) {
            throw std::runtime_error("Failed to allocate decode buffer");
        }
        av_image_fill_arrays(frame_bgra->data, frame_bgra->linesize, buffer, AV_PIX_FMT_BGRA, width, height, 1);

        sws_ctx = sws_getContext(width, height, AV_PIX_FMT_YUV420P,
                                 width, height, AV_PIX_FMT_BGRA,
                                 SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws_ctx) {
            av_freep(&frame_bgra->data[0]);
            throw std::runtime_error("Could not initialize sws context");
        }
    }

    ~VideoDecoder() {
        if (sws_ctx) sws_freeContext(sws_ctx);
        if (frame_bgra) { av_freep(&frame_bgra->data[0]); av_frame_free(&frame_bgra); }
        if (frame) av_frame_free(&frame);
        if (pkt) av_packet_free(&pkt);
        if (codec_ctx) avcodec_free_context(&codec_ctx);
    }

    const uint8_t* Decode(const uint8_t* data, size_t size, int& out_pitch) {
        pkt->data = (uint8_t*)data;
        pkt->size = size;

        int ret = avcodec_send_packet(codec_ctx, pkt);
        if (ret < 0) return nullptr;

        ret = avcodec_receive_frame(codec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return nullptr;
        if (ret < 0) return nullptr;

        sws_scale(sws_ctx, frame->data, frame->linesize, 0, height, frame_bgra->data, frame_bgra->linesize);
        
        out_pitch = frame_bgra->linesize[0];
        return frame_bgra->data[0];
    }
};

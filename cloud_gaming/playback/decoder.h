#pragma once
extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}
#include <cstring>
#include <climits>
#include <stdexcept>
#include <vector>

class VideoDecoder {
    AVCodecContext* ctx=nullptr;
    AVFrame* frame=nullptr;
    AVPacket* packet=nullptr;
    SwsContext* scaler=nullptr;
    int width,height;
    std::vector<uint8_t> pixels;
    void release(){if(scaler)sws_freeContext(scaler);av_frame_free(&frame);av_packet_free(&packet);avcodec_free_context(&ctx);}
    bool drain() {
        bool got=false;
        for(;;) {
            int ret=avcodec_receive_frame(ctx,frame);
            if(ret==AVERROR(EAGAIN)||ret==AVERROR_EOF) break;
            if(ret<0) throw std::runtime_error("invalid H264 frame");
            scaler=sws_getCachedContext(scaler,frame->width,frame->height,static_cast<AVPixelFormat>(frame->format),
                width,height,AV_PIX_FMT_BGRA,SWS_FAST_BILINEAR,nullptr,nullptr,nullptr);
            if(!scaler) throw std::runtime_error("decoder conversion unavailable");
            uint8_t* out[]={pixels.data()};int pitch[]={width*4};
            if(sws_scale(scaler,frame->data,frame->linesize,0,frame->height,out,pitch)!=height)
                throw std::runtime_error("decoder conversion failed");
            got=true;av_frame_unref(frame);
        }
        return got;
    }
public:
    VideoDecoder(int w,int h):width(w),height(h) {
        try {
            if(w<=0||h<=0||w>8192||h>8192) throw std::runtime_error("invalid decoder dimensions");
            const AVCodec* codec=avcodec_find_decoder(AV_CODEC_ID_H264);
            if(!codec) throw std::runtime_error("H264 decoder unavailable");
            ctx=avcodec_alloc_context3(codec);frame=av_frame_alloc();packet=av_packet_alloc();
            if(!ctx||!frame||!packet) throw std::bad_alloc();
            ctx->thread_count=1;ctx->flags|=AV_CODEC_FLAG_LOW_DELAY;
            if(avcodec_open2(ctx,codec,nullptr)<0) throw std::runtime_error("cannot open decoder");
            pixels.resize(static_cast<size_t>(w)*h*4);
        } catch(...) {release();throw;}
    }
    ~VideoDecoder(){release();}
    VideoDecoder(const VideoDecoder&)=delete;
    VideoDecoder& operator=(const VideoDecoder&)=delete;
    void Reset(){avcodec_flush_buffers(ctx);}
    const uint8_t* Decode(const uint8_t* data,size_t size,int& pitch) {
        if(!data||!size||size>INT_MAX) throw std::runtime_error("invalid compressed frame");
        av_packet_unref(packet);
        // av_new_packet owns the buffer and appends AV_INPUT_BUFFER_PADDING_SIZE zero bytes.
        if(av_new_packet(packet,static_cast<int>(size))<0) throw std::bad_alloc();
        std::memcpy(packet->data,data,size);
        int ret=avcodec_send_packet(ctx,packet);
        bool got=false;
        if(ret==AVERROR(EAGAIN)){got=drain();ret=avcodec_send_packet(ctx,packet);}
        av_packet_unref(packet);
        if(ret<0) throw std::runtime_error("decoder submission failed");
        got=drain()||got;pitch=width*4;
        return got?pixels.data():nullptr;
    }
};

#pragma once
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}
#include <cmath>
#include <stdexcept>
#include <vector>

class VideoEncoder {
    AVCodecContext* ctx=nullptr;
    AVFrame* frame=nullptr;
    AVPacket* packet=nullptr;
    SwsContext* scaler=nullptr;
    int width,height;
    int64_t sequence=0;
    bool keyframe=false, force_idr=false;
    void release() {
        if(scaler) sws_freeContext(scaler);
        av_frame_free(&frame);av_packet_free(&packet);avcodec_free_context(&ctx);
    }
public:
    VideoEncoder(int w,int h,int fps=35,double mbps=2):width(w),height(h) {
        try {
            if(w<=0||h<=0||w%2||h%2||fps<=0) throw std::runtime_error("invalid encoder dimensions/FPS");
            const AVCodec* codec=avcodec_find_encoder_by_name("libx264");
            if(!codec) throw std::runtime_error("libx264 encoder required for live bitrate adjustment");
            ctx=avcodec_alloc_context3(codec);
            if(!ctx) throw std::bad_alloc();
            ctx->width=w;ctx->height=h;ctx->pix_fmt=AV_PIX_FMT_YUV420P;
            ctx->time_base={1,fps};ctx->framerate={fps,1};
            // Long GOP: the receiver requests an IDR after loss, so periodic
            // keyframes (each many times a P-frame) are only a fallback.
            ctx->gop_size=fps*10;ctx->max_b_frames=0;
            SetBitrate(mbps);
            if(av_opt_set(ctx->priv_data,"preset","ultrafast",0)<0 ||
               av_opt_set(ctx->priv_data,"tune","zerolatency",0)<0 ||
               av_opt_set(ctx->priv_data,"forced-idr","1",0)<0 ||
               avcodec_open2(ctx,codec,nullptr)<0) throw std::runtime_error("cannot open libx264");
            frame=av_frame_alloc();packet=av_packet_alloc();
            if(!frame||!packet) throw std::bad_alloc();
            frame->width=w;frame->height=h;frame->format=ctx->pix_fmt;
            if(av_frame_get_buffer(frame,32)<0) throw std::bad_alloc();
            scaler=sws_getContext(w,h,AV_PIX_FMT_BGRA,w,h,ctx->pix_fmt,SWS_BILINEAR,nullptr,nullptr,nullptr);
            if(!scaler) throw std::runtime_error("cannot create encoder scaler");
        } catch(...) {release();throw;}
    }
    ~VideoEncoder(){release();}
    VideoEncoder(const VideoEncoder&)=delete;
    VideoEncoder& operator=(const VideoEncoder&)=delete;
    void SetBitrate(double mbps) {
        if(!std::isfinite(mbps)||mbps<=0||mbps>1000) throw std::runtime_error("invalid encoder bitrate");
        // FFmpeg's libx264 wrapper reconfigures ABR/VBV before encoding each frame.
        ctx->bit_rate=static_cast<int64_t>(mbps*1e6);
        ctx->rc_max_rate=ctx->bit_rate;
        // One-frame VBV: each frame stays close to bitrate/fps, which the
        // per-frame BUR probing assumes. A one-second buffer allowed 7x frames.
        ctx->rc_buffer_size=static_cast<int>(ctx->bit_rate/std::max(1,ctx->framerate.num/std::max(1,ctx->framerate.den)));
    }
    bool WasKeyframe() const {return keyframe;}
    void ForceKeyframe() {force_idr=true;}
    std::vector<std::vector<uint8_t>> Encode(const uint8_t* bgra) {
        if(bgra) {
            if(av_frame_make_writable(frame)<0) throw std::bad_alloc();
            const uint8_t* input[]={bgra};const int strides[]={4*width};
            if(sws_scale(scaler,input,strides,0,height,frame->data,frame->linesize)!=height)
                throw std::runtime_error("encoder conversion failed");
            frame->pts=sequence++;
            frame->pict_type=force_idr?AV_PICTURE_TYPE_I:AV_PICTURE_TYPE_NONE;
            force_idr=false;
        }
        if(avcodec_send_frame(ctx,bgra?frame:nullptr)<0) throw std::runtime_error("encoding submission failed");
        keyframe=false;
        std::vector<std::vector<uint8_t>> output;
        for(;;) {
            int ret=avcodec_receive_packet(ctx,packet);
            if(ret==AVERROR(EAGAIN)||ret==AVERROR_EOF) break;
            if(ret<0) throw std::runtime_error("encoding failed");
            keyframe|=(packet->flags&AV_PKT_FLAG_KEY)!=0;
            output.emplace_back(packet->data,packet->data+packet->size);av_packet_unref(packet);
        }
        return output;
    }
};

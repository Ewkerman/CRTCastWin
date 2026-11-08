#include "airplay_video.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include "AirPlayBridge.h"
}

#include <stdio.h>
#include <string.h>

#include "scale.h"

AirPlayVideo &AirPlayVideo::shared() {
    static AirPlayVideo instance;
    return instance;
}

void AirPlayVideo::install() {
    crt_airplay_set_video_callback([](const unsigned char *data, int length, int isH265, uint64_t remoteNs) {
        if (data && length > 0) shared().handle(data, length, isH265 != 0, remoteNs);
    });
}

bool AirPlayVideo::openDecoder(bool hevc) {
    closeDecoder();
    const AVCodec *codec = avcodec_find_decoder(hevc ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264);
    if (!codec) return false;
    ctx_ = avcodec_alloc_context3(codec);
    if (!ctx_) return false;
    // Lowest latency: no frame reordering delay, slice threads only (frame threads add a frame of delay).
    ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    ctx_->flags2 |= AV_CODEC_FLAG2_FAST;
    ctx_->thread_type = FF_THREAD_SLICE;
    ctx_->thread_count = 4;
    if (avcodec_open2(ctx_, codec, nullptr) < 0) {
        avcodec_free_context(&ctx_);
        return false;
    }
    frame_ = av_frame_alloc();
    packet_ = av_packet_alloc();
    hevc_ = hevc;
    return frame_ && packet_;
}

void AirPlayVideo::closeDecoder() {
    if (ctx_) avcodec_free_context(&ctx_);
    if (frame_) av_frame_free(&frame_);
    if (packet_) av_packet_free(&packet_);
    srcW_ = srcH_ = 0;
}

void AirPlayVideo::reset() {
    std::lock_guard<std::mutex> d(decodeLock_);
    closeDecoder();
    std::lock_guard<std::mutex> g(lock_);
    queue_.clear();
    sequence_++;
}

std::string AirPlayVideo::summary() {
    std::lock_guard<std::mutex> g(lock_);
    char buf[160];
    snprintf(buf, sizeof(buf), "%s %dx%d \xC2\xB7 decoded %llu%s", hevc_ ? "H.265" : "H.264", srcW_, srcH_,
             (unsigned long long) decoded_, errors_ ? (" \xC2\xB7 errors " + std::to_string(errors_)).c_str() : "");
    return buf;
}

/// Scales a decoded frame to 640x480 BGRA, letterboxed (black bars), keeping the picture's aspect.
void AirPlayVideo::scaleInto(const AVFrame *f, std::vector<uint8_t> &out) {
    out.assign(size_t(kWidth) * kHeight * 4, 0);
    for (size_t i = 3; i < out.size(); i += 4) out[i] = 255;

    Scale::Yuv420 y;
    y.width = f->width;
    y.height = f->height;
    y.y = f->data[0]; y.yStride = f->linesize[0];
    y.u = f->data[1]; y.uStride = f->linesize[1];
    switch (f->format) {
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUVJ420P:
        y.v = f->data[2]; y.vStride = f->linesize[2];
        break;
    case AV_PIX_FMT_YUV420P10LE:
        y.v = f->data[2]; y.vStride = f->linesize[2];
        y.bytesPerSample = 2; y.shift = 2;
        break;
    case AV_PIX_FMT_NV12:
        y.v = nullptr;
        break;
    case AV_PIX_FMT_P010LE:
        y.v = nullptr; y.bytesPerSample = 2; y.shift = 8;
        break;
    default:
        errors_++;   // unexpected pixel format: leave the picture black
        return;
    }
    y.fullRange = f->format == AV_PIX_FMT_YUVJ420P || f->color_range == AVCOL_RANGE_JPEG;
    if (f->colorspace == AVCOL_SPC_BT709) y.bt709 = true;
    else if (f->colorspace == AVCOL_SPC_SMPTE170M || f->colorspace == AVCOL_SPC_BT470BG) y.bt709 = false;
    else y.bt709 = f->height >= 720 || f->width >= 1280;

    double sx = double(kWidth) / f->width, sy = double(kHeight) / f->height;
    double s = sx < sy ? sx : sy;
    int dw = int(f->width * s + 0.5), dh = int(f->height * s + 0.5);
    if (dw > kWidth) dw = kWidth;
    if (dh > kHeight) dh = kHeight;
    if (dw < 1 || dh < 1) return;
    int ox = (kWidth - dw) / 2, oy = (kHeight - dh) / 2;
    Scale::yuvToBgra(y, out.data() + (size_t(oy) * kWidth + ox) * 4, kWidth * 4, dw, dh);
}

void AirPlayVideo::handle(const uint8_t *data, int length, bool hevc, uint64_t remoteNs) {
    // Decode and scale under decodeLock_ only; the queue lock is taken just to hand the picture over.
    std::lock_guard<std::mutex> d(decodeLock_);
    if (!ctx_ || hevc != hevc_) {
        if (!openDecoder(hevc)) { errors_++; return; }
    }
    bool failed = av_new_packet(packet_, length) < 0;
    if (!failed) {
        memcpy(packet_->data, data, size_t(length));
        int rc = avcodec_send_packet(ctx_, packet_);
        av_packet_unref(packet_);
        failed = rc < 0 && rc != AVERROR(EAGAIN);
    }
    if (failed) { errors_++; return; }

    while (avcodec_receive_frame(ctx_, frame_) == 0) {
        std::vector<uint8_t> pixels;
        {
            std::lock_guard<std::mutex> g(lock_);
            if (!spare_.empty()) { pixels.swap(spare_.back()); spare_.pop_back(); }
        }
        const int w = frame_->width, h = frame_->height;
        scaleInto(frame_, pixels);   // the slow part: outside lock_
        av_frame_unref(frame_);

        std::lock_guard<std::mutex> g(lock_);
        srcW_ = w;
        srcH_ = h;
        decoded_++;
        Entry e;
        e.remoteNs = remoteNs;
        e.sequence = ++sequence_;
        e.pixels.swap(pixels);
        // Keep sorted by sender time (insert from the end; frames normally arrive in order).
        size_t i = queue_.size();
        while (i > 0 && queue_[i - 1].remoteNs > remoteNs) i--;
        queue_.insert(queue_.begin() + ptrdiff_t(i), std::move(e));
        while (queue_.size() > kQueueLimit) {
            spare_.push_back(std::move(queue_.front().pixels));
            queue_.erase(queue_.begin());
        }
    }
}

bool AirPlayVideo::newestFrame(uint8_t *dst, uint64_t *seen, uint64_t *remoteNs) {
    std::lock_guard<std::mutex> g(lock_);
    if (queue_.empty() || queue_.back().sequence == *seen) return false;
    const Entry &e = queue_.back();
    memcpy(dst, e.pixels.data(), e.pixels.size());
    *seen = e.sequence;
    if (remoteNs) *remoteNs = e.remoteNs;
    return true;
}

bool AirPlayVideo::frameAtOrBefore(uint64_t remoteNs, uint8_t *dst, uint64_t *seen) {
    std::lock_guard<std::mutex> g(lock_);
    for (size_t i = queue_.size(); i > 0; i--) {
        const Entry &e = queue_[i - 1];
        if (e.remoteNs <= remoteNs) {
            if (e.sequence == *seen) return false;
            memcpy(dst, e.pixels.data(), e.pixels.size());
            *seen = e.sequence;
            return true;
        }
    }
    return false;
}

#include "airplay_audio.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include "AirPlayBridge.h"
}

#include <math.h>
#include <stdio.h>
#include <string.h>

AirPlayAudio &AirPlayAudio::shared() {
    static AirPlayAudio instance;
    return instance;
}

void AirPlayAudio::install() {
    crt_airplay_set_audio_callback([](const unsigned char *data, int length, int ct, uint64_t remoteNs) {
        if (data && length > 0) shared().handle(data, length, ct, remoteNs);
    });
}

void AirPlayAudio::reset() {
    std::lock_guard<std::mutex> d(decodeLock_);
    closeDecoder();
    std::lock_guard<std::mutex> g(lock_);
    readIndex_ = writeIndex_ = 0;
    markers_.clear();
    primed_ = false;
    gain_ = GainRamp();
    position_ = 0;
    prev_[0] = prev_[1] = 0;
}

std::string AirPlayAudio::summary() {
    std::lock_guard<std::mutex> g(lock_);
    const char *codec = ct_ == 2 ? "ALAC" : ct_ == 4 ? "AAC" : ct_ == 8 ? "AAC-ELD" : "\xE2\x80\x93";
    char buf[160];
    snprintf(buf, sizeof(buf), "%s \xC2\xB7 buffer %d ms \xC2\xB7 speed %+.2f%% \xC2\xB7 gaps %d", codec,
             available() / 48, correction_ * 100, underruns_);
    std::string s = buf;
    if (decodeErrors_) s += " \xC2\xB7 errors " + std::to_string(decodeErrors_);
    return s;
}

// ---- Decoding (receiver thread) ----

static std::vector<uint8_t> hex(const char *s) {
    std::vector<uint8_t> out;
    for (size_t i = 0; s[i] && s[i + 1]; i += 2) {
        unsigned v = 0;
        sscanf(s + i, "%2x", &v);
        out.push_back(uint8_t(v));
    }
    return out;
}

void AirPlayAudio::closeDecoder() {
    if (ctx_) avcodec_free_context(&ctx_);
    if (frame_) av_frame_free(&frame_);
    if (packet_) av_packet_free(&packet_);
    if (swr_) swr_free(&swr_);
    ct_ = -1;
}

bool AirPlayAudio::makeDecoder(int ct) {
    closeDecoder();
    ct_ = ct;
    // Same stream descriptions as the Apple app (magic cookies from UxPlay).
    AVCodecID id;
    std::vector<uint8_t> cookie;
    switch (ct) {
    case 2:   // ALAC 44.1 kHz / 16-bit / stereo, 352 frames per packet: 36-byte 'alac' atom
        id = AV_CODEC_ID_ALAC;
        cookie = hex("00000024616c616300000000000001600010280a0e0200ff00000000000000000000ac44");
        break;
    case 4:   // AAC-LC 44.1 kHz stereo
        id = AV_CODEC_ID_AAC;
        cookie = hex("1210");
        break;
    default:  // 8: AAC-ELD 44.1 kHz stereo, 480 frames per packet (screen mirroring)
        id = AV_CODEC_ID_AAC;
        cookie = hex("f8e85000");
        break;
    }
    const AVCodec *codec = avcodec_find_decoder(id);
    if (!codec) return false;
    ctx_ = avcodec_alloc_context3(codec);
    if (!ctx_) return false;
    ctx_->sample_rate = 44100;
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    av_channel_layout_copy(&ctx_->ch_layout, &stereo);
    ctx_->extradata = static_cast<uint8_t *>(av_mallocz(cookie.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    memcpy(ctx_->extradata, cookie.data(), cookie.size());
    ctx_->extradata_size = int(cookie.size());
    if (avcodec_open2(ctx_, codec, nullptr) < 0) {
        avcodec_free_context(&ctx_);
        return false;
    }
    frame_ = av_frame_alloc();
    packet_ = av_packet_alloc();
    return frame_ && packet_;
}

void AirPlayAudio::handle(const uint8_t *data, int length, int ct, uint64_t remoteNs) {
    std::lock_guard<std::mutex> d(decodeLock_);
    if (ct != ct_ || !ctx_) {
        if (!makeDecoder(ct)) { decodeErrors_++; return; }
    }
    packets_++;
    if (av_new_packet(packet_, length) < 0) { decodeErrors_++; return; }
    memcpy(packet_->data, data, size_t(length));
    int rc = avcodec_send_packet(ctx_, packet_);
    av_packet_unref(packet_);
    if (rc < 0 && rc != AVERROR(EAGAIN)) { decodeErrors_++; return; }

    bool marked = false;
    while (avcodec_receive_frame(ctx_, frame_) == 0) {
        if (!swr_) {
            // Exactly 48 kHz stereo Int16 for the MiSTer, from whatever the decoder produces.
            AVChannelLayout out = AV_CHANNEL_LAYOUT_STEREO;
            if (swr_alloc_set_opts2(&swr_, &out, AV_SAMPLE_FMT_S16, AudioFormat::sampleRate, &frame_->ch_layout,
                                    AVSampleFormat(frame_->format), frame_->sample_rate, 0, nullptr) < 0 ||
                swr_init(swr_) < 0) {
                swr_free(&swr_);
                decodeErrors_++;
                av_frame_unref(frame_);
                continue;
            }
        }
        int maxOut = swr_get_out_samples(swr_, frame_->nb_samples);
        converted_.resize(size_t(maxOut > 0 ? maxOut : 0) * 2 + 64);
        uint8_t *outPlanes[1] = {reinterpret_cast<uint8_t *>(converted_.data())};
        int got = swr_convert(swr_, outPlanes, maxOut, const_cast<const uint8_t **>(frame_->extended_data),
                              frame_->nb_samples);
        av_frame_unref(frame_);
        if (got <= 0) continue;
        std::lock_guard<std::mutex> g(lock_);   // brief: hand the samples over to the reader
        if (!marked) {
            markers_.emplace_back(writeIndex_, remoteNs);
            if (markers_.size() > 400) markers_.pop_front();
            marked = true;
        }
        ringWrite(converted_.data(), got);
        lastArrival_ = std::chrono::steady_clock::now();
    }
}

void AirPlayAudio::ringWrite(const int16_t *samples, int frames) {
    const int64_t capacity = int64_t(ring_.size() / 2);
    // Full: drop the oldest (the reader is far behind or stopped).
    int64_t overflow = int64_t(available()) + frames - capacity;
    if (overflow > 0) readIndex_ += overflow;
    for (int i = 0; i < frames; i++) {
        size_t at = size_t((writeIndex_ + i) % capacity) * 2;
        ring_[at] = samples[i * 2];
        ring_[at + 1] = samples[i * 2 + 1];
    }
    writeIndex_ += frames;
}

int AirPlayAudio::ringRead(int16_t *dst, int frames) {
    const int64_t capacity = int64_t(ring_.size() / 2);
    int n = frames < available() ? frames : available();
    for (int i = 0; i < n; i++) {
        size_t at = size_t((readIndex_ + i) % capacity) * 2;
        dst[i * 2] = ring_[at];
        dst[i * 2 + 1] = ring_[at + 1];
    }
    readIndex_ += n;
    return n;
}

void AirPlayAudio::skip(int frames) {
    int n = frames < available() ? frames : available();
    if (n > 0) readIndex_ += n;
}

/// Sender time of the frame at stream index `index` (interpolated from the packet timestamps).
bool AirPlayAudio::remoteTime(int64_t index, uint64_t *out) const {
    if (markers_.empty()) return false;
    const std::pair<int64_t, uint64_t> *m = &markers_.front();
    for (auto it = markers_.rbegin(); it != markers_.rend(); ++it) {
        if (it->first <= index) { m = &*it; break; }
    }
    double delta = double(index - m->first) / AudioFormat::sampleRate * 1e9;
    double t = double(m->second) + delta;
    *out = t > 0 ? uint64_t(t) : 0;
    return true;
}

// ---- Reading (sender thread) ----

bool AirPlayAudio::pull(int16_t *dst, int frames, uint64_t *remoteNs) {
    std::lock_guard<std::mutex> g(lock_);
    const bool fresh = std::chrono::steady_clock::now() - lastArrival_ < std::chrono::milliseconds(500);

    if (!primed_) {
        // Don't start on stale audio: keep only the newest kTarget frames.
        if (available() > kTarget * 2) skip(available() - kTarget);
        if (fresh && available() >= kTarget) {
            primed_ = true;
            gain_.target = 1;
        } else {
            memset(dst, 0, size_t(frames) * 2 * sizeof(int16_t));
            correction_ = 0;
            return false;
        }
    }

    const int level = available();
    // Gentle drift correction toward the target fill level.
    double c = double(level - kTarget) / kTarget * 0.004;
    correction_ = c < -kMaxCorrection ? -kMaxCorrection : c > kMaxCorrection ? kMaxCorrection : c;
    const double step = 1 + correction_;
    const int64_t startIndex = readIndex_;
    int consume = int(floor(position_ + frames * step));

    bool underrun = false;
    if (consume + kTarget / 8 > level) {
        // About to run dry (phone paused its audio or a Wi-Fi hiccup): fade out with what we have.
        underrun = true;
        if (consume > level) consume = level;
        gain_.target = 0;
    }
    if (scratch_.size() < size_t(consume + 1) * 2) scratch_.resize(size_t(consume + 1) * 2);
    const int got = ringRead(scratch_.data(), consume);

    // Linear-interpolating resampler over [prev, scratch[0..<got]].
    auto sample = [&](int k, int ch) -> float {
        if (k == 0 || got == 0) return float(prev_[ch]);
        int i = (k < got ? k : got) - 1;
        return float(scratch_[size_t(i) * 2 + ch]);
    };
    for (int i = 0; i < frames; i++) {
        double x = position_ + i * step;
        int j = int(x);
        float f = float(x - j);
        for (int ch = 0; ch < 2; ch++) {
            float a = sample(j, ch), b = sample(j + 1, ch);
            float v = a + (b - a) * f;
            dst[i * 2 + ch] = int16_t(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
        }
    }
    if (got > 0) {
        prev_[0] = scratch_[size_t(got - 1) * 2];
        prev_[1] = scratch_[size_t(got - 1) * 2 + 1];
    }
    position_ = position_ + frames * step - got;
    if (position_ < 0) position_ = 0;
    if (position_ > 1) position_ = fmod(position_, 1.0);

    gain_.apply(dst, frames);
    if (underrun) {
        primed_ = false;
        underruns_++;
        position_ = 0;
    }
    return remoteTime(startIndex, remoteNs);
}

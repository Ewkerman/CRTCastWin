// Decodes AirPlay audio (AAC-ELD for screen mirroring, AAC-LC / ALAC for some apps) to 48 kHz stereo,
// buffers it with the sender's timestamps, and hands it to the MiSTer sender at the MiSTer's pace.
// Port of CRTCast/AirPlay/AirPlayAudio.swift (FFmpeg instead of AVAudioConverter).
//
// The phone's clock and the MiSTer's clock drift slightly apart, so the buffer is held near `kTarget` by
// resampling by at most +-0.5% (inaudible) instead of dropping or repeating audio. If the phone stops
// sending sound, the output fades to silence and fades back in later.
#pragma once

#include "media.h"

#include <stdint.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwrContext;

class AirPlayAudio {
public:
    static constexpr int kTarget = 3840;            // 80 ms at 48 kHz: absorbs Wi-Fi jitter
    static constexpr double kMaxCorrection = 0.005;

    static AirPlayAudio &shared();
    static void install();

    void reset();
    std::string summary();

    /// Receiver thread: one compressed packet. ct: 2 = ALAC, 4 = AAC-LC, 8 = AAC-ELD (44.1 kHz stereo).
    void handle(const uint8_t *data, int length, int ct, uint64_t remoteNs);

    /// Sender thread: fills exactly `frames` stereo frames. Returns true and sets *remoteNs to the sender
    /// time of the first frame handed out (false while silent / priming), for lip sync.
    bool pull(int16_t *dst, int frames, uint64_t *remoteNs);

private:
    bool makeDecoder(int ct);
    void closeDecoder();
    void ringWrite(const int16_t *samples, int frames);
    int ringRead(int16_t *dst, int frames);
    int available() const { return int(writeIndex_ - readIndex_); }
    void skip(int frames);
    bool remoteTime(int64_t index, uint64_t *out) const;

    // decodeLock_: decoder (receiver thread; reset). lock_: ring, markers and reader state, held briefly:
    // the MiSTer sender thread takes it every field.
    std::mutex decodeLock_;
    std::mutex lock_;

    // 1 s ring of 48 kHz stereo Int16, indexed by frames ever written / read.
    std::vector<int16_t> ring_ = std::vector<int16_t>(size_t(AudioFormat::sampleRate) * 2);
    int64_t writeIndex_ = 0;
    int64_t readIndex_ = 0;
    std::deque<std::pair<int64_t, uint64_t>> markers_;   // (stream index, sender ns)
    std::chrono::steady_clock::time_point lastArrival_{};

    // Decoder (receiver thread)
    std::atomic<int> ct_{-1};
    AVCodecContext *ctx_ = nullptr;
    AVFrame *frame_ = nullptr;
    AVPacket *packet_ = nullptr;
    SwrContext *swr_ = nullptr;
    std::vector<int16_t> converted_;
    uint64_t packets_ = 0;
    std::atomic<uint64_t> decodeErrors_{0};

    // Reader state (sender thread)
    bool primed_ = false;
    GainRamp gain_;
    double position_ = 0;          // fractional read position between prev and next input frame
    int16_t prev_[2] = {0, 0};
    std::vector<int16_t> scratch_ = std::vector<int16_t>(16384);
    int underruns_ = 0;
    double correction_ = 0;
};

// Decodes the AirPlay screen-mirror stream (FFmpeg) and keeps recent pictures, scaled to 640x480 BGRA
// letterboxed, with the sender's capture time. Port of CRTCast/AirPlay/AirPlayVideo.swift.
//
// UxPlay delivers one frame per callback as Annex-B NAL units, with the parameter sets prepended to
// keyframes. Decoding runs continuously while a phone mirrors, even when nothing is sent to the MiSTer,
// because AirPlay only sends a keyframe at the start: skipped frames couldn't be decoded later.
#pragma once

#include <stdint.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;

class AirPlayVideo {
public:
    static constexpr int kWidth = 640;
    static constexpr int kHeight = 480;

    static AirPlayVideo &shared();

    /// Hook the decoder up to the receiver (call once, before the receiver starts).
    static void install();

    /// Copies the newest picture into `dst` (kWidth*kHeight*4 bytes) if newer than `*seen`.
    bool newestFrame(uint8_t *dst, uint64_t *seen, uint64_t *remoteNs = nullptr);
    /// Copies the newest picture captured at or before `remoteNs` (sender clock) if it isn't `*seen`.
    bool frameAtOrBefore(uint64_t remoteNs, uint8_t *dst, uint64_t *seen);

    /// Forget everything (phone disconnected).
    void reset();
    std::string summary();

    /// Receiver thread: one Annex-B access unit.
    void handle(const uint8_t *data, int length, bool hevc, uint64_t remoteNs);

private:
    struct Entry {
        uint64_t remoteNs;
        uint64_t sequence;
        std::vector<uint8_t> pixels;   // kWidth*kHeight*4 BGRA
    };

    bool openDecoder(bool hevc);
    void closeDecoder();
    void scaleInto(const AVFrame *frame, std::vector<uint8_t> &out);

    // decodeLock_: decoder state (receiver thread; reset). lock_: the picture queue and counters, held
    // only briefly, because the MiSTer sender thread takes it every field and must never wait on a decode.
    std::mutex decodeLock_;
    std::mutex lock_;
    AVCodecContext *ctx_ = nullptr;
    AVFrame *frame_ = nullptr;
    AVPacket *packet_ = nullptr;
    bool hevc_ = false;
    int srcW_ = 0, srcH_ = 0;
    std::vector<Entry> queue_;
    std::vector<std::vector<uint8_t>> spare_;   // recycled picture buffers
    static constexpr size_t kQueueLimit = 24;  // ~0.8 s at 30 fps: enough to delay video for lip sync
    uint64_t sequence_ = 0;
    uint64_t decoded_ = 0;
    std::atomic<uint64_t> errors_{0};
};

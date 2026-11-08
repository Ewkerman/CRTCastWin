// Shows the iPhone's mirrored screen on the CRT. Port of CRTCast/AirPlay/AirPlayMirrorSource.swift.
//
// Audio is pulled at the MiSTer's pace from AirPlayAudio (drift-corrected). In lip-sync mode the picture
// is the one the phone captured at the same moment as the audio the MiSTer will be playing when this field
// is on screen (same rule as file playback: audio time - videoDelay). In lowest-delay mode, or while the
// phone isn't sending sound, the newest picture is shown.
#pragma once

#include "media.h"

#include <vector>

class AirPlayMirrorSource : public MediaSource {
public:
    explicit AirPlayMirrorSource(bool lowLatency) : lowLatency_(lowLatency) {}

    void prepare(int width, int height, double videoDelay) override;
    const Picture *nextField(int16_t *audio, int frames) override;
    void beginFadeOut() override { master_.target = 0; }
    void stop() override {}
    std::string statusText() override;

private:
    bool lowLatency_;
    int width_ = 0, height_ = 0;
    int64_t videoDelayNs_ = 0;
    uint64_t seen_ = 0;
    uint64_t shown_ = 0;
    GainRamp master_{0, 1};
    std::vector<uint8_t> frame_;     // 640x480 BGRA from the decoder
    std::vector<uint8_t> output_;    // width x height BGRA when the CRT mode differs
    bool resize_ = false;
    bool synced_ = false;
    uint64_t clock_ = 0;          // sender time (ns) of the audio handed out next
    bool clockValid_ = false;
    int64_t sinceAudio_ = 0;      // audio frames since the phone last sent sound
    Picture picture_;
};

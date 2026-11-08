#include "airplay_source.h"

#include "airplay_audio.h"
#include "airplay_video.h"

#include "scale.h"

#include <string.h>

void AirPlayMirrorSource::prepare(int width, int height, double videoDelay) {
    width_ = width;
    height_ = height;
    videoDelayNs_ = int64_t(videoDelay * 1e9);
    frame_.assign(size_t(AirPlayVideo::kWidth) * AirPlayVideo::kHeight * 4, 0);
    const bool native = width == AirPlayVideo::kWidth && height == AirPlayVideo::kHeight;
    if (!native) {
        // Frames are already 640x480 letterboxed; this only resizes for other CRT modes (320x240p).
        output_.assign(size_t(width) * height * 4, 0);
        resize_ = true;
    }
    picture_.data = native ? frame_.data() : output_.data();
    picture_.stride = width * 4;
    picture_.width = width;
    picture_.height = height;
}

const Picture *AirPlayMirrorSource::nextField(int16_t *audio, int frames) {
    // Audio at the MiSTer's pace (drift-corrected); silence while the phone sends none.
    uint64_t audioTime = 0;
    const bool haveAudio = AirPlayAudio::shared().pull(audio, frames, &audioTime);
    master_.apply(audio, frames);

    // Media clock for lip sync: the phone's timestamp of the audio handed out this field. During a short
    // audio gap (Wi-Fi hiccup, phone pausing its sound) keep counting from the last timestamp instead of
    // jumping to the newest picture, which would make the picture jump forward and back.
    const double nsPerFrame = 1e9 / AudioFormat::sampleRate;
    if (haveAudio) {
        clock_ = audioTime;
        clockValid_ = true;
        sinceAudio_ = 0;
    } else if (clockValid_) {
        sinceAudio_ += frames;
        if (sinceAudio_ > 2 * AudioFormat::sampleRate) clockValid_ = false;   // > 2 s without sound
    }
    const uint64_t fieldClock = clock_;
    if (clockValid_) clock_ += uint64_t(frames * nsPerFrame);   // where the next field's audio starts

    bool got;
    if (!lowLatency_ && clockValid_) {
        // Lip sync: the picture the phone captured when the audio the MiSTer will be playing (while this
        // field is on screen) was captured.
        synced_ = true;
        const int64_t target = int64_t(fieldClock) - videoDelayNs_;
        got = AirPlayVideo::shared().frameAtOrBefore(uint64_t(target > 0 ? target : 0), frame_.data(), &seen_);
    } else {
        synced_ = false;
        got = AirPlayVideo::shared().newestFrame(frame_.data(), &seen_);
    }
    if (!got) return nullptr;
    if (resize_)
        Scale::bgraResize(frame_.data(), AirPlayVideo::kWidth * 4, AirPlayVideo::kWidth, AirPlayVideo::kHeight,
                          output_.data(), width_ * 4, width_, height_);
    shown_++;
    return &picture_;
}

std::string AirPlayMirrorSource::statusText() {
    std::string mode = lowLatency_ ? "lowest delay" : synced_ ? "lip sync" : "no sound yet";
    return "AirPlay mirror (" + mode + ") \xC2\xB7 " + AirPlayVideo::shared().summary() + " \xC2\xB7 shown " +
           std::to_string(shown_) + " \xC2\xB7 audio " + AirPlayAudio::shared().summary();
}

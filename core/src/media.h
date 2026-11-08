// Shared types for the CRTCast Windows engine. Mirrors CRTCast/Modeline.swift and
// CRTCast/FrameSources.swift so both platforms behave the same.
#pragma once

#include <stdint.h>
#include <string>

/// A CRT video mode in Switchres format (pixel clock in MHz, timings in pixels/lines).
struct Modeline {
    const char *id;
    const char *name;          // UTF-8
    double pClockMHz;
    int hActive, hBegin, hEnd, hTotal;
    int vActive, vBegin, vEnd, vTotal;
    bool interlaced;
    /// Height the picture is rendered at before it is sampled into lines
    /// (640x240 is rendered at 640x480 and every other line is used).
    int renderHeight;

    int linesPerBlit() const { return interlaced ? vActive / 2 : vActive; }
    double refreshHz() const {
        double frameHz = pClockMHz * 1e6 / double(hTotal * vTotal);
        return interlaced ? frameHz * 2 : frameHz;
    }
    double megabitsPerSecond(int bytesPerPixel) const {
        return double(hActive * linesPerBlit() * bytesPerPixel) * refreshHz() * 8 / 1e6;
    }
};

inline const Modeline kModelines[3] = {
    {"320x240p", "320\xC3\x97" "240p", 6.700, 320, 336, 367, 426, 240, 244, 247, 262, false, 240},
    {"640x240p", "640\xC3\x97" "240p", 13.400, 640, 672, 734, 852, 240, 244, 247, 262, false, 480},
    {"640x480i", "640\xC3\x97" "480i", 12.336, 640, 662, 720, 784, 480, 488, 494, 525, true, 480},
};

/// Audio sent to the MiSTer is always 48 kHz stereo Int16. The Groovy core plays 48 kHz exactly
/// (24.576 MHz / 512); its 44.1 kHz mode actually runs slightly slow.
namespace AudioFormat {
constexpr int sampleRate = 48000;
constexpr int channels = 2;
/// 100 ms of silence queued in the MiSTer's FIFO (8192 stereo frames) before playback so it never
/// runs dry if we're a few fields late. The picture is delayed by the same amount.
constexpr int cushionFrames = 4800;
}

/// Click-free volume ramp applied per audio frame (10 ms at 48 kHz by default).
struct GainRamp {
    float gain = 0;
    float target = 0;
    float step = 1.0f / 480;

    bool isSilent() const { return gain == 0 && target == 0; }

    void apply(int16_t *samples, int frames) {
        if (gain == 1 && target == 1) return;
        if (isSilent()) {
            for (int i = 0; i < frames * AudioFormat::channels; i++) samples[i] = 0;
            return;
        }
        for (int i = 0; i < frames; i++) {
            if (gain < target) gain = gain + step > target ? target : gain + step;
            else if (gain > target) gain = gain - step < target ? target : gain - step;
            for (int c = 0; c < AudioFormat::channels; c++) {
                int16_t &s = samples[i * AudioFormat::channels + c];
                s = int16_t(float(s) * gain);
            }
        }
    }
};

/// A BGRA picture (top-down rows).
struct Picture {
    const uint8_t *data = nullptr;
    int stride = 0;
    int width = 0;
    int height = 0;
};

/// Something that produces picture + sound on the MiSTer's timeline.
/// The sender calls nextField once per CRT field/frame and asks for exactly the number of audio
/// frames the MiSTer will play during that field. A source advances its own clock only by the audio
/// it hands out, so picture and sound can't drift apart: both are paced by the MiSTer's raster.
class MediaSource {
public:
    virtual ~MediaSource() = default;
    /// videoDelay: seconds the picture must lag the audio handed out in the same field.
    virtual void prepare(int width, int height, double videoDelay) = 0;
    /// Fill `audio` with `frames` stereo frames (silence if nothing to play) and return the
    /// picture to show, or nullptr to keep the previous one.
    virtual const Picture *nextField(int16_t *audio, int frames) = 0;
    /// Ramp the sound down to silence (before disconnecting, so the speaker doesn't pop).
    virtual void beginFadeOut() = 0;
    virtual void stop() = 0;
    virtual std::string statusText() = 0;
};

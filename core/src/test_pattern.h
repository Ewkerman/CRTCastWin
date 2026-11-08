// Color bars, border, center cross and a moving box, plus an A/V sync check: for the first
// 100 ms of every second a white box flashes and a 1 kHz tone beeps. On the CRT they should
// happen together. Port of TestPatternSource in CRTCast/FrameSources.swift (drawn with GDI).
#pragma once

#include "media.h"

#include <windows.h>

class TestPatternSource : public MediaSource {
public:
    explicit TestPatternSource(const char *label) : label_(label) {}
    ~TestPatternSource() override;

    void prepare(int width, int height, double videoDelay) override;
    const Picture *nextField(int16_t *audio, int frames) override;
    void beginFadeOut() override { master_.target = 0; }
    void stop() override {}
    std::string statusText() override { return "Test pattern \xC2\xB7 beep + flash every second"; }

private:
    void draw(bool flash, double pictureTime);

    std::string label_;
    int width_ = 0, height_ = 0;
    double videoDelay_ = 0;
    int64_t audioPosition_ = 0;     // media clock, in audio frames
    double tonePhase_ = 0;
    float toneEnvelope_ = 0;
    GainRamp master_{0, 1};
    int fieldCount_ = 0;

    HDC dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ oldBitmap_ = nullptr;
    HFONT font_ = nullptr;
    uint8_t *bits_ = nullptr;
    Picture picture_;
};

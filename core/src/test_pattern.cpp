#include "test_pattern.h"

#include <math.h>
#include <stdio.h>

static const double kPi = 3.14159265358979323846;

TestPatternSource::~TestPatternSource() {
    if (dc_) {
        if (oldBitmap_) SelectObject(dc_, oldBitmap_);
        DeleteDC(dc_);
    }
    if (bitmap_) DeleteObject(bitmap_);
    if (font_) DeleteObject(font_);
}

void TestPatternSource::prepare(int width, int height, double videoDelay) {
    width_ = width;
    height_ = height;
    videoDelay_ = videoDelay;

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;   // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    dc_ = CreateCompatibleDC(nullptr);
    bitmap_ = CreateDIBSection(dc_, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dc_ || !bitmap_ || !bits) return;
    oldBitmap_ = SelectObject(dc_, bitmap_);
    bits_ = static_cast<uint8_t *>(bits);

    int fontHeight = height / 18 > 10 ? height / 18 : 10;
    font_ = CreateFontW(-fontHeight, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY,
                        FIXED_PITCH | FF_MODERN, L"Consolas");

    picture_.data = bits_;
    picture_.stride = width * 4;
    picture_.width = width;
    picture_.height = height;
}

const Picture *TestPatternSource::nextField(int16_t *audio, int frames) {
    const int64_t fieldStart = audioPosition_;
    const int64_t rate = AudioFormat::sampleRate;
    const int64_t beepFrames = rate / 10;
    for (int i = 0; i < frames; i++) {
        int64_t t = fieldStart + i;
        bool inBeep = t % rate < beepFrames;
        // 2 ms attack/release so the beep itself doesn't click.
        const float envStep = 1.0f / 96;
        toneEnvelope_ = inBeep ? (toneEnvelope_ + envStep > 1 ? 1 : toneEnvelope_ + envStep)
                               : (toneEnvelope_ - envStep < 0 ? 0 : toneEnvelope_ - envStep);
        tonePhase_ += 2 * kPi * 1000 / double(AudioFormat::sampleRate);
        if (tonePhase_ > 2 * kPi) tonePhase_ -= 2 * kPi;
        int16_t s = int16_t(sin(tonePhase_) * toneEnvelope_ * 0.3 * 32767);
        audio[i * 2] = s;
        audio[i * 2 + 1] = s;
    }
    master_.apply(audio, frames);
    audioPosition_ += frames;

    // Picture time that matches the audio the MiSTer will be playing when this field is on screen.
    double pictureTime = double(fieldStart) / double(rate) - videoDelay_;
    bool flash = pictureTime >= 0 && fmod(pictureTime, 1.0) < 0.1;
    fieldCount_++;
    if (!bits_) return nullptr;
    draw(flash, pictureTime);
    return &picture_;
}

static void fill(HDC dc, double x, double y, double w, double h, COLORREF color) {
    RECT r = {LONG(x), LONG(y), LONG(x + w), LONG(y + h)};
    HBRUSH b = CreateSolidBrush(color);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

void TestPatternSource::draw(bool flash, double pictureTime) {
    const double w = width_, h = height_;
    static const COLORREF bars[8] = {RGB(255, 255, 255), RGB(255, 255, 0), RGB(0, 255, 255), RGB(0, 255, 0),
                                     RGB(255, 0, 255), RGB(255, 0, 0), RGB(0, 0, 255), RGB(0, 0, 0)};
    const double barW = w / 8;
    for (int i = 0; i < 8; i++) fill(dc_, i * barW, 0, barW + 1, h * 0.62, bars[i]);
    for (int i = 0; i < 8; i++) {
        int v = i * 255 / 7;
        fill(dc_, i * barW, h * 0.62, barW + 1, h * 0.38 + 1, RGB(v, v, v));
    }

    const COLORREF white = RGB(255, 255, 255);
    fill(dc_, 0, 0, w, 2, white);
    fill(dc_, 0, h - 2, w, 2, white);
    fill(dc_, 0, 0, 2, h, white);
    fill(dc_, w - 2, 0, 2, h, white);
    fill(dc_, w / 2 - 1, 0, 2, h * 0.62, white);
    fill(dc_, 0, h * 0.31 - 1, w, 2, white);

    // Moving box.
    const double box = h / 8;
    const int travel = int(w - box - 8);
    int x = (fieldCount_ * 4) % (travel * 2);
    if (x > travel) x = travel * 2 - x;
    fill(dc_, 4 + x, h * 0.70, box, box, white);
    fill(dc_, 4 + x + box / 4, h * 0.70 + box / 4, box / 2, box / 2, RGB(0, 0, 0));

    // A/V sync flash: big box in the middle of the color bars.
    if (flash) fill(dc_, w * 0.3, h * 0.12, w * 0.4, h * 0.38, white);

    double seconds = pictureTime > 0 ? pictureTime : 0;
    char text[96];
    snprintf(text, sizeof(text), "CRTCast %s  %02d:%05.2f", label_.c_str(), int(seconds) / 60, fmod(seconds, 60.0));
    HGDIOBJ oldFont = SelectObject(dc_, font_);
    SetBkMode(dc_, TRANSPARENT);
    SetTextColor(dc_, RGB(255, 255, 0));
    TextOutA(dc_, 8, int(h * 0.86), text, int(strlen(text)));
    SelectObject(dc_, oldFont);
    GdiFlush();
}

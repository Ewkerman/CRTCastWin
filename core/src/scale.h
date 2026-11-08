// Small CPU scalers for the mirror picture (instead of FFmpeg's libswscale, which needs a newer
// Windows SDK to build). Area averaging, so downscaling a phone screen (e.g. 1080 lines -> 480) keeps
// text readable instead of aliasing.
#pragma once

#include <stdint.h>

#include <vector>

namespace Scale {

/// One planar/semi-planar 4:2:0 YUV picture.
struct Yuv420 {
    const uint8_t *y = nullptr, *u = nullptr, *v = nullptr;   // v == nullptr: u is interleaved UV (NV12)
    int yStride = 0, uStride = 0, vStride = 0;
    int width = 0, height = 0;
    int bytesPerSample = 1;    // 2 for 10-bit (little-endian, value in the low bits)
    int shift = 0;             // right shift to 8 bits (2 for 10-bit)
    bool fullRange = false;    // JPEG range (0-255) instead of video range (16-235)
    bool bt709 = true;         // colour matrix (BT.709 for HD, BT.601 for SD)
};

/// Source spans for area averaging: dst index i covers src [start[i], end[i]).
inline void spans(int src, int dst, std::vector<int> &start, std::vector<int> &end) {
    start.resize(size_t(dst));
    end.resize(size_t(dst));
    for (int i = 0; i < dst; i++) {
        int a = int(int64_t(i) * src / dst);
        int b = int(int64_t(i + 1) * src / dst);
        start[size_t(i)] = a;
        end[size_t(i)] = b > a ? b : a + 1;
    }
}

inline int sample(const uint8_t *row, int x, int bps, int shift) {
    return bps == 1 ? row[x] : int(reinterpret_cast<const uint16_t *>(row)[x] >> shift);
}

inline uint8_t clamp8(int v) { return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v); }

/// YUV 4:2:0 -> BGRA into a dw x dh rectangle at `dst` (row stride dstStride bytes), area-averaged.
inline void yuvToBgra(const Yuv420 &s, uint8_t *dst, int dstStride, int dw, int dh) {
    static thread_local std::vector<int> xs, xe, ys, ye, cxs, cxe, cys, cye;
    spans(s.width, dw, xs, xe);
    spans(s.height, dh, ys, ye);
    const int cw = (s.width + 1) / 2, ch = (s.height + 1) / 2;
    spans(cw, dw, cxs, cxe);
    spans(ch, dh, cys, cye);
    const int bps = s.bytesPerSample, sh = s.shift;
    const bool nv12 = s.v == nullptr;

    // Fixed-point (x1024) YUV -> RGB coefficients.
    int ky, kr, kgu, kgv, kb, yOff;
    if (s.fullRange) {
        ky = 1024; yOff = 0;
        if (s.bt709) { kr = 1613; kgu = 192; kgv = 479; kb = 1900; } else { kr = 1436; kgu = 352; kgv = 731; kb = 1815; }
    } else {
        ky = 1192; yOff = 16;
        if (s.bt709) { kr = 1836; kgu = 218; kgv = 546; kb = 2163; } else { kr = 1634; kgu = 401; kgv = 832; kb = 2066; }
    }

    for (int j = 0; j < dh; j++) {
        uint8_t *out = dst + size_t(j) * dstStride;
        const int y0 = ys[size_t(j)], y1 = ye[size_t(j)], c0 = cys[size_t(j)], c1 = cye[size_t(j)];
        for (int i = 0; i < dw; i++) {
            const int x0 = xs[size_t(i)], x1 = xe[size_t(i)];
            int sumY = 0;
            for (int yy = y0; yy < y1; yy++) {
                const uint8_t *row = s.y + size_t(yy) * s.yStride;
                for (int xx = x0; xx < x1; xx++) sumY += sample(row, xx, bps, sh);
            }
            const int Y = sumY / ((y1 - y0) * (x1 - x0));

            const int u0 = cxs[size_t(i)], u1 = cxe[size_t(i)];
            int sumU = 0, sumV = 0;
            for (int yy = c0; yy < c1; yy++) {
                const uint8_t *ur = s.u + size_t(yy) * s.uStride;
                if (nv12) {
                    for (int xx = u0; xx < u1; xx++) { sumU += sample(ur, 2 * xx, bps, sh); sumV += sample(ur, 2 * xx + 1, bps, sh); }
                } else {
                    const uint8_t *vr = s.v + size_t(yy) * s.vStride;
                    for (int xx = u0; xx < u1; xx++) { sumU += sample(ur, xx, bps, sh); sumV += sample(vr, xx, bps, sh); }
                }
            }
            const int n = (c1 - c0) * (u1 - u0);
            const int U = sumU / n - 128, V = sumV / n - 128;
            const int L = (Y - yOff) * ky;
            out[i * 4 + 0] = clamp8((L + kb * U + 512) >> 10);
            out[i * 4 + 1] = clamp8((L - kgu * U - kgv * V + 512) >> 10);
            out[i * 4 + 2] = clamp8((L + kr * V + 512) >> 10);
            out[i * 4 + 3] = 255;
        }
    }
}

/// BGRA -> BGRA resize, area-averaged (used for 320x240p from the 640x480 picture).
inline void bgraResize(const uint8_t *src, int srcStride, int sw, int sh, uint8_t *dst, int dstStride, int dw, int dh) {
    static thread_local std::vector<int> xs, xe, ys, ye;
    spans(sw, dw, xs, xe);
    spans(sh, dh, ys, ye);
    for (int j = 0; j < dh; j++) {
        uint8_t *out = dst + size_t(j) * dstStride;
        for (int i = 0; i < dw; i++) {
            int sum[4] = {0, 0, 0, 0};
            for (int yy = ys[size_t(j)]; yy < ye[size_t(j)]; yy++) {
                const uint8_t *p = src + size_t(yy) * srcStride;
                for (int xx = xs[size_t(i)]; xx < xe[size_t(i)]; xx++)
                    for (int c = 0; c < 4; c++) sum[c] += p[xx * 4 + c];
            }
            const int n = (ye[size_t(j)] - ys[size_t(j)]) * (xe[size_t(i)] - xs[size_t(i)]);
            for (int c = 0; c < 4; c++) out[i * 4 + c] = uint8_t(sum[c] / n);
        }
    }
}

}  // namespace Scale

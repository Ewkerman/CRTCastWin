// Copies lines out of a BGRA picture into the Groovy blit buffer, converting to the MiSTer's
// pixel format. Same rules as CRTCast/FramePacker.swift:
//  - Groovy "RGB888" is byte order B,G,R.
//  - RGB565 is red in the high bits, little-endian.
#pragma once

#include "media.h"

#include <string.h>

namespace Packer {

/// firstRow: first source row (0/1 for interlaced fields). rowStep: take every Nth source row.
inline void pack(const Picture &src, uint8_t *dst, int width, int firstRow, int rowStep, int rows, bool rgb565) {
    const int w = src.width < width ? src.width : width;
    int available = src.height > firstRow ? (src.height - firstRow + rowStep - 1) / rowStep : 0;
    const int h = rows < available ? rows : available;
    if (w <= 0 || h <= 0) return;
    const int bpp = rgb565 ? 2 : 3;
    const int dstStride = width * bpp;

    for (int y = 0; y < h; y++) {
        const uint8_t *s = src.data + size_t(firstRow + y * rowStep) * src.stride;
        uint8_t *d = dst + size_t(y) * dstStride;
        if (rgb565) {
            for (int x = 0; x < w; x++, s += 4, d += 2) {
                uint16_t v = uint16_t(((s[2] >> 3) << 11) | ((s[1] >> 2) << 5) | (s[0] >> 3));
                d[0] = uint8_t(v & 0xFF);
                d[1] = uint8_t(v >> 8);
            }
        } else {
            for (int x = 0; x < w; x++, s += 4, d += 3) {
                d[0] = s[0];   // B
                d[1] = s[1];   // G
                d[2] = s[2];   // R
            }
        }
    }
}

}  // namespace Packer

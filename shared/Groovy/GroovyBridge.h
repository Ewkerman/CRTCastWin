// Plain C interface over the Groovy_MiSTer sender (groovymister.cpp) so Swift can call it.
#ifndef GROOVY_BRIDGE_H
#define GROOVY_BRIDGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Mirror of the FPGA status the MiSTer sends back after each command.
typedef struct GMStatus {
    uint32_t frame;        // frame currently on the FPGA
    uint32_t frameEcho;    // last frame received
    uint16_t vCount;       // current raster line
    uint16_t vCountEcho;
    uint8_t vramEndFrame;  // FPGA has all pixels of the last blit
    uint8_t vramReady;     // FPGA has free VRAM
    uint8_t vramSynced;    // 1 = in sync (no red screen)
    uint8_t vgaFrameskip;
    uint8_t vgaVblank;
    uint8_t vgaF1;         // current interlaced field
    uint8_t audio;
    uint8_t vramQueue;
} GMStatus;

typedef struct GMHandle GMHandle;

GMHandle *gm_create(void);
void gm_destroy(GMHandle *h);

/// lz4Mode: 0 raw, 1 LZ4, 3 LZ4HC.  rgbMode: 0 BGR888, 1 BGRA8888, 2 RGB565.
/// soundRate: 0 (no audio), 22050, 44100 or 48000.  soundChannels: 0, 1 or 2.
/// mtu: 1500 normal, 3800 jumbo. Returns 0 when the MiSTer answered.
int gm_init(GMHandle *h, const char *host, uint16_t port, int lz4Mode,
            uint32_t soundRate, uint8_t soundChannels, uint8_t rgbMode, uint16_t mtu);

/// pclockMHz plus a Switchres-style modeline. interlace: 0 progressive, 1 interlaced (field buffers).
void gm_switchres(GMHandle *h, double pclockMHz,
                  uint16_t hActive, uint16_t hBegin, uint16_t hEnd, uint16_t hTotal,
                  uint16_t vActive, uint16_t vBegin, uint16_t vEnd, uint16_t vTotal,
                  uint8_t interlace);

/// Buffer to fill with pixels before gm_blit (one per field).
uint8_t *gm_blit_buffer(GMHandle *h, uint8_t field);

/// Buffer for audio: signed 16-bit little-endian PCM, channels interleaved. Fill before gm_audio.
int16_t *gm_audio_buffer(GMHandle *h);

/// Sends `bytes` of audio from gm_audio_buffer. Ignored unless the MiSTer reports audio enabled.
void gm_audio(GMHandle *h, uint16_t bytes);

/// vSyncLine: raster line to sync to (0 = automatic frame delay).
void gm_blit(GMHandle *h, uint32_t frame, uint8_t field, uint16_t vSyncLine, uint32_t margin);

/// Sleeps until it is time for the next frame/field (follows the CRT raster).
void gm_wait_sync(GMHandle *h);

GMStatus gm_status(GMHandle *h);

void gm_close(GMHandle *h);

void gm_set_verbose(GMHandle *h, uint8_t level);

#ifdef __cplusplus
}
#endif

#endif

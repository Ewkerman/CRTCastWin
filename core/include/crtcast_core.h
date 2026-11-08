// CRTCast Windows core: plain C interface over the native engine (Groovy sender, test pattern,
// and later the AirPlay receiver + decoders). Called from the C# app with P/Invoke.
// Everything timing-critical runs on native threads, so .NET garbage collection can't delay a field.
#ifndef CRTCAST_CORE_H
#define CRTCAST_CORE_H

#include <stdint.h>

#ifdef CRTCAST_CORE_BUILD
#define CC_API __declspec(dllexport)
#else
#define CC_API __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/// Video mode presets (same as CRTCast/Modeline.swift).
enum { CC_MODE_320x240p = 0, CC_MODE_640x240p = 1, CC_MODE_640x480i = 2, CC_MODE_COUNT = 3 };

/// What to show on the CRT.
enum { CC_SOURCE_TEST_PATTERN = 0, CC_SOURCE_AIRPLAY = 1 };

typedef struct cc_config {
    char host[64];          // MiSTer IPv4 address
    int32_t mode;           // CC_MODE_*
    int32_t rgb565;         // 1 = RGB565, 0 = BGR888
    int32_t lz4;            // 1 = LZ4 compression
    int32_t jumbo;          // 1 = 3800-byte MTU (wired only)
    int32_t swap_fields;    // 1 = swap even/odd field order (interlaced)
    int32_t send_audio;     // 1 = send 48 kHz stereo audio
    int32_t low_latency;    // 1 = AirPlay lowest-delay mode (no lip sync)
    double audio_delay_ms;  // + = sound later, - = sound earlier
} cc_config;

typedef struct cc_stats {
    int32_t running;           // sender thread active (including fade-out)
    char state[96];            // "Streaming 640×480i", "Stopped", ...
    char error[256];           // empty when fine
    uint64_t blits;
    double blits_per_second;
    uint64_t new_pictures;
    double pack_ms;
    double send_ms;
    int32_t mister_synced;
    int32_t mister_audio;
    uint32_t mister_frame;
    double audio_seconds;
    int32_t audio_catch_ups;
    uint64_t late_fields;      // fields the MiSTer showed without a fresh blit from us
    char source_status[256];
    uint64_t net_sends_completed;  // RIO send completions taken off groovymister's queue
    int32_t net_queue_overflowed;  // 1 if that queue ever overflowed (should stay 0)
    double pack_max_ms;            // slowest picture+audio preparation in the last stats window
    double send_max_ms;            // slowest audio+blit send in the last stats window
} cc_stats;

typedef struct cc_mode_info {
    char name[32];             // UTF-8, e.g. "640×480i"
    int32_t width;
    int32_t lines_per_blit;
    int32_t interlaced;
    double refresh_hz;
    double mbps_rgb888;        // before compression
    double mbps_rgb565;
} cc_mode_info;

CC_API const char *cc_version(void);
CC_API int cc_get_mode_info(int32_t mode, cc_mode_info *out);

/// Starts streaming `source` to the MiSTer on a native high-priority thread.
/// Returns 0 if started, -1 if already running, -2 for a bad config.
CC_API int cc_start(const cc_config *config, int32_t source);
/// Asks the stream to stop (fades the sound out first). Returns immediately.
CC_API void cc_stop(void);
/// Waits up to `timeout_ms` for the sender thread to finish. Returns 1 if it has.
CC_API int cc_wait_stopped(int32_t timeout_ms);
CC_API void cc_get_stats(cc_stats *out);

/// Copies the newest picture sent to the MiSTer (BGRA, top-down) if it changed since `*seq`.
/// `capacity` is the size of `bgra` in bytes. Returns 1 and updates *width/*height/*seq when copied.
CC_API int cc_get_preview(uint8_t *bgra, int32_t capacity, int32_t *width, int32_t *height, uint64_t *seq);

/// Connectivity check: INIT + status + CLOSE, no video mode change. Returns 0 if the MiSTer answered.
/// Fails if a stream is running.
CC_API int cc_probe(const char *host, int32_t *audio_on, char *message, int32_t message_size);

// ---- AirPlay receiver (UxPlay) ----

/// Same layout as crt_airplay_status in CRTCast/AirPlay/AirPlayBridge.h.
typedef struct cc_airplay_status {
    int32_t running;             // receiver started and advertised
    int32_t connections;         // clients currently connected
    int32_t total_connections;
    char client_name[128];       // e.g. "Eric's iPhone" (UTF-8)
    char client_model[64];       // e.g. "iPhone18,1"
    int32_t mirroring;           // screen-mirror video is flowing
    int32_t codec;               // 1 = H.264, 2 = H.265
    int32_t width, height;       // size of the mirrored picture
    uint64_t video_frames;
    uint64_t video_bytes;
    uint64_t audio_packets;
    int32_t audio_codec;         // AirPlay "ct": 2 = ALAC, 4 = AAC-LC, 8 = AAC-ELD
    uint16_t port;               // TCP port the receiver listens on
    char last_event[160];
    char last_error[160];
} cc_airplay_status;

/// Starts the receiver and advertises it (Windows mDNS) as `name` in the iPhone's Screen Mirroring list.
/// Returns 0 on success; see cc_airplay_get_status().last_error otherwise.
CC_API int cc_airplay_start(const char *name);
CC_API void cc_airplay_stop(void);
CC_API void cc_airplay_get_status(cc_airplay_status *out);
/// Recent receiver log lines, newest last, '\n'-separated.
CC_API void cc_airplay_get_log(char *buffer, int32_t size);
/// Forget decoded pictures and decoder state (the phone disconnected).
CC_API void cc_airplay_reset_media(void);
/// Newest decoded mirror picture, 640x480 BGRA (capacity >= 640*480*4), if newer than *seq. Returns 1 if copied.
CC_API int cc_airplay_get_preview(uint8_t *bgra, int32_t capacity, uint64_t *seq);
/// Decoder summary, e.g. "H.264 498x1080 · decoded 812".
CC_API void cc_airplay_get_video_summary(char *buffer, int32_t size);

#ifdef __cplusplus
}
#endif

#endif

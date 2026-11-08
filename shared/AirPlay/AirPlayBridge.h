// C interface between CRTCast (Swift) and the UxPlay AirPlay receiver core.
#ifndef CRTCAST_AIRPLAY_BRIDGE_H
#define CRTCAST_AIRPLAY_BRIDGE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int running;                 // receiver started and advertised
    int connections;             // clients currently connected
    int total_connections;
    char client_name[128];       // e.g. "Eric's iPhone"
    char client_model[64];       // e.g. "iPhone18,1"
    int mirroring;               // screen-mirror video is flowing
    int codec;                   // 1 = H.264, 2 = H.265
    int width, height;           // size of the mirrored picture
    uint64_t video_frames;
    uint64_t video_bytes;
    uint64_t audio_packets;
    int audio_codec;             // AirPlay "ct": 2 = ALAC, 4 = AAC-LC, 8 = AAC-ELD
    unsigned short port;         // TCP port the receiver listens on
    char last_event[160];
    char last_error[160];
} crt_airplay_status;

/// Starts the receiver and advertises it over Bonjour as `name` (shows up in the iPhone's
/// Screen Mirroring list). `hw_addr` is a 6-byte, stable, locally administered address.
/// Returns 0 on success.
int crt_airplay_start(const char *name, const unsigned char hw_addr[6]);
void crt_airplay_stop(void);

/// Called on the receiver's thread for every mirrored video frame: Annex-B NAL units
/// (4-byte start codes), with the parameter sets (VPS/SPS/PPS) prepended to keyframes.
/// `remote_ns` is the sender's capture time (nanoseconds, sender clock) for lip sync.
typedef void (*crt_airplay_video_callback)(const unsigned char *data, int length, int is_h265, uint64_t remote_ns);
void crt_airplay_set_video_callback(crt_airplay_video_callback callback);

/// Called for every decrypted audio packet. ct: 2 = ALAC, 4 = AAC-LC, 8 = AAC-ELD (44.1 kHz stereo).
typedef void (*crt_airplay_audio_callback)(const unsigned char *data, int length, int ct, uint64_t remote_ns);
void crt_airplay_set_audio_callback(crt_airplay_audio_callback callback);

/// Testing only: pretend a phone is (or isn't) mirroring, so the app's auto start/stop can be exercised.
void crt_airplay_debug_set_mirroring(int mirroring);

/// Snapshot of the current state (thread-safe copy).
void crt_airplay_get_status(crt_airplay_status *out);

/// Recent log lines from the receiver, newest last, separated by '\n'. Thread-safe copy.
void crt_airplay_get_log(char *buffer, int size);

#ifdef __cplusplus
}
#endif

#endif

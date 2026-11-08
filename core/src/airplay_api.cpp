// CRTCast Windows core: AirPlay receiver API. Thin wrapper over CRTCast/AirPlay/AirPlayBridge.c
// (shared unchanged with the Apple apps), which drives the UxPlay core.
#define CRTCAST_CORE_BUILD
#include "crtcast_core.h"

#include <winsock2.h>
#include <iphlpapi.h>
#include <windows.h>

#include <stdio.h>
#include <string.h>
#include <vector>

#include "airplay_audio.h"
#include "airplay_video.h"

extern "C" {
#include "AirPlayBridge.h"
}

static_assert(sizeof(cc_airplay_status) == sizeof(crt_airplay_status), "cc_airplay_status must mirror crt_airplay_status");

/// A stable, locally administered 6-byte device ID: the LAN adapter's MAC with the "local" bit set,
/// so the iPhone sees the same receiver every launch but it never equals a real hardware address.
static void make_device_id(unsigned char out[6]) {
    const unsigned char fallback[6] = {0x02, 0xC7, 0x7C, 0xA5, 0x70, 0x01};
    memcpy(out, fallback, 6);

    DWORD best = 0;
    if (GetBestInterface(inet_addr("8.8.8.8"), &best) != NO_ERROR) best = 0;
    ULONG size = 32 * 1024;
    std::vector<unsigned char> buf(size);
    auto *list = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buf.data());
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                             nullptr, list, &size) != NO_ERROR)
        return;
    for (auto *a = list; a; a = a->Next) {
        if (a->PhysicalAddressLength != 6) continue;
        if (best && a->IfIndex != best) continue;
        memcpy(out, a->PhysicalAddress, 6);
        out[0] = (unsigned char) ((out[0] | 0x02) & ~0x01);   // locally administered, unicast
        return;
    }
}

extern "C" {

CC_API int cc_airplay_start(const char *name) {
    // This DLL has its own C runtime (static /MT): make its stdout unbuffered so receiver log lines
    // reach a console (crtcast_test) immediately.
    setvbuf(stdout, nullptr, _IONBF, 0);
    WSADATA wsd;
    WSAStartup(MAKEWORD(2, 2), &wsd);   // UxPlay starts Winsock itself, but only after Bonjour setup
    static bool installed = false;
    if (!installed) { AirPlayVideo::install(); AirPlayAudio::install(); installed = true; }
    unsigned char id[6];
    make_device_id(id);
    return crt_airplay_start(name && *name ? name : "CRTCast", id);
}

CC_API void cc_airplay_stop(void) { crt_airplay_stop(); }

CC_API void cc_airplay_get_status(cc_airplay_status *out) {
    if (!out) return;
    crt_airplay_status s;
    crt_airplay_get_status(&s);
    memcpy(out, &s, sizeof(s));
}

CC_API void cc_airplay_get_log(char *buffer, int32_t size) { crt_airplay_get_log(buffer, size); }

CC_API void cc_airplay_reset_media(void) {
    AirPlayVideo::shared().reset();
    AirPlayAudio::shared().reset();
}

CC_API int cc_airplay_get_preview(uint8_t *bgra, int32_t capacity, uint64_t *seq) {
    if (!bgra || !seq || capacity < AirPlayVideo::kWidth * AirPlayVideo::kHeight * 4) return 0;
    return AirPlayVideo::shared().newestFrame(bgra, seq) ? 1 : 0;
}

CC_API void cc_airplay_get_video_summary(char *buffer, int32_t size) {
    if (buffer && size > 0) snprintf(buffer, size_t(size), "%s", AirPlayVideo::shared().summary().c_str());
}

}  // extern "C"

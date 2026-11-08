// CRTCast Windows stream engine: port of CRTCast/StreamEngine.swift.
//
// Runs the Groovy send loop on its own time-critical thread:
// pick field -> audio + picture from the source -> pack -> send audio -> blit -> wait for the raster.
//
// Audio scheduling: the MiSTer plays exactly 48 kHz. We keep its FIFO at a constant level by sending,
// each field, the audio for the fields the MiSTer has actually displayed (its own frame counter), on
// top of a 100 ms cushion queued at the start. A late or repeated field therefore can't drain the
// FIFO, and nothing is ever dropped or inserted, so there are no pops.
#define CRTCAST_CORE_BUILD
#include "crtcast_core.h"

#include "GroovyBridge.h"
#include "groovy_rio.h"
#include "media.h"
#include "packer.h"
#include "test_pattern.h"
#include "airplay_source.h"

#include <windows.h>
#include <avrt.h>
#include <timeapi.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread>
#include <vector>

namespace {

const uint16_t kPort = 32100;

double now() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return double(t.QuadPart) / double(freq.QuadPart);
}

template <size_t N> void copyText(char (&dst)[N], const std::string &s) {
    snprintf(dst, N, "%s", s.c_str());
}

struct Engine {
    std::mutex statsLock;
    cc_stats stats{};
    std::atomic<bool> keepGoing{false};
    std::atomic<bool> active{false};
    std::thread thread;

    std::mutex previewLock;
    std::vector<uint8_t> preview;
    int previewWidth = 0, previewHeight = 0;
    uint64_t previewSeq = 0;

    ~Engine() {
        // DLL unload / process exit: never destroy a joinable std::thread (that calls std::terminate).
        keepGoing = false;
        if (thread.joinable()) thread.detach();
    }

    template <class F> void update(F change) {
        std::lock_guard<std::mutex> g(statsLock);
        change(stats);
    }

    void storePreview(const Picture &p) {
        std::lock_guard<std::mutex> g(previewLock);
        previewWidth = p.width;
        previewHeight = p.height;
        preview.resize(size_t(p.width) * p.height * 4);
        for (int y = 0; y < p.height; y++)
            memcpy(&preview[size_t(y) * p.width * 4], p.data + size_t(y) * p.stride, size_t(p.width) * 4);
        previewSeq++;
    }

    void run(cc_config config, std::unique_ptr<MediaSource> source);
};

Engine g_engine;

/// Keeps Windows from slowing our timing down while streaming: 1 ms timer resolution, no power
/// throttling (EcoQoS) for the process, no idle sleep, and a "Pro Audio" MMCSS boost for the thread.
struct TimingGuard {
    HANDLE mmcss = nullptr;
    TimingGuard() {
        timeBeginPeriod(1);
        PROCESS_POWER_THROTTLING_STATE pt = {};
        pt.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        pt.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
        pt.StateMask = 0;
        SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof(pt));
        SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);
        DWORD taskIndex = 0;
        mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    }
    ~TimingGuard() {
        if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
        SetThreadExecutionState(ES_CONTINUOUS);
        timeEndPeriod(1);
    }
};

void Engine::run(cc_config config, std::unique_ptr<MediaSource> source) {
    TimingGuard timing;
    const Modeline &m = kModelines[config.mode];
    const int rate = AudioFormat::sampleRate;
    const int channels = AudioFormat::channels;

    GMHandle *gm = gm_create();
    if (!gm) {
        update([](cc_stats &s) { copyText(s.state, "Stopped"); copyText(s.error, "Could not create the Groovy sender."); });
        return;
    }
    int rc = gm_init(gm, config.host, kPort, config.lz4 ? 1 : 0,
                     config.send_audio ? uint32_t(rate) : 0, config.send_audio ? uint8_t(channels) : 0,
                     config.rgb565 ? 2 : 0, config.jumbo ? 3800 : 1500);
    if (rc != 0) {
        gm_close(gm);   // releases the sockets and Winsock
        gm_destroy(gm);
        std::string host = config.host;
        update([&](cc_stats &s) {
            copyText(s.state, "Stopped");
            copyText(s.error, "No answer from " + host + ". Is the MiSTer running the Groovy core, and is the IP right?");
        });
        return;
    }

    uint64_t sendsCompleted = 0;
    bool queueOverflowed = false;
    auto drain = [&] {
        long n = gm_rio_drain_sends(gm);
        if (n < 0) queueOverflowed = true; else sendsCompleted += uint64_t(n);
    };
    drain();

    // Send the video mode once per stream: re-sending it makes the CRT re-lock and jump.
    gm_switchres(gm, m.pClockMHz, uint16_t(m.hActive), uint16_t(m.hBegin), uint16_t(m.hEnd), uint16_t(m.hTotal),
                 uint16_t(m.vActive), uint16_t(m.vBegin), uint16_t(m.vEnd), uint16_t(m.vTotal),
                 m.interlaced ? 1 : 0);
    drain();

    const int bytesPerPixel = config.rgb565 ? 2 : 3;
    const int blitBytes = m.hActive * m.linesPerBlit() * bytesPerPixel;
    for (uint8_t field = 0; field <= 1; field++)
        if (uint8_t *b = gm_blit_buffer(gm, field)) memset(b, 0, size_t(blitBytes));

    // Exact audio frames per field from the modeline (e.g. 800.8 for 59.94 Hz).
    const double framesPerField = double(rate) / m.refreshHz();
    // Audio handed out now is heard after the FIFO cushion; the picture blitted now is on screen
    // during the next field. Aim at the middle of that field.
    const double videoDelay = double(AudioFormat::cushionFrames) / rate - 1.5 / m.refreshHz()
                              - config.audio_delay_ms / 1000;
    source->prepare(m.hActive, m.renderHeight, videoDelay);

    std::vector<int16_t> scratchAudio(65536);

    std::string modeName = m.name;
    update([&](cc_stats &s) { copyText(s.state, "Streaming " + modeName); s.error[0] = 0; });

    const int progressiveStep = m.renderHeight / m.vActive > 1 ? m.renderHeight / m.vActive : 1;
    uint32_t frame = 0;
    const Picture *lastPicture = nullptr;
    Picture lastCopy;
    uint64_t blits = 0, pictures = 0;
    double windowStart = now();
    int windowBlits = 0;
    double packTotal = 0, sendTotal = 0, packMax = 0, sendMax = 0;

    // Audio schedule, counted in MiSTer fields since the first audio send.
    bool haveBase = false;
    uint32_t audioBaseField = 0;
    int64_t framesSent = 0;       // audio frames handed out (excluding the cushion)
    bool cushionSent = false;
    int catchUps = 0;
    uint64_t lateFields = 0;
    int fadeFieldsLeft = -1;      // >= 0 while fading out before disconnect

    // Raster line in each blit header that the MiSTer syncs to (1 = start of the field, like the Apple apps;
    // 0 = groovymister picks it from the measured send time). Overridable for testing: CRTCAST_VSYNC_LINE.
    uint16_t vSyncLine = 1;
    if (const char *v = getenv("CRTCAST_VSYNC_LINE")) vSyncLine = uint16_t(atoi(v));

    // Diagnostics: CRTCAST_FIELD_LOG=<file.csv> records every field (timing, MiSTer counters, parity).
    FILE *fieldLog = nullptr;
    const double logStart = now();
    if (const char *path = getenv("CRTCAST_FIELD_LOG")) {
        fieldLog = fopen(path, "w");
        if (fieldLog) fprintf(fieldLog, "t_ms,frame,st_frame,st_echo,st_vcount,st_f1,field,send_ms,after_frame,after_vcount,after_f1\n");
    }

    while (true) {
        if (fadeFieldsLeft < 0 && !keepGoing) {
            // Fade the sound out and let it reach the speaker before closing.
            source->beginFadeOut();
            fadeFieldsLeft = int(ceil(double(AudioFormat::cushionFrames) / framesPerField)) + 3;
            update([](cc_stats &s) { copyText(s.state, "Stopping\xE2\x80\xA6"); });
        }
        if (fadeFieldsLeft == 0) break;
        if (fadeFieldsLeft > 0) fadeFieldsLeft--;

        frame++;
        GMStatus status = gm_status(gm);
        const double tField = now();
        if (status.frame > frame) {
            if (blits > 0) lateFields += uint64_t(status.frame - frame + 1);
            frame = status.frame + 1;
        }

        uint8_t field = 0;
        if (m.interlaced) {
            uint8_t notF1 = status.vgaF1 == 0 ? 1 : 0;
            field = notF1 ^ uint8_t((frame - status.frame) % 2);
        }

        // Audio schedule pinned to the MiSTer's own field counter: after this send we have handed out
        // audio up to "fields displayed + 1". If we were late and it repeated a field, the next send
        // simply covers two fields.
        const bool audioOn = config.send_audio && status.audio != 0;
        if (!haveBase || status.frame < audioBaseField) {
            haveBase = true;
            audioBaseField = status.frame;
            framesSent = 0;
            cushionSent = false;
        }
        if (audioOn && !cushionSent) {
            if (int16_t *abuf = gm_audio_buffer(gm)) {
                memset(abuf, 0, size_t(AudioFormat::cushionFrames) * channels * 2);
                gm_audio(gm, uint16_t(AudioFormat::cushionFrames * channels * 2));
                drain();
                cushionSent = true;
            }
        }
        int64_t displayed = int64_t(status.frame - audioBaseField);
        int64_t targetFrames = int64_t(llround(double(displayed + 1) * framesPerField));
        // One audio command carries at most 16383 frames, so cap at 16 fields per send.
        const int64_t maxFrames = int64_t(framesPerField * 16);
        if (targetFrames - framesSent > int64_t(rate) * 2) {
            // Counter jumped by > 2 s (MiSTer restarted/reset): re-anchor instead of flooding.
            audioBaseField = status.frame;
            framesSent = 0;
            displayed = 0;
            targetFrames = int64_t(llround(framesPerField));
            if (blits > 0) catchUps++;
        }
        int64_t owed = targetFrames - framesSent;
        int audioFrames = int(owed < 0 ? 0 : (owed > maxFrames ? maxFrames : owed));
        if (double(audioFrames) > framesPerField * 1.5) catchUps++;
        framesSent += audioFrames;

        double t0 = now();
        int16_t *audioDst = scratchAudio.data();
        if (audioOn) if (int16_t *b = gm_audio_buffer(gm)) audioDst = b;
        if (const Picture *picture = source->nextField(audioDst, audioFrames)) {
            lastCopy = *picture;
            lastPicture = &lastCopy;
            pictures++;
            if (pictures % 4 == 1) storePreview(*picture);
        }
        if (lastPicture) {
            if (uint8_t *dst = gm_blit_buffer(gm, field)) {
                if (m.interlaced) {
                    int rowOffset = int(field ^ (config.swap_fields ? 1 : 0));
                    Packer::pack(*lastPicture, dst, m.hActive, rowOffset, 2, m.linesPerBlit(), config.rgb565 != 0);
                } else {
                    Packer::pack(*lastPicture, dst, m.hActive, 0, progressiveStep, m.linesPerBlit(), config.rgb565 != 0);
                }
            }
        }
        double t1 = now();
        if (audioOn && audioFrames > 0) {
            gm_audio(gm, uint16_t(audioFrames * channels * 2));
            drain();
        }
        gm_blit(gm, frame, field, vSyncLine, 0);
        drain();
        double t2 = now();
        gm_wait_sync(gm);
        if (fieldLog) {
            GMStatus after = gm_status(gm);
            fprintf(fieldLog, "%.3f,%u,%u,%u,%u,%u,%u,%.3f,%u,%u,%u\n", (tField - logStart) * 1000, frame, status.frame,
                    status.frameEcho, status.vCount, status.vgaF1, field, (t2 - t1) * 1000, after.frame, after.vCount,
                    after.vgaF1);
        }

        blits++;
        windowBlits++;
        packTotal += t1 - t0;
        sendTotal += t2 - t1;
        if (t1 - t0 > packMax) packMax = t1 - t0;
        if (t2 - t1 > sendMax) sendMax = t2 - t1;

        double t = now();
        if (t - windowStart >= 0.5) {
            double n = double(windowBlits > 0 ? windowBlits : 1);
            double blitRate = double(windowBlits) / (t - windowStart);
            std::string sourceStatus = source->statusText();
            update([&](cc_stats &s) {
                s.blits = blits;
                s.blits_per_second = blitRate;
                s.new_pictures = pictures;
                s.pack_ms = packTotal / n * 1000;
                s.send_ms = sendTotal / n * 1000;
                s.mister_synced = status.vramSynced != 0;
                s.mister_audio = status.audio != 0;
                s.mister_frame = status.frame;
                s.audio_seconds = double(framesSent) / rate;
                s.audio_catch_ups = catchUps;
                s.late_fields = lateFields;
                copyText(s.source_status, sourceStatus);
                s.net_sends_completed = sendsCompleted;
                s.net_queue_overflowed = queueOverflowed;
                s.pack_max_ms = packMax * 1000;
                s.send_max_ms = sendMax * 1000;
            });
            windowStart = t;
            windowBlits = 0;
            packTotal = 0;
            sendTotal = 0;
            packMax = 0;
            sendMax = 0;
        }
    }
    if (fieldLog) fclose(fieldLog);
    source->stop();
    // groovymister's CmdClose closes the RIO send queue right away; let in-flight packets finish first.
    for (int quiet = 0, tries = 0; quiet < 2 && tries < 20; tries++) {
        Sleep(2);
        long n = gm_rio_drain_sends(gm);
        quiet = n == 0 ? quiet + 1 : 0;
    }
    gm_close(gm);
    gm_destroy(gm);
    update([](cc_stats &s) { copyText(s.state, "Stopped"); });
}

}  // namespace

extern "C" {

CC_API const char *cc_version(void) { return "CRTCast Windows core 0.4-dev"; }

CC_API int cc_get_mode_info(int32_t mode, cc_mode_info *out) {
    if (mode < 0 || mode >= CC_MODE_COUNT || !out) return -1;
    const Modeline &m = kModelines[mode];
    memset(out, 0, sizeof(*out));
    snprintf(out->name, sizeof(out->name), "%s", m.name);
    out->width = m.hActive;
    out->lines_per_blit = m.linesPerBlit();
    out->interlaced = m.interlaced;
    out->refresh_hz = m.refreshHz();
    out->mbps_rgb888 = m.megabitsPerSecond(3);
    out->mbps_rgb565 = m.megabitsPerSecond(2);
    return 0;
}

CC_API int cc_start(const cc_config *config, int32_t source) {
    if (!config || config->mode < 0 || config->mode >= CC_MODE_COUNT || config->host[0] == 0) return -2;
    Engine &e = g_engine;
    if (e.active) return -1;
    if (e.thread.joinable()) e.thread.join();

    std::unique_ptr<MediaSource> src;
    switch (source) {
    case CC_SOURCE_TEST_PATTERN: src = std::make_unique<TestPatternSource>("Windows"); break;
    case CC_SOURCE_AIRPLAY: src = std::make_unique<AirPlayMirrorSource>(config->low_latency != 0); break;
    default: return -2;
    }

    cc_config copy = *config;
    copy.host[sizeof(copy.host) - 1] = 0;
    e.update([&](cc_stats &s) {
        s = cc_stats{};
        s.running = 1;
        snprintf(s.state, sizeof(s.state), "Connecting to %s\xE2\x80\xA6", copy.host);
    });
    e.keepGoing = true;
    e.active = true;
    e.thread = std::thread([copy, s = std::move(src)]() mutable {
        g_engine.run(copy, std::move(s));
        g_engine.keepGoing = false;
        g_engine.update([](cc_stats &st) { st.running = 0; });
        g_engine.active = false;
    });
    return 0;
}

CC_API void cc_stop(void) { g_engine.keepGoing = false; }

CC_API int cc_wait_stopped(int32_t timeout_ms) {
    double end = now() + timeout_ms / 1000.0;
    while (g_engine.active) {
        if (now() > end) return 0;
        Sleep(5);
    }
    if (g_engine.thread.joinable()) g_engine.thread.join();
    return 1;
}

CC_API void cc_get_stats(cc_stats *out) {
    if (!out) return;
    std::lock_guard<std::mutex> g(g_engine.statsLock);
    *out = g_engine.stats;
}

CC_API int cc_get_preview(uint8_t *bgra, int32_t capacity, int32_t *width, int32_t *height, uint64_t *seq) {
    Engine &e = g_engine;
    std::lock_guard<std::mutex> g(e.previewLock);
    if (!bgra || !seq || e.preview.empty() || e.previewSeq == *seq) return 0;
    if (int64_t(e.preview.size()) > capacity) return 0;
    memcpy(bgra, e.preview.data(), e.preview.size());
    if (width) *width = e.previewWidth;
    if (height) *height = e.previewHeight;
    *seq = e.previewSeq;
    return 1;
}

CC_API int cc_probe(const char *host, int32_t *audio_on, char *message, int32_t message_size) {
    auto say = [&](const char *fmt, auto... args) {
        if (message && message_size > 0) snprintf(message, size_t(message_size), fmt, args...);
    };
    if (g_engine.active) { say("A stream is running."); return -1; }
    if (!host || !*host) { say("No IP address."); return -2; }
    GMHandle *gm = gm_create();
    double t0 = now();
    int rc = gm_init(gm, host, kPort, 0, 48000, 2, 0, 1500);
    double ms = (now() - t0) * 1000;
    if (rc != 0) {
        gm_close(gm);
        gm_destroy(gm);
        say("No answer from %s on UDP %d.", host, int(kPort));
        return -3;
    }
    GMStatus s = gm_status(gm);
    if (audio_on) *audio_on = s.audio;
    gm_close(gm);
    gm_destroy(gm);
    // The core reports its audio flag reliably only once a video mode is set, so don't judge it here.
    say("MiSTer answered (init %.1f ms).", ms);
    return 0;
}

}  // extern "C"

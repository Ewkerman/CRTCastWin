// Command-line test for the CRTCast Windows core (step 1: test pattern + beep to the MiSTer).
//
//   crtcast_test 192.168.0.13                 640x480i, RGB888, raw, with audio, until Enter
//   crtcast_test 192.168.0.13 --mode 0 --565 --lz4 --seconds 30
//   crtcast_test 192.168.0.13 --probe         only check the MiSTer answers (no mode change)
#include "crtcast_core.h"

#include <windows.h>

#include <conio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage() {
    printf("usage: crtcast_test <mister-ip> [--probe] [--mode 0|1|2] [--565] [--lz4] [--jumbo]\n"
           "                    [--swap] [--noaudio] [--delay ms] [--seconds n]\n"
           "  modes: 0 = 320x240p, 1 = 640x240p, 2 = 640x480i (default)\n");
}

int main(int argc, char **argv) {
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc >= 2 && !strcmp(argv[1], "--airplay")) {
        // AirPlay receiver only: advertise, print status once a second, show the log at the end.
        double secs = argc >= 3 ? atof(argv[2]) : 60;
        printf("%s\nStarting AirPlay receiver \"CRTCast Windows\" for %.0f s...\n", cc_version(), secs);
        int rc = cc_airplay_start("CRTCast Windows");
        cc_airplay_status st;
        cc_airplay_get_status(&st);
        printf("start rc=%d running=%d port=%u %s\n", rc, st.running, st.port, st.last_error);
        for (int t = 0; t < int(secs) && rc == 0; t++) {
            Sleep(1000);
            cc_airplay_get_status(&st);
            printf("\r%3ds  connections %d  mirroring %d  %dx%d  video %llu  audio %llu  %-60.60s", t + 1,
                   st.connections, st.mirroring, st.width, st.height, (unsigned long long) st.video_frames,
                   (unsigned long long) st.audio_packets, st.last_event);
            if (_kbhit() && _getch() == '\r') break;
        }
        static char log[16384];
        cc_airplay_get_log(log, sizeof(log));
        printf("\n--- log ---\n%s", log);
        cc_airplay_stop();
        return rc == 0 ? 0 : 1;
    }
    if (argc < 2) { usage(); return 2; }

    cc_config c = {};
    snprintf(c.host, sizeof(c.host), "%s", argv[1]);
    c.mode = CC_MODE_640x480i;
    c.send_audio = 1;
    bool probe = false;
    double seconds = 0;
    int cycles = 0;   // > 0: start/stop this many times in one process (like Stop/Resume in the app)
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--probe")) probe = true;
        else if (!strcmp(a, "--mode") && i + 1 < argc) c.mode = atoi(argv[++i]);
        else if (!strcmp(a, "--565")) c.rgb565 = 1;
        else if (!strcmp(a, "--lz4")) c.lz4 = 1;
        else if (!strcmp(a, "--jumbo")) c.jumbo = 1;
        else if (!strcmp(a, "--swap")) c.swap_fields = 1;
        else if (!strcmp(a, "--noaudio")) c.send_audio = 0;
        else if (!strcmp(a, "--delay") && i + 1 < argc) c.audio_delay_ms = atof(argv[++i]);
        else if (!strcmp(a, "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(a, "--cycles") && i + 1 < argc) cycles = atoi(argv[++i]);
        else { usage(); return 2; }
    }
    printf("%s\n", cc_version());

    if (probe) {
        char msg[256] = {};
        int audio = 0;
        int rc = cc_probe(c.host, &audio, msg, sizeof(msg));
        printf("%s\n", msg);
        return rc == 0 ? 0 : 1;
    }

    cc_mode_info mi;
    cc_get_mode_info(c.mode, &mi);
    printf("Mode %s  %.3f Hz  %.0f Mbps raw (%s)%s%s\n", mi.name, mi.refresh_hz,
           c.rgb565 ? mi.mbps_rgb565 : mi.mbps_rgb888, c.rgb565 ? "RGB565" : "RGB888",
           c.lz4 ? " + LZ4" : "", c.send_audio ? ", 48 kHz audio" : ", no audio");

    for (int n = 1; n <= cycles; n++) {
        printf("cycle %d: start\n", n);
        if (cc_start(&c, CC_SOURCE_TEST_PATTERN) != 0) { printf("Could not start.\n"); return 1; }
        Sleep(DWORD(seconds > 0 ? seconds * 1000 : 1500));
        cc_stats s;
        cc_get_stats(&s);
        printf("cycle %d: %s, %.1f fields/s, late %llu, sends done %llu, overflow %d%s%s\n", n, s.state,
               s.blits_per_second, (unsigned long long) s.late_fields, (unsigned long long) s.net_sends_completed,
               s.net_queue_overflowed, s.error[0] ? ", error: " : "", s.error);
        cc_stop();
        printf("cycle %d: stopped=%d\n", n, cc_wait_stopped(3000));
    }
    if (cycles > 0) { printf("all %d cycles done\n", cycles); return 0; }

    if (cc_start(&c, CC_SOURCE_TEST_PATTERN) != 0) { printf("Could not start.\n"); return 1; }
    printf("Streaming the test pattern. Press Enter to stop.\n");

    ULONGLONG start = GetTickCount64();
    cc_stats s;
    while (true) {
        Sleep(500);
        cc_get_stats(&s);
        if (!s.running) break;
        printf("\r%-24s %5.1f fields/s  late %llu  catch-ups %d  pack %.2f ms  send %.2f ms  sync %s  sound %s   ",
               s.state, s.blits_per_second, (unsigned long long) s.late_fields, s.audio_catch_ups,
               s.pack_ms, s.send_ms, s.mister_synced ? "yes" : "no", s.mister_audio ? "on" : "OFF");
        fflush(stdout);
        bool enter = _kbhit() && _getch() == '\r';
        bool timeUp = seconds > 0 && (GetTickCount64() - start) / 1000.0 >= seconds;
        if (enter || timeUp) {
            cc_stop();
            cc_wait_stopped(3000);
            break;
        }
    }
    cc_wait_stopped(3000);   // joins the sender thread
    cc_get_stats(&s);
    printf("\n%s", s.state);
    if (s.error[0]) printf(": %s", s.error);
    printf("\nfields sent %llu, late fields %llu, audio %.1f s\n", (unsigned long long) s.blits,
           (unsigned long long) s.late_fields, s.audio_seconds);
    return s.error[0] ? 1 : 0;
}

// CRTCast AirPlay receiver glue around the UxPlay core (step 1: advertise, accept,
// handshake, and count incoming mirror video/audio; no playback yet).
#include "AirPlayBridge.h"

#include "dnssd.h"
#include "logger.h"
#include "raop.h"
#include "stream.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static crt_airplay_status status;
static raop_t *raop;
static dnssd_t *dnssd;
static int ntp_initialized;
static crt_airplay_video_callback video_callback;
static crt_airplay_audio_callback audio_callback;

#define LOG_LINES 40
#define LOG_LINE_LEN 200
static char log_ring[LOG_LINES][LOG_LINE_LEN];
static int log_next;

static void set_text(char *dst, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(dst, size, fmt, ap);
    va_end(ap);
}

static void add_log(const char *msg) {
    pthread_mutex_lock(&lock);
    char *line = log_ring[log_next % LOG_LINES];
    snprintf(line, LOG_LINE_LEN, "%s", msg);
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    log_next++;
    pthread_mutex_unlock(&lock);
}

static void event(const char *fmt, ...) {
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&lock);
    snprintf(status.last_event, sizeof(status.last_event), "%s", buf);
    pthread_mutex_unlock(&lock);
    add_log(buf);
    printf("[CRTCast AirPlay] %s\n", buf);
}

// ---- UxPlay callbacks ----

static void log_callback(void *cls, int level, const char *msg) {
    add_log(msg);
    printf("[UxPlay] %s\n", msg);
}

static void conn_init(void *cls) {
    pthread_mutex_lock(&lock);
    status.connections++;
    status.total_connections++;
    pthread_mutex_unlock(&lock);
    event("client connected");
}

static void conn_destroy(void *cls) {
    pthread_mutex_lock(&lock);
    if (status.connections > 0) status.connections--;
    if (status.connections == 0) status.mirroring = 0;
    pthread_mutex_unlock(&lock);
    event("client disconnected");
}

static void conn_reset(void *cls, int reason) { event("connection reset (%d)", reason); }
static void conn_feedback(void *cls) {}

static void report_client_request(void *cls, char *deviceid, char *model, char *name, bool *admit) {
    pthread_mutex_lock(&lock);
    snprintf(status.client_name, sizeof(status.client_name), "%s", name ? name : "?");
    snprintf(status.client_model, sizeof(status.client_model), "%s", model ? model : "?");
    pthread_mutex_unlock(&lock);
    *admit = true;
    event("request from %s (%s)", name ? name : "?", model ? model : "?");
}

static void video_process(void *cls, raop_ntp_t *ntp, video_decode_struct *data) {
    pthread_mutex_lock(&lock);
    status.mirroring = 1;
    status.video_frames++;
    status.video_bytes += (uint64_t) (data->data_len > 0 ? data->data_len : 0);
    status.codec = data->is_h265 ? 2 : 1;
    crt_airplay_video_callback cb = video_callback;
    pthread_mutex_unlock(&lock);
    if (cb && data->data && data->data_len > 0) cb(data->data, data->data_len, data->is_h265 ? 1 : 0, data->ntp_time_remote);
}

void crt_airplay_set_video_callback(crt_airplay_video_callback callback) {
    pthread_mutex_lock(&lock);
    video_callback = callback;
    pthread_mutex_unlock(&lock);
}

void crt_airplay_debug_set_mirroring(int mirroring) {
    pthread_mutex_lock(&lock);
    status.mirroring = mirroring;
    status.connections = mirroring ? 1 : 0;
    if (mirroring) snprintf(status.client_name, sizeof(status.client_name), "%s", "Test feed");
    pthread_mutex_unlock(&lock);
}

static void audio_process(void *cls, raop_ntp_t *ntp, audio_decode_struct *data) {
    pthread_mutex_lock(&lock);
    status.audio_packets++;
    status.audio_codec = data->ct;
    crt_airplay_audio_callback cb = audio_callback;
    pthread_mutex_unlock(&lock);
    if (cb && data->data && data->data_len > 0) cb(data->data, data->data_len, data->ct, data->ntp_time_remote);
}

void crt_airplay_set_audio_callback(crt_airplay_audio_callback callback) {
    pthread_mutex_lock(&lock);
    audio_callback = callback;
    pthread_mutex_unlock(&lock);
}

static int video_set_codec_and_size(void *cls, video_codec_t codec, unsigned short width_source,
                                    unsigned short height_source, unsigned short x_offset,
                                    unsigned short y_offset, unsigned short width, unsigned short height) {
    pthread_mutex_lock(&lock);
    status.codec = codec == VIDEO_CODEC_H265 ? 2 : 1;
    status.width = width_source;
    status.height = height_source;
    pthread_mutex_unlock(&lock);
    event("mirror video %s %ux%u", codec == VIDEO_CODEC_H265 ? "H.265" : "H.264", width_source, height_source);
    return 0;
}

static void video_pause(void *cls) { event("video paused"); }
static void video_resume(void *cls) { event("video resumed"); }
static void video_reset(void *cls, reset_type_t type) { event("video reset (%d)", (int) type); }
static void audio_flush(void *cls) {}
static void video_flush(void *cls) {}
static double audio_set_client_volume(void *cls) { return 0.0; }
static void audio_set_volume(void *cls, float volume) {}
static void audio_set_metadata(void *cls, const void *buffer, int buflen) {}
static void audio_set_coverart(void *cls, const void *buffer, int buflen) {}
static void audio_stop_coverart_rendering(void *cls) {}
static void audio_set_progress(void *cls, uint32_t *start, uint32_t *curr, uint32_t *end) {}
static void audio_get_format(void *cls, unsigned char *ct, unsigned short *spf, bool *usingScreen,
                             bool *isMedia, uint64_t *audioFormat) {
    pthread_mutex_lock(&lock);
    status.audio_codec = *ct;
    pthread_mutex_unlock(&lock);
    event("audio format ct=%d (%s)", *ct, *ct == 8 ? "AAC-ELD" : *ct == 4 ? "AAC" : *ct == 2 ? "ALAC" : "other");
}
static void mirror_video_running(void *cls, bool is_running) {
    pthread_mutex_lock(&lock);
    status.mirroring = is_running ? 1 : 0;
    pthread_mutex_unlock(&lock);
    event(is_running ? "screen mirroring started" : "screen mirroring stopped");
}
static void display_pin(void *cls, char *pin) { event("PIN requested (not supported)"); }
static void register_client(void *cls, const char *device_id, const char *pk_str, const char *name) {}
static bool check_register(void *cls, const char *pk_str) { return true; }
static void export_dacp(void *cls, const char *active_remote, const char *dacp_id) {}
static void on_video_play(void *cls, const char *location, const float start_position) {}
static void on_video_scrub(void *cls, const float position) {}
static void on_video_rate(void *cls, const float rate) {}
static void on_video_stop(void *cls) {}
static void on_video_acquire_playback_info(void *cls, playback_info_t *info) {}
static float on_video_playlist_remove(void *cls) { return 0.0f; }
static void get_custom_profile(void *cls, const char **profile) { *profile = NULL; }

// ---- Start / stop ----

int crt_airplay_start(const char *name, const unsigned char hw_addr[6]) {
    if (raop) return 0;
    pthread_mutex_lock(&lock);
    memset(&status, 0, sizeof(status));
    pthread_mutex_unlock(&lock);

    if (!ntp_initialized) { ntp_global_init(); ntp_initialized = 1; }

    int err = 0;
    dnssd = dnssd_init(name, (int) strlen(name), (const char *) hw_addr, 6, 0, &err);
    if (err || !dnssd) {
        set_text(status.last_error, sizeof(status.last_error), "Bonjour setup failed (%d)", err);
        return -1;
    }
    // Same advertised feature set as UxPlay's defaults: mirroring + audio, FairPlay auth,
    // no HLS, H.264 only, AirPlay 2 pairing (not legacy).
    static const int on_bits[] = {1, 2, 5, 6, 7, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 25, 28, 30};
    for (int bit = 0; bit < 64; bit++) dnssd_set_airplay_features(dnssd, bit, 0);
    for (size_t i = 0; i < sizeof(on_bits) / sizeof(on_bits[0]); i++) dnssd_set_airplay_features(dnssd, on_bits[i], 1);

    raop_callbacks_t cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.conn_init = conn_init;
    cbs.conn_destroy = conn_destroy;
    cbs.conn_reset = conn_reset;
    cbs.conn_feedback = conn_feedback;
    cbs.audio_process = audio_process;
    cbs.video_process = video_process;
    cbs.audio_flush = audio_flush;
    cbs.video_flush = video_flush;
    cbs.video_pause = video_pause;
    cbs.video_resume = video_resume;
    cbs.video_reset = video_reset;
    cbs.audio_set_client_volume = audio_set_client_volume;
    cbs.audio_set_volume = audio_set_volume;
    cbs.audio_get_format = audio_get_format;
    cbs.audio_set_metadata = audio_set_metadata;
    cbs.audio_set_coverart = audio_set_coverart;
    cbs.audio_stop_coverart_rendering = audio_stop_coverart_rendering;
    cbs.audio_set_progress = audio_set_progress;
    cbs.report_client_request = report_client_request;
    cbs.display_pin = display_pin;
    cbs.register_client = register_client;
    cbs.check_register = check_register;
    cbs.export_dacp = export_dacp;
    cbs.video_set_codec_and_size = video_set_codec_and_size;
    cbs.mirror_video_running = mirror_video_running;
    cbs.get_custom_profile = get_custom_profile;
    cbs.on_video_play = on_video_play;
    cbs.on_video_scrub = on_video_scrub;
    cbs.on_video_rate = on_video_rate;
    cbs.on_video_stop = on_video_stop;
    cbs.on_video_acquire_playback_info = on_video_acquire_playback_info;
    cbs.on_video_playlist_remove = on_video_playlist_remove;

    raop = raop_init(&cbs);
    if (!raop) {
        set_text(status.last_error, sizeof(status.last_error), "AirPlay receiver init failed");
        dnssd_destroy(dnssd); dnssd = NULL;
        return -2;
    }
    raop_set_log_callback(raop, log_callback, NULL);
    raop_set_log_level(raop, LOGGER_INFO);

    char device_id[18];
    snprintf(device_id, sizeof(device_id), "%02X:%02X:%02X:%02X:%02X:%02X",
             hw_addr[0], hw_addr[1], hw_addr[2], hw_addr[3], hw_addr[4], hw_addr[5]);
    if (raop_init2(raop, 0, device_id, "")) {
        set_text(status.last_error, sizeof(status.last_error), "AirPlay receiver init (2) failed");
        raop_destroy(raop); raop = NULL;
        dnssd_destroy(dnssd); dnssd = NULL;
        return -3;
    }
    // Ask the phone for a 1080p, 30 fps mirror (UxPlay defaults). Scaled to the CRT later.
    raop_set_plist(raop, "width", 1920);
    raop_set_plist(raop, "height", 1080);
    raop_set_plist(raop, "refreshRate", 60);
    raop_set_plist(raop, "maxFPS", 30);

    // Port 0 everywhere = pick free ports, so we never clash with tvOS's own AirPlay receiver.
    unsigned short tcp[2] = {0, 0};
    unsigned short udp[3] = {0, 0, 0};
    raop_set_tcp_ports(raop, tcp);
    raop_set_udp_ports(raop, udp);
    unsigned short port = raop_get_port(raop);
    if (raop_start_httpd(raop, &port) < 0) {
        set_text(status.last_error, sizeof(status.last_error), "Couldn't open a network port");
        raop_destroy(raop); raop = NULL;
        dnssd_destroy(dnssd); dnssd = NULL;
        return -4;
    }
    raop_set_port(raop, port);
    raop_set_dnssd(raop, dnssd);

    int e1 = dnssd_register_raop(dnssd, port);
    int e2 = dnssd_register_airplay(dnssd, port);
    if (e1 || e2) {
        set_text(status.last_error, sizeof(status.last_error), "Bonjour advertising failed (%d/%d)", e1, e2);
        crt_airplay_stop();
        return -5;
    }
    pthread_mutex_lock(&lock);
    status.running = 1;
    status.port = port;
    pthread_mutex_unlock(&lock);
    event("advertising \"%s\" on port %u", name, port);
    return 0;
}

void crt_airplay_stop(void) {
    if (dnssd) {
        dnssd_unregister_raop(dnssd);
        dnssd_unregister_airplay(dnssd);
    }
    if (raop) { raop_destroy(raop); raop = NULL; }
    if (dnssd) { dnssd_destroy(dnssd); dnssd = NULL; }
    pthread_mutex_lock(&lock);
    status.running = 0;
    status.connections = 0;
    status.mirroring = 0;
    pthread_mutex_unlock(&lock);
}

void crt_airplay_get_status(crt_airplay_status *out) {
    pthread_mutex_lock(&lock);
    *out = status;
    pthread_mutex_unlock(&lock);
}

void crt_airplay_get_log(char *buffer, int size) {
    if (size <= 0) return;
    buffer[0] = 0;
    pthread_mutex_lock(&lock);
    int count = log_next < LOG_LINES ? log_next : LOG_LINES;
    for (int i = log_next - count; i < log_next; i++) {
        strncat(buffer, log_ring[i % LOG_LINES], (size_t) size - strlen(buffer) - 1);
        strncat(buffer, "\n", (size_t) size - strlen(buffer) - 1);
    }
    pthread_mutex_unlock(&lock);
}

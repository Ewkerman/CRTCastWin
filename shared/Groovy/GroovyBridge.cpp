#include "GroovyBridge.h"
#include "groovymister.h"

struct GMHandle {
    GroovyMister gm;
};

extern "C" {

GMHandle *gm_create(void) { return new GMHandle(); }

void gm_destroy(GMHandle *h) { delete h; }

int gm_init(GMHandle *h, const char *host, uint16_t port, int lz4Mode,
            uint32_t soundRate, uint8_t soundChannels, uint8_t rgbMode, uint16_t mtu) {
    return h->gm.CmdInit(host, port, lz4Mode, soundRate, soundChannels, rgbMode, mtu);
}

int16_t *gm_audio_buffer(GMHandle *h) {
    return reinterpret_cast<int16_t *>(h->gm.getPBufferAudio());
}

void gm_audio(GMHandle *h, uint16_t bytes) { h->gm.CmdAudio(bytes); }

void gm_switchres(GMHandle *h, double pclockMHz,
                  uint16_t hActive, uint16_t hBegin, uint16_t hEnd, uint16_t hTotal,
                  uint16_t vActive, uint16_t vBegin, uint16_t vEnd, uint16_t vTotal,
                  uint8_t interlace) {
    h->gm.CmdSwitchres(pclockMHz, hActive, hBegin, hEnd, hTotal, vActive, vBegin, vEnd, vTotal, interlace);
}

uint8_t *gm_blit_buffer(GMHandle *h, uint8_t field) {
    return reinterpret_cast<uint8_t *>(h->gm.getPBufferBlit(field));
}

void gm_blit(GMHandle *h, uint32_t frame, uint8_t field, uint16_t vSyncLine, uint32_t margin) {
    h->gm.CmdBlit(frame, field, vSyncLine, margin, 0);
}

void gm_wait_sync(GMHandle *h) { h->gm.WaitSync(); }

GMStatus gm_status(GMHandle *h) {
    const fpgaStatus &s = h->gm.fpga;
    GMStatus out;
    out.frame = s.frame;
    out.frameEcho = s.frameEcho;
    out.vCount = s.vCount;
    out.vCountEcho = s.vCountEcho;
    out.vramEndFrame = s.vramEndFrame;
    out.vramReady = s.vramReady;
    out.vramSynced = s.vramSynced;
    out.vgaFrameskip = s.vgaFrameskip;
    out.vgaVblank = s.vgaVblank;
    out.vgaF1 = s.vgaF1;
    out.audio = s.audio;
    out.vramQueue = s.vramQueue;
    return out;
}

void gm_close(GMHandle *h) { h->gm.CmdClose(); }

void gm_set_verbose(GMHandle *h, uint8_t level) { h->gm.setVerbose(level); }

}

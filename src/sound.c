/* Native replacement for the game's sound drivers.
 *
 * DIGI.DRV interface (entry DRV:0100, function in AL) - from the original driver code:
 *   0 detect -> AX>=0 ok, BX=1, CX=0F50h, DX=FFFFh      1 install -> AX=0 ok
 *   2 uninstall                                          3 stop playback (AX=1 if idle)
 *   4 speaker on (AH != 0) / off (AH = 0)                5,6 no-op
 *   7 play: ES:BX = 8-bit unsigned PCM, CX = length, DX = rate (Hz), DI:SI = far callback
 *     called like an interrupt when the sample ends; AX=1 if busy
 *   8 -> AX=8
 * MIDI.DRV interface: AL < 8 control functions; otherwise AL = status (high nibble),
 *   AH = channel, DL = data 1, DH = data 2.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu.h"

void logmsg(const char *fmt, ...);

/* ---------------------------------------------------------------- digital */
static HWAVEOUT wo; static int wo_rate;
static WAVEHDR hdr; static uint8_t *buf; static int playing;
static uint16_t cb_seg, cb_off;
static int volume = 15;                 /* 15 = speaker on, 0 = off (fn 4) */

/* scripted test runs (POP2_KEYS) make no sound: they run hidden in the background */
static int silent(void) { static int v = -1; if (v < 0) v = getenv("POP2_KEYS") != NULL; return v; }

static void wo_open(int rate) {
    if (silent()) return;
    if (wo && wo_rate == rate) return;
    if (wo) { waveOutReset(wo); waveOutClose(wo); wo = NULL; }
    WAVEFORMATEX f = {0};
    f.wFormatTag = WAVE_FORMAT_PCM; f.nChannels = 1; f.nSamplesPerSec = rate; f.wBitsPerSample = 8;
    f.nBlockAlign = 1; f.nAvgBytesPerSec = rate;
    if (waveOutOpen(&wo, WAVE_MAPPER, &f, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) { wo = NULL; return; }
    wo_rate = rate;
}
static void digi_stop(void) {
    if (wo && playing) { waveOutReset(wo); }
    if (wo && (hdr.dwFlags & WHDR_PREPARED)) waveOutUnprepareHeader(wo, &hdr, sizeof hdr);
    playing = 0;
}
static int digi_play(uint16_t seg, uint16_t off, uint16_t len, uint16_t rate) {
    if (playing) return 1;
    if (rate < 3000 || rate > 48000) rate = 11025;
    wo_open(rate);
    if (!wo || !len) return 0;
    if (hdr.dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(wo, &hdr, sizeof hdr);
    buf = realloc(buf, len);
    uint32_t a = LIN(seg, off);
    for (int i = 0; i < len; i++) {
        int s = (int)MEM[(a + i) & 0xFFFFF] - 128;
        buf[i] = (uint8_t)(128 + s * volume / 15);
    }
    memset(&hdr, 0, sizeof hdr);
    hdr.lpData = (LPSTR)buf; hdr.dwBufferLength = len;
    waveOutPrepareHeader(wo, &hdr, sizeof hdr);
    waveOutWrite(wo, &hdr, sizeof hdr);
    playing = 1;
    return 0;
}

int digi_busy(void) { return playing; }
void digi_abort(void) { digi_stop(); }          /* stop without running the game's callback */

/* called from the runtime's main-thread poll: finish samples and run the game's callback */
void snd_poll(void) {
    if (playing && (hdr.dwFlags & WHDR_DONE)) {
        playing = 0;
        waveOutUnprepareHeader(wo, &hdr, sizeof hdr);
        if (getenv("POP2_SNDLOG")) logmsg("DIGI done t=%lu cb=%04x:%04x\n", GetTickCount(), cb_seg, cb_off);
        if (cb_seg || cb_off) {
            PUSH(get_flags()); PUSH(0xFFFF); PUSH(0xFFFF);
            /* the driver invokes the callback with PUSHF; CALL FAR, so it returns with IRET */
            extern void call_far(uint16_t, uint16_t);
            uint16_t sax = AX, sbx = BX, scx = CX, sdx = DX, ssi = SI, sdi = DI, sds = DS, ses = ES, sbp = BP;
            call_far(cb_seg, cb_off);
            AX = sax; BX = sbx; CX = scx; DX = sdx; SI = ssi; DI = sdi; DS = sds; ES = ses; BP = sbp;
        }
    }
}

static void midi_reset_all(void);

/* ---------------------------------------------------------------- FM (OPL2) */
/* When MIDI.DRV is the AdLib / Sound Blaster FM driver, the runtime runs the driver's own
 * (recompiled) code at FM_SEG and its port 388h/389h writes land here. */
void opl_reset(void); void opl_write(int r, int v); int opl_status(void); void opl_render(int16_t *buf, int n, int rate);
#define FM_RATE 44100
#define FM_BUFS 10        /* ~120 ms queued ahead: headroom on slower or busy machines */
#define FM_LEN  512       /* short blocks, so register writes still land within ~12 ms */
static CRITICAL_SECTION fm_lock; static int fm_on, fm_index;
static HWAVEOUT fm_wo; static WAVEHDR fm_hdr[FM_BUFS]; static int16_t fm_buf[FM_BUFS][FM_LEN]; static HANDLE fm_event;

static DWORD WINAPI fm_thread(LPVOID p) {
    (void)p;
    for (;;) {
        WaitForSingleObject(fm_event, 50);
        for (int i = 0; i < FM_BUFS; i++) {
            if (!(fm_hdr[i].dwFlags & WHDR_DONE)) continue;
            EnterCriticalSection(&fm_lock); opl_render(fm_buf[i], FM_LEN, FM_RATE); LeaveCriticalSection(&fm_lock);
            {   /* debug: POP2_FMWAV=<file> dumps the raw 16-bit mono FM output */
                static FILE *dump; static int init;
                if (!init) { const char *p = getenv("POP2_FMWAV"); if (p) dump = fopen(p, "wb"); init = 1; }
                if (dump) { fwrite(fm_buf[i], 2, FM_LEN, dump); fflush(dump); }
            }
            fm_hdr[i].dwFlags &= ~WHDR_DONE;
            waveOutWrite(fm_wo, &fm_hdr[i], sizeof fm_hdr[i]);
        }
    }
    return 0;
}
void fm_start(void) {
    if (fm_on || silent()) return;
    InitializeCriticalSection(&fm_lock);
    opl_reset();
    WAVEFORMATEX f = {0};
    f.wFormatTag = WAVE_FORMAT_PCM; f.nChannels = 1; f.nSamplesPerSec = FM_RATE; f.wBitsPerSample = 16;
    f.nBlockAlign = 2; f.nAvgBytesPerSec = FM_RATE * 2;
    fm_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (waveOutOpen(&fm_wo, WAVE_MAPPER, &f, (DWORD_PTR)fm_event, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) { logmsg("FM: waveOutOpen failed\n"); return; }
    for (int i = 0; i < FM_BUFS; i++) {
        memset(fm_buf[i], 0, sizeof fm_buf[i]);
        fm_hdr[i].lpData = (LPSTR)fm_buf[i]; fm_hdr[i].dwBufferLength = sizeof fm_buf[i];
        waveOutPrepareHeader(fm_wo, &fm_hdr[i], sizeof fm_hdr[i]);
        waveOutWrite(fm_wo, &fm_hdr[i], sizeof fm_hdr[i]);
    }
    fm_on = 1;
    {   /* the mixer must not lose against the game thread, or the music breaks up */
        HANDLE th = CreateThread(NULL, 0, fm_thread, NULL, 0, NULL);
        if (th) { SetThreadPriority(th, THREAD_PRIORITY_ABOVE_NORMAL); CloseHandle(th); }
    }
    logmsg("FM synthesis started\n");
}
void fm_port_write(uint16_t port, uint8_t v) {
    if (!fm_on) return;
    if (port == 0x388) fm_index = v;
    else { EnterCriticalSection(&fm_lock); opl_write(fm_index, v); LeaveCriticalSection(&fm_lock); }
}
void opl_all_off(void);
void fm_all_notes_off(void) {
    if (!fm_on) { midi_reset_all(); return; }
    EnterCriticalSection(&fm_lock); opl_all_off(); LeaveCriticalSection(&fm_lock);
}
uint8_t fm_port_read(void) {
    if (!fm_on) return 0xFF;
    EnterCriticalSection(&fm_lock); int s = opl_status(); LeaveCriticalSection(&fm_lock);
    return (uint8_t)s;
}

/* ---------------------------------------------------------------- MIDI */
static HMIDIOUT mo;
static void midi_send(uint8_t st, uint8_t d1, uint8_t d2) {
    if (silent()) return;
    if (!mo) { if (midiOutOpen(&mo, MIDI_MAPPER, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) { mo = NULL; return; } }
    midiOutShortMsg(mo, (DWORD)st | ((DWORD)d1 << 8) | ((DWORD)d2 << 16));
}
static void midi_reset(void) {
    if (!mo) return;
    for (int ch = 0; ch < 16; ch++) { midi_send((uint8_t)(0xB0 | ch), 123, 0); midi_send((uint8_t)(0xB0 | ch), 121, 0); }
}

static void midi_reset_all(void) { midi_reset(); }

void snd_driver_call(int kind) {
    static int logged[2][256];
    int fn = AL;
    if (logged[kind - 1][fn & 0xF0 ? fn & 0xF0 : fn]++ < 2 || (kind == 1 && getenv("POP2_SNDLOG")))
        logmsg("%s driver fn %02x  ax=%04x bx=%04x cx=%04x dx=%04x si=%04x di=%04x es=%04x\n",
               kind == 1 ? "DIGI" : "MIDI", fn, AX, BX, CX, DX, SI, DI, ES);
    if (kind == 1) {
        switch (fn) {
        case 0: AX = 0; BX = 1; CX = 0x0F50; DX = 0xFFFF; return;
        case 1: AX = 0; BX = 1; CX = 0x0F50; DX = 0xFFFF; return;
        case 2: digi_stop(); return;
        case 3: AX = playing ? 0 : 1; digi_stop(); return;
        case 4: volume = AH ? 15 : 0; AX = 0; return;   /* original: DSP speaker on (D1h) if AH != 0, else off (D3h) */
        case 5: case 6: return;
        case 7: cb_off = SI; cb_seg = DI; AX = (uint16_t)digi_play(ES, BX, CX, DX); return;
        case 8: AX = 8; return;
        }
        return;
    }
    /* MIDI */
    if (fn < 8) {
        if (fn == 2 || fn == 4) midi_reset();
        AX = 0; return;
    }
    uint8_t st = (uint8_t)((fn & 0xF0) | (AH & 0x0F));
    switch (fn & 0xF0) {
    case 0x80: case 0x90: case 0xA0: case 0xB0: case 0xE0: midi_send(st, DL & 0x7F, DH & 0x7F); break;
    case 0xC0: case 0xD0: midi_send(st, DL & 0x7F, 0); break;
    }
}

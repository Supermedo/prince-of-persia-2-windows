/* Runtime for the recompiled prince.exe: DOS/BIOS services, PC hardware, overlays, window. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "cpu.h"
#include "dispatch.h"

#define LOADSEG 0x0100
#define PSPSEG  (LOADSEG - 0x10)
#define BIOSSEG 0xF000              /* fake BIOS: interrupt vectors point here (F000:00nn) */
#define MEMTOP  0x9FFF

Reg rA, rB, rC, rD;
uint16_t rSI, rDI, rBP, rSP, sDS, sES, sSS;
uint8_t FC, FZ, FS, FO, FP, FA, FD, FI;
uint8_t *MEM;
const uint8_t parity_tab[256] = {
#define P2(n) n, n^1, n^1, n
#define P4(n) P2(n), P2(n^1), P2(n^1), P2(n)
#define P6(n) P4(n), P4(n^1), P4(n^1), P4(n)
    P6(1), P6(0), P6(0), P6(1)
};

static FILE *logf;
void logmsg(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    if (!logf) logf = fopen("pop2native.log", "w");
    vfprintf(logf, fmt, ap); fflush(logf); va_end(ap);
}
static void fatal(const char *fmt, ...) {
    char buf[1024]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    logmsg("FATAL: %s\n", buf);
    if (!getenv("POP2_KEYS")) MessageBoxA(NULL, buf, "PoP2 native - fatal", MB_OK);   /* scripted runs: just exit */
    exit(1);
}

/* ---------------------------------------------------------------- misc cpu helpers */
void rt_divide_error(void) { logmsg("divide error\n"); do_int(0, 0, 0); }
void op_daa(void) { uint8_t a = AL; if ((a & 15) > 9 || FA) { AL += 6; FA = 1; } else FA = 0; if (a > 0x99 || FC) { AL += 0x60; FC = 1; } else FC = 0; SZP8(AL); }
void op_das(void) { uint8_t a = AL; if ((a & 15) > 9 || FA) { AL -= 6; FA = 1; } else FA = 0; if (a > 0x99 || FC) { AL -= 0x60; FC = 1; } else FC = 0; SZP8(AL); }
void op_aaa(void) { if ((AL & 15) > 9 || FA) { AX += 0x106; FA = FC = 1; } else FA = FC = 0; AL &= 15; }
void op_aas(void) { if ((AL & 15) > 9 || FA) { AX -= 6; AH -= 1; FA = FC = 1; } else FA = FC = 0; AL &= 15; }
void op_aam(void) { AH = AL / 10; AL = AL % 10; SZP8(AL); }
void op_aad(void) { AL = (uint8_t)(AH * 10 + AL); AH = 0; SZP8(AL); }
void fpu_esc(int op) { static int n; if (n++ < 20) logmsg("fpu esc %x ignored\n", op); }

#define STEP(sz) (FD ? -(sz) : (sz))
void str_movsb(uint16_t seg, int rep) { do { if (rep && !CX) break; WB(ES, DI, RB(seg, SI)); SI += STEP(1); DI += STEP(1); if (rep) CX--; } while (rep); }
void str_movsw(uint16_t seg, int rep) { do { if (rep && !CX) break; WW(ES, DI, RW(seg, SI)); SI += STEP(2); DI += STEP(2); if (rep) CX--; } while (rep); }
void str_stosb(uint16_t seg, int rep) { (void)seg; do { if (rep && !CX) break; WB(ES, DI, AL); DI += STEP(1); if (rep) CX--; } while (rep); }
void str_stosw(uint16_t seg, int rep) { (void)seg; do { if (rep && !CX) break; WW(ES, DI, AX); DI += STEP(2); if (rep) CX--; } while (rep); }
void str_lodsb(uint16_t seg, int rep) { do { if (rep && !CX) break; AL = RB(seg, SI); SI += STEP(1); if (rep) CX--; } while (rep); }
void str_lodsw(uint16_t seg, int rep) { do { if (rep && !CX) break; AX = RW(seg, SI); SI += STEP(2); if (rep) CX--; } while (rep); }
void str_cmpsb(uint16_t seg, int rep) { do { if (rep && !CX) break; op_sub8(RB(seg, SI), RB(ES, DI)); SI += STEP(1); DI += STEP(1); if (rep) { CX--; if ((rep == 1 && !FZ) || (rep == 2 && FZ)) break; } } while (rep); }
void str_cmpsw(uint16_t seg, int rep) { do { if (rep && !CX) break; op_sub16(RW(seg, SI), RW(ES, DI)); SI += STEP(2); DI += STEP(2); if (rep) { CX--; if ((rep == 1 && !FZ) || (rep == 2 && FZ)) break; } } while (rep); }
void str_scasb(uint16_t seg, int rep) { (void)seg; do { if (rep && !CX) break; op_sub8(AL, RB(ES, DI)); DI += STEP(1); if (rep) { CX--; if ((rep == 1 && !FZ) || (rep == 2 && FZ)) break; } } while (rep); }
void str_scasw(uint16_t seg, int rep) { (void)seg; do { if (rep && !CX) break; op_sub16(AX, RW(ES, DI)); DI += STEP(2); if (rep) { CX--; if ((rep == 1 && !FZ) || (rep == 2 && FZ)) break; } } while (rep); }

/* ---------------------------------------------------------------- dispatch + overlays */
typedef struct { uint32_t lin; uint16_t seg; Fn fn; } LinFn;
typedef struct { const SpaceTab *t; LinFn *fns; int n; uint32_t lo, hi; } Space;
static Space spaces[32]; static int nspaces;
static uint8_t para_ovl[0x10000];          /* per paragraph: index+1 of the overlay space loaded there */

static int cmp_linfn(const void *a, const void *b) {
    uint32_t x = ((const LinFn *)a)->lin, y = ((const LinFn *)b)->lin; return x < y ? -1 : x > y;
}
static void init_dispatch(void) {
    for (const SpaceTab *t = space_tabs; t->name; t++) {
        Space *s = &spaces[nspaces++];
        s->t = t;
        int n = 0; while (t->tab[n].fn) n++;
        s->fns = malloc(sizeof(LinFn) * (n + 1)); s->n = n;
        for (int i = 0; i < n; i++) { s->fns[i].lin = LIN(t->tab[i].seg, t->tab[i].off); s->fns[i].seg = t->tab[i].seg; s->fns[i].fn = t->tab[i].fn; }
        qsort(s->fns, n, sizeof(LinFn), cmp_linfn);
        s->lo = (uint32_t)t->seg << 4; s->hi = s->lo + t->size;
    }
}
/* the function at linear address `lin`; if several were recompiled there (same code reached with
 * different CS values), prefer the one with the CS actually used by the caller */
static Fn find_in_seg(Space *s, uint32_t lin, uint16_t seg) {
    int lo = 0, hi = s->n - 1, m = -1;
    while (lo <= hi) { int k = (lo + hi) / 2; if (s->fns[k].lin == lin) { m = k; break; } if (s->fns[k].lin < lin) lo = k + 1; else hi = k - 1; }
    if (m < 0) return NULL;
    while (m > 0 && s->fns[m - 1].lin == lin) m--;
    for (int k = m; k < s->n && s->fns[k].lin == lin; k++) if (s->fns[k].seg == seg) return s->fns[k].fn;
    return s->fns[m].fn;
}
static void record_missing(const char *sp, uint32_t lin, uint16_t seg, uint16_t off) {
    FILE *f = fopen("missing_entries.txt", "a");
    if (f) { fprintf(f, "%s %04x:%04x\n", sp, (seg - LOADSEG) & 0xFFFF, off); fclose(f); }
    logmsg("missing code: last function %s  ss:sp=%04x:%04x bp=%04x ds=%04x es=%04x ax=%04x\n", cur_fn, SS, SP, BP, DS, ES, AX);
    fatal("no recompiled code for %04x:%04x (space %s, linear %05x)", seg, off, sp, lin);
}
static void bios_call(uint16_t off);

/* ---- sound drivers (*.DRV loaded by the game at runtime): replaced by native handlers */
enum { DRV_NONE, DRV_DIGI, DRV_MIDI };
static uint32_t drv_base[3], drv_size[3];   /* linear address of the driver file's byte 0, file size */
static int cur_drv_kind;                     /* kind of the most recently opened .DRV */
static void fm_driver_check(const char *name);
static int cfg_sound_keep;  /* pop2.ini Sound=keep: use the game's own SETUP instead of ours */

static void note_driver_read(const char *name, uint32_t dst, uint32_t fpos) {
    size_t l = strlen(name);
    if (l < 4 || _stricmp(name + l - 4, ".DRV") != 0) return;
    const char *b = strrchr(name, '\\'); b = b ? b + 1 : name;
    int kind = (_strnicmp(b, "M", 1) == 0) ? DRV_MIDI : DRV_DIGI;
    drv_base[kind] = dst - fpos;
    { FILE *f = fopen(name, "rb"); if (f) { fseek(f, 0, SEEK_END); drv_size[kind] = (uint32_t)ftell(f); fclose(f); } }
    logmsg("driver %s read: file %u -> %05x (base %05x, kind %d)\n", name, fpos, dst, drv_base[kind], kind);
    if (kind == DRV_MIDI) fm_driver_check(name);
}
/* FM music driver: if MIDI.DRV is the AdLib / Sound Blaster FM driver (the code recompiled as
 * space "fm"), run that code at FM_SEG and emulate the OPL2 chip instead of mapping to GM. */
#define FM_SEG 0xD000
static int fm_mode;
void fm_start(void); void fm_port_write(uint16_t port, uint8_t v); uint8_t fm_port_read(void);   /* sound.c */
static void fm_driver_check(const char *name) {
    if (getenv("POP2_NOFM")) return;           /* debug: map the music to the Windows MIDI synth */
    const SpaceTab *t = NULL;
    for (int i = 0; i < nspaces; i++) if (spaces[i].t->ovl_id == 100) { t = spaces[i].t; break; }
    if (!t || fm_mode) return;
    char ref[MAX_PATH]; const char *b = strrchr(name, '\\');
    snprintf(ref, sizeof ref, "%.*smsb_pro.drv", b ? (int)(b - name + 1) : 0, name);
    static uint8_t a[4096], r[4096];
    FILE *f = fopen(name, "rb"), *g = fopen(ref, "rb");
    size_t na = f ? fread(a, 1, sizeof a, f) : 0, nr = g ? fread(r, 1, sizeof r, g) : 0;
    if (f) fclose(f);
    if (g) fclose(g);
    /* The recompiled driver IS the game's msb_pro.drv, so that file is the only image allowed to
     * run here - never whatever MIDI.DRV happens to be.  If the player set the game up for some
     * other music device, run the FM driver anyway: it ships with the game, the game only calls
     * it through its fixed entry points, and FM is how this soundtrack is meant to sound. */
    if (nr != (size_t)t->size) {
        logmsg("msb_pro.drv missing or not the recompiled version: General MIDI output\n");
        return;
    }
    /* bytes 0..63 are just the card name */
    if (na != nr || memcmp(a + 64, r + 64, nr - 64) != 0)
        logmsg("MIDI.DRV is not the FM driver - using the game's own msb_pro.drv (pop2.ini Music=gm for General MIDI)\n");
    memcpy(MEM + FM_SEG * 16, r, nr);
    int si = 0;
    for (int i = 0; i < nspaces; i++) if (spaces[i].t == t) si = i;
    for (uint32_t p = 0; p < nr; p += 16) para_ovl[FM_SEG + p / 16] = (uint8_t)(si + 1);
    fm_mode = 1;
    fm_start();
    logmsg("FM driver: running recompiled driver code with OPL2 emulation\n");
}
void snd_driver_call(int kind);          /* sound.c */
static int driver_dispatch(uint16_t seg, uint16_t off) {
    uint32_t lin = LIN(seg, off);
    for (int k = DRV_DIGI; k <= DRV_MIDI; k++) {
        if (drv_base[k] && lin >= drv_base[k] && lin < drv_base[k] + drv_size[k]) {
            if (k == DRV_MIDI && fm_mode) {
                int fn = AL;
                uint16_t iax = AX, ibx = BX, icx = CX, idx = DX;
                call_far(FM_SEG, (uint16_t)(lin - drv_base[k]));      /* driver code returns with RETF itself */
                if (getenv("POP2_MIDILOG") && fn < 0x80)
                    logmsg("FMDRV fn %02x: in ax=%04x bx=%04x cx=%04x dx=%04x -> ax=%04x bx=%04x cx=%04x dx=%04x\n",
                           fn, iax, ibx, icx, idx, AX, BX, CX, DX);
                if (fn == 1) { WW(FM_SEG, 0x147, 1); WW(FM_SEG, 0x149, 1); }   /* OPL write delays: not needed */
                return 1;
            }
            snd_driver_call(k);
            POP(); POP();                    /* RETF */
            return 1;
        }
    }
    return 0;
}

static void call_dispatch(uint16_t seg, uint16_t off) {
    uint32_t lin = LIN(seg, off);
    if (seg == BIOSSEG) { bios_call(off); return; }
    if (driver_dispatch(seg, off)) return;
    /* root */
    Space *root = &spaces[0];
    for (int i = 0; i < nspaces; i++) if (spaces[i].t->ovl_id == 0) { root = &spaces[i]; break; }
    if (lin >= root->lo && lin < root->hi) {
        Fn f = find_in_seg(root, lin, seg); if (f) { f(); return; }
        record_missing("root", lin, (uint16_t)(seg), off); return;
    }
    int si = para_ovl[(lin >> 4) & 0xFFFF];
    if (si) {
        Space *s = &spaces[si - 1];
        Fn f = find_in_seg(s, lin, seg); if (f) { f(); return; }
        record_missing(s->t->name, lin, seg, off); return;
    }
    fatal("far call to %04x:%04x: no code loaded there", seg, off);
}
/* A call must leave the stack exactly as it found it (minus the return address the caller
 * pushed).  POP2_SPCHK reports any call that does not, which is how a slow stack leak shows up. */
static void bp_check(uint16_t seg, uint16_t off, uint16_t bp0) {
    static int n;
    uint32_t a = LIN(seg, off);
    /* only standard frame functions ("push bp; mov bp,sp"): those must restore BP */
    if (!(MEM[a] == 0x55 && MEM[a + 1] == 0x8B && MEM[a + 2] == 0xEC)) return;
    if (BP == bp0 || n >= 30) return;
    n++;
    logmsg("BP CLOBBER: call %04x:%04x changed bp %04x -> %04x (in %s)\n", seg, off, bp0, BP, cur_fn);
}
static void sp_check(uint16_t seg, uint16_t off, uint16_t sp0, int expect) {
    static int n;
    if (SP == (uint16_t)(sp0 + expect) || n >= 40) return;
    logmsg("STACK LEAK: call %04x:%04x returned sp %04x, expected %04x (%+d) in %s\n",
           seg, off, SP, (uint16_t)(sp0 + expect), (int16_t)(SP - (uint16_t)(sp0 + expect)), cur_fn);
    n++;
}
void call_far(uint16_t seg, uint16_t off) {
    static int chk = -1; if (chk < 0) chk = getenv("POP2_SPCHK") != NULL;
    uint16_t sp0 = SP, bp0 = BP;
    call_dispatch(seg, off);
    if (chk) { sp_check(seg, off, sp0, 4); bp_check(seg, off, bp0); }
}
void call_near(uint16_t cs, uint16_t off) {
    static int chk = -1; if (chk < 0) chk = getenv("POP2_SPCHK") != NULL;
    uint16_t sp0 = SP, bp0 = BP;
    call_dispatch(cs, off);
    if (chk) { sp_check(cs, off, sp0, 2); bp_check(cs, off, bp0); }
}
void rt_halt(uint16_t cs, uint16_t ip) { fatal("HLT at %04x:%04x", cs, ip); }
void rt_unimpl(uint16_t cs, uint16_t ip) { fatal("unimplemented instruction at %04x:%04x", cs, ip); }
void rt_badjump(uint16_t cs, uint16_t ip, uint16_t t) { fatal("jump table at %04x:%04x: unexpected target %04x", cs, ip, t); }
void rt_missing(const char *sp, uint16_t seg, uint16_t off) {
    FILE *f = fopen("missing_entries.txt", "a");
    if (f) { fprintf(f, "%s %04x:%04x\n", sp, seg, off); fclose(f); }
    fatal("missing function %s %04x:%04x", sp, seg, off);
}

/* overlay tracking: file data read into memory marks the paragraphs with the overlay that holds it */
static void note_code_read(uint32_t dst, uint32_t fpos, uint32_t len) {
    for (int i = 0; i < nspaces; i++) {
        const SpaceTab *t = spaces[i].t;
        if (t->ovl_id == 0 || t->ovl_id >= 100) continue;     /* root / runtime-loaded drivers */
        uint32_t a = t->fileoff, b = t->fileoff + t->size;
        if (fpos + len <= a || fpos >= b) continue;
        for (uint32_t k = 0; k < len; k += 16) {
            uint32_t fp = fpos + k;
            if (fp >= a && fp < b) para_ovl[((dst + k) >> 4) & 0xFFFF] = (uint8_t)(i + 1);
        }
        logmsg("overlay %s (id %d) loaded at %05x\n", t->name, t->ovl_id, dst - (fpos - a));
    }
}

/* ---------------------------------------------------------------- video + window */
static HWND hwnd; static BITMAPINFO *bmi; static uint8_t pal[256][3]; static int pal_idx, pal_comp, pal_read_idx, pal_read_comp;
static uint8_t frame[320 * 200];
static int video_mode = 3;
static int cfg_filter = 0, cfg_aspect = 0;          /* see video.c; pop2.ini Filter= / Aspect= */
void video_init(HWND hwnd); void video_present(HWND hwnd, const uint8_t *frame, const uint8_t pal[256][3], int filter, int aspect);
static void present(void) {
    if (!hwnd) return;
    for (int i = 0; i < 256; i++) {
        bmi->bmiColors[i].rgbRed = (uint8_t)(pal[i][0] << 2 | pal[i][0] >> 4);
        bmi->bmiColors[i].rgbGreen = (uint8_t)(pal[i][1] << 2 | pal[i][1] >> 4);
        bmi->bmiColors[i].rgbBlue = (uint8_t)(pal[i][2] << 2 | pal[i][2] >> 4);
    }
    memcpy(frame, MEM + 0xA0000, sizeof frame);
    {   /* debug screenshots: POP2_SHOTS=<dir> saves a BMP every second */
        static DWORD last; static int shotn; static char *dir; static int init;
        if (!init) { dir = getenv("POP2_SHOTS"); init = 1; }
        if (dir && GetTickCount() - last > 1000) {
            last = GetTickCount();
            char fn[MAX_PATH]; snprintf(fn, MAX_PATH, "%s/shot%03d.bmp", dir, shotn++);
            FILE *f = fopen(fn, "wb");
            if (f) {
                BITMAPFILEHEADER bf = {0}; BITMAPINFOHEADER bh = bmi->bmiHeader; bh.biHeight = 200;
                bf.bfType = 0x4D42; bf.bfOffBits = sizeof bf + sizeof bh + 1024; bf.bfSize = bf.bfOffBits + 64000;
                fwrite(&bf, sizeof bf, 1, f); fwrite(&bh, sizeof bh, 1, f); fwrite(bmi->bmiColors, 1024, 1, f);
                for (int y = 199; y >= 0; y--) fwrite(frame + y * 320, 320, 1, f);
                fclose(f);
            }
        }
    }
    video_present(hwnd, frame, (const uint8_t (*)[3])pal, cfg_filter, cfg_aspect);   /* video.c */
}

/* keyboard: scancode queue feeding port 0x60 + IRQ1 */
static uint8_t kq[256]; static int kq_head, kq_tail; static uint8_t port60;
static uint16_t bios_kbuf[64]; static int bk_head, bk_tail;
static int vk_to_scan(WPARAM vk, LPARAM lp) { return (int)((lp >> 16) & 0xFF) | ((lp & (1 << 24)) ? 0x100 : 0); }
static uint8_t key_down[0x200];
/* Keys are translated with the US layout, like the PC BIOS this game was written for: with a
 * non-Latin layout active (e.g. Arabic) Windows gives no character, and the game would then read
 * plain letters as Alt+letter commands (save, restart level, quit). */
static HKL us_layout(void) {
    static HKL h; static int init;
    if (!init) { init = 1; h = LoadKeyboardLayoutA("00000409", KLF_NOTELLSHELL); }
    return h;
}
static int key_ascii(UINT vk, UINT scan, const BYTE *state) {
    WORD ch = 0;
    HKL us = us_layout();
    int n = us ? ToAsciiEx(vk, scan, state, &ch, 0, us) : ToAscii(vk, scan, state, &ch, 0);
    return n == 1 ? (ch & 0xFF) : 0;
}
/* checkpoint restart (see rt_frame_hook): while the kid is dead, a key press restores the snapshot */
static int cp_kid_dead, cp_valid, cp_restore_pending; static DWORD cp_dead_since;
/* recent input, so the save-menu hook can report what the game was fed just before it opened */
static struct { DWORD t; int scan, up, ascii; } key_log[32]; static int key_log_n;
static struct { DWORD t; int val; } doskey_log[32]; static int doskey_log_n;
static void key_event(int scan, int up, int ascii) {
    { int i = key_log_n++ & 31; key_log[i].t = GetTickCount(); key_log[i].scan = scan; key_log[i].up = up; key_log[i].ascii = ascii; }
    if (getenv("POP2_KEYLOG")) logmsg("key_event %03x %s ascii %02x\n", scan, up ? "up" : "down", ascii);
    if (!up && cp_kid_dead && cp_valid && !cp_restore_pending && GetTickCount() - cp_dead_since > 1200
        && (scan & 0xFF) != 0x1D && (scan & 0xFF) != 0x38) {           /* not for Ctrl/Alt (game shortcuts) */
        cp_restore_pending = 1; key_down[scan & 0x1FF] = 1;
        return;                                                        /* the game must not see this key */
    }
    key_down[scan & 0x1FF] = (uint8_t)!up;
    {   /* BIOS keyboard flags at 0040:0017 (the game reads Shift/Ctrl/Alt from here, like a real BIOS keeps it) */
        uint8_t bit = 0; int s = scan & 0xFF;
        if (s == 0x36) bit = 0x01;                 /* right shift */
        else if (s == 0x2A) bit = 0x02;            /* left shift */
        else if (s == 0x1D) bit = 0x04;            /* ctrl (left or right) */
        else if (s == 0x38) bit = 0x08;            /* alt */
        if (bit) {
            int held = (bit == 0x04) ? (key_down[0x1D] | key_down[0x11D]) : (bit == 0x08) ? (key_down[0x38] | key_down[0x138]) : !up;
            uint8_t f = RB(0x40, 0x17); f = held ? (uint8_t)(f | bit) : (uint8_t)(f & ~bit); WB(0x40, 0x17, f);
        }
    }
    if (scan & 0x100) kq[kq_tail++ & 255] = 0xE0;
    kq[kq_tail++ & 255] = (uint8_t)((scan & 0x7F) | (up ? 0x80 : 0));
    {   /* BIOS keyboard buffer: like a real BIOS, modifier and lock keys produce no entry */
        int s = scan & 0x7F;
        int modifier = s == 0x2A || s == 0x36 || s == 0x1D || s == 0x38 || s == 0x3A || s == 0x45 || s == 0x46;
        if (!up && !modifier) bios_kbuf[bk_tail++ & 63] = (uint16_t)((s << 8) | (ascii & 0xFF));
    }
}
static int quit_req;
static void update_title(void) {
    static const char *fn[] = { "Sharp", "Pixel", "Soft", "Smooth HQ" }, *an[] = { "4:3", "Wide", "Wide panorama" };
    char t[128]; snprintf(t, sizeof t, "Prince of Persia 2   [%s, %s  -  F7 filter, F8 view, F11 fullscreen]", fn[cfg_filter & 3], an[cfg_aspect % 3]);
    if (hwnd) SetWindowTextA(hwnd, t);
}
static void toggle_fullscreen(void) {
    static WINDOWPLACEMENT prev = { sizeof prev }; static int full;
    DWORD st = (DWORD)GetWindowLongA(hwnd, GWL_STYLE);
    if (!full) {
        MONITORINFO mi = { sizeof mi };
        GetWindowPlacement(hwnd, &prev);
        GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
        SetWindowLongA(hwnd, GWL_STYLE, st & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        ShowCursor(FALSE);
    } else {
        SetWindowLongA(hwnd, GWL_STYLE, st | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(hwnd, &prev);
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        ShowCursor(TRUE);
    }
    full = !full;
    InvalidateRect(hwnd, NULL, TRUE);
}
static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CLOSE: quit_req = 1; return 0;
    case WM_KILLFOCUS:                     /* release held keys so nothing sticks */
        for (int s = 0; s < 0x200; s++) if (key_down[s]) key_event(s, 1, 0);
        return 0;
    case WM_SYSKEYDOWN:
        if (w == VK_RETURN) { toggle_fullscreen(); return 0; }
        if (w == VK_F4) { quit_req = 1; return 0; }          /* Alt+F4 always quits (also in fullscreen) */
        /* fall through */
    case WM_KEYDOWN: {
        if (w == VK_F11) { toggle_fullscreen(); return 0; }
        if (w == VK_F7) { cfg_filter = (cfg_filter + 1) & 3; update_title(); return 0; }   /* cycle display filter */
        if (w == VK_F8) { cfg_aspect = (cfg_aspect + 1) % 3; update_title(); return 0; }   /* cycle aspect */
        BYTE ks[256]; WORD ch = 0; GetKeyboardState(ks);
        ks[VK_MENU] = ks[VK_LMENU] = ks[VK_RMENU] = 0;        /* Alt never changes the character */
        int asc = key_ascii((UINT)w, (l >> 16) & 0xFF, ks);
        /* Alt+key has no ASCII code, as from a PC BIOS.  The message itself carries the Alt state
         * (lParam bit 29), which stays right even when Alt was released over another window. */
        if (m == WM_SYSKEYDOWN && ((l >> 29) & 1)) asc = 0;
        key_event(vk_to_scan(w, l), 0, asc); return 0; }
    case WM_KEYUP: case WM_SYSKEYUP: key_event(vk_to_scan(w, l), 1, 0); return 0;
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps); present(); return 0; }
    case WM_ERASEBKGND: return 1;
    }
    return DefWindowProcA(h, m, w, l);
}
/* ---- settings (pop2.ini next to the exe, written by the launcher) */
static char cfg_game_dir[MAX_PATH]; static int cfg_fullscreen, cfg_scale = 3, cfg_controller = 1, cfg_checkpoints = 0;
static void load_settings(void) {
    char ini[MAX_PATH]; GetModuleFileNameA(NULL, ini, MAX_PATH);
    char *s = strrchr(ini, '\\'); strcpy(s ? s + 1 : ini, "pop2.ini");
    GetPrivateProfileStringA("pop2", "GameDir", "", cfg_game_dir, MAX_PATH, ini);
    cfg_fullscreen = GetPrivateProfileIntA("pop2", "Fullscreen", 0, ini);
    cfg_scale = GetPrivateProfileIntA("pop2", "Scale", 3, ini);
    if (cfg_scale < 1) cfg_scale = 1;
    if (cfg_scale > 8) cfg_scale = 8;
    cfg_controller = GetPrivateProfileIntA("pop2", "Controller", 1, ini);
    cfg_checkpoints = GetPrivateProfileIntA("pop2", "Checkpoints", 0, ini);
    { char m[16]; GetPrivateProfileStringA("pop2", "Sound", "port", m, sizeof m, ini);
      cfg_sound_keep = _stricmp(m, "keep") == 0; }
    cfg_filter = GetPrivateProfileIntA("pop2", "Filter", 0, ini) & 3;
    cfg_aspect = GetPrivateProfileIntA("pop2", "Aspect", 0, ini);
    if (cfg_aspect < 0 || cfg_aspect > 2) cfg_aspect = 0;
}

/* ---- extra checkpoints (optional enhancement, off by default; pop2.ini Checkpoints=1).
 * Off = the original game's own restart points (level start / level-specific restart rooms).
 * The original restarts a dead prince at the start of the level.  Here the whole machine state is
 * snapshotted the first time the kid stands safely in each new room; after a death, a key press
 * restores that snapshot instead (the remaining time is kept, so dying still costs time).
 * Snapshot and restore both happen at the same point of the per-frame update, so the C call chain
 * and the emulated stack match. */
#define DG 0x31F3
static uint8_t *cp_mem; static uint8_t cp_para[0x10000];
static Reg cp_ra, cp_rb, cp_rc, cp_rd; static uint16_t cp_si, cp_di, cp_bp, cp_sp, cp_ds, cp_es, cp_ss, cp_flags;
static int cp_room = -1, cp_level = -1;
int digi_busy(void); void digi_abort(void); void fm_all_notes_off(void);   /* sound.c */
static void cp_save_ext(void); static void cp_restore_ext(void);
void rt_frame_hook(void) {
    if (getenv("POP2_FPSLOG")) {    /* debug: game logic frames per second */
        static DWORD lt; static int n; n++;
        if (GetTickCount() - lt >= 1000) { logmsg("FPS %d\n", n); n = 0; lt = GetTickCount(); }
    }
    if (getenv("POP2_SPLOG")) {     /* debug: watch the game's stack pointer for a slow leak */
        static DWORD lt; static uint16_t lo = 0xFFFF;
        if (SP < lo) lo = SP;
        if (GetTickCount() - lt >= 1000) { logmsg("SS:SP %04x:%04x (lowest %04x)\n", SS, SP, lo); lt = GetTickCount(); }
    }
    if (!cfg_checkpoints) return;
    int level = (int8_t)MEM[LIN(DG, 0x0A18)], room = MEM[LIN(DG, 0x5C0E)], hp = MEM[LIN(DG, 0x5C12)];
    int action = MEM[LIN(DG, 0x5C0B)];
    if (level != cp_level) { cp_level = level; cp_valid = 0; cp_room = -1; }
    if (cp_restore_pending) {
        cp_restore_pending = 0;
        if (cp_valid) {
            uint8_t tmin[2] = { MEM[LIN(DG, 0x5E2A)], MEM[LIN(DG, 0x5E2B)] }, ttick[2] = { MEM[LIN(DG, 0x5E44)], MEM[LIN(DG, 0x5E45)] };
            memcpy(MEM, cp_mem, 0x110000);
            memcpy(para_ovl, cp_para, sizeof cp_para);
            cp_restore_ext();
            MEM[LIN(DG, 0x5E2A)] = tmin[0]; MEM[LIN(DG, 0x5E2B)] = tmin[1];      /* keep the time left */
            MEM[LIN(DG, 0x5E44)] = ttick[0]; MEM[LIN(DG, 0x5E45)] = ttick[1];
            rA = cp_ra; rB = cp_rb; rC = cp_rc; rD = cp_rd; SI = cp_si; DI = cp_di; BP = cp_bp; SP = cp_sp;
            DS = cp_ds; ES = cp_es; SS = cp_ss; set_flags(cp_flags);
            /* keyboard: the restored key table must match the keys held right now */
            kq_head = kq_tail; bk_head = bk_tail;
            for (int s = 0; s < 0x59; s++) MEM[LIN(DG, 0x1E7B + s)] = (uint8_t)(key_down[s] | key_down[0x100 + s]);
            uint8_t f = 0;
            if (key_down[0x36]) f |= 1;
            if (key_down[0x2A]) f |= 2;
            if (key_down[0x1D] | key_down[0x11D]) f |= 4;
            if (key_down[0x38] | key_down[0x138]) f |= 8;
            WB(0x40, 0x17, f);
            digi_abort(); fm_all_notes_off();
            cp_kid_dead = 0;
            logmsg("checkpoint: restored room %d\n", cp_room);
            return;
        }
    }
    if (hp == 0) { if (!cp_kid_dead) { cp_kid_dead = 1; cp_dead_since = GetTickCount(); } return; }
    cp_kid_dead = 0;
    /* first safe moment in a new room: standing / running / turning, no sample playing */
    if (room != 0 && room != cp_room && (action == 0 || action == 1 || action == 7) && !digi_busy()) {
        if (!cp_mem) cp_mem = malloc(0x110000);
        memcpy(cp_mem, MEM, 0x110000);
        memcpy(cp_para, para_ovl, sizeof cp_para);
        cp_save_ext();
        cp_ra = rA; cp_rb = rB; cp_rc = rC; cp_rd = rD; cp_si = SI; cp_di = DI; cp_bp = BP; cp_sp = SP;
        cp_ds = DS; cp_es = ES; cp_ss = SS; cp_flags = get_flags();
        cp_room = room; cp_valid = 1;
        logmsg("checkpoint: saved room %d (level %d)\n", room, level);
    }
}

void rt_savemenu_hook(void) {
    DWORD now = GetTickCount();
    logmsg("SAVE MENU opened (flag 1504=%d, level=%d)\n", RW(DG, 0x1504), (int8_t)MEM[LIN(DG, 0x0A18)]);
    for (int i = 0; i < 32; i++) {
        int k = (key_log_n + i) & 31;
        if (key_log[k].t) logmsg("   key  -%5lu ms  scan %03x %s ascii %02x\n", now - key_log[k].t,
                                 key_log[k].scan, key_log[k].up ? "up  " : "down", key_log[k].ascii);
    }
    for (int i = 0; i < 32; i++) {
        int k = (doskey_log_n + i) & 31;
        if (doskey_log[k].t) logmsg("   read -%5lu ms  key %04x\n", now - doskey_log[k].t, doskey_log[k].val);
    }
}

/* ---- game controller (XInput): pad buttons become the keys the game reads */
typedef struct { WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } PadState;
typedef struct { DWORD packet; PadState pad; } PadInfo;
typedef DWORD (WINAPI *XInputGetStateFn)(DWORD, PadInfo *);
static XInputGetStateFn xinput_get;
static void pad_init(void) {
    const char *dll[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
    for (int i = 0; i < 3 && !xinput_get; i++) {
        HMODULE m = LoadLibraryA(dll[i]);
        if (m) xinput_get = (XInputGetStateFn)(void *)GetProcAddress(m, "XInputGetState");
    }
    logmsg("controller support: %s\n", xinput_get ? "XInput" : "not available");
}
enum { PB_UP = 0x0001, PB_DOWN = 0x0002, PB_LEFT = 0x0004, PB_RIGHT = 0x0008, PB_START = 0x0010, PB_BACK = 0x0020,
       PB_LB = 0x0100, PB_RB = 0x0200, PB_A = 0x1000, PB_B = 0x2000, PB_X = 0x4000, PB_Y = 0x8000 };
/* generic DirectInput / HID pads (PlayStation, Switch, USB) through the Windows joystick API */
#include <mmsystem.h>
static int joy_id = -1;
static int joy_read(JOYINFOEX *ji) {
    static DWORD last_scan;
    ji->dwSize = sizeof *ji; ji->dwFlags = JOY_RETURNALL;
    if (joy_id >= 0 && joyGetPosEx((UINT)joy_id, ji) == JOYERR_NOERROR) return 1;
    if (GetTickCount() - last_scan < 1000) return 0;
    last_scan = GetTickCount(); joy_id = -1;
    UINT n = joyGetNumDevs();
    for (UINT i = 0; i < n && i < 16; i++) {
        ji->dwSize = sizeof *ji; ji->dwFlags = JOY_RETURNALL;
        if (joyGetPosEx(i, ji) == JOYERR_NOERROR) {
            JOYCAPSA caps; joy_id = (int)i;
            if (joyGetDevCapsA(i, &caps, sizeof caps) == JOYERR_NOERROR) logmsg("controller: %s (joystick API)\n", caps.szPname);
            return 1;
        }
    }
    return 0;
}
static void pad_poll(void) {
    static int pad_idx = 0; static uint8_t held[0x200]; static DWORD last_scan;
    if (!cfg_controller) return;
    int up = 0, down = 0, left = 0, right = 0, jump = 0, crouch = 0, action = 0, strike = 0, pause = 0, skip = 0;
    PadInfo st; int ok = 0;
    if (xinput_get) {
        if (xinput_get(pad_idx, &st) == 0) ok = 1;
        else if (GetTickCount() - last_scan > 1000) {       /* look for another connected pad now and then */
            last_scan = GetTickCount();
            for (int i = 0; i < 4 && !ok; i++) if (xinput_get(i, &st) == 0) { pad_idx = i; ok = 1; }
        }
    }
    if (ok) {                                                /* Xbox layout */
        WORD b = st.pad.buttons; const int dz = 14000;
        up = (b & PB_UP) || st.pad.ly > dz; down = (b & PB_DOWN) || st.pad.ly < -dz;
        left = (b & PB_LEFT) || st.pad.lx < -dz; right = (b & PB_RIGHT) || st.pad.lx > dz;
        jump = (b & PB_A) != 0; crouch = (b & PB_Y) != 0;
        action = (b & (PB_B | PB_RB)) || st.pad.rt > 100; strike = (b & (PB_X | PB_LB)) || st.pad.lt > 100;
        pause = (b & PB_START) != 0; skip = (b & PB_BACK) != 0;
    } else {
        JOYINFOEX ji;
        if (joy_read(&ji)) {                                 /* PlayStation layout: 1 Square 2 Cross 3 Circle 4 Triangle 5 L1 6 R1 7 L2 8 R2 9 Share 10 Options */
            DWORD b = ji.dwButtons; const DWORD lo = 16384, hi = 49152;
            DWORD pov = ji.dwPOV;
            int pu = 0, pd = 0, pl = 0, pr = 0;
            if (pov != JOY_POVCENTERED && pov <= 36000) {
                pu = pov >= 31500 || pov <= 4500; pr = pov >= 4500 && pov <= 13500;
                pd = pov >= 13500 && pov <= 22500; pl = pov >= 22500 && pov <= 31500;
            }
            up = pu || ji.dwYpos < lo; down = pd || ji.dwYpos > hi;
            left = pl || ji.dwXpos < lo; right = pr || ji.dwXpos > hi;
            jump = (b & 0x002) != 0;                         /* Cross */
            strike = (b & (0x001 | 0x010 | 0x040)) != 0;     /* Square, L1, L2 */
            action = (b & (0x004 | 0x020 | 0x080)) != 0;     /* Circle, R1, R2 */
            crouch = (b & 0x008) != 0;                       /* Triangle */
            skip = (b & 0x100) != 0;                         /* Share / Select */
            pause = (b & 0x200) != 0;                        /* Options / Start */
        }
    }
    struct { int scan, on, ascii; } map[] = {
        { 0x148, up || jump, 0 },                            /* up: jump, climb, block */
        { 0x150, down || crouch, 0 },                        /* down: crouch, sheathe sword */
        { 0x14B, left, 0 }, { 0x14D, right, 0 },
        { 0x2A, action, 0 },                                 /* Shift: action, grab */
        { 0x1D, strike, 0 },                                 /* Ctrl: sword strike */
        { 0x39, skip, 32 },                                  /* Space: skip intro, time left */
        { 0x01, pause, 27 },                                 /* Esc: pause */
    };
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++) {
        int sc = map[i].scan, on = map[i].on ? 1 : 0;
        if (on != held[sc]) { held[sc] = (uint8_t)on; key_event(sc, !on, map[i].ascii); }
    }
}
static void pump(void) {
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    if (quit_req) { logmsg("window closed\n"); exit(0); }
    pad_poll();
}
static void create_window(void) {
    bmi = calloc(1, sizeof(BITMAPINFOHEADER) + 256 * sizeof(RGBQUAD));
    bmi->bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bmi->bmiHeader.biWidth = 320; bmi->bmiHeader.biHeight = -200;
    bmi->bmiHeader.biPlanes = 1; bmi->bmiHeader.biBitCount = 8;
    WNDCLASSA wc = {0}; wc.lpfnWndProc = wndproc; wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "PoP2Native"; wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.style = CS_OWNDC;
    wc.hIcon = LoadIconA(wc.hInstance, MAKEINTRESOURCEA(1));
    RegisterClassA(&wc);
    RECT r = {0, 0, 320 * cfg_scale, 240 * cfg_scale}; AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    /* scripted test runs (POP2_KEYS) stay invisible so they don't pop up on the desktop */
    hwnd = CreateWindowA("PoP2Native", "Prince of Persia 2", WS_OVERLAPPEDWINDOW | (getenv("POP2_KEYS") ? 0 : WS_VISIBLE),
                         CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
}

/* ---------------------------------------------------------------- timer / interrupts */
static uint32_t ivt_seg[256], ivt_off[256];      /* vectors as set by the program (via INT 21h/25h or direct) */
static double pit_hz = 1193182.0 / 65536.0;       /* channel 0 rate */
static int pit_latch_lo, pit_write_hi; static uint16_t pit_reload = 0;
static LARGE_INTEGER qpf, t_last_tick, t_last_present;
static int in_irq;

static void raise_irq(int vec) {
    /* hardware interrupt: push flags/cs/ip and call the handler (vector from the IVT in memory) */
    uint16_t off = RW(0, vec * 4), seg = RW(0, vec * 4 + 2);
    uint16_t bp0 = BP, sp0 = SP, ax0 = AX, bx0 = BX, cx0 = CX, dx0 = DX, si0 = SI, di0 = DI, ds0 = DS, es0 = ES;
    PUSH(get_flags()); FI = 0;
    PUSH(0xFFFF); PUSH(0xFFFF);                    /* dummy return address */
    call_far(seg, off);
    {   /* an interrupt handler must leave the interrupted code's registers alone */
        static int n;
        if (n < 20 && (BP != bp0 || SP != sp0 || AX != ax0 || BX != bx0 || CX != cx0 || DX != dx0
                       || SI != si0 || DI != di0 || DS != ds0 || ES != es0)) {
            n++;
            logmsg("IRQ %d handler %04x:%04x changed registers: bp %04x->%04x sp %04x->%04x ax %04x->%04x "
                   "bx %04x->%04x cx %04x->%04x dx %04x->%04x si %04x->%04x di %04x->%04x ds %04x->%04x es %04x->%04x (in %s)\n",
                   vec, seg, off, bp0, BP, sp0, SP, ax0, AX, bx0, BX, cx0, CX, dx0, DX, si0, SI, di0, DI, ds0, DS, es0, ES, cur_fn);
        }
    }
}
static long n_tick_checks, n_presents, n_irq8;
/* scripted input for automated tests: POP2_KEYS="ms:scan[:hold_ms],..." */
typedef struct { DWORD t, hold; int scan; int state; } AutoKey;
static AutoKey autokeys[256]; static int n_autokeys; static DWORD t_start;
static void autokeys_init(void) {
    const char *s = getenv("POP2_KEYS"); t_start = GetTickCount();
    while (s && *s && n_autokeys < 256) {
        unsigned t = 0, sc = 0, hold = 80; int n = 0;
        if (sscanf(s, "%u:%x:%u%n", &t, &sc, &hold, &n) == 3 || (hold = 80, sscanf(s, "%u:%x%n", &t, &sc, &n) == 2)) {
            autokeys[n_autokeys].t = t; autokeys[n_autokeys].scan = (int)sc; autokeys[n_autokeys].hold = hold; n_autokeys++;
        }
        s = strchr(s, ','); if (s) s++;
    }
}
static void autokeys_poll(void) {
    DWORD now = GetTickCount() - t_start;
    for (int i = 0; i < n_autokeys; i++) {
        AutoKey *k = &autokeys[i];
        if (k->state == 0 && now >= k->t) {
            /* character as a real keyboard would produce it, with the scripted Shift/Ctrl state */
            BYTE ks[256] = {0}; int asc = 0;
            if (key_down[0x2A] || key_down[0x36]) ks[VK_SHIFT] = 0x80;
            if (key_down[0x1D] || key_down[0x11D]) ks[VK_CONTROL] = 0x80;
            UINT vk = MapVirtualKeyA((UINT)(k->scan & 0xFF), MAPVK_VSC_TO_VK);
            if (!(k->scan & 0x100) && vk) asc = key_ascii(vk, (UINT)(k->scan & 0xFF), ks);
            if (key_down[0x38] || key_down[0x138]) asc = 0;           /* Alt+key: scan code only */
            key_event(k->scan, 0, asc); k->state = 1;
        }
        else if (k->state == 1 && now >= k->t + k->hold) { key_event(k->scan, 1, 0); k->state = 2; }
    }
}
static void tick_check(void) {
    n_tick_checks++;
    if (in_irq) return;
    if (n_autokeys) autokeys_poll();
    if (getenv("POP2_TIMELOG")) {  /* debug: the game's remaining time, against real time */
        static DWORD lt; DWORD now = GetTickCount();
        if (now - lt > 1000) { lt = now; logmsg("TIME real=%lums game=%d min + %d ticks (flag 01d6=%d, kid alive 5c91=%d, hp=%d)\n",
            now - t_start, (int16_t)RW(0x31F3, 0x5E2A), (int16_t)RW(0x31F3, 0x5E44),
            (int8_t)MEM[LIN(0x31F3, 0x01D6)], (int8_t)MEM[LIN(0x31F3, 0x5C91)], MEM[LIN(0x31F3, 0x5C12)]);
            logmsg("     level a18=%d 6ccc=%d 5e00=%d\n", (int8_t)MEM[LIN(0x31F3, 0x0A18)],
                   (int8_t)MEM[LIN(0x31F3, 0x6CCC)], (int16_t)RW(0x31F3, 0x5E00)); }
    }
    if (getenv("POP2_CPLOG")) {   /* debug: checkpoint state once a second */
        static DWORD lt; DWORD now = GetTickCount();
        if (now - lt > 1000) { lt = now; logmsg("CP t=%lu level=%d room=%d cp_room=%d start_room=%d hp=%d\n", now - t_start,
            (int8_t)MEM[LIN(0x31F3, 0x0A18)], MEM[LIN(0x31F3, 0x5C0E)], MEM[LIN(0x31F3, 0x6CCD)], MEM[LIN(0x31F3, 0x5E34)], MEM[LIN(0x31F3, 0x5C12)]); }
    }
    /* Interrupts only reach the program while it has them enabled (STI), exactly as on a PC:
     * code that runs with interrupts off (CLI) must not be re-entered by a handler. */
    if (FI) { extern void snd_poll(void); in_irq = 1; snd_poll(); in_irq = 0; }
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    double dt = (double)(now.QuadPart - t_last_tick.QuadPart) / qpf.QuadPart;
    double period = 1.0 / pit_hz;
    int n = 0;
    if (FI) {
        while (dt >= period && n < 4) {
            t_last_tick.QuadPart += (LONGLONG)(period * qpf.QuadPart);
            dt -= period; n++;
            in_irq = 1; n_irq8++;
            raise_irq(8);
            in_irq = 0;
        }
        if (dt > 0.25) QueryPerformanceCounter(&t_last_tick);   /* don't try to catch up after stalls */
        /* keyboard IRQ */
        if (kq_head != kq_tail && !in_irq) {
            port60 = kq[kq_head++ & 255];
            in_irq = 1; raise_irq(9); in_irq = 0;
        }
    } else if (dt > 0.5) QueryPerformanceCounter(&t_last_tick); /* long CLI: drop the backlog */
    double pd = (double)(now.QuadPart - t_last_present.QuadPart) / qpf.QuadPart;
    if (pd > 1.0 / 70) { t_last_present = now; pump(); if (video_mode == 0x13) { present(); n_presents++; } }
}

int poll_ctr = 2000;
const char *volatile cur_fn = "start";
static DWORD WINAPI watchdog(LPVOID p) {
    (void)p;
    for (;;) { Sleep(1000); logmsg("[watch] ticks %ld irq8 %ld presents %ld in_irq %d mode %x\n", n_tick_checks, n_irq8, n_presents, in_irq, video_mode); logmsg("[watch] in %s  ax=%04x bx=%04x cx=%04x dx=%04x ds=%04x es=%04x ss:sp=%04x:%04x\n",
               cur_fn, AX, BX, CX, DX, DS, ES, SS, SP);
        if (getenv("POP2_WATCHSTACK")) {          /* the emulated stack: return addresses of the callers */
            for (int i = 0; i < 28; i += 2)
                logmsg("[watch]   sp+%02x: %04x %04x\n", i * 2, RW(SS, (uint16_t)(SP + i * 2)), RW(SS, (uint16_t)(SP + i * 2 + 2)));
        } }
    return 0;
}
static const char *ring[16]; static uint16_t ring_sp[16], ring_bp[16]; static unsigned n_ring;      /* recent functions (debug builds) */
/* the game's own "print an error and quit" routine: record the machine state that led there */
void rt_fatal_hook(void) {
    logmsg("GAME FATAL EXIT: ss:sp %04x:%04x bp %04x ds %04x - args:", SS, SP, BP, DS);
    for (int i = 0; i < 6; i++) logmsg(" %04x", RW(SS, (uint16_t)(SP + i * 2)));
    logmsg("\n");
    for (int i = 0; i < 16; i++) { const char *f = ring[(n_ring + i) & 15]; if (f) logmsg("      %s\n", f); }
}

#ifdef SPCHECK
static const char *watch_owner;      /* function whose saved-BP slot is currently watched */
/* a framed function whose BP differs at return from its value at entry: that broken frame
 * pointer is what later turns into a wild stack pointer */
uint32_t bp_watch;                   /* stack byte that got scribbled on; watched afterwards */
/* BP changing only in its high byte is the signature of this bug: report where it happens */
void rt_bp_track(const char *where) {
    static uint16_t prev; static int n, init;
    if (!init) { init = 1; prev = BP; return; }
    if (n < 8 && BP != prev && (BP & 0xFF) == (prev & 0xFF) && (uint16_t)(BP - prev) >= 0x1000) {
        n++;
        logmsg("BP HIGH BYTE CHANGED %04x -> %04x at %s (fn %s, ss:sp %04x:%04x ax %04x bx %04x)\n",
               prev, BP, where, cur_fn, SS, SP, AX, BX);
    }
    prev = BP;
}
void rt_bp_here(const char *fn, int off) {
    static uint16_t prev; static int n;
    if (prev && BP != prev && BP > 0x9600 && n < 6) {
        n++; logmsg("BP WENT WILD inside %s at %04x: %04x -> %04x (ss:sp %04x:%04x)\n", fn, off, prev, BP, SS, SP);
    }
    prev = BP;
}
void rt_watch_write(uint32_t a, uint16_t v) {
    static int n;
    extern uint8_t bp_watch_val;
    {   /* any write above the current frame scribbles on a caller's saved registers/returns */
        static int m;
        if (m < 20 && GetTickCount() - t_start > 8000 && SS == 0x31F3 && BP >= 0x2000 && BP <= 0x91E0
            && a > LIN(SS, (uint16_t)(BP + 6)) && a < LIN(SS, 0x91E0)) {
            m++;
            logmsg("WRITE ABOVE FRAME: %05x = %02x (ss:sp %04x:%04x bp %04x) in %s\n",
                   a, (uint8_t)v, SS, SP, BP, cur_fn);
        }
    }
    if (a != bp_watch || n >= 12) return;
    n++;
    logmsg("WATCHED BYTE WRITTEN: %05x = %02x by %s (ss:sp %04x:%04x bp %04x ds %04x es %04x di %04x si %04x)\n",
           a, (uint8_t)v, cur_fn, SS, SP, BP, DS, ES, DI, SI);
}
uint8_t bp_watch_val;
void rt_bp_broken(const char *fn, uint16_t expect) {
    static int n;
    if (!bp_watch) {                 /* the saved-BP slot of this frame: watch it from now on */
        bp_watch = LIN(SS, (uint16_t)(SP - 6)) + 1;
        bp_watch_val = (uint8_t)(expect >> 8);
        logmsg("watching stack byte %05x (should stay %02x)\n", bp_watch, bp_watch_val);
    }
    if (n >= 5) return;
    n++;
    logmsg("FRAME POINTER BROKEN in %s: bp %04x at return, was %04x at entry (sp %04x)\n", fn, BP, expect, SP);
    for (int i = 0; i < 16; i++) { const char *f = ring[(n_ring + i) & 15]; if (f) logmsg("      %s\n", f); }
}

void rt_retchk(const char *fn, uint16_t caller_bp, uint16_t entry_sp) {
    {   /* the stack must come back to where it started (plus the return address and any args) */
        static int n;
        int delta = (int16_t)(SP - entry_sp);
        if (n < 8 && (delta < 2 || delta > 0x40)) {
            n++;
            logmsg("STACK UNBALANCED in %s: sp %04x at entry, %04x at return (%+d), bp %04x\n",
                   fn, entry_sp, SP, delta, BP);
        }
    }
    if (BP != caller_bp) rt_bp_broken(fn, caller_bp);
}

/* debug build: every recompiled function reports in here, so a stack pointer that has left the
 * stack can be traced back to the last functions that ran */
void rt_bp_track(const char *where);
void rt_spchk(void) {
    rt_bp_track("function entry");
    unsigned n = n_ring;
    static int reported;
    {   /* where did the stack pointer move? report the function it happened inside */
        static uint16_t prev_sp; static const char *prev_fn = "?"; static int jumps;
        if (SS == 0x31F3 && prev_sp && (uint16_t)(SP - prev_sp) > 0x400 && (uint16_t)(SP - prev_sp) < 0x8000 && jumps < 6) {
            jumps++;
            logmsg("SP jumped %04x -> %04x inside %s (next: %s)\n", prev_sp, SP, prev_fn, cur_fn);
        }
        prev_sp = SP; prev_fn = cur_fn;
    }
    if (!bp_watch && getenv("POP2_WATCHADDR")) {     /* fixed address from the environment */
        bp_watch = LIN(0x31F3, (uint16_t)strtol(getenv("POP2_WATCHADDR"), NULL, 16));
        bp_watch_val = MEM[bp_watch];
        logmsg("watching fixed stack byte %05x (currently %02x)\n", bp_watch, bp_watch_val);
    }
    if (!getenv("POP2_WATCHADDR") && cur_fn && strstr(cur_fn, "1fe6_209c")) {
        bp_watch = LIN(SS, (uint16_t)(SP - 2)) + 1; bp_watch_val = (uint8_t)(BP >> 8); watch_owner = cur_fn;
    }
    {   /* watchpoint: who overwrites the high byte of a saved frame pointer? */
        static int hit;
        if (bp_watch && !hit && MEM[bp_watch] != bp_watch_val) {
            hit = 1;
            logmsg("WATCH HIT: %05x became %02x (was %02x) by the time %s was entered - history:\n",
                   bp_watch, MEM[bp_watch], bp_watch_val, cur_fn);
            for (int i = 0; i < 16; i++) { const char *f = ring[(n_ring + i) & 15]; if (f) logmsg("      %s\n", f); }
        }
    }
    {   /* long trace: every function entry, so the first bad frame pointer can be traced back */
        static struct { const char *fn; uint16_t sp, bp; } *tr; static unsigned tn; static int done;
        if (!tr) tr = malloc(sizeof *tr * 200000);
        if (tr && tn < 200000) { tr[tn].fn = cur_fn; tr[tn].sp = SP; tr[tn].bp = BP; tn++; }
        /* BP outside the data segment (DGROUP ends at 0x95F0) while the stack is still sane */
        /* only the game's own code: library helpers legitimately use BP as a scratch register */
        int game_fn = cur_fn && !strstr(cur_fn, "_1101_") && !strstr(cur_fn, "_1fe6_") && !strstr(cur_fn, "_1de8_")
                      && !strstr(cur_fn, "_1f61_") && !strstr(cur_fn, "_2203_") && !strstr(cur_fn, "_1f74_")
                      && !strstr(cur_fn, "f_fm_") && !strstr(cur_fn, "_1d76_") && !strstr(cur_fn, "_1de8_");
        if (!done && tr && game_fn && BP > 0x9700 && SP < 0x9600 && SS == 0x31F3) {
            done = 1;
            logmsg("FIRST BAD BP %04x (sp %04x) entering %s - preceding trace:\n", BP, SP, cur_fn);
            unsigned from = tn > 40 ? tn - 40 : 0;
            for (unsigned i = from; i < tn; i++) logmsg("      sp %04x bp %04x  %s\n", tr[i].sp, tr[i].bp, tr[i].fn);
        }
    }
    {   /* a 16-bit stack can never be odd: catch the instruction that made it so */
        static int odd_done;
        if ((SP & 1) && !odd_done) {
            odd_done = 1;
            logmsg("ODD SP %04x:%04x on entering %s - history:\n", SS, SP, cur_fn);
            for (int i = 0; i < 16; i++) { int k = (n + i) & 15; if (ring[k]) logmsg("      %s\n", ring[k]); }
        }
    }
    {   /* BP is the frame pointer: functions end with "mov sp,bp", so a wrong BP wrecks the stack */
        static uint16_t prev_bp; static const char *prev_fn = "?"; static int bad;
        if (SS == 0x31F3 && BP >= 0x9a00 && prev_bp >= 0x2000 && prev_bp <= 0x99ff && bad < 3) {
            bad++;
            logmsg("BP went bad %04x -> %04x inside %s (next: %s, sp %04x) - history:\n",
                   prev_bp, BP, prev_fn, cur_fn, SP);
            for (int i = 0; i < 16; i++) { int k = (n + i) & 15; if (ring[k]) logmsg("      %s\n", ring[k]); }
        }
        prev_bp = BP; prev_fn = cur_fn;
    }
    ring_sp[n & 15] = SP; ring_bp[n & 15] = BP;
    ring[n & 15] = cur_fn; n_ring = ++n;
    if (reported >= 3 || SS != 0x31F3 || (SP > 0x2000 && SP < 0x9800)) return;
    reported++;
    logmsg("BAD SP %04x:%04x - last functions entered (sp, bp at entry):\n", SS, SP);
    for (int i = 0; i < 16; i++) {
        int k = (n + i) & 15;
        if (ring[k]) logmsg("    sp %04x bp %04x  %s\n", ring_sp[k], ring_bp[k], ring[k]);
    }
}
#endif

void rt_poll(void) {
    poll_ctr = 2000;
#ifdef SPCHECK
    { void rt_bp_track(const char *); rt_bp_track("loop"); }
    {   /* the watched stack byte, checked at every loop back-edge */
        extern uint32_t bp_watch; extern uint8_t bp_watch_val;
        static int hit;
        if (bp_watch && !hit && MEM[bp_watch] != bp_watch_val) {
            hit = 1;
            logmsg("WATCH HIT (in loop): %05x became %02x in %s\n", bp_watch, MEM[bp_watch], cur_fn);
        }
    }
#endif
    if (getenv("POP2_SPLOG")) {     /* debug: report which code is eating the game's stack */
        static uint16_t low = 0xFFFF; static int n;
        static uint16_t prev; static int init;
        if (!init) { init = 1; prev = SP; }
        if ((uint16_t)(SP - prev) > 0x400 && (uint16_t)(prev - SP) > 0x400 && n < 60) {
            n++; logmsg("SP JUMP %04x -> %04x (%+d) in %s\n", prev, SP, (int16_t)(SP - prev), cur_fn);
        }
        prev = SP;
        if (SP < low - 0x200 && n < 60) { low = SP; n++; logmsg("SP falling: %04x:%04x in %s\n", SS, SP, cur_fn); }
    }
    tick_check();
}

/* ---------------------------------------------------------------- ports */
static int vsync_phase;
/* PIT channel 2 (used by the FM driver's OPL detection as a delay timer): 1.193182 MHz down-counter */
static uint32_t pit2_reload = 0x10000; static LARGE_INTEGER pit2_t0; static int pit2_wr_hi, pit2_rd_hi, pit2_latched; static uint16_t pit2_latch;
static uint16_t pit2_count(void) {
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    uint64_t ticks = (uint64_t)((double)(now.QuadPart - pit2_t0.QuadPart) * 1193182.0 / (double)qpf.QuadPart);
    return (uint16_t)(pit2_reload - ticks % pit2_reload);
}
uint8_t port_in8(uint16_t port) {
    tick_check();
    switch (port) {
    case 0x60: return port60;
    case 0x61: return 0;
    case 0x64: return (kq_head != kq_tail) ? 1 : 0;
    case 0x3DA: { vsync_phase = (vsync_phase + 1) & 7; if (vsync_phase == 0) { Sleep(0); } return (vsync_phase < 2) ? 0x09 : 0x00; }
    case 0x3C9: { uint8_t v = pal[pal_read_idx][pal_read_comp]; if (++pal_read_comp == 3) { pal_read_comp = 0; pal_read_idx = (pal_read_idx + 1) & 255; } return v; }
    case 0x40: return 0;
    case 0x42: {
        uint16_t c = pit2_latched ? pit2_latch : pit2_count();
        uint8_t v = pit2_rd_hi ? (uint8_t)(c >> 8) : (uint8_t)c;
        if (pit2_rd_hi) pit2_latched = 0;
        pit2_rd_hi ^= 1;
        return v; }
    case 0x21: return 0;
    case 0x201: return 0xFF;           /* joystick: none */
    case 0x388: case 0x389: return fm_port_read();   /* OPL2 status */
    }
    return 0xFF;
}
uint16_t port_in16(uint16_t port) { return (uint16_t)(port_in8(port) | (port_in8((uint16_t)(port + 1)) << 8)); }
void port_out8(uint16_t port, uint8_t v) {
    switch (port) {
    case 0x3C8: pal_idx = v; pal_comp = 0; return;
    case 0x3C7: pal_read_idx = v; pal_read_comp = 0; return;
    case 0x3C9: pal[pal_idx][pal_comp] = v & 63; if (++pal_comp == 3) { pal_comp = 0; pal_idx = (pal_idx + 1) & 255; } return;
    case 0x43:
        if ((v >> 6) == 2) {
            if (((v >> 4) & 3) == 0) { pit2_latch = pit2_count(); pit2_latched = 1; pit2_rd_hi = 0; }
            else { pit2_wr_hi = 0; pit2_rd_hi = 0; }
            return;
        }
        pit_write_hi = 0; return;
    case 0x42:
        if (!pit2_wr_hi) { pit2_reload = (pit2_reload & 0xFF00) | v; pit2_wr_hi = 1; }
        else { pit2_reload = (pit2_reload & 0xFF) | ((uint32_t)v << 8); if (!pit2_reload) pit2_reload = 0x10000; pit2_wr_hi = 0; QueryPerformanceCounter(&pit2_t0); }
        return;
    case 0x40:
        if (!pit_write_hi) { pit_latch_lo = v; pit_write_hi = 1; }
        else { pit_reload = (uint16_t)(pit_latch_lo | (v << 8)); pit_write_hi = 0;
               pit_hz = 1193182.0 / (pit_reload ? pit_reload : 65536); logmsg("PIT rate %.1f Hz\n", pit_hz); }
        return;
    case 0x20: case 0x21: case 0x61: return;
    case 0x388: case 0x389: fm_port_write(port, v); return;   /* OPL2 index / data */
    }
}
void port_out16(uint16_t port, uint16_t v) { port_out8(port, (uint8_t)v); port_out8((uint16_t)(port + 1), (uint8_t)(v >> 8)); }

/* ---------------------------------------------------------------- DOS */
static char game_dir[MAX_PATH];
typedef struct { FILE *f; char name[260]; } DosFile;
static DosFile files[64];
/* The port emulates one sound card - a Sound Blaster Pro: FM music through the OPL2 emulator and
 * digital effects through waveOut.  What the game sends depends on what its own DOS SETUP wrote:
 * CONFIG.DAT picks the music data (an FM patch bank the game uploads to the driver, then patch
 * numbers), and MIDI.DRV / DIGI.DRV are whichever drivers SETUP copied.  A copy of the game set
 * up for anything else therefore sends data its driver never expected, and the music comes out as
 * a mess.  So serve our own: both drivers ship with every copy, and only the three sound fields
 * of CONFIG.DAT are touched, in a private copy that leaves the player's file alone.
 * pop2.ini Sound=keep uses the game's own setup instead. */
static const char *sound_sub(const char *name) {
    static char cfg[MAX_PATH]; static int cfg_done;
    if (cfg_sound_keep) return NULL;
    if (_stricmp(name, "MIDI.DRV") == 0) return "msb_pro.drv";
    if (_stricmp(name, "DIGI.DRV") == 0) return "dsb_pro.drv";
    if (_stricmp(name, "CONFIG.DAT") != 0) return NULL;
    if (!cfg_done) {
        cfg_done = 1;
        char src[MAX_PATH]; snprintf(src, MAX_PATH, "%s\\CONFIG.DAT", game_dir);
        uint8_t b[64]; size_t n = 0;
        FILE *f = fopen(src, "rb");
        if (f) { n = fread(b, 1, sizeof b, f); fclose(f); }
        if (n == 32) {
            b[4] = 1; b[5] = 0;            /* digital sound: Sound Blaster */
            b[6] = 1; b[7] = 0;            /* music: FM (AdLib / Sound Blaster) */
            b[8] = 0x21; b[9] = 0;         /* both enabled */
            char exe[MAX_PATH]; GetModuleFileNameA(NULL, exe, MAX_PATH);
            char *s = strrchr(exe, '\\'); if (s) *s = 0;
            snprintf(cfg, MAX_PATH, "%s\\pop2_config.dat", exe);
            FILE *g = fopen(cfg, "wb");
            if (!g) {                      /* installed somewhere unwritable: keep it in TEMP */
                char tmp[MAX_PATH];
                if (GetTempPathA(MAX_PATH, tmp)) { snprintf(cfg, MAX_PATH, "%spop2_config.dat", tmp); g = fopen(cfg, "wb"); }
            }
            if (g) { fwrite(b, 1, n, g); fclose(g); } else cfg[0] = 0;
        }
    }
    return cfg[0] ? cfg : NULL;
}
static void host_path(uint16_t seg, uint16_t off, char *out) {
    char dos[260]; int i = 0;
    while (i < 255 && RB(seg, (uint16_t)(off + i))) { dos[i] = (char)RB(seg, (uint16_t)(off + i)); i++; }
    dos[i] = 0;
    const char *p = dos;
    if (p[0] && p[1] == ':') p += 2;
    const char *last = p;                       /* keep only the file name: everything lives in the game dir */
    for (const char *q = p; *q; q++) if (*q == '\\' || *q == '/') last = q + 1;
    snprintf(out, MAX_PATH, "%s\\%s", game_dir, last);
    {   /* always present the sound hardware the port emulates, whatever this copy was set up for */
        const char *sub = sound_sub(last);
        if (sub) {
            char cand[MAX_PATH];
            if (strchr(sub, '\\')) snprintf(cand, MAX_PATH, "%s", sub);
            else snprintf(cand, MAX_PATH, "%s\\%s", game_dir, sub);
            if (GetFileAttributesA(cand) != INVALID_FILE_ATTRIBUTES) {
                static int said[3];
                int k = (_stricmp(last, "MIDI.DRV") == 0) ? 0 : (_stricmp(last, "DIGI.DRV") == 0 ? 1 : 2);
                if (!said[k]++) logmsg("sound: %s served from %s\n", last, cand);
                strcpy(out, cand);
            }
        }
    }
    /* scripted test runs must never touch the player's saved game: use a separate file */
    size_t n = strlen(last);
    if (getenv("POP2_KEYS") && n > 4 && _stricmp(last + n - 4, ".SAV") == 0) {
        char exe[MAX_PATH]; GetModuleFileNameA(NULL, exe, MAX_PATH);
        char *s = strrchr(exe, '\\'); if (s) *s = 0;
        snprintf(out, MAX_PATH, "%s\\test_%s", exe, last);
    }
}
/* conventional memory allocator (DOS 48h/49h/4Ah) */
typedef struct { uint16_t seg, paras; int used; } Blk;
static Blk blks[256]; static int nblk;
static void mem_init(uint16_t first_free) {
    nblk = 2;
    blks[0].seg = PSPSEG; blks[0].paras = (uint16_t)(first_free - PSPSEG); blks[0].used = 1;
    blks[1].seg = first_free; blks[1].paras = (uint16_t)(MEMTOP - first_free); blks[1].used = 0;
}
static int dos_alloc(uint16_t paras, uint16_t *seg, uint16_t *largest) {
    uint16_t big = 0;
    for (int i = 0; i < nblk; i++) {
        if (blks[i].used) continue;
        if (blks[i].paras >= paras) {
            if (blks[i].paras > paras) {
                memmove(&blks[i + 2], &blks[i + 1], sizeof(Blk) * (nblk - i - 1)); nblk++;
                blks[i + 1].seg = (uint16_t)(blks[i].seg + paras); blks[i + 1].paras = (uint16_t)(blks[i].paras - paras); blks[i + 1].used = 0;
                blks[i].paras = paras;
            }
            blks[i].used = 1; *seg = blks[i].seg; return 1;
        }
        if (blks[i].paras > big) big = blks[i].paras;
    }
    *largest = big; return 0;
}
static void dos_free(uint16_t seg) {
    for (int i = 0; i < nblk; i++) if (blks[i].seg == seg) { blks[i].used = 0; break; }
    for (int i = 0; i + 1 < nblk; ) {
        if (!blks[i].used && !blks[i + 1].used) { blks[i].paras = (uint16_t)(blks[i].paras + blks[i + 1].paras); memmove(&blks[i + 1], &blks[i + 2], sizeof(Blk) * (nblk - i - 2)); nblk--; }
        else i++;
    }
}
static int dos_resize(uint16_t seg, uint16_t paras, uint16_t *maxp) {
    for (int i = 0; i < nblk; i++) if (blks[i].seg == seg) {
        uint16_t avail = blks[i].paras;
        if (i + 1 < nblk && !blks[i + 1].used) avail = (uint16_t)(avail + blks[i + 1].paras);
        if (paras > avail) { *maxp = avail; return 0; }
        uint16_t total = avail;
        if (i + 1 < nblk && !blks[i + 1].used) { memmove(&blks[i + 1], &blks[i + 2], sizeof(Blk) * (nblk - i - 2)); nblk--; }
        blks[i].paras = paras;
        if (total > paras) { memmove(&blks[i + 2], &blks[i + 1], sizeof(Blk) * (nblk - i - 1)); nblk++;
            blks[i + 1].seg = (uint16_t)(seg + paras); blks[i + 1].paras = (uint16_t)(total - paras); blks[i + 1].used = 0; }
        return 1;
    }
    *maxp = 0; return 0;
}
static void set_cf(int c) { FC = (uint8_t)(c != 0); }
static uint16_t dta_seg = PSPSEG, dta_off = 0x80;

/* text the program writes to the screen (DOS console output): the game prints its error
 * messages this way, so keep them in the log */
static void dos_out(char c) {
    static char line[256]; static int n;
    if (c == '\r') return;
    if (c == '\n' || n >= (int)sizeof line - 1) { line[n] = 0; if (n) logmsg("DOS output: %s\n", line); n = 0; return; }
    line[n++] = c;
}
static void dos21(void) {
    uint8_t ah = AH;
    switch (ah) {
    case 0x02: dos_out((char)DL); return;                       /* print char */
    case 0x06: case 0x07: case 0x08: case 0x01: {
        /* keyboard input.  Like DOS: a key without ASCII code (Alt+letter, F-keys, arrows) returns 0,
         * and the next call returns its scan code. */
        static int ext_pending; static uint8_t ext_scan;
        if (ah == 0x06 && DL != 0xFF) { dos_out((char)DL); return; }   /* 06 with DL != FF is character output */
        if (getenv("POP2_KEYLOG")) { static DWORD lt; static int n; n++; if (GetTickCount() - lt > 1000) { logmsg("DOS key polls: %d\n", n); n = 0; lt = GetTickCount(); } }
        if (ah != 0x06) while (!ext_pending && bk_head == bk_tail) { Sleep(5); tick_check(); pump(); }
        if (ext_pending) { AL = ext_scan; ext_pending = 0; FZ = 0; return; }
        if (bk_head == bk_tail) { AL = 0; FZ = 1; return; }
        uint16_t k = bios_kbuf[bk_head++ & 63];
        AL = (uint8_t)k; FZ = 0;
        if (getenv("POP2_KEYLOG")) logmsg("DOS key %04x (fn %02x)\n", k, ah);
        if (AL == 0) { ext_pending = 1; ext_scan = (uint8_t)(k >> 8); }
        return;
    }
    case 0x09: { char s[256]; int i = 0; while (i < 255 && RB(DS, (uint16_t)(DX + i)) != '$') { s[i] = (char)RB(DS, (uint16_t)(DX + i)); i++; } s[i] = 0; logmsg("DOS print: %s\n", s); return; }
    case 0x0B: AL = (bk_head != bk_tail) ? 0xFF : 0; return;
    case 0x0C: if (getenv("POP2_KEYLOG") && bk_head != bk_tail) logmsg("DOS flush drops %d keys\n", bk_tail - bk_head);
        bk_head = bk_tail;                                      /* flush buffer, then run function AL */
        if (AL == 0x06 || AL == 0x07 || AL == 0x08 || AL == 0x01 || AL == 0x0A) { AH = AL; dos21(); }
        return;
    case 0x0E: AL = 26; return;                                 /* select drive */
    case 0x19: AL = 2; return;                                  /* current drive C: */
    case 0x1A: dta_seg = DS; dta_off = DX; return;
    case 0x25: WW(0, AL * 4, DX); WW(0, AL * 4 + 2, DS); logmsg("set vector %02x -> %04x:%04x\n", AL, DS, DX); return;
    case 0x2A: { SYSTEMTIME t; GetLocalTime(&t); CX = t.wYear; DH = (uint8_t)t.wMonth; DL = (uint8_t)t.wDay; AL = (uint8_t)t.wDayOfWeek; return; }
    case 0x2C: { SYSTEMTIME t; GetLocalTime(&t); CH = (uint8_t)t.wHour; CL = (uint8_t)t.wMinute; DH = (uint8_t)t.wSecond; DL = (uint8_t)(t.wMilliseconds / 10); return; }
    case 0x2F: ES = dta_seg; BX = dta_off; return;
    case 0x30: AX = 0x0005; BX = 0; CX = 0; return;             /* DOS 5.0 */
    case 0x33: DL = 0; return;
    case 0x35: BX = RW(0, AL * 4); ES = RW(0, AL * 4 + 2); return;
    case 0x36: AX = 64; BX = 30000; CX = 512; DX = 60000; return; /* disk free */
    case 0x3B: set_cf(0); return;                               /* chdir */
    case 0x3C: case 0x3D: {
        char p[MAX_PATH]; host_path(DS, DX, p);
        const char *mode = (ah == 0x3C) ? "w+b" : ((AL & 3) == 0 ? "rb" : "r+b");
        FILE *f = fopen(p, mode);
        if (!f && ah == 0x3D && (AL & 3)) f = fopen(p, "rb");
        if (!f) { logmsg("open failed: %s\n", p); AX = 2; set_cf(1); return; }
        int h = 5; while (h < 64 && files[h].f) h++;
        files[h].f = f; strncpy(files[h].name, p, 259);
        logmsg("open %s -> %d\n", p, h);
        AX = (uint16_t)h; set_cf(0); return; }
    case 0x3E: if (BX < 64 && files[BX].f) { fclose(files[BX].f); files[BX].f = NULL; } set_cf(0); return;
    case 0x3F: {
        if (BX == 0) { AX = 0; set_cf(0); return; }
        if (BX >= 64 || !files[BX].f) { AX = 6; set_cf(1); return; }
        uint32_t dst = LIN(DS, DX); long pos = ftell(files[BX].f);
        {   /* does this read land on the program's stack?  That would wreck saved frames. */
            /* a read landing above the stack pointer overwrites live frames (saved BP, returns) */
            uint32_t live_lo = LIN(SS, SP), live_hi = LIN(SS, 0x9600);
            if (getenv("POP2_MEMLOG") && dst + CX > live_lo && dst < live_hi)
                logmsg("READ OVER LIVE STACK: %u bytes to %04x:%04x (%05x..%05x), ss:sp %04x:%04x bp %04x, in %s\n",
                       CX, DS, DX, dst, dst + CX, SS, SP, BP, cur_fn);
        }
#ifdef SPCHECK
        { extern uint32_t bp_watch; if (bp_watch && dst <= bp_watch && bp_watch < dst + CX)
            logmsg("WATCHED BYTE hit by FILE READ: %u bytes to %05x in %s\n", CX, dst, cur_fn); }
#endif
        size_t n = fread(MEM + dst, 1, CX, files[BX].f);
        note_code_read(dst, (uint32_t)pos, (uint32_t)n);
        note_driver_read(files[BX].name, dst, (uint32_t)pos);
        AX = (uint16_t)n; set_cf(0); return; }
    case 0x40: {
        if (BX == 1 || BX == 2) { for (uint16_t i = 0; i < CX; i++) dos_out((char)RB(DS, (uint16_t)(DX + i))); AX = CX; set_cf(0); return; }
        if (BX >= 64 || !files[BX].f) { AX = 6; set_cf(1); return; }
        size_t n = fwrite(MEM + LIN(DS, DX), 1, CX, files[BX].f); AX = (uint16_t)n; set_cf(0); return; }
    case 0x41: { char p[MAX_PATH]; host_path(DS, DX, p); remove(p); set_cf(0); return; }
    case 0x42: {
        if (BX >= 64 || !files[BX].f) { AX = 6; set_cf(1); return; }
        long off = (long)(((uint32_t)CX << 16) | DX);
        fseek(files[BX].f, off, AL == 0 ? SEEK_SET : AL == 1 ? SEEK_CUR : SEEK_END);
        long p = ftell(files[BX].f); AX = (uint16_t)p; DX = (uint16_t)(p >> 16); set_cf(0); return; }
    case 0x43: set_cf(0); CX = 0; return;
    case 0x44: if (AL == 0) { DX = (BX < 5) ? 0x80D3 : 0x0002; set_cf(0); return; } set_cf(1); AX = 1; return;
    case 0x47: WB(DS, SI, 0); set_cf(0); return;                /* cwd = root */
    case 0x48: { uint16_t s, big = 0; if (dos_alloc(BX, &s, &big)) { AX = s; set_cf(0); }
        else { logmsg("DOS alloc FAILED: wanted %u paragraphs (%u KB), largest free %u (%u KB), in %s\n",
                      BX, BX / 64, big, big / 64, cur_fn); AX = 8; BX = big; set_cf(1); } return; }
    case 0x49: dos_free(ES); set_cf(0); return;
    case 0x4A: { uint16_t mx = 0; if (dos_resize(ES, BX, &mx)) set_cf(0); else { AX = 8; BX = mx; set_cf(1); } return; }
    case 0x4C:
        logmsg("program exit code %d (last function %s, ss:sp=%04x:%04x)\n", AL, cur_fn, SS, SP);
        for (int i = 0; i < 24; i++) logmsg("   stack +%02x: %04x\n", i * 2, RW(SS, (uint16_t)(SP + i * 2)));
        exit(AL);
    case 0x4E: case 0x4F: AX = 18; set_cf(1); return;           /* find first/next: none */
    case 0x58: AX = 0; set_cf(0); return;
    case 0x62: BX = PSPSEG; return;
    }
    logmsg("unhandled INT 21h AH=%02x AL=%02x\n", AH, AL);
    set_cf(1);
}

/* fake BIOS handlers (vectors point at F000:00nn): called for IRET chains / far calls to old vectors */
static void bios_int(int n);
/* XMS 3.0 driver (extended memory blocks backed by host memory) */
#define XMS_HANDLES 64
static uint8_t *xms_blk[XMS_HANDLES]; static uint32_t xms_size[XMS_HANDLES]; static int xms_lock[XMS_HANDLES];
static uint32_t xms_total_kb = 15 * 1024;
static uint32_t xms_used_kb(void) { uint32_t s = 0; for (int i = 1; i < XMS_HANDLES; i++) s += xms_size[i] / 1024; return s; }
static void xms_call(void) {
    switch (AH) {
    case 0x00: AX = 0x0300; BX = 0x0300; DX = 0; return;                       /* version 3.0, no HMA */
    case 0x08: { uint32_t f = xms_total_kb - xms_used_kb(); AX = (uint16_t)(f > 0xFFFF ? 0xFFFF : f); DX = AX; BL = 0; return; }
    case 0x09: {                                                                /* allocate DX KB */
        for (int h = 1; h < XMS_HANDLES; h++) if (!xms_blk[h]) {
            if (DX > xms_total_kb - xms_used_kb()) break;
            xms_size[h] = (uint32_t)DX * 1024; xms_blk[h] = calloc(1, xms_size[h] ? xms_size[h] : 1);
            AX = 1; DX = (uint16_t)h; BL = 0; return; }
        AX = 0; BL = 0xA0; return; }
    case 0x0A: if (DX && DX < XMS_HANDLES && xms_blk[DX]) { free(xms_blk[DX]); xms_blk[DX] = NULL; xms_size[DX] = 0; AX = 1; } else { AX = 0; BL = 0xA2; } return;
    case 0x0B: {                                                                /* move: DS:SI -> descriptor */
        uint32_t len = RW(DS, SI) | ((uint32_t)RW(DS, (uint16_t)(SI + 2)) << 16);
        uint16_t sh = RW(DS, (uint16_t)(SI + 4)); uint32_t so = RW(DS, (uint16_t)(SI + 6)) | ((uint32_t)RW(DS, (uint16_t)(SI + 8)) << 16);
        uint16_t dh = RW(DS, (uint16_t)(SI + 10)); uint32_t dof = RW(DS, (uint16_t)(SI + 12)) | ((uint32_t)RW(DS, (uint16_t)(SI + 14)) << 16);
        uint8_t *src = sh ? (xms_blk[sh] ? xms_blk[sh] + so : NULL) : MEM + LIN(so >> 16, so & 0xFFFF);
        uint8_t *dst = dh ? (xms_blk[dh] ? xms_blk[dh] + dof : NULL) : MEM + LIN(dof >> 16, dof & 0xFFFF);
        if (!src || !dst || (sh && so + len > xms_size[sh]) || (dh && dof + len > xms_size[dh])) { AX = 0; BL = 0xA7; return; }
#ifdef SPCHECK
        { extern uint32_t bp_watch; uint32_t d0 = (uint32_t)(dst - MEM);
          if (getenv("POP2_MEMLOG") && !dh && d0 < LIN(SS, 0x9600) && d0 + len > LIN(SS, SP))
            logmsg("XMS MOVE INTO LIVE STACK: %u bytes, src handle %u off %u, dst %04x:%04x (%05x), "
                   "ss:sp %04x:%04x bp %04x, in %s\n", (unsigned)len, sh, (unsigned)so,
                   (uint16_t)(dof >> 16), (uint16_t)dof, d0, SS, SP, BP, cur_fn);
          if (bp_watch && !dh && d0 <= bp_watch && bp_watch < d0 + len)
            logmsg("WATCHED BYTE hit by XMS MOVE: %u bytes to %05x in %s\n", (unsigned)len, d0, cur_fn); }
#endif
        memmove(dst, src, len); AX = 1; BL = 0; return; }
    case 0x0C: if (DX < XMS_HANDLES && xms_blk[DX]) { xms_lock[DX]++; AX = 1; DX = 0x0010; BX = 0; } else { AX = 0; BL = 0xA2; } return;
    case 0x0D: if (DX < XMS_HANDLES && xms_blk[DX]) { if (xms_lock[DX]) xms_lock[DX]--; AX = 1; } else { AX = 0; BL = 0xA2; } return;
    case 0x0E: if (DX < XMS_HANDLES && xms_blk[DX]) { AX = 1; BH = (uint8_t)xms_lock[DX]; BL = 32; DX = (uint16_t)(xms_size[DX] / 1024); } else { AX = 0; BL = 0xA2; } return;
    case 0x0F: if (DX < XMS_HANDLES && xms_blk[DX]) { uint32_t n = (uint32_t)BX * 1024; uint8_t *p = realloc(xms_blk[DX], n ? n : 1);
                   if (p) { if (n > xms_size[DX]) memset(p + xms_size[DX], 0, n - xms_size[DX]); xms_blk[DX] = p; xms_size[DX] = n; AX = 1; } else { AX = 0; BL = 0xA0; } }
               else { AX = 0; BL = 0xA2; } return;
    case 0x03: case 0x04: case 0x05: case 0x06: AX = 1; BL = 0; return;        /* A20 */
    case 0x07: AX = 1; BL = 0; return;
    case 0x10: AX = 0; BL = 0xB1; DX = 0; return;                               /* no UMBs */
    }
    logmsg("unhandled XMS AH=%02x\n", AH); AX = 0; BL = 0x80;
}

/* checkpoint snapshot of the runtime-side machine state (DOS memory, XMS, open files, timer) */
static struct {
    Blk blks[256]; int nblk;
    uint8_t *xms[XMS_HANDLES]; uint32_t xms_size[XMS_HANDLES]; int xms_lock[XMS_HANDLES];
    int open[64]; char name[64][260]; long pos[64];
    uint16_t dta_seg, dta_off; double pit_hz; uint16_t pit_reload;
} cpx;
static void cp_save_ext(void) {
    memcpy(cpx.blks, blks, sizeof blks); cpx.nblk = nblk;
    for (int i = 0; i < XMS_HANDLES; i++) {
        free(cpx.xms[i]); cpx.xms[i] = NULL;
        cpx.xms_size[i] = xms_size[i]; cpx.xms_lock[i] = xms_lock[i];
        if (xms_blk[i] && xms_size[i]) { cpx.xms[i] = malloc(xms_size[i]); memcpy(cpx.xms[i], xms_blk[i], xms_size[i]); }
    }
    for (int i = 0; i < 64; i++) {
        cpx.open[i] = files[i].f != NULL;
        if (cpx.open[i]) { strcpy(cpx.name[i], files[i].name); cpx.pos[i] = ftell(files[i].f); }
    }
    cpx.dta_seg = dta_seg; cpx.dta_off = dta_off; cpx.pit_hz = pit_hz; cpx.pit_reload = pit_reload;
}
static void cp_restore_ext(void) {
    memcpy(blks, cpx.blks, sizeof blks); nblk = cpx.nblk;
    for (int i = 0; i < XMS_HANDLES; i++) {
        if (cpx.xms[i]) {
            if (xms_size[i] != cpx.xms_size[i] || !xms_blk[i]) { free(xms_blk[i]); xms_blk[i] = malloc(cpx.xms_size[i]); }
            memcpy(xms_blk[i], cpx.xms[i], cpx.xms_size[i]);
        } else if (xms_blk[i] && !cpx.xms_size[i]) { free(xms_blk[i]); xms_blk[i] = NULL; }
        xms_size[i] = cpx.xms_size[i]; xms_lock[i] = cpx.xms_lock[i];
    }
    for (int i = 0; i < 64; i++) {
        if (files[i].f && (!cpx.open[i] || strcmp(files[i].name, cpx.name[i]) != 0)) { fclose(files[i].f); files[i].f = NULL; }
        if (cpx.open[i]) {
            if (!files[i].f) { files[i].f = fopen(cpx.name[i], "r+b"); if (!files[i].f) files[i].f = fopen(cpx.name[i], "rb"); strcpy(files[i].name, cpx.name[i]); }
            if (files[i].f) fseek(files[i].f, cpx.pos[i], SEEK_SET);
        }
    }
    dta_seg = cpx.dta_seg; dta_off = cpx.dta_off; pit_hz = cpx.pit_hz; pit_reload = cpx.pit_reload;
}

static void bios_call(uint16_t off) {
    if (off == 0x0100) { xms_call(); POP(); POP(); return; }   /* XMS entry: called with CALL FAR */
    /* reached via "pushf; call far [oldvec]" or a jump: behave like the BIOS ISR then IRET */
    bios_int(off & 0xFF);
    POP(); POP(); set_flags(POP());
}
static void bios_int(int n) {
    switch (n) {
    case 0x08: { uint32_t t = RW(0x40, 0x6C) | ((uint32_t)RW(0x40, 0x6E) << 16); t++; WW(0x40, 0x6C, (uint16_t)t); WW(0x40, 0x6E, (uint16_t)(t >> 16)); return; }
    case 0x09: return;
    }
}

void do_int(int n, uint16_t cs, uint16_t ip) {
    (void)cs; (void)ip;
    tick_check();
    /* program-installed handler for software interrupts (other than DOS/BIOS services we provide) */
    uint16_t hseg = RW(0, n * 4), hoff = RW(0, n * 4 + 2) ? RW(0, n * 4) : 0;
    (void)hoff;
    switch (n) {
    case 0x21: dos21(); return;
    case 0x10:
        if (AH == 0x00) { video_mode = AL & 0x7F; logmsg("video mode %02x\n", AL); if (video_mode == 0x13) memset(MEM + 0xA0000, 0, 64000); return; }
        if (AH == 0x0F) { AL = (uint8_t)video_mode; AH = 40; BH = 0; return; }
        if (AH == 0x10 && AL == 0x12) { for (int i = 0; i < CX; i++) for (int c = 0; c < 3; c++) pal[(BX + i) & 255][c] = RB(ES, (uint16_t)(DX + i * 3 + c)) & 63; return; }
        if (AH == 0x10 && AL == 0x17) { for (int i = 0; i < CX; i++) for (int c = 0; c < 3; c++) WB(ES, (uint16_t)(DX + i * 3 + c), pal[(BX + i) & 255][c]); return; }
        if (AH == 0x1A) { AL = 0x1A; BX = 0x0008; return; }     /* VGA present */
        if (AH == 0x12) { BH = 0; BL = 3; return; }
        { static int n10; if (n10++ < 40) logmsg("int10 unhandled ax=%04x bx=%04x cx=%04x dx=%04x\n", AX, BX, CX, DX); }
        return;
    case 0x16:
        if (AH == 0x00 || AH == 0x10) { while (bk_head == bk_tail) { Sleep(5); tick_check(); } AX = bios_kbuf[bk_head++ & 63]; return; }
        if (AH == 0x01 || AH == 0x11) { if (bk_head != bk_tail) { AX = bios_kbuf[bk_head & 63]; FZ = 0; } else FZ = 1; return; }
        if (AH == 0x02) { AL = 0; return; }
        return;
    case 0x1A: if (AH == 0) { uint32_t t = RW(0x40, 0x6C) | ((uint32_t)RW(0x40, 0x6E) << 16); CX = (uint16_t)(t >> 16); DX = (uint16_t)t; AL = 0; } return;
    case 0x33: AX = 0; return;                                  /* no mouse */
    case 0x67: AH = 0x80; return;                               /* EMS: failure */
    case 0x2F:                                                  /* XMS driver present */
        if (AX == 0x4300) { AL = 0x80; return; }
        if (AX == 0x4310) { ES = BIOSSEG; BX = 0x0100; return; } /* XMS entry point: F000:0100 */
        return;
    case 0x11: AX = 0x4426; return;                             /* equipment list */
    case 0x12: AX = 640; return;                                /* conventional memory KB */
    case 0x15: FC = 1; AH = 0x86; return;
    case 0x05: return;
    }
    if (hseg != BIOSSEG && hseg != 0) {
        /* program-installed software interrupt handler */
        PUSH(get_flags()); PUSH(cs); PUSH(ip);
        call_far(hseg, RW(0, n * 4));
        return;
    }
    logmsg("unhandled INT %02x AX=%04x\n", n, AX);
}

/* ---------------------------------------------------------------- program start */
extern const SpaceTab space_tabs[];
void mem_write_hook(uint32_t a) { (void)a; }

/* a crash in the recompiled code would otherwise close the window with no message at all */
static LONG WINAPI crash_handler(EXCEPTION_POINTERS *ep) {
    char buf[512];
    snprintf(buf, sizeof buf,
             "The game stopped unexpectedly.\n\nException %08lX at %p\nGame code: %s\n"
             "cs:ip %04x:%04x  ss:sp %04x:%04x  ds=%04x es=%04x\nax=%04x bx=%04x cx=%04x dx=%04x si=%04x di=%04x bp=%04x\n\n"
             "Details were written to pop2native.log next to the game.",
             ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress, cur_fn,
             0, 0, SS, SP, DS, ES, AX, BX, CX, DX, SI, DI, BP);
    logmsg("CRASH: %s\n", buf);
    if (!getenv("POP2_KEYS")) MessageBoxA(NULL, buf, "PoP2 native - crash", MB_OK | MB_ICONERROR);
    return EXCEPTION_EXECUTE_HANDLER;
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR cmd, int show) {
    SetUnhandledExceptionFilter(crash_handler);
    (void)hi; (void)hp; (void)cmd; (void)show;
    QueryPerformanceFrequency(&qpf); QueryPerformanceCounter(&t_last_tick); t_last_present = t_last_tick;
    load_settings();
    /* game directory: pop2.ini GameDir, else next to the exe, else D:\Dos\prince 2.pc */
    char test[MAX_PATH];
    if (cfg_game_dir[0]) strcpy(game_dir, cfg_game_dir);
    else {
        GetModuleFileNameA(NULL, game_dir, MAX_PATH);
        char *s = strrchr(game_dir, '\\'); if (s) *s = 0;
        snprintf(test, MAX_PATH, "%s\\PRINCE.EXE", game_dir);
        if (GetFileAttributesA(test) == INVALID_FILE_ATTRIBUTES) strcpy(game_dir, "D:\\Dos\\prince 2.pc");
    }
    snprintf(test, MAX_PATH, "%s\\PRINCE.EXE", game_dir);
    FILE *ef = fopen(test, "rb");
    if (!ef) fatal("PRINCE.EXE not found in %s\n\nRun the PoP2 Launcher and choose the game folder.", game_dir);
    fseek(ef, 0, SEEK_END); long esz = ftell(ef); fseek(ef, 0, SEEK_SET);
    uint8_t *exe = malloc(esz); fread(exe, 1, esz, ef); fclose(ef);

    FI = 1;                      /* DOS hands control to a program with interrupts enabled */
    MEM = calloc(1, 0x110000);
    init_dispatch();
    /* IVT: everything points at the fake BIOS */
    for (int i = 0; i < 256; i++) { WW(0, i * 4, (uint16_t)i); WW(0, i * 4 + 2, BIOSSEG); }
    WW(0x40, 0x13, 640);                                         /* BIOS data: memory size */
    /* load root image with relocations */
    uint16_t *h = (uint16_t *)exe;
    uint32_t img0 = h[4] * 16, img1 = h[2] * 512 - (h[1] ? 512 - h[1] : 0);
    memcpy(MEM + LOADSEG * 16, exe + img0, img1 - img0);
    for (int i = 0; i < h[3]; i++) {
        uint16_t off = *(uint16_t *)(exe + h[12] + 4 * i), seg = *(uint16_t *)(exe + h[12] + 4 * i + 2);
        uint32_t a = (uint32_t)(LOADSEG + seg) * 16 + off;
        uint16_t v = (uint16_t)(MEM[a] | (MEM[a + 1] << 8)); v += LOADSEG; MEM[a] = (uint8_t)v; MEM[a + 1] = (uint8_t)(v >> 8);
    }
    uint32_t image_paras = (img1 - img0 + 15) / 16;
    uint16_t first_free = (uint16_t)(LOADSEG + image_paras + h[5]);
    mem_init(first_free);
    logmsg("conventional memory: program ends at %04x, %u KB free below %04x\n",
           first_free, (unsigned)((MEMTOP - first_free) / 64), MEMTOP);
    /* PSP + environment (program path at its end) */
    uint16_t envseg = (uint16_t)(PSPSEG - 0x20);
    WB(PSPSEG, 0, 0xCD); WB(PSPSEG, 1, 0x20); WW(PSPSEG, 2, MEMTOP); WW(PSPSEG, 0x2C, envseg);
    {   /* command tail: pass our command line through (e.g. "megahit") */
        int n = 0;
        if (cmd && *cmd) { WB(PSPSEG, 0x81, ' '); n = 1; for (; cmd[n - 1] && n < 126; n++) WB(PSPSEG, (uint16_t)(0x81 + n), (uint8_t)cmd[n - 1]); }
        WB(PSPSEG, 0x80, (uint8_t)n); WB(PSPSEG, (uint16_t)(0x81 + n), 0x0D);
    }
    const char *env = "COMSPEC=C:\\COMMAND.COM\0PATH=C:\\\0\0\x01\0C:\\PRINCE2\\PRINCE.EXE\0";
    memcpy(MEM + envseg * 16, env, 60);
    create_window();
    video_init(hwnd);
    update_title();
    if (cfg_fullscreen && !getenv("POP2_KEYS")) toggle_fullscreen();
    pad_init();
    autokeys_init();
    if (getenv("POP2_WATCH")) CreateThread(NULL, 0, watchdog, NULL, 0, NULL);
    /* registers per MZ header */
    SS = (uint16_t)(LOADSEG + h[7]); SP = h[8]; DS = ES = PSPSEG;
    uint16_t cs = (uint16_t)(LOADSEG + h[11]), ip = h[10];
    logmsg("start %04x:%04x ss:sp %04x:%04x first free %04x\n", cs, ip, SS, SP, first_free);
    call_far(cs, ip);
    logmsg("entry returned\n");
    return 0;
}

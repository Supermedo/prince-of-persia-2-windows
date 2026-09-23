/* CPU state + exact 8086 operation semantics for recompiled prince.exe code. */
#ifndef CPU_H
#define CPU_H
#include <stdint.h>

typedef union { uint16_t x; struct { uint8_t l, h; } b; } Reg;

extern Reg rA, rB, rC, rD;
extern uint16_t rSI, rDI, rBP, rSP, sDS, sES, sSS;
extern uint8_t FC, FZ, FS, FO, FP, FA, FD, FI;
extern uint8_t *MEM;
extern const uint8_t parity_tab[256];

#define AX rA.x
#define BX rB.x
#define CX rC.x
#define DX rD.x
#define AL rA.b.l
#define AH rA.b.h
#define BL rB.b.l
#define BH rB.b.h
#define CL rC.b.l
#define CH rC.b.h
#define DL rD.b.l
#define DH rD.b.h
#define SI rSI
#define DI rDI
#define BP rBP
#define SP rSP
#define DS sDS
#define ES sES
#define SS sSS

/* ---- memory (flat 1 MB + HMA; addresses wrap like an 8086 with A20 on) */
#define LIN(s, o) ((((uint32_t)(s)) << 4) + (uint16_t)(o))
static inline uint8_t RB(uint16_t s, uint16_t o) { return MEM[LIN(s, o)]; }
static inline uint16_t RW(uint16_t s, uint16_t o) {
    uint32_t a = LIN(s, o); return (uint16_t)(MEM[a] | (MEM[a + 1] << 8));
}
void mem_write_hook(uint32_t a);
#ifdef SPCHECK
void rt_watch_write(uint32_t a, uint16_t v);   /* debug: report a write to the watched address */
static inline void WB(uint16_t s, uint16_t o, uint8_t v) { uint32_t a = LIN(s, o); rt_watch_write(a, v); MEM[a] = v; }
static inline void WW(uint16_t s, uint16_t o, uint16_t v) {
    uint32_t a = LIN(s, o); rt_watch_write(a, v); rt_watch_write(a + 1, v >> 8);
    MEM[a] = (uint8_t)v; MEM[a + 1] = (uint8_t)(v >> 8);
}
#else
static inline void WB(uint16_t s, uint16_t o, uint8_t v) { MEM[LIN(s, o)] = v; }
static inline void WW(uint16_t s, uint16_t o, uint16_t v) {
    uint32_t a = LIN(s, o); MEM[a] = (uint8_t)v; MEM[a + 1] = (uint8_t)(v >> 8);
}
#endif
static inline void PUSH(uint16_t v) { SP -= 2; WW(SS, SP, v); }
static inline uint16_t POP(void) { uint16_t v = RW(SS, SP); SP += 2; return v; }

/* ---- flags */
static inline uint16_t get_flags(void) {
    return (uint16_t)(0xF002 | FC | (FP << 2) | (FA << 4) | (FZ << 6) | (FS << 7) | (FI << 9) | (FD << 10) | (FO << 11));
}
static inline void set_flags(uint16_t f) {
    FC = f & 1; FP = (f >> 2) & 1; FA = (f >> 4) & 1; FZ = (f >> 6) & 1; FS = (f >> 7) & 1;
    FI = (f >> 9) & 1; FD = (f >> 10) & 1; FO = (f >> 11) & 1;
}
#define SZP8(r)  do { FZ = ((uint8_t)(r) == 0); FS = ((r) >> 7) & 1; FP = parity_tab[(uint8_t)(r)]; } while (0)
#define SZP16(r) do { FZ = ((uint16_t)(r) == 0); FS = ((r) >> 15) & 1; FP = parity_tab[(uint8_t)(r)]; } while (0)

static inline uint8_t op_add8(uint8_t a, uint8_t b) { uint16_t r = a + b; FC = r >> 8; FO = ((a ^ r) & (b ^ r) & 0x80) != 0; FA = ((a ^ b ^ r) >> 4) & 1; SZP8((uint8_t)r); return (uint8_t)r; }
static inline uint16_t op_add16(uint16_t a, uint16_t b) { uint32_t r = a + b; FC = r >> 16; FO = ((a ^ r) & (b ^ r) & 0x8000) != 0; FA = ((a ^ b ^ r) >> 4) & 1; SZP16((uint16_t)r); return (uint16_t)r; }
static inline uint8_t op_adc8(uint8_t a, uint8_t b) { uint16_t r = a + b + FC; FC = r >> 8; FO = ((a ^ r) & (b ^ r) & 0x80) != 0; FA = ((a ^ b ^ r) >> 4) & 1; SZP8((uint8_t)r); return (uint8_t)r; }
static inline uint16_t op_adc16(uint16_t a, uint16_t b) { uint32_t r = a + b + FC; FC = r >> 16; FO = ((a ^ r) & (b ^ r) & 0x8000) != 0; FA = ((a ^ b ^ r) >> 4) & 1; SZP16((uint16_t)r); return (uint16_t)r; }
static inline uint8_t op_sub8(uint8_t a, uint8_t b) { uint16_t r = a - b; FC = (r >> 8) & 1; FO = ((a ^ b) & (a ^ r) & 0x80) != 0; FA = ((a ^ b ^ r) >> 4) & 1; SZP8((uint8_t)r); return (uint8_t)r; }
static inline uint16_t op_sub16(uint16_t a, uint16_t b) { uint32_t r = (uint32_t)a - b; FC = (r >> 16) & 1; FO = ((a ^ b) & (a ^ r) & 0x8000) != 0; FA = ((a ^ b ^ r) >> 4) & 1; SZP16((uint16_t)r); return (uint16_t)r; }
static inline uint8_t op_sbb8(uint8_t a, uint8_t b) { uint16_t r = a - b - FC; FC = (r >> 8) & 1; FO = ((a ^ b) & (a ^ r) & 0x80) != 0; FA = ((a ^ b ^ r) >> 4) & 1; SZP8((uint8_t)r); return (uint8_t)r; }
static inline uint16_t op_sbb16(uint16_t a, uint16_t b) { uint32_t r = (uint32_t)a - b - FC; FC = (r >> 16) & 1; FO = ((a ^ b) & (a ^ r) & 0x8000) != 0; FA = ((a ^ b ^ r) >> 4) & 1; SZP16((uint16_t)r); return (uint16_t)r; }
static inline uint8_t op_and8(uint8_t a, uint8_t b) { uint8_t r = a & b; FC = FO = 0; SZP8(r); return r; }
static inline uint16_t op_and16(uint16_t a, uint16_t b) { uint16_t r = a & b; FC = FO = 0; SZP16(r); return r; }
static inline uint8_t op_or8(uint8_t a, uint8_t b) { uint8_t r = a | b; FC = FO = 0; SZP8(r); return r; }
static inline uint16_t op_or16(uint16_t a, uint16_t b) { uint16_t r = a | b; FC = FO = 0; SZP16(r); return r; }
static inline uint8_t op_xor8(uint8_t a, uint8_t b) { uint8_t r = a ^ b; FC = FO = 0; SZP8(r); return r; }
static inline uint16_t op_xor16(uint16_t a, uint16_t b) { uint16_t r = a ^ b; FC = FO = 0; SZP16(r); return r; }
static inline uint8_t op_inc8(uint8_t a) { uint8_t r = a + 1; FO = (r == 0x80); FA = (r & 15) == 0; SZP8(r); return r; }
static inline uint16_t op_inc16(uint16_t a) { uint16_t r = a + 1; FO = (r == 0x8000); FA = (r & 15) == 0; SZP16(r); return r; }
static inline uint8_t op_dec8(uint8_t a) { uint8_t r = a - 1; FO = (r == 0x7F); FA = (r & 15) == 15; SZP8(r); return r; }
static inline uint16_t op_dec16(uint16_t a) { uint16_t r = a - 1; FO = (r == 0x7FFF); FA = (r & 15) == 15; SZP16(r); return r; }
static inline uint8_t op_neg8(uint8_t a) { uint8_t r = (uint8_t)(0 - a); FC = a != 0; FO = (a == 0x80); FA = (a & 15) != 0; SZP8(r); return r; }
static inline uint16_t op_neg16(uint16_t a) { uint16_t r = (uint16_t)(0 - a); FC = a != 0; FO = (a == 0x8000); FA = (a & 15) != 0; SZP16(r); return r; }

/* shifts / rotates: count masked to 5 bits like 186+ */
static inline uint8_t op_shl8(uint8_t a, uint8_t c) { c &= 31; if (!c) return a; uint16_t r = (uint16_t)a << c; FC = (r >> 8) & 1; uint8_t v = (uint8_t)r; FO = ((v >> 7) & 1) ^ FC; SZP8(v); return v; }
static inline uint16_t op_shl16(uint16_t a, uint8_t c) { c &= 31; if (!c) return a; uint32_t r = (uint32_t)a << c; FC = (r >> 16) & 1; uint16_t v = (uint16_t)r; FO = ((v >> 15) & 1) ^ FC; SZP16(v); return v; }
static inline uint8_t op_shr8(uint8_t a, uint8_t c) { c &= 31; if (!c) return a; FC = (c <= 8) ? (a >> (c - 1)) & 1 : 0; uint8_t v = (c < 8) ? a >> c : 0; FO = (c == 1) ? (a >> 7) & 1 : 0; SZP8(v); return v; }
static inline uint16_t op_shr16(uint16_t a, uint8_t c) { c &= 31; if (!c) return a; FC = (c <= 16) ? (a >> (c - 1)) & 1 : 0; uint16_t v = (c < 16) ? a >> c : 0; FO = (c == 1) ? (a >> 15) & 1 : 0; SZP16(v); return v; }
static inline uint8_t op_sar8(uint8_t a, uint8_t c) { c &= 31; if (!c) return a; int8_t s = (int8_t)a; if (c > 8) c = 8; FC = (s >> (c - 1)) & 1; uint8_t v = (uint8_t)(s >> (c > 7 ? 7 : c)); if (c >= 8) v = (s < 0) ? 0xFF : 0; FO = 0; SZP8(v); return v; }
static inline uint16_t op_sar16(uint16_t a, uint8_t c) { c &= 31; if (!c) return a; int16_t s = (int16_t)a; if (c > 16) c = 16; FC = (s >> (c - 1)) & 1; uint16_t v = (uint16_t)(s >> (c > 15 ? 15 : c)); if (c >= 16) v = (s < 0) ? 0xFFFF : 0; FO = 0; SZP16(v); return v; }
static inline uint8_t op_rol8(uint8_t a, uint8_t c) { c &= 31; if (!c) return a; c &= 7; uint8_t v = (uint8_t)((a << c) | (a >> ((8 - c) & 7))); FC = v & 1; FO = ((v >> 7) & 1) ^ FC; return v; }
static inline uint16_t op_rol16(uint16_t a, uint8_t c) { c &= 31; if (!c) return a; c &= 15; uint16_t v = (uint16_t)((a << c) | (a >> ((16 - c) & 15))); FC = v & 1; FO = ((v >> 15) & 1) ^ FC; return v; }
static inline uint8_t op_ror8(uint8_t a, uint8_t c) { c &= 31; if (!c) return a; c &= 7; uint8_t v = (uint8_t)((a >> c) | (a << ((8 - c) & 7))); FC = (v >> 7) & 1; FO = ((v >> 7) ^ (v >> 6)) & 1; return v; }
static inline uint16_t op_ror16(uint16_t a, uint8_t c) { c &= 31; if (!c) return a; c &= 15; uint16_t v = (uint16_t)((a >> c) | (a << ((16 - c) & 15))); FC = (v >> 15) & 1; FO = ((v >> 15) ^ (v >> 14)) & 1; return v; }
static inline uint8_t op_rcl8(uint8_t a, uint8_t c) { c = (c & 31) % 9; while (c--) { uint8_t nc = a >> 7; a = (uint8_t)((a << 1) | FC); FC = nc; } FO = ((a >> 7) & 1) ^ FC; return a; }
static inline uint16_t op_rcl16(uint16_t a, uint8_t c) { c = (c & 31) % 17; while (c--) { uint8_t nc = a >> 15; a = (uint16_t)((a << 1) | FC); FC = nc; } FO = ((a >> 15) & 1) ^ FC; return a; }
static inline uint8_t op_rcr8(uint8_t a, uint8_t c) { c = (c & 31) % 9; while (c--) { uint8_t nc = a & 1; a = (uint8_t)((a >> 1) | (FC << 7)); FC = nc; } FO = ((a >> 7) ^ (a >> 6)) & 1; return a; }
static inline uint16_t op_rcr16(uint16_t a, uint8_t c) { c = (c & 31) % 17; while (c--) { uint8_t nc = a & 1; a = (uint16_t)((a >> 1) | (FC << 15)); FC = nc; } FO = ((a >> 15) ^ (a >> 14)) & 1; return a; }

/* multiply / divide */
static inline void op_mul8(uint8_t b) { AX = (uint16_t)(AL * b); FC = FO = (AH != 0); }
static inline void op_mul16(uint16_t b) { uint32_t r = (uint32_t)AX * b; AX = (uint16_t)r; DX = (uint16_t)(r >> 16); FC = FO = (DX != 0); }
static inline void op_imul8(uint8_t b) { int16_t r = (int16_t)(int8_t)AL * (int8_t)b; AX = (uint16_t)r; FC = FO = (r != (int8_t)r); }
static inline void op_imul16(uint16_t b) { int32_t r = (int32_t)(int16_t)AX * (int16_t)b; AX = (uint16_t)r; DX = (uint16_t)(r >> 16); FC = FO = (r != (int16_t)r); }
static inline uint16_t op_imul3(uint16_t a, uint16_t b) { int32_t r = (int32_t)(int16_t)a * (int16_t)b; FC = FO = (r != (int16_t)r); return (uint16_t)r; }
void rt_divide_error(void);
static inline void op_div8(uint8_t b) { if (!b) { rt_divide_error(); return; } uint16_t q = AX / b; if (q > 0xFF) { rt_divide_error(); return; } AH = (uint8_t)(AX % b); AL = (uint8_t)q; }
static inline void op_div16(uint16_t b) { if (!b) { rt_divide_error(); return; } uint32_t n = ((uint32_t)DX << 16) | AX; uint32_t q = n / b; if (q > 0xFFFF) { rt_divide_error(); return; } DX = (uint16_t)(n % b); AX = (uint16_t)q; }
static inline void op_idiv8(uint8_t b) { if (!b) { rt_divide_error(); return; } int16_t n = (int16_t)AX; int16_t q = n / (int8_t)b; if (q > 127 || q < -128) { rt_divide_error(); return; } AH = (uint8_t)(n % (int8_t)b); AL = (uint8_t)q; }
static inline void op_idiv16(uint16_t b) { if (!b) { rt_divide_error(); return; } int32_t n = (int32_t)(((uint32_t)DX << 16) | AX); int32_t q = n / (int16_t)b; if (q > 32767 || q < -32768) { rt_divide_error(); return; } DX = (uint16_t)(n % (int16_t)b); AX = (uint16_t)q; }

void op_daa(void); void op_das(void); void op_aaa(void); void op_aas(void); void op_aam(void); void op_aad(void);

/* string instructions: rep 0 none, 1 rep/repe, 2 repne */
void str_movsb(uint16_t seg, int rep); void str_movsw(uint16_t seg, int rep);
void str_stosb(uint16_t seg, int rep); void str_stosw(uint16_t seg, int rep);
void str_lodsb(uint16_t seg, int rep); void str_lodsw(uint16_t seg, int rep);
void str_cmpsb(uint16_t seg, int rep); void str_cmpsw(uint16_t seg, int rep);
void str_scasb(uint16_t seg, int rep); void str_scasw(uint16_t seg, int rep);

extern const char *volatile cur_fn;
/* interrupt polling at loop back-edges */
extern int poll_ctr;
void rt_poll(void);
#define POLL() do { if (--poll_ctr <= 0) rt_poll(); } while (0)

/* runtime services */
uint8_t port_in8(uint16_t port); uint16_t port_in16(uint16_t port);
void port_out8(uint16_t port, uint8_t v); void port_out16(uint16_t port, uint16_t v);
void do_int(int n, uint16_t cs, uint16_t ip);
void call_far(uint16_t seg, uint16_t off);
void call_near(uint16_t cs, uint16_t off);
void fpu_esc(int op);
void rt_halt(uint16_t cs, uint16_t ip);
void rt_unimpl(uint16_t cs, uint16_t ip);
void rt_badjump(uint16_t cs, uint16_t ip, uint16_t target);
#ifdef SPCHECK                       /* debug build: catch a stack pointer that left its stack */
void rt_spchk(void);
void rt_bp_broken(const char *fn, uint16_t expect);
#define SPCHK() rt_spchk()
#define RETCHK_ENTER() const uint16_t _caller_bp = BP, _entry_sp = SP
void rt_retchk(const char *fn, uint16_t caller_bp, uint16_t entry_sp);
#define RETCHK() rt_retchk(cur_fn, _caller_bp, _entry_sp)
#else
#define SPCHK() ((void)0)
#define RETCHK_ENTER() ((void)0)
#define RETCHK() ((void)0)
#endif
void rt_frame_hook(void);            /* runtime hook in the per-frame game update (checkpoints) */
void rt_savemenu_hook(void);
void rt_fatal_hook(void);            /* runtime hook: the game is about to exit with an error */         /* runtime hook: the game opened the "Saved Games" screen */
void rt_missing(const char *space, uint16_t seg, uint16_t off);
#endif

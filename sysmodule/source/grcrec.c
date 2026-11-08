#include <string.h>
#include "grcrec.h"

volatile s32 g_grc_peak = 0;
static volatile u64 g_sig_ms[2] = { 0, 0 };   // last time each source delivered non-silent audio

static u64 now_ms(void) { return armTicksToNs(armGetSystemTick()) / 1000000ULL; }

bool arb_accept(int src, s32 peak) {
    u64 now = now_ms();
    if (peak > 0) { g_sig_ms[src] = now; return true; }
    // silence: only the source that is (or was just) the owner may keep the timeline going
    return (now - g_sig_ms[1 - src]) > 1000;
}

// ---- thin wrappers: all libnx grc:d calls live here so signature fixes stay in one place ----
// libnx here exposes a single global grc:d session: grcdInitialize / grcdBegin / grcdTransfer / grcdExit
// (the compiler suggested "grcdTransfer"; if another signature is reported, fix it only in these four lines)
#define G_OPEN(st)                       grcdInitialize()
#define G_BEGIN(st)                      grcdBegin(GrcStream_Audio)
#define G_TRANSFER(st, buf, sz, a, b, c) grcdTransfer(GrcStream_Audio, (buf), (sz), (a), (b), (c))
#define G_CLOSE(st)                      grcdExit()

#define MAXW   8
#define WSTACK 0x8000
#define ABUF   0x1000

static Thread g_thr[MAXW];
static u8     g_stk[MAXW][WSTACK] __attribute__((aligned(0x1000)));
static u8     g_abuf[MAXW][ABUF]  __attribute__((aligned(0x1000)));
static volatile bool g_alive[MAXW];
static bool   g_started[MAXW];
static volatile int g_gen = 0;
static Ring  *g_ringp = NULL;

typedef struct { int slot; int gen; } WArg;
static WArg g_args[MAXW];

static void worker(void *p) {
    WArg *a = (WArg *)p;
    u8 *buf = g_abuf[a->slot];
    GrcStream st;
    bool opened = false;

    while (a->gen == g_gen) {
        if (!opened) {
            memset(&st, 0, sizeof(st));
            Result rc = G_OPEN(&st);
            if (R_SUCCEEDED(rc)) rc = G_BEGIN(&st);
            if (R_FAILED(rc)) { svcSleepThread(500000000ULL); continue; }
            opened = true;
        }

        u32 unk = 0;
        u64 sz = 0, ts = 0;
        Result rc = G_TRANSFER(&st, buf, ABUF, &unk, &sz, &ts);   // blocks until audio is available
        if (a->gen != g_gen) break;                                // superseded while blocked
        if (R_FAILED(rc)) { G_CLOSE(&st); opened = false; svcSleepThread(200000000ULL); continue; }

        if (sz > ABUF) sz = ABUF;
        sz &= ~3ULL;
        if (sz == 0) continue;

        s32 peak = 0;
        const s16 *s = (const s16 *)buf;
        for (u32 i = 0; i < sz / 2; i++) { s32 v = s[i] < 0 ? -s[i] : s[i]; if (v > peak) peak = v; }
        if (peak > g_grc_peak) g_grc_peak = peak;
        if (g_ringp && arb_accept(SRC_GRC, peak)) ring_push(g_ringp, buf, (u32)sz);
    }

    if (opened) G_CLOSE(&st);
    g_alive[a->slot] = false;
}

void grcrec_start(Ring *ring) {
    g_ringp = ring;
    int gen = ++g_gen;

    for (int i = 0; i < MAXW; i++) {                 // reap workers that have finished
        if (g_started[i] && !g_alive[i]) {
            threadWaitForExit(&g_thr[i]);
            threadClose(&g_thr[i]);
            g_started[i] = false;
        }
    }
    for (int i = 0; i < MAXW; i++) {
        if (g_started[i]) continue;                  // still blocked on an old application
        g_args[i].slot = i;
        g_args[i].gen = gen;
        g_alive[i] = true;
        if (R_FAILED(threadCreate(&g_thr[i], worker, &g_args[i], g_stk[i], WSTACK, 0x2C, -2))) { g_alive[i] = false; return; }
        if (R_FAILED(threadStart(&g_thr[i]))) { threadClose(&g_thr[i]); g_alive[i] = false; return; }
        g_started[i] = true;
        return;
    }
}

void grcrec_stop(void) { g_gen++; }

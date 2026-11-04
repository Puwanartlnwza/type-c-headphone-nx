#pragma once
// Tiny byte ring for 16-bit stereo PCM (all sizes are multiples of 4).
#include <switch.h>
#include <string.h>

#define RING_SIZE 0x4000          // power of two
#define RING_MAX  8192            // max buffered bytes (~42 ms at 48 kHz stereo 16-bit)

typedef struct {
    u8  data[RING_SIZE];
    u32 head;                     // read counter
    u32 tail;                     // write counter
} Ring;

static inline void ring_init(Ring *r) { r->head = r->tail = 0; }
static inline u32  ring_level(const Ring *r) { return r->tail - r->head; }

static inline void ring_push(Ring *r, const u8 *src, u32 len) {
    if (len > RING_MAX) { src += len - RING_MAX; len = RING_MAX; }
    u32 lvl = ring_level(r);
    if (lvl + len > RING_MAX) r->head += lvl + len - RING_MAX;   // drop oldest to bound latency
    for (u32 i = 0; i < len; i++) r->data[(r->tail + i) & (RING_SIZE - 1)] = src[i];
    r->tail += len;
}

static inline u32 ring_pop(Ring *r, u8 *dst, u32 len) {
    u32 lvl = ring_level(r);
    if (len > lvl) len = lvl;
    for (u32 i = 0; i < len; i++) dst[i] = r->data[(r->head + i) & (RING_SIZE - 1)];
    r->head += len;
    return len;
}

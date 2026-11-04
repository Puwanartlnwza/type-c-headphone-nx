// usbaudio: Atmosphere sysmodule that records the Switch's final audio output (audrec:u)
// and plays it on Apple USB-C EarPods through usb:hs (isochronous, UAC2).
// Log: sdmc:/config/usbaudio/log.txt   Config: sdmc:/config/usbaudio/config.ini  (gain=NN percent)
#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/stat.h>

#include "ring.h"
#include "audrec.h"
#include "usbout.h"

// Startup breadcrumbs: appended to sdmc:/usbaudio_stage.txt (SD root) so we can see how far we got.
static void stage(const char *msg) {
    FILE *f = fopen("sdmc:/usbaudio_stage.txt", "a");
    if (!f) return;
    fprintf(f, "%s\n", msg);
    fclose(f);
}

// ---- libnx sysmodule plumbing ----
#define INNER_HEAP_SIZE 0x60000
u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 1;
size_t nx_inner_heap_size = INNER_HEAP_SIZE;
char   nx_inner_heap[INNER_HEAP_SIZE];

void __libnx_initheap(void) {
    extern void *fake_heap_start;
    extern void *fake_heap_end;
    fake_heap_start = nx_inner_heap;
    fake_heap_end   = nx_inner_heap + nx_inner_heap_size;
}

void __appInit(void) {
    Result rc = smInitialize();
    if (R_FAILED(rc)) diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_SM));

    rc = setsysInitialize();
    if (R_SUCCEEDED(rc)) {
        SetSysFirmwareVersion fw;
        rc = setsysGetFirmwareVersion(&fw);
        if (R_SUCCEEDED(rc)) hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
        setsysExit();
    }

    rc = fsInitialize();
    if (R_FAILED(rc)) diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_FS));
    fsdevMountSdmc();
    stage("appInit done (sm, setsys, fs, sdmc ok)");
}

void __appExit(void) {
    fsdevUnmountAll();
    fsExit();
    smExit();
}

// ---- logging / config ----
static FILE *g_log;
static u32 g_gain = 40;           // percent; EarPods are loud

static void LOG(const char *fmt, ...) {
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

static void load_config(void) {
    FILE *f = fopen("sdmc:/config/usbaudio/config.ini", "r");
    if (!f) return;
    char line[64];
    while (fgets(line, sizeof(line), f)) {
        unsigned v;
        if (sscanf(line, "gain=%u", &v) == 1 && v <= 100) g_gain = v;
    }
    fclose(f);
}

// ---- streaming session ----
#define RECN 4
static Ring         g_ring;
static UsbOut       g_usb;
static AudRec       g_rec;
static AudRecBuffer g_rb[RECN];
static u8           g_recmem[RECN * 0x1000] __attribute__((aligned(0x1000)));

static Result rec_arm(int i) {
    AudRecBuffer *b = &g_rb[i];
    memset(b, 0, sizeof(*b));
    b->sample_buffer = (u64)(uintptr_t)(g_recmem + i * 0x1000);
    b->capacity = 0x1000;
    b->size = 0x1000;
    return audrec_append(&g_rec, b);
}

// returns 0 = no device (idle), 1 = session ended (error / unplug)
static int run_session(void) {
    ring_init(&g_ring);

    Result rc = usbout_open(&g_usb, &g_ring, g_gain);
    if (rc == UO_NOT_FOUND) { usbout_close(&g_usb, false); return 0; }
    if (R_FAILED(rc)) {
        LOG("usb open failed: 0x%x", rc);
        usbout_close(&g_usb, true);
        return 1;
    }
    LOG("usb open ok, frame0=%u gain=%u%%", g_usb.frame0, g_gain);

    rc = audrec_open(&g_rec, 48000);
    if (R_FAILED(rc)) {
        LOG("audrec open failed: 0x%x", rc);
        usbout_close(&g_usb, true);
        return 1;
    }
    LOG("audrec open ok: rate=%u ch=%u fmt=%u state=%u", g_rec.sample_rate, g_rec.channels, g_rec.format, g_rec.state);

    for (int i = 0; i < RECN; i++) {
        rc = rec_arm(i);
        if (R_FAILED(rc)) LOG("audrec append %d failed: 0x%x", i, rc);
    }
    rc = audrec_start(&g_rec);
    LOG("audrec start: 0x%x", rc);

    bool reset_dev = false;
    int timeouts = 0, stats = 0;
    u32 captured = 0;

    for (;;) {
        Waiter ws[2] = { waiterForEvent(&g_rec.ev), waiterForEvent(g_usb.ev) };
        s32 idx = -1;
        Result w = waitObjects(&idx, ws, 2, 200000000ULL);

        if (R_SUCCEEDED(w)) {
            timeouts = 0;
            if (idx == 0) {
                eventClear(&g_rec.ev);
                AudRecBuffer *done[RECN];
                u32 n = 0;
                audrec_released(&g_rec, done, RECN, &n);
                for (u32 i = 0; i < n; i++) {
                    AudRecBuffer *b = done[i];
                    u32 size = (u32)b->size;
                    if (size == 0 || size > b->capacity) size = (u32)b->capacity;
                    size &= ~3u;
                    const u8 *src = (const u8 *)(uintptr_t)b->sample_buffer;
                    ring_push(&g_ring, src, size);

                    if (stats < 20 && (captured++ % 100) == 1) {
                        s16 peak = 0;
                        const s16 *p = (const s16 *)src;
                        for (u32 k = 0; k < size / 2; k++) { s16 a = p[k] < 0 ? -p[k] : p[k]; if (a > peak) peak = a; }
                        LOG("rec: size=%u peak=%d ring=%u underruns=%u batches=%u", size, peak, ring_level(&g_ring), g_usb.underruns, g_usb.batches);
                        stats++;
                    }
                    b->size = b->capacity;
                    audrec_append(&g_rec, b);
                }
            } else {
                eventClear(g_usb.ev);
            }
        } else if (R_VALUE(w) == KERNELRESULT(TimedOut)) {
            if (++timeouts > 10) { LOG("no events for 2 s, ending session"); break; }
        } else {
            LOG("wait failed: 0x%x", w);
            break;
        }

        rc = usbout_pump(&g_usb, &g_ring, g_gain);
        if (R_FAILED(rc)) { LOG("usb pump error: 0x%x", rc); reset_dev = true; break; }
    }

    audrec_stop(&g_rec);
    audrec_close(&g_rec);
    usbout_close(&g_usb, reset_dev);
    LOG("session ended (underruns=%u batches=%u)", g_usb.underruns, g_usb.batches);
    return 1;
}

int main(void) {
    stage("main entered");
    mkdir("sdmc:/config", 0777);
    mkdir("sdmc:/config/usbaudio", 0777);
    g_log = fopen("sdmc:/config/usbaudio/log.txt", "w");
    if (!g_log) {
        stage("config/usbaudio/log.txt could not be opened, using sdmc:/usbaudio_log.txt");
        g_log = fopen("sdmc:/usbaudio_log.txt", "w");
    }
    load_config();
    LOG("usbaudio sysmodule v0.1 starting (gain=%u%%)", g_gain);

    stage("calling usbHsInitialize");
    while (R_FAILED(usbHsInitialize())) svcSleepThread(1000000000ULL);
    stage("usbHsInitialize ok, entering main loop");
    LOG("usb:hs ready");

    for (;;) {
        int r = run_session();
        svcSleepThread(r == 0 ? 1000000000ULL : 3000000000ULL);
    }
    return 0;
}
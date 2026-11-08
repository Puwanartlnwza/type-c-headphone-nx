// usbaudio: Atmosphere sysmodule that records the Switch's final audio output (audrec:u)
// and plays it on Apple USB-C EarPods through usb:hs (isochronous, UAC2).
// Log: sdmc:/config/usbaudio/log.txt   Config: sdmc:/config/usbaudio/config.ini  (gain=NN percent)
#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <sys/stat.h>

#include "ring.h"
#include "audrec.h"
#include "usbout.h"
#include "grcrec.h"

// Startup breadcrumbs: appended to sdmc:/usbaudio_stage.txt (SD root) so we can see how far we got.
static void stage(const char *msg) {
    FILE *f = fopen("sdmc:/usbaudio_stage.txt", "a");
    if (!f) return;
    fprintf(f, "%s\n", msg);
    fclose(f);
}

// ---- libnx sysmodule plumbing ----
#define INNER_HEAP_SIZE 0x30000
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
    }   // set:sys stays open: used to follow the console volume buttons

    rc = fsInitialize();
    if (R_FAILED(rc)) diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_FS));
    fsdevMountSdmc();
    stage("appInit done (sm, setsys, fs, sdmc ok)");
}

void __appExit(void) {
    setsysExit();
    fsdevUnmountAll();
    fsExit();
    smExit();
}

// ---- logging / config ----
static const char *g_log_path = NULL;     // set in main(); NULL = logging off
#define APP_VERSION "0.4.1"
static u64 g_aruid = 0;           // config.ini: aruid=<hex>, only read when a session starts
static u32 g_gain = 50;
static u32 g_sysvol = 15;         // console volume 0-15
static u32 g_mul = 2500;          // final multiplier, 1/10000 units (see update_volume)
static s32 g_in_peak = 0;         // loudest captured sample since the last status write
static s32 g_out_peak = 0;        // loudest sample sent to the earphones since the last status write
static u32 g_stat_ur = 0, g_refresh_total = 0;
static char g_state[24] = "starting";           // percent; EarPods are loud (change with the overlay slider or config.ini)

// Open/append/close on every line: an always-open file is not committed to the SD until closed,
// so it would never show up when the card is read on a PC.
static void LOG(const char *fmt, ...) {
    if (!g_log_path) return;
    FILE *f = fopen(g_log_path, "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static void load_config(void) {
    FILE *f = fopen("sdmc:/config/usbaudio/config.ini", "r");
    if (!f) return;
    char line[64];
    while (fgets(line, sizeof(line), f)) {
        unsigned v;
        unsigned long long a;
        if (sscanf(line, "gain=%u", &v) == 1 && v <= 100) g_gain = v;
        if (sscanf(line, "aruid=%llx", &a) == 1) g_aruid = (u64)a;
    }
    fclose(f);
}

// ---- control files shared with the Tesla overlay ----
//   enabled : "1" / "0"  (missing = enabled)       status : one word describing what we are doing
static bool g_enabled = true;

static u64 ms_now(void) { return armTicksToNs(armGetSystemTick()) / 1000000ULL; }

static void poll_config(void) {
    FILE *f = fopen("sdmc:/config/usbaudio/enabled", "r");
    if (f) { int c = fgetc(f); g_enabled = (c != '0'); fclose(f); }
    else g_enabled = true;
    load_config();                // gain= (live)
}

static void write_status(void) {
    FILE *f = fopen("sdmc:/config/usbaudio/status.txt", "w");
    if (!f) return;
    fprintf(f, "%s v" APP_VERSION "\n", g_state);
    fprintf(f, "sys=%u slider=%u mul=%u\n", g_sysvol, g_gain, g_mul);
    fprintf(f, "rec=%d grc=%d out=%d ur=%u rf=%u\n", g_in_peak, g_grc_peak, g_out_peak, g_stat_ur, g_refresh_total);
    fclose(f);
}

static void set_status(const char *st) {
    if (strncmp(g_state, st, sizeof(g_state)) == 0) return;
    strncpy(g_state, st, sizeof(g_state) - 1);
    write_status();
}

// Final volume = slider trim (gain/100)^2  x  console volume 0-15 (0.7236^(15-v), same curve as nx-btred).
// The recorded signal is taken BEFORE the console volume, so we apply it ourselves.

static void update_volume(void) {
    SetSysAudioVolume v;
    if (R_SUCCEEDED(setsysGetAudioVolume(SetSysAudioDevice_Console, &v)) && v.volume <= 15) g_sysvol = v.volume;
    u32 sys_mul = (g_sysvol == 0) ? 0 : (u32)(10000.0f * powf(0.7236f, (float)(15 - (int)g_sysvol)));
    g_mul = (g_gain * g_gain) * sys_mul / 10000;
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

// nx-btred: "If audrec gets out of sync (when switching apps), we need to refresh it."
// -> close the recorder and open a new one.
static Result rec_refresh(void) {
    audrec_stop(&g_rec);
    audrec_close(&g_rec);
    Result rc = audrec_open(&g_rec, 48000, g_aruid);
    if (R_FAILED(rc)) return rc;
    for (int i = 0; i < RECN; i++) rec_arm(i);
    return audrec_start(&g_rec);
}

// returns 0 = no device (idle), 1 = session ended (error / unplug)
static int run_session(void) {
    ring_init(&g_ring);

    update_volume();
    Result rc = usbout_open(&g_usb, &g_ring, g_mul);
    if (rc == UO_NOT_FOUND) { usbout_close(&g_usb, false); return 0; }
    if (R_FAILED(rc)) {
        LOG("usb open failed: 0x%x", rc);
        usbout_close(&g_usb, true);
        return 1;
    }
    LOG("usb ok frame0=%u gain=%u", g_usb.frame0, g_gain);

    rc = audrec_open(&g_rec, 48000, g_aruid);
    if (R_FAILED(rc)) {
        LOG("audrec open failed: 0x%x", rc);
        usbout_close(&g_usb, true);
        return 1;
    }
    LOG("audrec ok rate=%u ch=%u fmt=%u aruid=%llx", g_rec.sample_rate, g_rec.channels, g_rec.format, (unsigned long long)g_aruid);

    for (int i = 0; i < RECN; i++) {
        rc = rec_arm(i);
        if (R_FAILED(rc)) LOG("audrec append %d failed: 0x%x", i, rc);
    }
    rc = audrec_start(&g_rec);
    LOG("audrec start: 0x%x", rc);
    set_status("streaming");
    grcrec_start(&g_ring);
    u64 last_pid = 0, last_pidpoll = 0;
    u64 last_poll = ms_now(), last_vol = ms_now(), last_refresh = 0, last_stat = 0;
    u32 refreshes = 0;

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
                u64 released_ns = 0;
                audrec_released(&g_rec, done, RECN, &n, &released_ns);
                for (u32 i = 0; i < n; i++) {
                    AudRecBuffer *b = done[i];
                    u32 size = (u32)b->size;
                    if (size == 0 || size > b->capacity) size = (u32)b->capacity;
                    size &= ~3u;
                    const u8 *src = (const u8 *)(uintptr_t)b->sample_buffer;

                    s16 peak = 0;
                    {
                        const s16 *p = (const s16 *)src;
                        for (u32 k = 0; k < size / 2; k++) { s16 a = p[k] < 0 ? -p[k] : p[k]; if (a > peak) peak = a; }
                    }
                    if (peak > g_in_peak) g_in_peak = peak;
                    if (arb_accept(SRC_REC, peak)) ring_push(&g_ring, src, size);
                    if (stats < 200 && (captured++ % 200) == 1) {
                        LOG("t=%u in=%d out=%d ring=%u ur=%u b=%u g=%u sv=%u", (unsigned)(armTicksToNs(armGetSystemTick()) / 1000000ULL), peak, g_usb.peak_out, ring_level(&g_ring), g_usb.underruns, g_usb.batches, g_gain, g_sysvol);
                        stats++;
                    }
                    b->size = b->capacity;
                    audrec_append(&g_rec, b);
                }

                // stale recording clock => the recorder lost sync with the running app: recreate it
                if (n > 0) {
                    u64 now_ns = armTicksToNs(armGetSystemTick());
                    if (now_ns > released_ns && (now_ns - released_ns) > 100000000ULL && ms_now() - last_refresh > 500) {
                        last_refresh = ms_now();
                        refreshes++;
                        g_refresh_total++;
                        if (refreshes <= 30) LOG("rec stale %ums -> refresh #%u", (unsigned)((now_ns - released_ns) / 1000000ULL), refreshes);
                        rc = rec_refresh();
                        if (R_FAILED(rc)) { LOG("rec refresh failed: 0x%x", rc); break; }
                    }
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

        if (ms_now() - last_vol >= 200) { last_vol = ms_now(); update_volume(); }
        if (ms_now() - last_pidpoll >= 1000) {            // new application => new grc session
            last_pidpoll = ms_now();
            u64 pid = 0;
            if (R_SUCCEEDED(pmdmntInitialize())) {
                if (R_FAILED(pmdmntGetApplicationProcessId(&pid))) pid = 0;
                pmdmntExit();
            }
            if (pid != last_pid) { last_pid = pid; LOG("application pid=%llu -> restart grc worker", (unsigned long long)pid); grcrec_start(&g_ring); }
        }
        if (ms_now() - last_stat >= 3000) {
            last_stat = ms_now();
            g_stat_ur = g_usb.underruns;
            g_out_peak = g_usb.peak_out; g_usb.peak_out = 0;
            write_status();
            g_in_peak = 0;
            g_grc_peak = 0;
        }

        if (ms_now() - last_poll >= 2000) {
            last_poll = ms_now();
            poll_config();
            if (!g_enabled) { LOG("disabled from overlay, ending session"); break; }
        }

        rc = usbout_pump(&g_usb, &g_ring, g_mul);
        if (R_FAILED(rc)) {
            LOG("usb err 0x%x b=%u ur=%u, restarting stream", rc, g_usb.batches, g_usb.underruns);
            usbout_close(&g_usb, false);
            rc = usbout_open(&g_usb, &g_ring, g_mul);
            if (R_FAILED(rc)) { LOG("stream restart failed: 0x%x", rc); reset_dev = true; break; }
            LOG("stream restarted, frame0=%u", g_usb.frame0);
        }
    }

    grcrec_stop();
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
    FILE *t = fopen("sdmc:/config/usbaudio/log.txt", "w");
    if (t) { fclose(t); g_log_path = "sdmc:/config/usbaudio/log.txt"; }
    else {
        stage("config/usbaudio/log.txt could not be opened, using sdmc:/usbaudio_log.txt");
        t = fopen("sdmc:/usbaudio_log.txt", "w");
        if (t) { fclose(t); g_log_path = "sdmc:/usbaudio_log.txt"; }
    }
    load_config();
    LOG("usbaudio v" APP_VERSION " start gain=%u", g_gain);

    stage("calling usbHsInitialize");
    while (R_FAILED(usbHsInitialize())) svcSleepThread(1000000000ULL);
    stage("usbHsInitialize ok, entering main loop");
    LOG("usb:hs ready");

    for (;;) {
        poll_config();
        if (!g_enabled) { set_status("disabled"); svcSleepThread(1000000000ULL); continue; }
        int r = run_session();
        set_status(r == 0 ? "no-device" : "error");
        svcSleepThread(r == 0 ? 1000000000ULL : 3000000000ULL);
    }
    return 0;
}

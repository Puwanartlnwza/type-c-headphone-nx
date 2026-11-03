// USB Audio Probe v2
// Y = play a 3 s 440 Hz test tone on the EarPods (low volume)
// A = rescan interfaces, X = acquire device and dump full config descriptor
// (hex + parsed alt settings / endpoints / formats / clock + sample rates),
// + = exit. Everything is also written to sdmc:/usbaudio_probe.txt
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <switch.h>

static FILE *g_log;
#define OUT(...) do { printf(__VA_ARGS__); if (g_log) fprintf(g_log, __VA_ARGS__); } while (0)

static UsbHsInterface g_ifs[16];
static s32 g_total;
static int g_clock_id = -1;
static int g_ac_if = 0;

static void scan(void) {
    UsbHsInterfaceFilter filter;
    memset(&filter, 0, sizeof(filter));   // Flags = 0: match everything
    memset(g_ifs, 0, sizeof(g_ifs));
    g_total = 0;

    Result rc = usbHsQueryAvailableInterfaces(&filter, g_ifs, sizeof(g_ifs), &g_total);
    OUT("\n--- scan: rc=0x%x, interfaces=%d ---\n", rc, g_total);
    if (R_FAILED(rc)) { g_total = 0; return; }

    for (s32 i = 0; i < g_total && i < 16; i++) {
        struct usb_interface_descriptor *d = &g_ifs[i].inf.interface_desc;
        OUT("[%d] VID %04x PID %04x if#%d alt%d class %02x/%02x/%02x\n",
            i, g_ifs[i].device_desc.idVendor, g_ifs[i].device_desc.idProduct,
            d->bInterfaceNumber, d->bAlternateSetting,
            d->bInterfaceClass, d->bInterfaceSubClass, d->bInterfaceProtocol);
    }
    if (g_log) fflush(g_log);
}

static void hex_to_log(const u8 *p, u32 n) {
    if (!g_log) return;
    fprintf(g_log, "RAW CONFIG DESCRIPTOR (%u bytes):\n", n);
    for (u32 i = 0; i < n; i++) {
        fprintf(g_log, "%02x%s", p[i], (i % 16 == 15 || i + 1 == n) ? "\n" : " ");
    }
}

static void parse_config(const u8 *d, u32 len) {
    int cls = -1, sub = -1;
    u32 i = 0;
    g_clock_id = -1;
    while (i + 2 <= len) {
        u8 l = d[i], t = d[i + 1];
        if (l < 2 || i + l > len) break;

        if (t == 0x04 && l >= 9) {                    // interface
            cls = d[i + 5]; sub = d[i + 6];
            OUT("IF %d alt %d eps %d class %02x/%02x\n", d[i + 2], d[i + 3], d[i + 4], cls, sub);
            if (cls == 1 && sub == 1) g_ac_if = d[i + 2];
        } else if (t == 0x05 && l >= 7) {             // endpoint
            u16 mp = d[i + 4] | (d[i + 5] << 8);
            OUT("  EP 0x%02x attr 0x%02x pkt %u itv %u\n", d[i + 2], d[i + 3], mp, d[i + 6]);
        } else if (t == 0x24 && cls == 1 && l >= 4) { // class-specific interface
            u8 st = d[i + 2];
            if (sub == 1 && st == 0x0a && l >= 8) {   // UAC2 clock source
                OUT("  ClockSource id %d\n", d[i + 3]);
                if (g_clock_id < 0) g_clock_id = d[i + 3];
            } else if (sub == 2 && st == 0x01 && l >= 16) {   // AS_GENERAL
                OUT("  AS_GENERAL ch %d fmt %02x%02x%02x%02x\n",
                    d[i + 10], d[i + 9], d[i + 8], d[i + 7], d[i + 6]);
            } else if (sub == 2 && st == 0x02 && l >= 6) {    // FORMAT_TYPE
                OUT("  FORMAT subslot %d bytes, %d bits\n", d[i + 4], d[i + 5]);
            }
        }
        i += l;
    }
}

static void dump_config(void) {
    if (g_total < 1) { OUT("no device: press A to scan first\n"); return; }

    UsbHsClientIfSession s;
    Result rc = 1;
    s32 used = -1;
    for (s32 i = 0; i < g_total && i < 16; i++) {
        rc = usbHsAcquireUsbIf(&s, &g_ifs[i]);
        OUT("acquire [%d]: 0x%x\n", i, rc);
        if (R_SUCCEEDED(rc)) { used = i; break; }
    }
    if (used < 0) return;

    u8 *buf = aligned_alloc(0x1000, 0x1000);
    if (!buf) { OUT("alloc failed\n"); usbHsIfClose(&s); return; }
    u32 got = 0;

    memset(buf, 0, 0x1000);
    rc = usbHsIfCtrlXfer(&s, 0x80, 6, 0x0200, 0, 9, buf, &got);
    OUT("get cfg hdr: rc=0x%x got=%u\n", rc, got);
    if (R_SUCCEEDED(rc) && got >= 4) {
        u32 total = buf[2] | (buf[3] << 8);
        if (total > 0x1000) total = 0x1000;
        memset(buf, 0, 0x1000);
        got = 0;
        rc = usbHsIfCtrlXfer(&s, 0x80, 6, 0x0200, 0, (u16)total, buf, &got);
        OUT("get cfg full: rc=0x%x got=%u (want %u)\n", rc, got, total);
        if (R_SUCCEEDED(rc)) {
            hex_to_log(buf, got);
            parse_config(buf, got);
        }
    }

    if (g_clock_id >= 0) {
        u16 idx = (u16)((g_clock_id << 8) | g_ac_if);
        u32 hz = 0;

        memset(buf, 0, 0x1000);
        got = 0;
        rc = usbHsIfCtrlXfer(&s, 0xA1, 0x01, 0x0100, idx, 4, buf, &got);
        memcpy(&hz, buf, 4);
        OUT("cur rate: rc=0x%x %u Hz\n", rc, hz);

        memset(buf, 0, 0x1000);
        got = 0;
        rc = usbHsIfCtrlXfer(&s, 0xA1, 0x02, 0x0100, idx, 2 + 12 * 8, buf, &got);
        OUT("rate range: rc=0x%x got=%u\n", rc, got);
        if (R_SUCCEEDED(rc) && got >= 2) {
            u32 n = buf[0] | (buf[1] << 8);
            for (u32 k = 0; k < n && 2 + 12 * (k + 1) <= got; k++) {
                u32 mn, mx, rs;
                memcpy(&mn, buf + 2 + 12 * k, 4);
                memcpy(&mx, buf + 6 + 12 * k, 4);
                memcpy(&rs, buf + 10 + 12 * k, 4);
                OUT("  range %u..%u step %u\n", mn, mx, rs);
            }
        }
    }

    free(buf);
    usbHsIfClose(&s);
    if (g_log) fflush(g_log);
}

// ---- tone test: 440 Hz sine, 48 kHz / 16-bit / stereo, ~3 s, LOW volume ----
#define STREAM_IF   1      // Audio Streaming (playback)
#define STREAM_ALT  2      // alt 2 = 16-bit stereo, 192-byte packets
#define STREAM_EP   0x02
#define PKT_BYTES   192
#define CLOCK_ID    9
#define CHUNK_PKTS  20     // 20 ms per post
#define CHUNK_BYTES (PKT_BYTES * CHUNK_PKTS)
#define BATCH_PKTS  64     // packets per batch: 64 x 192 = 12288 = 3 pages (0x1000-aligned)
#define BATCH_BYTES (PKT_BYTES * BATCH_PKTS)
#define NBUF        4      // batches kept in flight (64 ms each)

static void fill_sine(s16 *out, int frames, double *phase) {
    const double step = 2.0 * 3.14159265358979 * 440.0 / 48000.0;
    for (int f = 0; f < frames; f++) {
        s16 v = (s16)(sin(*phase) * 2500.0);       // ~8% of full scale
        *phase += step;
        if (*phase > 6.283185307179586) *phase -= 6.283185307179586;
        out[f * 2] = v;
        out[f * 2 + 1] = v;
    }
}

static void tone_test(void) {
    s32 idx = -1;
    for (s32 i = 0; i < g_total && i < 16; i++) {
        struct usb_interface_descriptor *d = &g_ifs[i].inf.interface_desc;
        if (d->bInterfaceClass == 1 && d->bInterfaceSubClass == 2 && d->bInterfaceNumber == STREAM_IF) { idx = i; break; }
    }
    if (idx < 0) { OUT("streaming interface not found: press A to scan first\n"); return; }

    UsbHsClientIfSession s;
    Result rc = usbHsAcquireUsbIf(&s, &g_ifs[idx]);
    OUT("tone: acquire if#%d: 0x%x\n", STREAM_IF, rc);
    if (R_FAILED(rc)) return;

    u8 *buf = aligned_alloc(0x1000, 0x2000);
    if (!buf) { OUT("alloc failed\n"); usbHsIfClose(&s); return; }
    u32 got = 0;

    // 1) sample rate -> 48000 (UAC2 SET_CUR on clock source)
    u32 hz = 48000;
    memcpy(buf, &hz, 4);
    rc = usbHsIfCtrlXfer(&s, 0x21, 0x01, 0x0100, (u16)((CLOCK_ID << 8) | 0), 4, buf, &got);
    OUT("set rate 48000: rc=0x%x\n", rc);

    // 2) select alt setting through usb:hs so Horizon refreshes its endpoint list
    UsbHsInterfaceInfo info;
    memset(&info, 0, sizeof(info));
    rc = usbHsIfSetInterface(&s, &info, STREAM_ALT);
    OUT("set alt %d (usb:hs): rc=0x%x\n", STREAM_ALT, rc);
    if (R_SUCCEEDED(rc)) {
        OUT("  now alt %d, eps %d\n", info.interface_desc.bAlternateSetting, info.interface_desc.bNumEndpoints);
        for (int j = 0; j < 15; j++) {
            struct usb_endpoint_descriptor *e = &info.output_endpoint_descs[j];
            if (e->bLength) OUT("  OUT ep 0x%02x attr 0x%02x pkt %u itv %u\n",
                                e->bEndpointAddress, e->bmAttributes, e->wMaxPacketSize, e->bInterval);
        }
    }

    // 3) open the isochronous OUT endpoint (prefer the descriptor Horizon reports)
    struct usb_endpoint_descriptor desc;
    memset(&desc, 0, sizeof(desc));
    desc.bLength = 7;
    desc.bDescriptorType = 5;
    desc.bEndpointAddress = STREAM_EP;
    desc.bmAttributes = 0x0d;
    desc.wMaxPacketSize = PKT_BYTES;
    desc.bInterval = 1;
    for (int j = 0; j < 15; j++) {
        if (info.output_endpoint_descs[j].bLength && info.output_endpoint_descs[j].bEndpointAddress == STREAM_EP)
            desc = info.output_endpoint_descs[j];
    }

    UsbHsClientEpSession ep;
    u16 urb_try[3] = { 128, 64, 16 };
    for (int k = 0; k < 3; k++) {
        rc = usbHsIfOpenUsbEp(&s, &ep, urb_try[k], BATCH_BYTES, &desc);
        OUT("open ep 0x%02x (maxUrb %u, maxXfer %u): rc=0x%x\n", STREAM_EP, urb_try[k], BATCH_BYTES, rc);
        if (R_SUCCEEDED(rc)) break;
    }

    if (R_SUCCEEDED(rc)) {
        // Batches of 64 one-packet URBs; buffer address and size are 0x1000-aligned.
        u8 *bufs = aligned_alloc(0x1000, (size_t)NBUF * BATCH_BYTES);
        if (!bufs) { OUT("batch alloc failed\n"); }
        else {
            double phase = 0.0;
            u32 urbs[BATCH_PKTS];
            u32 ids[NBUF];
            for (int i = 0; i < BATCH_PKTS; i++) urbs[i] = PKT_BYTES;

            const int total = 47;                   // 47 x 64 ms = ~3 s
            int posted = 0, done = 0, errs = 0, shown = 0;
            Event *ev = usbHsEpGetXferEvent(&ep);

            for (int i = 0; i < NBUF && posted < total; i++) {
                fill_sine((s16 *)(bufs + i * BATCH_BYTES), BATCH_PKTS * PKT_BYTES / 4, &phase);
                Result r = usbHsEpBatchBufferAsync(&ep, bufs + i * BATCH_BYTES, urbs, BATCH_PKTS, 0, 0, 0, &ids[i]);
                OUT("batch %d post: rc=0x%x\n", i, r);
                if (R_FAILED(r)) { errs++; break; }
                posted++;
            }

            while (done < posted && !errs) {
                Result w = eventWait(ev, 500000000ULL);   // 500 ms
                if (R_FAILED(w)) { OUT("event wait: rc=0x%x (posted %d, done %d)\n", w, posted, done); errs++; break; }
                eventClear(ev);

                UsbHsXferReport reps[NBUF * 2];
                u32 n = 0;
                Result r = usbHsEpGetXferReport(&ep, reps, NBUF * 2, &n);
                if (R_FAILED(r)) { OUT("get report: rc=0x%x\n", r); errs++; break; }

                for (u32 k = 0; k < n; k++) {
                    if (shown < 6) { OUT("report: res=0x%x size=%u\n", reps[k].res, reps[k].transferredSize); shown++; }
                    if (R_FAILED(reps[k].res)) { OUT("xfer failed: rc=0x%x\n", reps[k].res); errs++; }
                    done++;
                    if (posted < total) {
                        for (int sl = 0; sl < NBUF; sl++) {
                            if (ids[sl] != reps[k].xferId) continue;
                            fill_sine((s16 *)(bufs + sl * BATCH_BYTES), BATCH_PKTS * PKT_BYTES / 4, &phase);
                            Result pr = usbHsEpBatchBufferAsync(&ep, bufs + sl * BATCH_BYTES, urbs, BATCH_PKTS, 0, 0, 0, &ids[sl]);
                            if (R_FAILED(pr)) { OUT("repost: rc=0x%x\n", pr); errs++; }
                            else posted++;
                            break;
                        }
                    }
                }
            }
            OUT("tone done: batches posted=%d done=%d errs=%d\n", posted, done, errs);
            free(bufs);
        }
        usbHsEpClose(&ep);
    }

    // back to alt 0 (no bandwidth)
    usbHsIfSetInterface(&s, &info, 0);
    free(buf);
    usbHsIfClose(&s);
    if (g_log) fflush(g_log);
}

int main(void) {
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    g_log = fopen("sdmc:/usbaudio_probe.txt", "w");

    Result rc = usbHsInitialize();
    OUT("usbHsInitialize: 0x%x\n", rc);
    OUT("A = rescan, X = dump config, Y = 3s test tone (low vol), + = exit\n");
    if (R_SUCCEEDED(rc)) scan();

    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 down = padGetButtonsDown(&pad);
        if (down & HidNpadButton_Plus) break;
        if (R_SUCCEEDED(rc)) {
            if (down & HidNpadButton_A) scan();
            if (down & HidNpadButton_X) dump_config();
            if (down & HidNpadButton_Y) tone_test();
        }
        consoleUpdate(NULL);
    }

    if (R_SUCCEEDED(rc)) usbHsExit();
    if (g_log) fclose(g_log);
    consoleExit(NULL);
    return 0;
}
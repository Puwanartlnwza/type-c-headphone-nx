// USB Audio Probe v2
// A = rescan interfaces, X = acquire device and dump full config descriptor
// (hex + parsed alt settings / endpoints / formats / clock + sample rates),
// + = exit. Everything is also written to sdmc:/usbaudio_probe.txt
#include <stdio.h>
#include <stdlib.h>
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

int main(void) {
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    g_log = fopen("sdmc:/usbaudio_probe.txt", "w");

    Result rc = usbHsInitialize();
    OUT("usbHsInitialize: 0x%x\n", rc);
    OUT("A = rescan, X = dump config, + = exit\n");
    if (R_SUCCEEDED(rc)) scan();

    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 down = padGetButtonsDown(&pad);
        if (down & HidNpadButton_Plus) break;
        if (R_SUCCEEDED(rc)) {
            if (down & HidNpadButton_A) scan();
            if (down & HidNpadButton_X) dump_config();
        }
        consoleUpdate(NULL);
    }

    if (R_SUCCEEDED(rc)) usbHsExit();
    if (g_log) fclose(g_log);
    consoleExit(NULL);
    return 0;
}
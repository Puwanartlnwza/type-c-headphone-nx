#include <string.h>
#include "usbout.h"

static u8 g_bufs[UO_NBUF * UO_STRIDE] __attribute__((aligned(0x1000)));
static u8 g_ctl[0x1000]               __attribute__((aligned(0x1000)));
static UsbHsInterface   g_ifs[16];
static UsbHsXferReport  g_reps[UO_NBUF * UO_BATCH];

static void fill_batch(UsbOut *u, int sl, Ring *ring, u32 gain) {
    u8 *dst = g_bufs + sl * UO_STRIDE;
    const u32 want = UO_BATCH * UO_PKT;
    u32 got = ring_pop(ring, dst, want);
    if (got < want) { memset(dst + got, 0, want - got); u->underruns++; }
    if (gain < 100) {
        s16 *p = (s16 *)dst;
        for (u32 i = 0; i < want / 2; i++) p[i] = (s16)(((s32)p[i] * (s32)gain) / 100);
    }
}

static Result post_batch(UsbOut *u, int sl) {
    // The start frame goes in unk2 (found experimentally); batches are scheduled back to back.
    u32 frame = u->frame0 + 8 + u->seq * UO_BATCH;
    u->seq++;
    return usbHsEpBatchBufferAsync(&u->ep, g_bufs + sl * UO_STRIDE, u->urbs, UO_BATCH, 0, 0, frame, &u->ids[sl]);
}

Result usbout_open(UsbOut *u, Ring *ring, u32 gain) {
    memset(u, 0, sizeof(*u));

    UsbHsInterfaceFilter filter;
    memset(&filter, 0, sizeof(filter));
    memset(g_ifs, 0, sizeof(g_ifs));
    s32 total = 0;
    Result rc = usbHsQueryAvailableInterfaces(&filter, g_ifs, sizeof(g_ifs), &total);
    if (R_FAILED(rc)) return rc;

    int idx = -1;
    for (s32 i = 0; i < total && i < 16; i++) {
        struct usb_interface_descriptor *d = &g_ifs[i].inf.interface_desc;
        if (g_ifs[i].device_desc.idVendor == UO_VID && g_ifs[i].device_desc.idProduct == UO_PID &&
            d->bInterfaceClass == 1 && d->bInterfaceSubClass == 2 && d->bInterfaceNumber == UO_IF) { idx = i; break; }
    }
    if (idx < 0) return UO_NOT_FOUND;

    rc = usbHsAcquireUsbIf(&u->s, &g_ifs[idx]);
    if (R_FAILED(rc)) return rc;
    u->if_open = true;

    u32 hz = 48000, got = 0;
    memcpy(g_ctl, &hz, 4);
    rc = usbHsIfCtrlXfer(&u->s, 0x21, 0x01, 0x0100, (u16)((UO_CLOCK << 8) | 0), 4, g_ctl, &got);
    if (R_FAILED(rc)) return rc;

    memset(&u->info, 0, sizeof(u->info));
    rc = usbHsIfSetInterface(&u->s, &u->info, UO_ALT);
    if (R_FAILED(rc)) return rc;

    struct usb_endpoint_descriptor desc;
    memset(&desc, 0, sizeof(desc));
    desc.bLength = 7; desc.bDescriptorType = 5; desc.bEndpointAddress = UO_EP;
    desc.bmAttributes = 0x0d; desc.wMaxPacketSize = UO_PKT; desc.bInterval = 1;
    for (int j = 0; j < 15; j++) {
        if (u->info.output_endpoint_descs[j].bLength && u->info.output_endpoint_descs[j].bEndpointAddress == UO_EP)
            desc = u->info.output_endpoint_descs[j];
    }

    rc = usbHsIfOpenUsbEp(&u->s, &u->ep, 16, UO_BATCH * UO_PKT, &desc);
    if (R_FAILED(rc)) return rc;
    u->ep_open = true;
    u->ev = usbHsEpGetXferEvent(&u->ep);

    rc = usbHsIfGetCurrentFrame(&u->s, &u->frame0);
    if (R_FAILED(rc)) return rc;

    for (int i = 0; i < UO_BATCH; i++) u->urbs[i] = UO_PKT;
    for (int sl = 0; sl < UO_NBUF; sl++) {
        fill_batch(u, sl, ring, gain);
        rc = post_batch(u, sl);
        if (R_FAILED(rc)) return rc;
    }
    return 0;
}

Result usbout_pump(UsbOut *u, Ring *ring, u32 gain) {
    for (;;) {
        u32 n = 0;
        Result rc = usbHsEpGetXferReport(&u->ep, g_reps, UO_NBUF * UO_BATCH, &n);
        if (R_FAILED(rc)) return rc;
        if (n == 0) return 0;

        for (u32 k = 0; k < n; k++) {
            if (R_FAILED(g_reps[k].res)) return g_reps[k].res;
            for (int sl = 0; sl < UO_NBUF; sl++) {
                if (u->ids[sl] != g_reps[k].xferId) continue;
                if (++u->cnt[sl] == UO_BATCH) {          // reports arrive per URB
                    u->cnt[sl] = 0;
                    u->batches++;
                    fill_batch(u, sl, ring, gain);
                    rc = post_batch(u, sl);
                    if (R_FAILED(rc)) return rc;
                }
                break;
            }
        }
    }
}

void usbout_close(UsbOut *u, bool reset_device) {
    if (u->ep_open) { usbHsEpClose(&u->ep); u->ep_open = false; }
    if (u->if_open) {
        usbHsIfSetInterface(&u->s, &u->info, 0);
        if (reset_device) usbHsIfResetDevice(&u->s);
        usbHsIfClose(&u->s);
        u->if_open = false;
    }
}

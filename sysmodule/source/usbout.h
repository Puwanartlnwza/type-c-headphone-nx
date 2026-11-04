#pragma once
#include <switch.h>
#include "ring.h"

// Apple USB-C EarPods (UAC2): playback = interface 1, alt 2 (16-bit stereo), endpoint 0x02, clock source 9
#define UO_VID     0x05ac
#define UO_PID     0x110b
#define UO_IF      1
#define UO_ALT     2
#define UO_EP      0x02
#define UO_CLOCK   9
#define UO_PKT     192            // bytes per 1 ms packet (48 frames x 2 ch x 2 bytes)
#define UO_BATCH   16             // URBs (packets) per batch; usb:hs rejected 64
#define UO_NBUF    4              // batches in flight
#define UO_STRIDE  0x1000         // every batch buffer starts on its own page

typedef struct {
    UsbHsClientIfSession s;
    UsbHsClientEpSession ep;
    UsbHsInterfaceInfo   info;
    bool if_open, ep_open;
    u32  frame0, seq;
    u32  ids[UO_NBUF];
    int  cnt[UO_NBUF];
    u32  urbs[UO_BATCH];
    u32  underruns, batches;
    Event *ev;
} UsbOut;

#define UO_NOT_FOUND MAKERESULT(Module_Libnx, LibnxError_NotFound)

Result usbout_open(UsbOut *u, Ring *ring, u32 gain_pct);
Result usbout_pump(UsbOut *u, Ring *ring, u32 gain_pct);
void   usbout_close(UsbOut *u, bool reset_device);

// USB Audio Probe: lists every USB interface Horizon sees via usb:hs
// and logs it to the screen and sdmc:/usbaudio_probe.txt.
// A = rescan, + = exit. Plug the EarPods in first, then press A.
#include <stdio.h>
#include <string.h>
#include <switch.h>

static FILE *g_log;

#define OUT(...) do { printf(__VA_ARGS__); if (g_log) fprintf(g_log, __VA_ARGS__); } while (0)

static void scan(void) {
    UsbHsInterfaceFilter filter;
    UsbHsInterface ifs[16];
    s32 total = 0;

    memset(&filter, 0, sizeof(filter));   // Flags = 0: match everything
    memset(ifs, 0, sizeof(ifs));

    Result rc = usbHsQueryAvailableInterfaces(&filter, ifs, sizeof(ifs), &total);
    OUT("\n--- scan: rc=0x%x, interfaces=%d ---\n", rc, total);
    if (R_FAILED(rc)) return;

    for (s32 i = 0; i < total && i < 16; i++) {
        struct usb_interface_descriptor *d = &ifs[i].inf.interface_desc;
        OUT("[%d] VID %04x PID %04x | if#%d alt%d class %02x sub %02x proto %02x\n",
            i, ifs[i].device_desc.idVendor, ifs[i].device_desc.idProduct,
            d->bInterfaceNumber, d->bAlternateSetting,
            d->bInterfaceClass, d->bInterfaceSubClass, d->bInterfaceProtocol);

        for (int j = 0; j < 15; j++) {
            struct usb_endpoint_descriptor *e = &ifs[i].inf.output_endpoint_desc[j];
            if (e->bLength) OUT("    OUT ep 0x%02x attr 0x%02x maxpkt %u interval %u\n",
                                e->bEndpointAddress, e->bmAttributes, e->wMaxPacketSize, e->bInterval);
        }
        for (int j = 0; j < 15; j++) {
            struct usb_endpoint_descriptor *e = &ifs[i].inf.input_endpoint_desc[j];
            if (e->bLength) OUT("    IN  ep 0x%02x attr 0x%02x maxpkt %u interval %u\n",
                                e->bEndpointAddress, e->bmAttributes, e->wMaxPacketSize, e->bInterval);
        }
    }
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
    OUT("A = rescan, + = exit\n");
    if (R_SUCCEEDED(rc)) scan();

    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 down = padGetButtonsDown(&pad);
        if (down & HidNpadButton_Plus) break;
        if (down & HidNpadButton_A && R_SUCCEEDED(rc)) scan();
        consoleUpdate(NULL);
    }

    if (R_SUCCEEDED(rc)) usbHsExit();
    if (g_log) fclose(g_log);
    consoleExit(NULL);
    return 0;
}
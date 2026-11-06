#include <string.h>
#include "audrec.h"

// audrec:u = nn::audio::detail::IFinalOutputRecorderManager (see switchbrew "Audio services")

Result audrec_open(AudRec *r, u32 sample_rate, u64 aruid) {
    memset(r, 0, sizeof(*r));

    Service mgr;
    Result rc = smGetService(&mgr, "audrec:u");
    if (R_FAILED(rc)) return rc;

    // FinalOutputRecorderParameter (u32 sample rate), process handle, AppletResourceUserId
    const struct { u32 sample_rate; u32 pad; u64 aruid; } in = { sample_rate, 0, aruid };
    struct { u32 sample_rate, channels, format, state; } out;
    memset(&out, 0, sizeof(out));

    rc = serviceDispatchInOut(&mgr, 0, in, out,
        .in_num_handles = 1,
        .in_handles = { CUR_PROCESS_HANDLE },
        .out_num_objects = 1,
        .out_objects = &r->s,
    );
    serviceClose(&mgr);
    if (R_FAILED(rc)) return rc;

    r->sample_rate = out.sample_rate;
    r->channels    = out.channels;
    r->format      = out.format;
    r->state       = out.state;

    // RegisterBufferEvent (cmd 4)
    Handle h = INVALID_HANDLE;
    rc = serviceDispatch(&r->s, 4,
        .out_handle_attrs = { SfOutHandleAttr_HipcCopy },
        .out_handles = &h,
    );
    if (R_SUCCEEDED(rc)) eventLoadRemote(&r->ev, h, true);
    else serviceClose(&r->s);
    return rc;
}

Result audrec_start(AudRec *r) { return serviceDispatch(&r->s, 1); }

void audrec_stop(AudRec *r) { serviceDispatch(&r->s, 2); }

Result audrec_append(AudRec *r, AudRecBuffer *b) {
    // AppendFinalOutputRecorderBufferAuto (cmd 8)
    u64 client_ptr = (u64)(uintptr_t)b;
    return serviceDispatchIn(&r->s, 8, client_ptr,
        .buffer_attrs = { SfBufferAttr_HipcAutoSelect | SfBufferAttr_In },
        .buffers = { { b, sizeof(*b) } },
    );
}

Result audrec_released(AudRec *r, AudRecBuffer **out, u32 max, u32 *count) {
    // GetReleasedFinalOutputRecorderBuffersAuto (cmd 9)
    u64 ids[8];
    if (max > 8) max = 8;
    struct { u64 count; u64 released; } o;
    memset(&o, 0, sizeof(o));
    Result rc = serviceDispatchOut(&r->s, 9, o,
        .buffer_attrs = { SfBufferAttr_HipcAutoSelect | SfBufferAttr_Out },
        .buffers = { { ids, sizeof(ids) } },
    );
    if (R_FAILED(rc)) { *count = 0; return rc; }
    u32 n = (u32)o.count;
    if (n > max) n = max;
    for (u32 i = 0; i < n; i++) out[i] = (AudRecBuffer *)(uintptr_t)ids[i];
    *count = n;
    return 0;
}

void audrec_close(AudRec *r) {
    eventClose(&r->ev);
    serviceClose(&r->s);
}

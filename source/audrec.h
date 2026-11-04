#pragma once
#include <switch.h>

// nn::audio::FinalOutputRecorderBuffer (0x30 bytes)
typedef struct {
    u64 released;
    u64 next;
    u64 sample_buffer;
    u64 capacity;
    u64 size;
    u64 offset;
} AudRecBuffer;

typedef struct {
    Service s;
    Event   ev;
    u32 sample_rate, channels, format, state;   // FinalOutputRecorderParameterInternal
} AudRec;

Result audrec_open(AudRec *r, u32 sample_rate);
Result audrec_start(AudRec *r);
void   audrec_stop(AudRec *r);
Result audrec_append(AudRec *r, AudRecBuffer *b);
Result audrec_released(AudRec *r, AudRecBuffer **out, u32 max, u32 *count);
void   audrec_close(AudRec *r);

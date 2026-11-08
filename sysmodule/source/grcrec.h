#pragma once
// Game audio for retail games comes from grc:d (the console's game-recording service, as SysDVR uses it).
// Homebrew / audren apps come from audrec:u. Both push into the same ring; arb_accept() lets only the
// source that currently has sound feed it.
#include <switch.h>
#include "ring.h"

enum { SRC_GRC = 0, SRC_REC = 1 };

extern volatile s32 g_grc_peak;               // loudest grc sample since the last status write
bool arb_accept(int src, s32 peak);           // true if this chunk may go into the ring

void grcrec_start(Ring *ring);                // (re)start a worker for the current application
void grcrec_stop(void);                       // let running workers retire

/* Host (high-level) replacements for Wii SDK functions. Each one defines the recompiled
 * function's symbol (via FN_<name> from names.h), overriding the weak generated version. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"
#include "names.h"

#define HLE(name) void FN_##name(CPU *c)
#define STUB(name, val) HLE(name) { c->r[3] = (u32)(val); }
#define LOG(...) do { if (getenv("RT_HLELOG")) fprintf(stderr, __VA_ARGS__); } while (0)

static const char *g_disc_root;

void hle_init(const char *disc_root) {
    g_disc_root = disc_root;
    if (disc_root && disc_init(disc_root)) {
        u32 at = disc_load_fst();
        mw32(0x80000038u, at);              /* BootInfo->FSTLocation */
        mw32(0x8000003Cu, disc_fst_size()); /* FSTMaxLength */
        mw32(0x80000034u, at);              /* arena hi ends below the FST */
        mw32(0x80003110u, at);
    }
}

void guest_call(CPU *c, u32 fn, u32 a0, u32 a1) {
    if (!fn)
        return;
    u32 lr = c->lr;
    c->r[3] = a0;
    c->r[4] = a1;
    call_indirect(c, fn);
    c->lr = lr;
}

/* ---- EXI: memory cards, the RTC/SRAM chip and USB Gecko live here; nothing is attached */
STUB(EXIInit, 0)
STUB(EXIGetID, 0)          /* "no device" */
STUB(EXIProbe, 0)
STUB(EXIProbeEx, -1)
STUB(EXILock, 0)
STUB(EXIUnlock, 1)
STUB(EXISelect, 0)
STUB(EXIDeselect, 1)
STUB(EXIImm, 0)
STUB(EXIImmEx, 0)
STUB(EXIDma, 0)
STUB(EXISync, 0)
STUB(EXIAttach, 0)
STUB(EXIDetach, 1)

/* ---- OSInit: hardware bring-up the host does not need */
HLE(__OSInitAudioSystem) { (void)c; }      /* DSP boot + AI setup */
HLE(__OSInitSTM) { (void)c; }              /* IOS state-transition manager (power/reset buttons) */
HLE(__OSInitNet) { (void)c; }
HLE(__OSStartPlayRecord) { (void)c; }
HLE(__OSInitSram) { (void)c; }             /* RTC SRAM over EXI */
HLE(EnableMetroTRKInterrupts) { (void)c; }
HLE(SIInit) { (void)c; }                   /* GameCube controller serial bus */
HLE(IPCCltInit) { c->r[3] = 0; }

/* ---- IOS IPC: no devices yet; every open fails with IPC_ENOENT */
HLE(IOS_Open) { LOG("IOS_Open(%s)\n", (char *)MEM + c->r[3]); c->r[3] = (u32)-6; }
HLE(IOS_OpenAsync) { LOG("IOS_OpenAsync(%s)\n", (char *)MEM + c->r[3]); c->r[3] = (u32)-6; }

/* ---- DVD: the SDK's file layer (DVDOpen, FST lookup) runs as recompiled code; the drive is ours */
void FN___DVDFSInit(CPU *c);
HLE(DVDInit) { FN___DVDFSInit(c); }
HLE(DVDReadAbsAsyncPrio) {
    u32 block = c->r[3], dst = c->r[4], len = c->r[5], off = c->r[6], cb = c->r[7];
    LOG("DVDRead %08X <- disc %llx +%x\n", dst, (unsigned long long)off << 2, len);
    u32 n = disc_read((u64)off << 2, dst, len);
    mw32(block + 0x0C, 0);   /* state: idle */
    mw32(block + 0x20, n);   /* transferTotal */
    guest_call(c, cb, n, block);
    c->r[3] = 1;
}
HLE(DVDInquiryAsync) {
    u32 block = c->r[3], info = c->r[4], cb = c->r[5];
    memset(MEM + info, 0, 0x20);
    mw16(info + 2, 0x0002);
    mw32(block + 0x0C, 0);
    guest_call(c, cb, 0x20, block);
    c->r[3] = 1;
}
STUB(DVDGetDriveStatus, 0)
STUB(DVDGetCommandBlockStatus, 0)
STUB(DVDCancel, 0)
STUB(DVDCancelAsync, 1)
STUB(DVDCompareDiskID, 1)
STUB(__DVDCheckDevice, 1)
STUB(DVDSetAutoFatalMessaging, 0)

/* ---- audio: AI/DSP inert for now (silence); AX runs recompiled and simply never gets a DSP frame */
HLE(AIInit) { (void)c; }
HLE(AIInitDMA) { (void)c; }
STUB(AIGetDMAStartAddr, 0)
STUB(DSPCheckInit, 1)
HLE(DSPInit) { (void)c; }
STUB(DSPCheckMailToDSP, 0)
STUB(DSPCheckMailFromDSP, 0)
STUB(DSPReadMailFromDSP, 0)
HLE(DSPSendMailToDSP) { (void)c; }
HLE(DSPAssertTask) { (void)c; }
HLE(DSPAddTask) { (void)c; }
HLE(__AXOutInitDSP) { (void)c; }     /* boots the AX microcode and waits for it */

/* ---- GX: the command stream is decoded as it is written; drain it where GX waits on the GPU */
#include "gpu.h"
void FN_GXDrawDone(CPU *c);

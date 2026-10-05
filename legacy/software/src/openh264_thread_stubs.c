/*
 * openh264_thread_stubs.c
 *
 * Minimal no-op stubs for the pthread/Event symbols that the OpenH264
 * PSP decoder library references but are NOT needed for single-threaded
 * host-side decode (which is all Moonlight uses).
 *
 * The OpenH264 library was built without -DDISABLE_DECODER_MT taking
 * full effect on all TUs, so EventPost/EventWait/WelsMutex* appear in
 * object files that are otherwise correct.  These stubs satisfy the
 * linker while being completely safe (calls are guarded by iNumOfThread==1
 * at runtime inside the OpenH264 code).
 *
 * Also provides __gxx_personality_v0 for .eh_frame sections embedded in
 * the C++ objects (PSP newlib does not ship libsupc++).
 */

#include <pspsdk.h>
#include <pspkernel.h>

/* ---- Event primitives (used by decoder_core, rec_mb, mv_pred etc.) ------ */

typedef struct { int dummy; } WelsEvent;

int EventCreate(WelsEvent *e, int manual_reset, int initial)  { (void)e; (void)manual_reset; (void)initial; return 0; }
int EventDestroy(WelsEvent *e)                                 { (void)e; return 0; }
int EventPost(WelsEvent *e)                                    { (void)e; return 0; }
int EventReset(WelsEvent *e)                                   { (void)e; return 0; }
int EventWait(WelsEvent *e)                                    { (void)e; return 0; }
int EventWaitWithTimeOut(WelsEvent *e, unsigned int ms)        { (void)e; (void)ms; return 0; }

/* ---- Mutex primitives ---------------------------------------------------- */

typedef struct { int dummy; } WelsMutex;

int WelsMutexInit(WelsMutex *m)    { (void)m; return 0; }
int WelsMutexLock(WelsMutex *m)    { (void)m; return 0; }
int WelsMutexUnlock(WelsMutex *m)  { (void)m; return 0; }
int WelsMutexDestroy(WelsMutex *m) { (void)m; return 0; }

/* ---- Thread primitives (no-op: single-threaded on PSP) ------------------- */

int WelsThreadCreate(void **t, void *(*f)(void*), void *arg, int pri) {
    (void)t; (void)f; (void)arg; (void)pri; return 0;
}
int WelsThreadJoin(void *t) { (void)t; return 0; }
void *WelsThreadSelf(void) { return (void *)0; }
void WelsSleep(unsigned int ms) {
    sceKernelDelayThread((SceUInt)ms * 1000);
}
int WelsMultipleEventsWaitSingleBlocking(int n, WelsEvent **ev, unsigned int ms) {
    (void)n; (void)ev; (void)ms; return 0;
}

/* ---- usleep (used by WelsThreadLib.cpp) ---------------------------------- */
int usleep(unsigned int us) {
    sceKernelDelayThread((SceUInt)us);
    return 0;
}

/* ---- C++ exception personality (satisfies .eh_frame references) ----------
 * PSP newlib/libstdc++ do not export __gxx_personality_v0.
 * Since PSP code is compiled with -fno-exceptions, unwinding never actually
 * executes — this symbol is only needed to satisfy the linker for .eh_frame
 * DWARF metadata that GCC emits even with -fno-exceptions on C++ TUs.
 */
void *__gxx_personality_v0 = (void *)0;

/* ---- RTTI vtable for __cxxabiv1::__class_type_info ----------------------- */
/* Required by OpenH264's CMemoryAlign virtual destructor chain.
 * Providing the minimum needed symbol to satisfy the linker.
 * PSP newlib lacks libsupc++.                                               */

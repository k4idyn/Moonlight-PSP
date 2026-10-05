/*
 * psp_thread_stubs.cpp — PSP-native replacements for OpenH264 threading
 *
 * Replaces: WelsThreadLib.cpp, WelsThread.cpp, WelsTaskThread.cpp, WelsThreadPool.cpp
 *
 * Single-threaded PSP decoder: all threading functions are no-ops or
 * trivial PSP sceKernel wrappers.  m_iThreadCount is always 0 due to
 * DISABLE_DECODER_MT, so none of the multi-thread code paths execute.
 * These stubs exist solely to satisfy the linker.
 */

#include <pspkernel.h>
#include <pspthreadman.h>
#include <string.h>

/* Pull in the OpenH264 type definitions via the injected shim */
#include "WelsThreadLib.h"

extern "C" {

/* ---- Mutex (no-ops: single-threaded PSP decoder) ------------------------- */

WELS_THREAD_ERROR_CODE WelsMutexInit(WELS_MUTEX *mutex) {
    if (!mutex) return WELS_THREAD_ERROR_GENERAL;
    *mutex = 0;
    return WELS_THREAD_ERROR_OK;
}

WELS_THREAD_ERROR_CODE WelsMutexLock(WELS_MUTEX *mutex) {
    (void)mutex;
    return WELS_THREAD_ERROR_OK;
}

WELS_THREAD_ERROR_CODE WelsMutexUnlock(WELS_MUTEX *mutex) {
    (void)mutex;
    return WELS_THREAD_ERROR_OK;
}

WELS_THREAD_ERROR_CODE WelsMutexDestroy(WELS_MUTEX *mutex) {
    if (mutex) *mutex = -1;
    return WELS_THREAD_ERROR_OK;
}

/* ---- Events (backed by PSP semaphore) ----------------------------------- */

WELS_THREAD_ERROR_CODE WelsEventOpen(WELS_EVENT *p_event, const char *event_name) {
    (void)event_name;
    if (!p_event) return WELS_THREAD_ERROR_GENERAL;
    *p_event = sceKernelCreateSema("oh264_evt", 0, 0, 1, NULL);
    return (*p_event >= 0) ? WELS_THREAD_ERROR_OK : WELS_THREAD_ERROR_GENERAL;
}

WELS_THREAD_ERROR_CODE WelsEventClose(WELS_EVENT *event, const char *event_name) {
    (void)event_name;
    if (!event || *event < 0) return WELS_THREAD_ERROR_GENERAL;
    sceKernelDeleteSema(*event);
    *event = -1;
    return WELS_THREAD_ERROR_OK;
}

WELS_THREAD_ERROR_CODE WelsEventSignal(WELS_EVENT *event, WELS_MUTEX *pMutex, int *iCondition) {
    (void)pMutex;
    if (iCondition) *iCondition = 1;
    if (event && *event >= 0) sceKernelSignalSema(*event, 1);
    return WELS_THREAD_ERROR_OK;
}

WELS_THREAD_ERROR_CODE WelsEventWait(WELS_EVENT *event, WELS_MUTEX *pMutex, int &iCondition) {
    (void)pMutex;
    if (iCondition) return WELS_THREAD_ERROR_OK;
    if (event && *event >= 0) {
        SceUInt t = 50000; /* 50ms timeout — avoid deadlock on single-threaded PSP */
        sceKernelWaitSema(*event, 1, &t);
    }
    iCondition = 1;
    return WELS_THREAD_ERROR_OK;
}

WELS_THREAD_ERROR_CODE WelsEventWaitWithTimeOut(WELS_EVENT *event, uint32_t dwMilliseconds,
                                                 WELS_MUTEX *pMutex) {
    (void)pMutex;
    if (event && *event >= 0) {
        SceUInt t = dwMilliseconds * 1000;
        sceKernelWaitSema(*event, 1, &t);
    }
    return WELS_THREAD_ERROR_OK;
}

WELS_THREAD_ERROR_CODE WelsMultipleEventsWaitSingleBlocking(uint32_t nCount,
                                                             WELS_EVENT *event_list,
                                                             WELS_EVENT *master_event,
                                                             WELS_MUTEX *pMutex) {
    (void)nCount; (void)event_list; (void)master_event; (void)pMutex;
    return WELS_THREAD_ERROR_OK;
}

/* ---- Thread management --------------------------------------------------- */

WELS_THREAD_ERROR_CODE WelsThreadCreate(WELS_THREAD_HANDLE *thread,
                                         LPWELS_THREAD_ROUTINE routine,
                                         void *arg, WELS_THREAD_ATTR attr) {
    (void)routine; (void)arg; (void)attr;
    if (thread) *thread = -1;
    return WELS_THREAD_ERROR_OK;
}

WELS_THREAD_ERROR_CODE WelsThreadSetName(const char *thread_name) {
    (void)thread_name;
    return WELS_THREAD_ERROR_OK;
}

WELS_THREAD_ERROR_CODE WelsThreadJoin(WELS_THREAD_HANDLE thread) {
    (void)thread;
    return WELS_THREAD_ERROR_OK;
}

WELS_THREAD_HANDLE WelsThreadSelf() {
    return (WELS_THREAD_HANDLE)sceKernelGetThreadId();
}

WELS_THREAD_ERROR_CODE WelsQueryLogicalProcessInfo(WelsLogicalProcessInfo *pInfo) {
    if (pInfo) pInfo->ProcessorCount = 1;
    return WELS_THREAD_ERROR_OK;
}

void WelsSleep(uint32_t dwMilliSecond) {
    sceKernelDelayThread(dwMilliSecond * 1000);
}

} /* extern "C" */

/* ============================================================================
 * Stubs for decoder-internal threading symbols
 *
 * These are referenced by decoder_core.cpp, decode_slice.cpp, rec_mb.cpp,
 * mv_pred.cpp, pic_queue.cpp, welsDecoderExt.cpp even when
 * DISABLE_DECODER_MT strips the call-sites in some TUs.
 * ============================================================================ */

extern "C" {

/* From wels_decoder_thread.h macros → EventPost/EventWait/EventCreate etc. */
int EventCreate(void *e, int manual_reset, int initial) {
    (void)e; (void)manual_reset; (void)initial;
    return 0;
}
int EventDestroy(void *e) { (void)e; return 0; }
int EventPost(void *e)    { (void)e; return 0; }
int EventReset(void *e)   { (void)e; return 0; }
int EventWait(void *e)    { (void)e; return 0; }

/* Sem* family used by welsDecoderExt.cpp thread control */
int SemCreate(void *s, int init, int max) { (void)s; (void)init; (void)max; return 0; }
int SemWait(void *s, int timeout)         { (void)s; (void)timeout; return 0; }
int SemRelease(void *s)                   { (void)s; return 0; }
int SemDestroy(void *s)                   { (void)s; return 0; }

/* ThreadCreate/ThreadWait used by welsDecoderExt.cpp */
int ThreadCreate(void *t, void *(*f)(void*), void *arg) {
    (void)t; (void)f; (void)arg; return 0;
}
int ThreadWait(void *t) { (void)t; return 0; }

/* GetCPUCount used by CWelsDecoder constructor */
int GetCPUCount(void) { return 1; }

/* ---- pthread stubs (all no-ops: DISABLE_DECODER_MT means no threading) ---- */

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a) {
    (void)a;
    if (m) *m = 0;
    return 0;
}
int pthread_mutex_destroy(pthread_mutex_t *m) {
    if (m) *m = -1;
    return 0;
}
int pthread_mutex_lock(pthread_mutex_t *m)   { (void)m; return 0; }
int pthread_mutex_unlock(pthread_mutex_t *m) { (void)m; return 0; }

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a) {
    (void)a;
    if (c) *c = 0;
    return 0;
}
int pthread_cond_destroy(pthread_cond_t *c) {
    if (c) *c = -1;
    return 0;
}
int pthread_cond_signal(pthread_cond_t *c)    { (void)c; return 0; }
int pthread_cond_broadcast(pthread_cond_t *c) { (void)c; return 0; }
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
    (void)c; (void)m;
    return 0;
}
int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *abstime) {
    (void)c; (void)m; (void)abstime;
    return 0;
}

int pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*f)(void*), void *arg) {
    (void)a; (void)f; (void)arg;
    if (t) *t = -1;
    return 0;
}
int pthread_join(pthread_t t, void **r) { (void)t; (void)r; return 0; }
pthread_t pthread_self(void) { return sceKernelGetThreadId(); }
int pthread_attr_init(pthread_attr_t *a) { (void)a; return 0; }
int pthread_attr_destroy(pthread_attr_t *a) { (void)a; return 0; }

} /* extern "C" */

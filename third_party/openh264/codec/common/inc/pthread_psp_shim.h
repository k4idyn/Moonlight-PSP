/*
 * pthread_psp_shim.h - Minimal pthread type shims for PSP / psp-gcc
 *
 * OpenH264's WelsThreadLib.h requires pthread_t, pthread_mutex_t, and
 * pthread_cond_t to be defined.  PSP newlib (psp-gcc 4.3.5) does not
 * ship a POSIX pthread header, so we provide bare-minimum type aliases
 * backed by PSP kernel primitives.  These types are ONLY used to satisfy
 * header-level type completeness; the actual thread functions are stubbed
 * in openh264_thread_stubs.c.
 *
 * This header is intended to be placed (or -include'd) before including
 * WelsThreadLib.h.
 */

#ifndef PTHREAD_PSP_SHIM_H
#define PTHREAD_PSP_SHIM_H

#include <pspkernel.h>
#include <pspthreadman.h>

/* ---- Thread handle ------------------------------------------------------- */
typedef SceUID pthread_t;

/* ---- Mutex --------------------------------------------------------------- */
typedef SceUID pthread_mutex_t;
typedef int    pthread_mutexattr_t;

static inline int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a) {
    (void)a;
    SceUID uid = sceKernelCreateMutex("wels_mutex", 0, 0, NULL);
    if (uid >= 0) { *m = uid; return 0; }
    return -1;
}
static inline int pthread_mutex_destroy(pthread_mutex_t *m) {
    if (*m >= 0) sceKernelDeleteMutex(*m);
    return 0;
}
static inline int pthread_mutex_lock(pthread_mutex_t *m) {
    return sceKernelLockMutex(*m, 1, NULL) >= 0 ? 0 : -1;
}
static inline int pthread_mutex_unlock(pthread_mutex_t *m) {
    return sceKernelUnlockMutex(*m, 1) >= 0 ? 0 : -1;
}

/* ---- Condition variable -------------------------------------------------- */
/* PSP has no native cond var; use a semaphore as a simple substitute. */
typedef SceUID pthread_cond_t;
typedef int    pthread_condattr_t;

static inline int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a) {
    (void)a;
    SceUID uid = sceKernelCreateSema("wels_cond", 0, 0, 0x7FFFFFFF, NULL);
    if (uid >= 0) { *c = uid; return 0; }
    return -1;
}
static inline int pthread_cond_destroy(pthread_cond_t *c) {
    if (*c >= 0) sceKernelDeleteSema(*c);
    return 0;
}
static inline int pthread_cond_signal(pthread_cond_t *c) {
    sceKernelSignalSema(*c, 1);
    return 0;
}
static inline int pthread_cond_broadcast(pthread_cond_t *c) {
    sceKernelSignalSema(*c, 1);
    return 0;
}
static inline int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
    pthread_mutex_unlock(m);
    SceUInt t = 50000; /* 50ms timeout to avoid deadlock on PSP */
    sceKernelWaitSema(*c, 1, &t);
    pthread_mutex_lock(m);
    return 0;
}
static inline int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m,
                                          const void *abstime) {
    (void)abstime;
    return pthread_cond_wait(c, m);
}

/* ---- Thread management --------------------------------------------------- */
typedef int pthread_attr_t;

static inline int pthread_attr_init(pthread_attr_t *a) { (void)a; return 0; }
static inline int pthread_attr_destroy(pthread_attr_t *a) { (void)a; return 0; }
static inline int pthread_create(pthread_t *t, const pthread_attr_t *a,
                                  void *(*f)(void *), void *arg) {
    (void)a; (void)f; (void)arg;
    *t = -1; /* single-threaded: never actually create threads */
    return 0;
}
static inline int pthread_join(pthread_t t, void **r) { (void)t; (void)r; return 0; }
static inline pthread_t pthread_self(void) { return sceKernelGetThreadId(); }

/* ---- usleep -------------------------------------------------------------- */
#ifndef _USLEEP_DEFINED
#define _USLEEP_DEFINED
static inline int usleep(unsigned int us) {
    sceKernelDelayThread((SceUInt)us);
    return 0;
}
#endif

#endif /* PTHREAD_PSP_SHIM_H */

/*
 * psp_thread_shim.h — PSP pthread/semaphore type shim for OpenH264
 *
 * Injected via -include into every OpenH264 TU compiled for PSP.
 * Provides the POSIX types that WelsThreadLib.h expects, backed by
 * PSP sceKernel primitives.
 *
 * The actual function bodies (WelsMutexInit etc.) live in
 * psp_thread_stubs.cpp which replaces WelsThreadLib.cpp.
 *
 * Key contract:
 *   - DISABLE_DECODER_MT is defined → m_iThreadCount is always 0
 *   - All threading code paths in OpenH264 check (m_iThreadCount >= 1)
 *     before calling thread functions, so these stubs are never invoked.
 *   - The types must be complete so structs that embed them compile.
 */

#ifndef PSP_THREAD_SHIM_H
#define PSP_THREAD_SHIM_H

/* Only activate when compiling for PSP with psp-gcc */
#if defined(PSP) || defined(__psp__)

/* Pull in PSP SDK headers FIRST so our typedefs don't conflict with theirs.
 * unistd.h provides usleep(), semaphore.h provides sem_t. */
#include <pspkerneltypes.h>
#include <unistd.h>
#include <semaphore.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- pthread_t: PSP thread ID ------------------------------------------- */
typedef SceUID  pthread_t;
typedef int     pthread_attr_t;

/* ---- pthread_mutex_t: PSP mutex UID ------------------------------------- */
typedef SceUID  pthread_mutex_t;
typedef int     pthread_mutexattr_t;

/* ---- pthread_cond_t: PSP semaphore UID ---------------------------------- */
typedef SceUID  pthread_cond_t;
typedef int     pthread_condattr_t;

/* sem_t and usleep are already provided by PSP SDK headers above */

/* ---- struct timespec (needed by pthread_cond_timedwait signature) -------- */
#include <time.h>

/* ---- pthread function declarations (bodies in psp_thread_stubs.cpp) ----- */
int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a);
int pthread_mutex_destroy(pthread_mutex_t *m);
int pthread_mutex_lock(pthread_mutex_t *m);
int pthread_mutex_unlock(pthread_mutex_t *m);

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a);
int pthread_cond_destroy(pthread_cond_t *c);
int pthread_cond_signal(pthread_cond_t *c);
int pthread_cond_broadcast(pthread_cond_t *c);
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m);
int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m,
                           const struct timespec *ts);

int pthread_create(pthread_t *t, const pthread_attr_t *a,
                   void *(*start)(void *), void *arg);
int pthread_join(pthread_t t, void **retval);

#ifdef __cplusplus
}
#endif

#endif /* PSP */
#endif /* PSP_THREAD_SHIM_H */

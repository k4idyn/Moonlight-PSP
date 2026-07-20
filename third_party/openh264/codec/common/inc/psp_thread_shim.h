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

/* Pull in PSP kernel types (SceUID, etc.) */
#include <pspkerneltypes.h>

/* Some PSP SDK builds ship a pthread.h that would conflict with our typedefs.
 * Pre-define its include guard so it is silently skipped. */
#ifndef _PTHREAD_H
#define _PTHREAD_H
#endif

/* Similarly block pte_types.h which is pulled in by some SDK pthread.h paths */
#ifndef _PTE_TYPES_H
#define _PTE_TYPES_H
#endif

/* unistd.h provides usleep() — include before any sem/pthread headers */
#include <unistd.h>

/* Do NOT include <semaphore.h> — it pulls in pthread.h on some SDK versions.
 * Define sem_t ourselves as a simple integer semaphore handle. */
#ifndef _SEMAPHORE_H
#define _SEMAPHORE_H
typedef int sem_t;
#endif

#include <time.h>

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

/* ---- sem_t function declarations ----------------------------------------- */
int sem_init(sem_t *s, int pshared, unsigned int value);
int sem_destroy(sem_t *s);
int sem_post(sem_t *s);
int sem_wait(sem_t *s);
int sem_trywait(sem_t *s);

#ifdef __cplusplus
}
#endif

#endif /* PSP */
#endif /* PSP_THREAD_SHIM_H */


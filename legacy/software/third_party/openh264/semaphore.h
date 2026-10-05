/*
 * semaphore.h — PSP shim interceptor
 *
 * This file intercepts any #include <semaphore.h> within the OpenH264
 * build tree. It prevents the PSP SDK's semaphore header (which pulls in
 * pthread.h) from being included within our OpenH264 compilation units.
 *
 * All real sem_t types and function declarations are provided by
 * codec/common/inc/psp_thread_shim.h, injected via -include in Makefile.psp.
 *
 * This file must be in a directory that appears BEFORE the PSP SDK include
 * path on the compiler's -I search list.
 */

/* Intentionally empty — sem_t and sem functions already declared by psp_thread_shim.h */

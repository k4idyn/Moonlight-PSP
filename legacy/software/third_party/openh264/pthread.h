/*
 * pthread.h — PSP shim interceptor
 *
 * This file intercepts any #include <pthread.h> within the OpenH264
 * build tree. It prevents the PSP SDK's pthreads-embedded header from
 * being included (which would conflict with our own PSP-native typedefs
 * already declared via -include psp_thread_shim.h).
 *
 * All real pthread types and function declarations are provided by
 * codec/common/inc/psp_thread_shim.h, injected via -include in Makefile.psp.
 *
 * This file must be in a directory that appears BEFORE the PSP SDK include
 * path on the compiler's -I search list.
 */

/* Intentionally empty — all types already declared by psp_thread_shim.h */

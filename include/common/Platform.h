#pragma once

#ifdef _WIN32
// Prevent bogus definitions of error codes
// that are incompatible with Winsock errors.
#define _CRT_NO_POSIX_ERROR_CODES

// Ignore CRT warnings about POSIX names
#define _CRT_NONSTDC_NO_DEPRECATE 1
#endif

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#elif defined(__APPLE__)
#include <mach/mach_time.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <fcntl.h>
#elif defined(__vita__)
#include <unistd.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <psp2/kernel/threadmgr.h>
#elif defined(__WIIU__)
#include <unistd.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <malloc.h>
#include <coreinit/thread.h>
#include <coreinit/fastmutex.h>
#include <coreinit/fastcondition.h>
#include <fcntl.h>
#elif defined(__3DS__)
#include <3ds.h>
#include <fcntl.h>
#elif defined(_PSP)
#include <unistd.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <malloc.h>
#include <pspkernel.h>
#include <fcntl.h>
#else
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <fcntl.h>
#endif

#ifdef _WIN32
# define LC_WINDOWS
#elif !defined(__PSP__)
# define LC_POSIX
# if defined(__APPLE__)
#  define LC_DARWIN
# endif
#endif

#ifdef LC_WINDOWS
// Windows doesn't have strtok_r() but it has the same
// function named strtok_s().
#define strtok_r strtok_s

# if defined(WINAPI_FAMILY) && WINAPI_FAMILY==WINAPI_FAMILY_APP
# define LC_UWP
# else
# define LC_WINDOWS_DESKTOP
#endif

#endif

#ifdef _PSP
// strtok_r is often missing from PSPSDK headers even if in libc
extern char *strtok_r(char *, const char *, char **);
#endif

#include <stdio.h>
#include "Limelight.h"

#define Limelog(s, ...) \
    if (ListenerCallbacks.logMessage) \
        ListenerCallbacks.logMessage(s, ##__VA_ARGS__)

#if defined(LC_WINDOWS)
#include <crtdbg.h>
#ifdef LC_DEBUG
#define LC_ASSERT(x) __analysis_assume(x); \
                       _ASSERTE(x)
#else
#define LC_ASSERT(x)
#endif
#else
#ifndef LC_DEBUG
#ifndef NDEBUG
#define NDEBUG
#endif
#else
#ifdef NDEBUG
#undef NDEBUG
#endif
#endif
#include <assert.h>
#define LC_ASSERT(x) assert(x)
#endif

// If we're fuzzing, we don't want to enable asserts that can be affected by
// bad input from the remote host. LC_ASSERT_VT() is used for assertions that
// check data that comes from the host. These checks are enabled for normal
// debug builds, since they indicate an error in Moonlight or on the host.
// These are disabled when fuzzing, since the traffic is intentionally invalid.
#ifdef LC_FUZZING
#define LC_ASSERT_VT(x)
#else
#define LC_ASSERT_VT(x) LC_ASSERT(x)
#endif

#ifdef _MSC_VER
#pragma intrinsic(_byteswap_ushort)
#define BSWAP16(x) _byteswap_ushort(x)
#pragma intrinsic(_byteswap_ulong)
#define BSWAP32(x) _byteswap_ulong(x)
#pragma intrinsic(_byteswap_uint64)
#define BSWAP64(x) _byteswap_uint64(x)
#elif defined(_PSP) || (__GNUC__ < 4) || (__GNUC__ == 4 && __GNUC_MINOR__ < 8)
#define BSWAP16(x) ((uint16_t)((((x) >> 8) & 0xff) | (((x) & 0xff) << 8)))
#define BSWAP32(x) ((uint32_t)((((x) >> 24) & 0xff) | (((x) >> 8) & 0xff00) | (((x) << 8) & 0xff0000) | (((x) << 24) & 0xff000000)))
#define BSWAP64(x) ((uint64_t)((((uint64_t)BSWAP32((uint32_t)(x))) << 32) | (uint64_t)BSWAP32((uint32_t)((x) >> 32))))
#elif (__GNUC__ > 4) || (__GNUC__ == 4 && __GNUC_MINOR__ >= 8)
#define BSWAP16(x) __builtin_bswap16(x)
#define BSWAP32(x) __builtin_bswap32(x)
#define BSWAP64(x) __builtin_bswap64(x)
#elif defined(__has_builtin) && __has_builtin(__builtin_bswap16)
#define BSWAP16(x) __builtin_bswap16(x)
#define BSWAP32(x) __builtin_bswap32(x)
#define BSWAP64(x) __builtin_bswap64(x)
#else
#error Please define your platform byteswap macros!
#endif

#if defined(_PSP)
#define LE16(x) (x)
#define LE32(x) (x)
#define LE64(x) (x)
#define BE16(x) BSWAP16(x)
#define BE32(x) BSWAP32(x)
#define BE64(x) BSWAP64(x)
#define IS_LITTLE_ENDIAN() (true)
#elif (defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)) || defined(__BIG_ENDIAN__)
#define LE16(x) BSWAP16(x)
#define LE32(x) BSWAP32(x)
#define LE64(x) BSWAP64(x)
#define BE16(x) (x)
#define BE32(x) (x)
#define BE64(x) (x)
#define IS_LITTLE_ENDIAN() (false)
#else
#define LE16(x) (x)
#define LE32(x) (x)
#define LE64(x) (x)
#define BE16(x) BSWAP16(x)
#define BE32(x) BSWAP32(x)
#define BE64(x) BSWAP64(x)
#define IS_LITTLE_ENDIAN() (true)
#endif

/**
 * @brief Initializes the platform abstraction layer.
 * @return 0 on success, negative error code on failure.
 */
int initializePlatform(void);

/**
 * @brief Cleans up the platform abstraction layer and releases resources.
 */
void cleanupPlatform(void);

/**
 * @brief Safely copies a string with buffer overflow protection.
 * @param dest Destination buffer.
 * @param dest_size Size of the destination buffer.
 * @param src Source string to copy.
 * @return true on success, false if truncation occurred or parameters was invalid.
 */
bool PltSafeStrcpy(char* dest, size_t dest_size, const char* src);

/**
 * @brief Initializes the platform-specific high-resolution timer.
 */
void PltTicksInit(void);

/**
 * @brief Gets the number of microseconds since an arbitrary start time.
 * @return Microseconds.
 */
uint64_t PltGetMicroseconds(void);

/**
 * @brief Gets the number of milliseconds since an arbitrary start time.
 * @return Milliseconds.
 */
uint64_t PltGetMillis(void);


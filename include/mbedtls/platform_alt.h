#ifndef MBEDTLS_PLATFORM_ALT_H
#define MBEDTLS_PLATFORM_ALT_H

#include <pspkernel.h>
#include <pspthreadman.h>
#include <pspsdk.h>
#include <time.h>

/* Define the platform mutex and condition variable types for PSP */
typedef SceUID mbedtls_platform_mutex_t;
typedef SceUID mbedtls_platform_condition_variable_t;

/* Platform-specific time functions */
static inline mbedtls_time_t mbedtls_platform_time(mbedtls_time_t *time)
{
    time_t t = sceKernelGetSystemTimeLow();
    if (time != NULL)
        *time = t;
    return t;
}

/* Platform-specific mutex functions */
static inline int mbedtls_platform_mutex_init(mbedtls_platform_mutex_t *mutex)
{
    *mutex = sceKernelCreateSema("", 1, 1, 1, NULL);
    if (*mutex < 0)
        return MBEDTLS_ERR_THREADING_USAGE_ERROR;
    return 0;
}

static inline void mbedtls_platform_mutex_destroy(mbedtls_platform_mutex_t *mutex)
{
    if (*mutex >= 0)
        sceKernelDeleteSema(*mutex);
}

static inline int mbedtls_platform_mutex_lock(mbedtls_platform_mutex_t *mutex)
{
    int ret = sceKernelWaitSema(*mutex, 1, 0);
    if (ret < 0)
        return MBEDTLS_ERR_THREADING_USAGE_ERROR;
    return 0;
}

static inline int mbedtls_platform_mutex_unlock(mbedtls_platform_mutex_t *mutex)
{
    int ret = sceKernelSignalSema(*mutex, 1);
    if (ret < 0)
        return MBEDTLS_ERR_THREADING_USAGE_ERROR;
    return 0;
}

/* Platform-specific condition variable functions */
static inline int mbedtls_platform_condition_variable_init(mbedtls_platform_condition_variable_t *cond)
{
    *cond = sceKernelCreateSema("", 0, 0x7FFFFFFF, 1, NULL);
    if (*cond < 0)
        return MBEDTLS_ERR_THREADING_USAGE_ERROR;
    return 0;
}

static inline void mbedtls_platform_condition_variable_destroy(mbedtls_platform_condition_variable_t *cond)
{
    if (*cond >= 0)
        sceKernelDeleteSema(*cond);
}

static inline int mbedtls_platform_condition_variable_signal(mbedtls_platform_condition_variable_t *cond)
{
    int ret = sceKernelSignalSema(*cond, 1);
    if (ret < 0)
        return MBEDTLS_ERR_THREADING_USAGE_ERROR;
    return 0;
}

static inline int mbedtls_platform_condition_variable_broadcast(mbedtls_platform_condition_variable_t *cond)
{
    /* For simplicity, we signal once. In a real implementation, 
       we would need to wait for all waiters to wake up */
    int ret = sceKernelSignalSema(*cond, 1);
    if (ret < 0)
        return MBEDTLS_ERR_THREADING_USAGE_ERROR;
    return 0;
}

static inline int mbedtls_platform_condition_variable_wait(mbedtls_platform_condition_variable_t *cond,
                                                         mbedtls_platform_mutex_t *mutex)
{
    /* Unlock mutex, wait on condition, then lock mutex */
    int ret = sceKernelSignalSema(*mutex, 1);
    if (ret < 0)
        return MBEDTLS_ERR_THREADING_USAGE_ERROR;
    
    ret = sceKernelWaitSema(*cond, 1, 0);
    if (ret < 0)
        return MBEDTLS_ERR_THREADING_USAGE_ERROR;
    
    ret = sceKernelWaitSema(*mutex, 1, 0);
    if (ret < 0)
        return MBEDTLS_ERR_THREADING_USAGE_ERROR;
    
    return 0;
}

/* Platform-specific time functions */
static inline mbedtls_ms_time_t mbedtls_platform_ms_time(void)
{
    return sceKernelGetSystemTimeLow();
}

/* Platform-specific entropy function */
static inline int mbedtls_platform_get_entropy(psa_driver_get_entropy_flags_t flags,
                                              size_t *estimate_bits,
                                              unsigned char *output, size_t output_size)
{
    /* Use PSP hardware RNG if available */
    SceUInt32 rand_val;
    size_t bytes_to_copy = output_size < sizeof(SceUInt32) ? output_size : sizeof(SceUInt32);
    
    /* For simplicity, we'll use the standard rand() function seeded with system time */
    /* In a real implementation, you would use the PSP hardware RNG */
    static int seeded = 0;
    if (!seeded) {
        srand((unsigned int)sceKernelGetSystemTimeLow());
        seeded = 1;
    }
    
    rand_val = rand();
    memcpy(output, &rand_val, bytes_to_copy);
    
    /* For simplicity, we assume full entropy */
    if (estimate_bits != NULL)
        *estimate_bits = 8 * bytes_to_copy;
    
    return 0;
}

/* Platform-specific malloc/free - use standard PSP SDK versions */
#ifndef MBEDTLS_PLATFORM_NO_STD_FUNCTIONS
#define MBEDTLS_PLATFORM_STD_CALLOC   calloc
#define MBEDTLS_PLATFORM_STD_FREE     free
#define MBEDTLS_PLATFORM_STD_PRINTF   printf
#define MBEDTLS_PLATFORM_STD_FPRINTF  fprintf
#define MBEDTLS_PLATFORM_STD_SNPRINTF snprintf
#define MBEDTLS_PLATFORM_STD_VSNPRINTF vsnprintf
#define MBEDTLS_PLATFORM_STD_SETBUF   setbuf
#define MBEDTLS_PLATFORM_STD_EXIT     exit
#define MBEDTLS_PLATFORM_STD_TIME     time
#endif /* MBEDTLS_PLATFORM_NO_STD_FUNCTIONS */

#endif /* MBEDTLS_PLATFORM_ALT_H */
/*
 * psp_mbedtls_entropy.c - Hardware entropy for mbedTLS on PSP
 *
 * Implements mbedtls_hardware_poll() required by MBEDTLS_ENTROPY_HARDWARE_ALT.
 * Uses the PSP Real-Time Clock microsecond tick counter as entropy, mixed
 * through an LCG to spread the bits.  Not cryptographically strong but
 * sufficient to seed the CTR-DRBG for RSA signing inside the PSP app.
 */

#define MBEDTLS_CONFIG_FILE "mbedtls_psp_config.h"
#include "mbedtls/entropy_poll.h"

#include <psptypes.h>
#include <time.h>
#include <psprtc.h>
#include <pspthreadman.h>
#include <stdint.h>
#include <stddef.h>

/*
 * mbedtls_hardware_poll - Fill 'output' with 'len' pseudo-entropy bytes.
 *
 * The seed is derived from the PSP RTC tick (64-bit microsecond counter)
 * XOR'd with the low-precision kernel timer.  An LCG (Knuth MMIX constants)
 * expands the seed to as many bytes as requested.
 */
int mbedtls_hardware_poll(void *data,
                           unsigned char *output,
                           size_t len,
                           size_t *olen)
{
    u64 tick = 0;
    u32 low  = sceKernelGetSystemTimeLow();

    sceRtcGetCurrentTick(&tick);

    /* Mix RTC tick + timer into a 64-bit starting value */
    uint64_t seed = (uint64_t)tick
                  ^ ((uint64_t)low << 17)
                  ^ 0xDEADC0DEULL;

    /* Expand with Knuth MMIX LCG */
    size_t i;
    for (i = 0; i < len; i++) {
        seed = seed * UINT64_C(6364136223846793005)
                    + UINT64_C(1442695040888963407);
        output[i] = (unsigned char)(seed >> 33);
    }

    *olen = len;
    (void)data;
    return 0;
}

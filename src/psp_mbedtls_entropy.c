/*
 * psp_mbedtls_entropy.c - Hardware entropy for mbedTLS on PSP
 *
 * Implements mbedtls_hardware_poll() required by MBEDTLS_ENTROPY_HARDWARE_ALT.
 * This source collects multiple timing/jitter signals and folds them through
 * SHA-256 into a rolling state. It is still platform-constrained, but stronger
 * than a single timer + LCG expansion.
 */

#define MBEDTLS_CONFIG_FILE "mbedtls_psp_config.h"
#include "mbedtls/entropy_poll.h"
#include "mbedtls/sha256.h"

#include <psptypes.h>
#include <time.h>
#include <psprtc.h>
#include <pspthreadman.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/*
 * mbedtls_hardware_poll - Fill output with hardware-derived entropy bytes.
 *
 * We mix runtime values (RTC tick, system timer, thread id, pointer noise,
 * timing jitter loops, and a rolling counter/state) into SHA-256, then
 * expand output by hashing state||counter blocks.
 */
int mbedtls_hardware_poll(void *data,
                           unsigned char *output,
                           size_t len,
                           size_t *olen)
{
    static unsigned char s_state[32];
    static u32 s_counter = 0;

    unsigned char material[128];
    unsigned char digest[32];
    unsigned char block_in[68]; /* state(32) + counter(4) + digest(32) */
    unsigned char block_out[32];
    u64 tick = 0;
    u32 low = 0;
    u32 thread_id = 0;
    u32 jitter_acc = 0;
    uintptr_t stack_ptr = 0;
    size_t off = 0;
    size_t out_off = 0;
    size_t i;

    if (!output || !olen) {
        return -1;
    }

    low = sceKernelGetSystemTimeLow();
    sceRtcGetCurrentTick(&tick);
    thread_id = (u32)sceKernelGetThreadId();
    stack_ptr = (uintptr_t)&tick;

    /* Fold timing jitter from short variable loops. */
    for (i = 0; i < 8; i++) {
        volatile int spin;
        u32 t0 = sceKernelGetSystemTimeLow();
        spin = 64 + (int)((t0 ^ s_counter ^ (u32)i) & 0x3F);
        while (spin-- > 0) {
            /* Busy loop to expose scheduler/timer jitter. */
        }
        {
            u32 t1 = sceKernelGetSystemTimeLow();
            jitter_acc ^= (t1 - t0) << (i & 7);
            jitter_acc ^= (t1 >> ((i + 3) & 7));
        }
    }

    memset(material, 0, sizeof(material));
    memcpy(material + off, &tick, sizeof(tick)); off += sizeof(tick);
    memcpy(material + off, &low, sizeof(low)); off += sizeof(low);
    memcpy(material + off, &thread_id, sizeof(thread_id)); off += sizeof(thread_id);
    memcpy(material + off, &jitter_acc, sizeof(jitter_acc)); off += sizeof(jitter_acc);
    memcpy(material + off, &stack_ptr, sizeof(stack_ptr)); off += sizeof(stack_ptr);
    memcpy(material + off, &s_counter, sizeof(s_counter)); off += sizeof(s_counter);
    memcpy(material + off, s_state, sizeof(s_state)); off += sizeof(s_state);

    mbedtls_sha256(material, off, digest, 0);

    /* Update rolling state with new digest material. */
    for (i = 0; i < sizeof(s_state); i++) {
        s_state[i] ^= digest[i];
    }

    while (out_off < len) {
        size_t chunk = len - out_off;
        if (chunk > sizeof(block_out)) {
            chunk = sizeof(block_out);
        }

        memcpy(block_in, s_state, sizeof(s_state));
        memcpy(block_in + 32, &s_counter, sizeof(s_counter));
        memcpy(block_in + 36, digest, sizeof(digest));

        mbedtls_sha256(block_in, sizeof(block_in), block_out, 0);
        memcpy(s_state, block_out, sizeof(s_state));
        memcpy(output + out_off, block_out, chunk);

        out_off += chunk;
        s_counter++;
    }

    *olen = len;
    (void)data;
    return 0;
}

/*
 * sw_me_worker.c - Media Engine Worker Thread
 *
 * Bridges the Main CPU and Media Engine (ME) for the software decode pipeline.
 *
 * Architecture:
 *   The ME is a separate MIPS core with its own VFPU that communicates
 *   through a shared me_struct in physical RAM. The ME helper kernel module
 *   (moonlight_me_helper.prx) manages initialization and dispatch.
 *
 * Cache Coherence Protocol:
 *   1. Main CPU fills SwPipelineState.mbs[] with CAVLC-decoded data
 *   2. Main CPU calls sceKernelDcacheWritebackInvalidateAll() to flush
 *   3. ME worker dispatches reconstruction via BeginME()
 *   4. ME invalidates its dcache (precache_len = -1), reads shared  data
 *   5. ME runs VFPU reconstruction, writes output
 *   6. ME flushes its dcache (postcache_len = -1)
 *   7. Main CPU invalidates dcache, reads RGBA output
 *
 * The ME reads from physical RAM bypassing the CPU's dcache. Without
 * proper flush/invalidate, the ME would see stale data.
 */

#include <pspsdk.h>
#include <pspkernel.h>
#include <pspthreadman.h>
#include <string.h>
#include <malloc.h>

#include "sw_decode_pipeline.h"
#include "me.h"
#include "diag_log.h"

/* ============================================================================
 * ME dispatch context — passed to ME via the int param
 *
 * Since me_struct.func takes (int) and returns (int), we pass a pointer
 * to our dispatch context cast as int. The ME dereferences it to find
 * the pipeline state and output buffer.
 * ============================================================================*/

typedef struct {
    SwPipelineState *state;
    u8 *rgba_output;
} __attribute__((aligned(64))) SwMEDispatch;

/* Module-level state */
static volatile struct me_struct *me_instance = NULL;
static SwMEDispatch *me_dispatch = NULL;
static SceUID me_sema = -1;
static int me_initialized = 0;

/* Forward declarations from sw_vfpu_recon.c */
extern void sw_reconstruct_frame(SwPipelineState *state, u8 *rgba_output);
extern void sw_reconstruct_frame_me(SwPipelineState *state, u8 *rgba_output);

/* ============================================================================
 * ME Entry Point — This function runs on the Media Engine core
 *
 * Called via me_struct.func(param) where param is a pointer to SwMEDispatch.
 * The ME's dcache has been invalidated before this call (precache_len = -1).
 * After return, the ME kernel module will flush dcache (postcache_len = -1).
 *
 * MUST NOT:
 *   - Call any kernel/user syscalls (sceIo*, sceKernel*, printf, etc.)
 *   - Access kernel memory
 *   - Use floating point (use VFPU instead via inline asm)
 *
 * CAN:
 *   - Read/write user-space physical memory
 *   - Execute VFPU instructions (ME has its own VFPU)
 *   - Use standard C library math (compiled to MIPS integer ops)
 * ============================================================================*/

static int me_recon_entry(int param)
{
    SwMEDispatch *ctx = (SwMEDispatch *)(unsigned int)param;
    if (!ctx || !ctx->state || !ctx->rgba_output) {
        return -1;
    }

    /* Reconstruct all macroblocks → YUV planes → RGBA output
     * Use ME-safe version (no syscalls) since this runs on the ME core */
    sw_reconstruct_frame_me(ctx->state, ctx->rgba_output);

    return 0;
}

/* ME helper PRX module ID — tracks whether we loaded it */
static SceUID me_prx_modid = -1;

/* ============================================================================
 * ME Worker Initialization
 * ============================================================================*/

int sw_me_worker_init(void)
{
    if (me_initialized) {
        return 0; /* Already initialized */
    }

    /* Load the ME helper kernel PRX (provides InitME/KillME).
     *
     * CRITICAL: The ME helper must be loaded AND started BEFORE
     * moonlight.prx is loaded by pspsh, because the import stubs
     * (MediaEngine.o) are resolved at moonlight.prx load time.
     * If the helper isn't started yet, InitME/KillME stubs resolve
     * to error handlers and return 0x8002013A (not started).
     *
     * The loading code here is a best-effort fallback — the test
     * script should always pre-load the helper first. */
    /* Try multiple paths: XMB runs from ms0:/PSP/GAME/Moonlight/,
     * PSPLink uses host0:/ CWD.  Try game directory first. */
    static const char *me_paths[] = {
        "ms0:/PSP/GAME/Moonlight/moonlight_me_helper.prx",
        "moonlight_me_helper.prx",
        NULL
    };
    me_prx_modid = -1;
    for (int pi = 0; me_paths[pi]; pi++) {
        me_prx_modid = sceKernelLoadModule(me_paths[pi], 0, NULL);
        if (me_prx_modid >= 0) {
            diag_log_write("SW_ME", "LoadModule OK from %s", me_paths[pi]);
            break;
        }
        if (me_prx_modid == (SceUID)0x80020139 ||
            me_prx_modid == (SceUID)0x8002032C) break;
        diag_log_write("SW_ME", "LoadModule %s failed: 0x%08X", me_paths[pi], (unsigned)me_prx_modid);
    }
    if (me_prx_modid >= 0) {
        /* Fresh load succeeded — start the module */
        int status = 0;
        int res = sceKernelStartModule(me_prx_modid, 0, NULL, &status, NULL);
        if (res < 0 && res != (int)0x80020139 && res != (int)0x8002032C) {
            diag_log_write("SW_ME",
                     "StartModule me_helper failed: 0x%08X", (unsigned)res);
            return -4;
        }
        diag_log_write("SW_ME",
                 "ME helper loaded (uid=0x%08X) and started (res=0x%08X)",
                 (unsigned)me_prx_modid, (unsigned)res);
    } else if (me_prx_modid == (SceUID)0x80020139 ||
               me_prx_modid == (SceUID)0x8002032C) {
        /* 0x80020139 = already loaded (SCE_KERNEL_ERROR_ALREADY_LOADED)
         * 0x8002032C = exclusive load  (SCE_KERNEL_ERROR_EXCLUSIVE_LOAD)
         * Module is resident from a prior session.  Import stubs should
         * have been resolved at moonlight.prx load time IF the helper
         * was started before moonlight was loaded. */
        diag_log_write("SW_ME",
                 "ME helper already loaded (0x%08X) — stubs must have been "
                 "resolved at moonlight.prx load time", (unsigned)me_prx_modid);
    } else {
        diag_log_write("SW_ME",
                 "LoadModule me_helper failed: 0x%08X — continuing without ME", (unsigned)me_prx_modid);
        /* Don't return -4; fall through to try InitME anyway in case
         * stubs were pre-resolved (PSPLink pre-loads the module). */
    }

    /* Allocate ME struct — must be 64-byte aligned, in uncached or
     * physically-addressable memory for ME access */
    me_instance = (volatile struct me_struct *)memalign(64,
                      sizeof(struct me_struct));
    if (!me_instance) {
        diag_log_write("SW_ME",
                 "Failed to allocate me_struct");
        return -1;
    }
    memset((void *)me_instance, 0, sizeof(struct me_struct));

    /* Allocate dispatch context — 64-byte aligned */
    me_dispatch = (SwMEDispatch *)memalign(64, sizeof(SwMEDispatch));
    if (!me_dispatch) {
        diag_log_write("SW_ME",
                 "Failed to allocate dispatch context");
        free((void *)me_instance);
        me_instance = NULL;
        return -1;
    }
    memset(me_dispatch, 0, sizeof(SwMEDispatch));

    /* Create semaphore for synchronization between orchestrator
     * and ME completion */
    me_sema = sceKernelCreateSema("SwMESema", 0, 0, 1, NULL);
    if (me_sema < 0) {
        diag_log_write("SW_ME",
                 "Failed to create ME semaphore: 0x%08X", me_sema);
        free(me_dispatch);
        free((void *)me_instance);
        me_instance = NULL;
        me_dispatch = NULL;
        return -2;
    }

    /* Initialize the Media Engine via kernel module.
     * If stubs were not resolved (helper not started before moonlight
     * was loaded), InitME returns 0x8002013A (SCE_KERNEL_ERROR_NOT_STARTED).
     * In that case, ME is unavailable and we fall back to CPU-only. */
    int ret = InitME(me_instance);
    if (ret < 0) {
        if (ret == (int)0x8002013A || ret == (int)0x80020001) {
            diag_log_write("SW_ME",
                     "InitME stub not resolved (0x%08X) — ME helper must be "
                     "loaded+started BEFORE moonlight.prx. Falling back to CPU.",
                     (unsigned)ret);
        } else {
            diag_log_write("SW_ME",
                     "InitME failed: %d (0x%08X)", ret, (unsigned)ret);
        }
        sceKernelDeleteSema(me_sema);
        free(me_dispatch);
        free((void *)me_instance);
        me_instance = NULL;
        me_dispatch = NULL;
        me_sema = -1;
        return -3;
    }

    me_initialized = 1;
    diag_log_write("SW_ME",
             "Media Engine worker initialized");
    return 0;
}

/* ============================================================================
 * ME Worker Shutdown
 * ============================================================================*/

void sw_me_worker_shutdown(void)
{
    if (!me_initialized) return;

    /* Kill the ME — resets the co-processor */
    if (me_instance) {
        KillME(me_instance);
    }

    /* Clean up resources */
    if (me_sema >= 0) {
        sceKernelDeleteSema(me_sema);
        me_sema = -1;
    }

    if (me_dispatch) {
        free(me_dispatch);
        me_dispatch = NULL;
    }

    if (me_instance) {
        free((void *)me_instance);
        me_instance = NULL;
    }

    me_initialized = 0;
    diag_log_write("SW_ME",
             "Media Engine worker shut down");
}

/* ============================================================================
 * Dispatch Reconstruction to ME
 *
 * Called by the orchestrator after CAVLC entropy decode is complete.
 * The caller must have already flushed dcache.
 *
 * Flow:
 *   1. Set dispatch context (pipeline state + output buffer pointers)
 *   2. Flush dcache so ME sees current dispatch context
 *   3. Call BeginME() — non-blocking, ME starts working
 *   4. Return immediately — caller uses sw_me_worker_wait() to block
 * ============================================================================*/

int sw_me_worker_dispatch(SwPipelineState *state, u8 *rgba_output)
{
    if (!me_initialized || !me_instance) {
        diag_log_write("SW_ME",
                 "ME not initialized for dispatch");
        return -1;
    }

    /* Wait for any previous ME job to complete */
    WaitME(me_instance);

    /* Fill dispatch context */
    me_dispatch->state = state;
    me_dispatch->rgba_output = rgba_output;

    /* Flush the dispatch context to physical RAM so ME can read it.
     * The caller is responsible for flushing SwPipelineState before
     * calling this function. */
    sceKernelDcacheWritebackInvalidateRange(me_dispatch,
                                             sizeof(SwMEDispatch));

    /* Dispatch to ME:
     * - func: me_recon_entry (cast to int since me_struct uses int)
     * - param: pointer to dispatch context (cast to int)
     * - precache_len: -1 → ME invalidates all dcache before running
     * - postcache_len: -1 → ME flushes all dcache after running
     * This ensures full cache coherence for the shared pipeline state. */
    int ret = BeginME(me_instance,
                      (int)(unsigned int)me_recon_entry,
                      (int)(unsigned int)me_dispatch,
                      -1, NULL,  /* pre: invalidate all */
                      -1, NULL); /* post: flush all */

    if (ret < 0) {
        diag_log_write("SW_ME",
                 "BeginME failed: %d", ret);
        return -2;
    }

    return 0;
}

/* ============================================================================
 * Wait for ME Reconstruction to Complete
 *
 * Bounded wait. After return, the RGBA output buffer is valid (if ret==0).
 * Returns -2 on timeout (ME likely crashed — reset needed).
 * Caller should invalidate dcache before reading the output.
 * ============================================================================*/

#define ME_TIMEOUT_US  2000000  /* 2 seconds — generous for 510 MBs */

int sw_me_worker_wait(void)
{
    if (!me_initialized || !me_instance) {
        return -1;
    }

    /* Bounded poll: check done flag, bail after timeout.
     * WaitME does `while (!mei->done);` which hangs forever if ME crashes.
     * sceKernelGetSystemTimeLow() gives microsecond monotonic time. */
    u32 start_us = sceKernelGetSystemTimeLow();
    while (!me_instance->done) {
        u32 now_us = sceKernelGetSystemTimeLow();
        u32 elapsed = now_us - start_us; /* handles wrap */
        if (elapsed > ME_TIMEOUT_US) {
            diag_log_write("SW_ME",
                     "ME timeout after %u us — ME likely crashed, resetting",
                     elapsed);
            /* Kill and reinitialize the ME to recover */
            KillME(me_instance);
            /* Reinitialize ME so next frame can attempt dispatch */
            memset((void *)me_instance, 0, sizeof(struct me_struct));
            int ret = InitME(me_instance);
            if (ret < 0) {
                diag_log_write("SW_ME",
                         "ME reinit failed: %d", ret);
                me_initialized = 0;
            }
            return -2;
        }
    }

    int result = me_instance->result;

    /* Invalidate CPU dcache so we see the ME's output */
    sceKernelDcacheWritebackInvalidateAll();

    return result;
}

/* ============================================================================
 * Check if ME has finished (non-blocking)
 * ============================================================================*/

int sw_me_worker_check_done(void)
{
    if (!me_initialized || !me_instance) {
        return 1; /* Not initialized = "done" */
    }

    return CheckME(me_instance);
}

/* ============================================================================
 * Fallback: CPU-only reconstruction (no ME)
 *
 * Used when ME is unavailable or for debugging.
 * Runs sw_reconstruct_frame() directly on the Main CPU.
 * Much slower (VFPU on Main CPU is shared with game logic) but functional.
 * ============================================================================*/

int sw_me_worker_cpu_fallback(SwPipelineState *state, u8 *rgba_output)
{
    if (!state || !rgba_output) return -1;

    sw_reconstruct_frame(state, rgba_output);

    return 0;
}

/* ============================================================================
 * Check if ME is initialized and available for dispatch
 * ============================================================================*/

int sw_me_worker_is_available(void)
{
    return me_initialized && me_instance != NULL;
}

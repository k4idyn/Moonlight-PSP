#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <pspiofilemgr_fcntl.h>
#include <pspdebug.h>
#include <psppower.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "exception_handler.h"
#include "logger.h"


/* 
 * Standard MIPS register names for display.
 * Note: $0 (zr) is always 0, so we start from $1 (at).
 */
static const char* regNames[] = {
    "zr", "at", "v0", "v1", "a0", "a1", "a2", "a3",
    "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
    "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
    "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"
};

/**
 * Custom exception callback called by the SDK when a crash occurs,
 * or manually triggered during a diagnostic "Absolute Perfection" event.
 */
static void exception_display(PspDebugRegBlock *regs, const char* custom_msg, int code) {
    /* 1. Force CPU clock to 333MHz to ensure stable rendering/handling */
    scePowerSetClockFrequency(333, 333, 166);

    /* 2. Direct VRAM manipulation for the "Blue Screen" effect. */
    uint32_t *vram = (uint32_t*)0x44000000;
    for (int i = 0; i < 512 * 272; i++) {
        vram[i] = 0xFFFF0000; // Blue
    }

    /* 3. Interface Setup using pspDebugScreen */
    pspDebugScreenInit();
    pspDebugScreenSetBackColor(0xFFFF0000);
    pspDebugScreenSetTextColor(0xFFFFFFFF);
    pspDebugScreenClear();
    
    LOG_ERROR(COMPONENT_MAIN, "!!! EXCEPTION DETECTED !!! Reason: %s, Code: 0x%08X", 
              custom_msg ? custom_msg : "Hardware Exception", code);
              
    pspDebugScreenPrintf("--- MOONLIGHT PSP: ABSOLUTE PERFECTION DIAGNOSTIC ---\n");
    if (custom_msg) {
        pspDebugScreenPrintf("FAILURE POINT: %s\n", custom_msg);
        pspDebugScreenPrintf("ERROR CODE:    0x%08X\n", code);
    } else {
        pspDebugScreenPrintf("FATAL HARDWARE EXCEPTION TRAP\n");
    }
    pspDebugScreenPrintf("----------------------------------------------------\n\n");

    /* 4. Display Register Data */
    pspDebugScreenPrintf("PC (EPC):  0x%08X    CAUSE: 0x%08X\n", (unsigned int)regs->epc, (unsigned int)regs->cause);
    pspDebugScreenPrintf("BADVADDR:  0x%08X    RA:    0x%08X\n\n", (unsigned int)regs->badvaddr, (unsigned int)regs->r[31]);

    pspDebugScreenPrintf("REGISTERS:\n");
    for (int i = 0; i < 32; i++) {
        pspDebugScreenPrintf("%-3s:curr=0x%08X ", regNames[i], (unsigned int)regs->r[i]);
        if ((i + 1) % 3 == 0) pspDebugScreenPrintf("\n");
    }

    // Stack Trace for "Absolute Perfection"
    PspDebugStackTrace traces[12];
    int found = pspDebugGetStackTrace2(regs, traces, 12);
    if (found > 0) {
        pspDebugScreenPrintf("\n\nCALL STACK TRACE:\n");
        for (int i = 0; i < found; i++) {
            pspDebugScreenPrintf("  [%02d] 0x%08X\n", i, (unsigned int)traces[i].call_addr);
        }
    }

    pspDebugScreenPrintf("\nSNAPSHOT SAVED TO ms0:/exception.log\n");
    
    // Log to file for automated analysis
    SceUID fd = sceIoOpen("ms0:/exception.log", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd >= 0) {
        char buf[1024];
        int len = snprintf(buf, sizeof(buf), "EXCEPTION_TYPE: %s\nEPC: 0x%08X\nCAUSE: 0x%08X\nBADVADDR: 0x%08X\nCODE: 0x%08X\n", 
                        custom_msg ? custom_msg : "HARDWARE", (unsigned int)regs->epc, 
                        (unsigned int)regs->cause, (unsigned int)regs->badvaddr, code);
        
        if (found > 0) {
            len += snprintf(buf + len, sizeof(buf) - len, "STACK_TRACE:\n");
            for (int i = 0; i < found && len < (int)sizeof(buf); i++) {
                len += snprintf(buf + len, sizeof(buf) - len, "0x%08X\n", (unsigned int)traces[i].call_addr);
            }
        }
        sceIoWrite(fd, buf, len);
        sceIoClose(fd);
    }

    pspDebugScreenPrintf("\nPress HOME to exit. Testing Loop will auto-restart.\n");
    
    /* Hang here to allow the user or camera to capture state */
    while (1) {
        sceKernelDelayThread(1000000);
    }
}

/**
 * Entry point for hardware exceptions.
 */
static void exception_callback(PspDebugRegBlock *regs) {
    exception_display(regs, NULL, 0);
}

/* 
 * Stub for kernel function that libpspdebug.a wrongly depends on in user mode.
 */
int sceKernelRegisterDefaultExceptionHandler(void* func) {
    (void)func;
    return 0;
}

void exception_handler_trigger_manually(const char* reason, int error_code) {
    PspDebugRegBlock regs;
    memset(&regs, 0, sizeof(regs));
    
    /* Capture current registers as accurately as possible from C.
       Split into two blocks to avoid GCC's 30-operand limit. */
    __asm__ volatile (
        ".set push\n\t"
        ".set noat\n\t"
        "sw $1,  %0\n\t"
        ".set pop\n\t"
        "sw $2,  %1\n\t" "sw $3,  %2\n\t" "sw $4,  %3\n\t"
        "sw $5,  %4\n\t" "sw $6,  %5\n\t" "sw $7,  %6\n\t" "sw $8,  %7\n\t"
        "sw $9,  %8\n\t" "sw $10, %9\n\t" "sw $11, %10\n\t" "sw $12, %11\n\t"
        "sw $13, %12\n\t" "sw $14, %13\n\t" "sw $15, %14\n\t"
        : "=m"(regs.r[1]),  "=m"(regs.r[2]),  "=m"(regs.r[3]),  "=m"(regs.r[4]),
          "=m"(regs.r[5]),  "=m"(regs.r[6]),  "=m"(regs.r[7]),  "=m"(regs.r[8]),
          "=m"(regs.r[9]),  "=m"(regs.r[10]), "=m"(regs.r[11]), "=m"(regs.r[12]),
          "=m"(regs.r[13]), "=m"(regs.r[14]), "=m"(regs.r[15])
    );
    __asm__ volatile (
        "sw $16, %0\n\t" "sw $17, %1\n\t" "sw $18, %2\n\t" "sw $19, %3\n\t"
        "sw $20, %4\n\t" "sw $21, %5\n\t" "sw $22, %6\n\t" "sw $23, %7\n\t"
        "sw $24, %8\n\t" "sw $25, %9\n\t" "sw $26, %10\n\t" "sw $27, %11\n\t"
        "sw $28, %12\n\t" "sw $29, %13\n\t" "sw $30, %14\n\t" "sw $31, %15\n\t"
        : "=m"(regs.r[16]), "=m"(regs.r[17]), "=m"(regs.r[18]), "=m"(regs.r[19]),
          "=m"(regs.r[20]), "=m"(regs.r[21]), "=m"(regs.r[22]), "=m"(regs.r[23]),
          "=m"(regs.r[24]), "=m"(regs.r[25]), "=m"(regs.r[26]), "=m"(regs.r[27]),
          "=m"(regs.r[28]), "=m"(regs.r[29]), "=m"(regs.r[30]), "=m"(regs.r[31])
    );
    
    regs.cause = 0xDEADDEAD; /* Magic value for manual panic */
    regs.epc = (uint32_t)__builtin_return_address(0);
    
    exception_display(&regs, reason, error_code);
}

void exception_handler_init(void) {
    /* Register the custom handler with the PSPSDK */
    pspDebugInstallErrorHandler(exception_callback);
}

/**
 * @file kernel_exception.c
 * @brief Kernel-mode exception handler for PSP Moonlight
 *
 * Traps CPU exceptions and displays a custom BSOD with register state.
 */

#include <pspkernel.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <string.h>

PSP_MODULE_INFO("KernelException", 0x1006, 1, 0);
PSP_MAIN_THREAD_ATTR(0);

static void exception_handler(PspDebugStackTrace* stackTrace) {
    pspDebugScreenInit();
    pspDebugScreenSetBackColor(0xFF0000);  /* Red screen for hardware failure */
    pspDebugScreenSetTextColor(0xFFFFFF);
    
    pspDebugScreenPrintf("--- MOONLIGHT PSP: ABSOLUTE PERFECTION EXCEPTION TRAP ---\n\n");
    pspDebugScreenPrintf("Exception cause: 0x%08X\n", (unsigned int)stackTrace->exceptionCause);
    pspDebugScreenPrintf("EPC: 0x%08X\n", (unsigned int)stackTrace->epc);
    pspDebugScreenPrintf("VADDR: 0x%08X\n\n", (unsigned int)stackTrace->vaddr);
    
    for (int i = 0; i < 32; i += 4) {
        pspDebugScreenPrintf("R%02d: 0x%08X  R%02d: 0x%08X  R%02d: 0x%08X  R%02d: 0x%08X\n",
                             i,   (unsigned int)stackTrace->regs[i],
                             i+1, (unsigned int)stackTrace->regs[i+1],
                             i+2, (unsigned int)stackTrace->regs[i+2],
                             i+3, (unsigned int)stackTrace->regs[i+3]);
    }

    pspDebugScreenPrintf("\nPress [HOME] to exit to VSH.\n");
    
    while (1) {
        sceKernelDelayThread(100000);
    }
}

int module_start(SceSize args, void *argp) {
    (void)args; (void)argp;
    pspDebugInstallErrorHandler(exception_handler);
    return 0;
}

int module_stop(void) {
    return 0;
}

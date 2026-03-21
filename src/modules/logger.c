#include "logger.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <pspdebug.h>

static SceUID logMutex = -1;
static int logInitialized = 0;
static FILE* g_logFile = NULL;

const char* LogComponentToString(LogComponent comp) {
    switch (comp) {
        case COMPONENT_MAIN:      return "MAIN";
        case COMPONENT_VIDEO:     return "VIDEO";
        case COMPONENT_AUDIO:     return "AUDIO";
        case COMPONENT_NETWORK:   return "NETWORK";
        case COMPONENT_LIMELIGHT: return "LIMELIGHT";
        default:                  return "UNKNOWN";
    }
}

void logger_init() {
    if (!logInitialized) {
        logMutex = sceKernelCreateSema("MoonlightLogMutex", 0, 1, 1, NULL);
        
<<<<<<< HEAD
        FILE* f = fopen("moonlight_debug.log", "w");
        if (f) {
            fprintf(f, "--- Moonlight Log Started ---\n");
            fclose(f);
        }
=======
        g_logFile = fopen("moonlight_debug.log", "w");
        if (g_logFile) {
            fprintf(g_logFile, "--- Moonlight Log Started ---\n");
            fflush(g_logFile);
        }
        logInitialized = 1;
>>>>>>> 07d781f (v0.1.0.2-alpha: Fix socket and file handle leaks (0x80020320))
    }
}

void logger_shutdown() {
    if (logInitialized) {
        if (logMutex >= 0) {
            sceKernelWaitSema(logMutex, 1, NULL);
            if (g_logFile) {
                fclose(g_logFile);
                g_logFile = NULL;
            }
            sceKernelDeleteSema(logMutex);
            logMutex = -1;
        }
        logInitialized = 0;
    }
}

void moonlight_log(const char* level, const char* component_str, const char* format, ...) {
    va_list args;
    char buffer[4096];
    
    /* Absolute Perfection: Defensive NULL checks for safety */
    if (level == NULL) level = "INFO";
    if (component_str == NULL) component_str = "UNKNOWN";
    if (format == NULL) return;
    
    // Acquire mutex FIRST to protect everything including the formatting buffer
    int locked = 0;
    if (logInitialized && logMutex >= 0) {
        SceUInt timeout = 100000; // 100ms
        int res = sceKernelWaitSema(logMutex, 1, &timeout);
        if (res >= 0) {
            locked = 1;
        }
    }

    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    SceSize free_ram = sceKernelTotalFreeMemSize();
    unsigned long long time_us = sceKernelGetSystemTimeWide();
    
    // Stdout (usually thread-safe in C libs, but buffer clobbering was the issue)
    printf("[%s] [%s] %s [RAM:%dKB]\n", level, component_str, buffer, (int)(free_ram/1024));

    // File I/O (must be protected by mutex)
    if (locked) {
<<<<<<< HEAD
        FILE* f = fopen("moonlight_debug.log", "a");
        if (f) {
            fprintf(f, "[%llu] [%s] [%s] %s [RAM:%dKB]\n", time_us, level, component_str, buffer, (int)(free_ram/1024));
            fflush(f);
            fclose(f);
=======
        if (g_logFile) {
            fprintf(g_logFile, "[%llu] [%s] [%s] %s [RAM:%dKB]\n", time_us, level, component_str, buffer, (int)(free_ram/1024));
            fflush(g_logFile);
>>>>>>> 07d781f (v0.1.0.2-alpha: Fix socket and file handle leaks (0x80020320))
        }
    }

    if (locked) {
        sceKernelSignalSema(logMutex, 1);
    } else if (logInitialized && logMutex >= 0) {
        // Fallback for timeout: at least we printed to stdout
        printf("[%s] [MUTEX_TIMEOUT] [%s] %s\n", level, component_str, buffer);
    }
}

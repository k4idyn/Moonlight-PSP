/* opus_psp_config.h - Opus codec build configuration for PSP (fixed-point, no FPU) */
#ifndef OPUS_PSP_CONFIG_H
#define OPUS_PSP_CONFIG_H

/* Use fixed-point arithmetic — PSP has no FPU */
#define FIXED_POINT 1

/* We only need the decoder */
#define OPUS_BUILD 1

/* Disable floating-point API */
#define DISABLE_FLOAT_API 1

/* Use alloca for stack-based allocation (available on PSP GCC) */
#define VAR_ARRAYS 1

/* Standard headers available */
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_MEMORY_H 1

/* No LRINTF on PSP — use our own */
#define HAVE_LRINTF 0

/* Restrict keyword (GCC supports __restrict__) */
#define restrict __restrict__

#endif /* OPUS_PSP_CONFIG_H */

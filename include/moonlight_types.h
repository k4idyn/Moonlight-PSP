/* moonlight_types.h - Common type definitions for PSP Moonlight */
#ifndef MOONLIGHT_TYPES_H
#define MOONLIGHT_TYPES_H

#include <psptypes.h>

/**
 * Moonlight PSP Custom Types
 *
 * Community-standard types only. All pmplayer-specific structs
 * (Mp4AvcDetail2Struct, Mp4AvcCscStruct, Mp4AvcYuvStruct, etc.)
 * have been removed — they relied on sceMpegAvcDecodeDetail2 and
 * sceMpegBaseCscAvc which are UNIMPLEMENTED in PPSSPP.
 *
 * The decoder now uses the standard PSMF ringbuffer path
 * (pspmpeg.h / pspmpegbase.h) which works on both real PSP
 * hardware and PPSSPP emulator.
 */

#endif

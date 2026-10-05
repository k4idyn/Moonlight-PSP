/* Shared stream-state storage for builds that use Sony's AVC decoder.
 * The OpenH264 translation unit owns these symbols in software builds, but
 * must not be linked into the hardware decoder image. */
#include <psptypes.h>

int g_saw_first_idr = 0;
volatile int g_idr_fully_decoded = 0;
volatile int g_refs_corrupted = 0;
volatile int g_current_frame_is_corrupt = 0;
volatile int g_decode_counters_reset_pending = 0;
volatile unsigned int g_oh264_me_wait_us = 0;

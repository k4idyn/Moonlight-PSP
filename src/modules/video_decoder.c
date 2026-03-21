/**
 * @file video_decoder.c
 * @brief Hardware H.264 decoder for PSP — optimized for real hardware
 *
 * Pipeline: Raw H.264 → MPEG-PS wrap → ringbuffer → ME AvcDecode → GU texture
 *
 * CRITICAL HARDWARE NOTES:
 * - sceMpegAvcDecode returns a ME-owned pointer. We must NOT free it.
 * - Cache must be flushed after ringbuffer writes so ME can see them.
 * - The ME output pointer changes per-frame; we track it separately.
 */

#include "video_decoder.h"
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspge.h>
#include <pspgu.h>
#include <pspmpeg.h>
#include <psputility_modules.h>
#include <malloc.h>
#include "logger.h"
#include "exception_handler.h"

#define MPEG_PACKET_SIZE        2048
#define MPEG_RINGBUFFER_PACKETS 512
     /* Reduced from 1024 (2MB) to 256 (512KB) to save RAM */
#define FRAME_STRIDE            512
#define MAX_FRAME_SIZE          (512 * 1024)  /* 512KB max to accommodate large I-frames */

/* MPEG-PS pack header (14 bytes) */
static const unsigned char mpeg_ps_pack_header[14] = {
    0x00, 0x00, 0x01, 0xBA,
    0x44, 0x00, 0x04, 0x00, 0x04, 0x01,
    0x01, 0x89, 0xC3, 0xF8
};

/* PES header prefix for video (stream ID 0xE0) */
static const unsigned char pes_header_prefix[9] = {
    0x00, 0x00, 0x01, 0xE0,
    0x00, 0x00,
    0x81, 0x80,
    0x05
};

struct VideoDecoder {
    int initialized;
    SceMpeg mpeg;
    SceMpegRingbuffer ringbuffer;
    SceMpegStream* videoStream;
    void* avcEsBuf;
    SceMpegAu avcAu;
    void* mpeg_mem_base;
    void* ringbuffer_mem_base;
    unsigned int output_width;
    unsigned int output_height;
    /* ME-returned decode output — DO NOT free this, it's ME-owned */
    void* me_output_frame;
    /* Our allocated buffer passed as initial decode target */
    unsigned char* decode_target;
    int frame_ready;
    unsigned int frame_count;
    unsigned int dropped_frames;
    unsigned long long total_bytes;
    int last_error;
    char error_msg[128];
    /* Staging buffer for H.264 NAL data before ringbuffer callback */
    unsigned char* staging_buf;
    int staging_size;
    int staging_consumed;
};

static VideoDecoder* g_active_decoder = NULL;
static unsigned long long s_current_pts = 0;

/**
 * Ringbuffer callback — wraps raw H.264 in MPEG-PS packets for the ME demuxer.
 * Called from sceMpegRingbufferPut context.
 */
static SceInt32 ringbuffer_callback(void* pData, SceInt32 iNumPackets, void* pParam) {
    (void)pParam;
    
    /* Flush CPU cache for the packets just written so ME sees them immediately */
    sceKernelDcacheWritebackInvalidateRange(pData, iNumPackets * MPEG_PACKET_SIZE);

    VideoDecoder* d = g_active_decoder;
    if (!d || d->staging_size == 0) return 0;

    /* HARDWARE SAFETY: sceMpeg ringbuffer MUST be 64-byte aligned for ME access */
    if (((unsigned int)pData & 0x3F) != 0) {
        LOG_ERROR(COMPONENT_VIDEO, "Ringbuffer alignment failure: %p", pData);
        return 0;
    }

    unsigned char* dest = (unsigned char*)pData;
    int written = 0;
    int remaining = d->staging_size - d->staging_consumed;
    const int overhead = 14 + 9 + 5;  /* pack(14) + PES prefix(9) + PTS(5) */
    const int payload_max = MPEG_PACKET_SIZE - overhead;

    for (int i = 0; i < iNumPackets && remaining > 0; i++) {
        memset(dest, 0, MPEG_PACKET_SIZE);

        /* Pack header */
        memcpy(dest, mpeg_ps_pack_header, 14);

        /* PES header */
        int payload = remaining < payload_max ? remaining : payload_max;
        int pes_len = 3 + 5 + payload;

        memcpy(dest + 14, pes_header_prefix, 9);
        dest[14 + 4] = (pes_len >> 8) & 0xFF;
        dest[14 + 5] = pes_len & 0xFF;

        /* PTS — increment only at start of each frame */
        if (d->staging_consumed == 0) s_current_pts += 3000;
        unsigned long long pts = s_current_pts;
        dest[14 + 9]  = 0x21 | ((pts >> 29) & 0x0E);
        dest[14 + 10] = (pts >> 22) & 0xFF;
        dest[14 + 11] = 0x01 | ((pts >> 14) & 0xFE);
        dest[14 + 12] = (pts >> 7) & 0xFF;
        dest[14 + 13] = 0x01 | ((pts << 1) & 0xFE);

        /* H.264 payload */
        memcpy(dest + overhead, d->staging_buf + d->staging_consumed, payload);
        d->staging_consumed += payload;
        remaining -= payload;
        dest += MPEG_PACKET_SIZE;
        written++;
    }

    return written;
}

static int init_media_engine(VideoDecoder* d);
static void term_media_engine(VideoDecoder* d);

VideoDecoder* video_decoder_create(void) {
    return (VideoDecoder*)calloc(1, sizeof(VideoDecoder));
}

void video_decoder_destroy(VideoDecoder* d) {
    if (!d) return;
    if (d->initialized) term_media_engine(d);
    free(d->decode_target);
    free(d->mpeg_mem_base);
    free(d->ringbuffer_mem_base);
    free(d->staging_buf);
    free(d);
}

int video_decoder_init(VideoDecoder* d) {
    if (!d) return -1;
    int res = init_media_engine(d);
    if (res < 0) {
        LOG_ERROR(COMPONENT_VIDEO, "ME init failed: 0x%08X", res);
        PANIC("ME Init Failure", res);
        return res;
    }
    d->initialized = 1;
    return 0;
}

#include <Limelight.h>

void video_decoder_update(VideoDecoder* d) {
    if (!d || !d->initialized) return;

    VIDEO_FRAME_HANDLE frameHandle;
    PDECODE_UNIT decodeUnit;

    /* Poll for the next available frame in the queue */
    while (LiPollNextVideoFrame(&frameHandle, &decodeUnit)) {
        bool success = false;
        if (decodeUnit->fullLength > 0 && decodeUnit->bufferList) {
            /* Stage and reassemble the frame list into a flat buffer for ME */
            if (decodeUnit->fullLength <= (int)MAX_FRAME_SIZE) {
                int offset = 0;
                PLENTRY entry = decodeUnit->bufferList;
                bool overflow = false;
                while (entry != NULL) {
                    if (entry->data == NULL || entry->length <= 0 || entry->length > (int)MAX_FRAME_SIZE) {
                        LOG_ERROR(COMPONENT_VIDEO, "Invalid NAL entry in DU: data=%p, len=%d", entry->data, entry->length);
                        overflow = true;
                        break;
                    }
                    if (offset + entry->length <= (int)MAX_FRAME_SIZE) {
                        memcpy(d->staging_buf + offset, entry->data, entry->length);
                        offset += entry->length;
                    } else {
                        LOG_ERROR(COMPONENT_VIDEO, "Staging buffer overflow during reassembly: %d + %d > %d", 
                                  offset, entry->length, MAX_FRAME_SIZE);
                        overflow = true;
                        break;
                    }
                    entry = entry->next;
                }
                
                if (!overflow) {
                    /* Submit the reassembled flat buffer to the hardware decoder */
                    /* Note: video_decoder_submit_frame no longer needs to copy data as it's already in staging_buf */
                    if (video_decoder_submit_frame(d, d->staging_buf, offset) == 0) {
                        d->total_bytes += offset;
                        success = true;
                    }
                }
            } else {
                LOG_ERROR(COMPONENT_VIDEO, "Frame fullLength too large for staging buffer: %d", decodeUnit->fullLength);
            }
        }
        
        /* Mark the frame as processed. If we failed to decode or reassemble, 
           we return DR_NEED_IDR to recover the stream state. */
        LiCompleteVideoFrame(frameHandle, success ? DR_OK : DR_NEED_IDR);
    }
}

/**
 * Submit raw H.264 NAL frame for hardware decode.
 * No per-frame logging on hot path — only errors are logged.
 */
int video_decoder_submit_frame(VideoDecoder* d, const void* data, unsigned int size) {
    d->frame_count++;

    if (!d || !d->initialized || !data || size == 0) return -1;
    if (size > MAX_FRAME_SIZE) return -1;

    /* Data is already in d->staging_buf from video_decoder_update reassembly. 
       We only update the size metadata and verify sanity. */
    d->staging_size = size;
    d->staging_consumed = 0;

    /* Calculate packets needed */
    const int payload_per_pkt = MPEG_PACKET_SIZE - (14 + 9 + 5);
    int needed = (size + payload_per_pkt - 1) / payload_per_pkt;

    /* Check ringbuffer space - NEVER push a partial frame on PSP-1000 hardware */
    int avail = sceMpegRingbufferAvailableSize(&d->ringbuffer);
    if (avail < needed) {
        /* If it doesn't fit, we drop the whole frame to prevent ME desync */
        d->dropped_frames++;
        if (d->frame_count % 60 == 0) {
            LOG_INFO(COMPONENT_VIDEO, "[F%d] Dropping frame: ringbuffer full (need %d, avail %d)", d->frame_count, needed, avail);
        }
        return -1;
    }

    g_active_decoder = d;

    /* Push into ringbuffer (triggers callback for each packet) */
    int put = sceMpegRingbufferPut(&d->ringbuffer, needed, avail);
    
    if (d->frame_count <= 3) LOG_INFO(COMPONENT_VIDEO, "[F%d] size=%u put=%d avail=%d", d->frame_count, size, put, avail);
    if (put < 0) return -1;

    /* Extract Access Unit */
    int au = sceMpegGetAvcAu(&d->mpeg, d->videoStream, &d->avcAu, NULL);
    
    if (au == (int)0x80618001) return 0;  /* Need more data — normal */
    if (au < 0) {
        LOG_ERROR(COMPONENT_VIDEO, "[F%03d] GetAvcAu CRITICAL ERROR: 0x%08X", d->frame_count, au);
        /* On real hardware, we attempt one flush and return instead of panicking immediately */
        sceMpegFlushAllStream(&d->mpeg);
        return -1;
    }

    /* Decode */
    SceInt32 iInit = 0;
    void* frame_out = d->me_output_frame;
    unsigned long long start = sceKernelGetSystemTimeWide();
    if (d->frame_count % 600 == 0) {
        LOG_INFO(COMPONENT_VIDEO, "[F%03d] HEARTBEAT: sceMpegAvcDecode START", d->frame_count);
    }
    int res = sceMpegAvcDecode(&d->mpeg, &d->avcAu, FRAME_STRIDE, &frame_out, &iInit);
    if (d->frame_count % 600 == 0) {
        LOG_INFO(COMPONENT_VIDEO, "[F%03d] HEARTBEAT: sceMpegAvcDecode END (res=0x%08X)", d->frame_count, res);
    }
    unsigned long long end = sceKernelGetSystemTimeWide();

    if ((end - start) > 33000) {
        int avail = sceMpegRingbufferAvailableSize(&d->ringbuffer);
        LOG_INFO(COMPONENT_VIDEO, "[F%03d] Decode Spike: %lluus, Avail: %d", d->frame_count, end - start, avail);
    }

    if (res < 0) {
        LOG_ERROR(COMPONENT_VIDEO, "[F%03d] AvcDecode CRITICAL ERROR: 0x%08X", d->frame_count, res);
        /* If ME crashes, we must refresh to attempt recovery */
        video_decoder_refresh(d);
        return -1;
    }

    if (iInit == 1) {
        d->me_output_frame = frame_out;
        d->frame_ready = 1;
    }

    return 0;
}

unsigned int video_decoder_get_frame_count(VideoDecoder* d) {
    return d ? d->frame_count : 0;
}

unsigned long long video_decoder_get_total_bytes(VideoDecoder* d) {
    return d ? d->total_bytes : 0;
}

unsigned int video_decoder_get_dropped_frames(VideoDecoder* d) {
    return d ? d->dropped_frames : 0;
}

void video_decoder_refresh(VideoDecoder* d) {
    if (d && d->initialized) {
        LOG_INFO(COMPONENT_VIDEO, "RAM Cycle Refresh: Flushing MPEG stream");
        sceMpegFlushAllStream(&d->mpeg);
    }
}

void* video_decoder_get_output_frame(VideoDecoder* d,
                                      unsigned int* width, unsigned int* height) {
    if (!d || !d->initialized || !d->frame_ready) {
        if (width) *width = 0;
        if (height) *height = 0;
        return NULL;
    }
    if (width) *width = d->output_width;
    if (height) *height = d->output_height;
    d->frame_ready = 0;
    return d->me_output_frame;  /* Return ME-owned pointer, not our allocation */
}

int video_decoder_is_initialized(VideoDecoder* d) { return d ? d->initialized : 0; }
int video_decoder_get_last_error(VideoDecoder* d) { (void)d; return 0; }
const char* video_decoder_get_error_message(VideoDecoder* d) { (void)d; return ""; }
void video_decoder_clear_error(VideoDecoder* d) { (void)d; }

static int init_media_engine(VideoDecoder* d) {
    int res;

    int avres = sceUtilityLoadModule(PSP_MODULE_AV_AVCODEC);
    if (avres < 0 && avres != (int)0x80020139) return -1;

    int mpgres = sceUtilityLoadModule(PSP_MODULE_AV_MPEGBASE);
    if (mpgres < 0 && mpgres != (int)0x80020139) return -1;

    res = sceMpegInit();
    if (res < 0) {
        exception_handler_trigger_manually("sceMpegInit Failure", res);
        return res;
    }

    /* Allocate MPEG context memory (64-byte aligned) */
    int mpeg_size = sceMpegQueryMemSize(0);
    d->mpeg_mem_base = memalign(64, mpeg_size);
    if (!d->mpeg_mem_base) return -1;

    /* Allocate ringbuffer memory */
    int rb_size = sceMpegRingbufferQueryMemSize(MPEG_RINGBUFFER_PACKETS);
    d->ringbuffer_mem_base = memalign(64, rb_size);
    if (!d->ringbuffer_mem_base) return -1;

    res = sceMpegRingbufferConstruct(&d->ringbuffer, MPEG_RINGBUFFER_PACKETS,
                                     d->ringbuffer_mem_base, rb_size,
                                     ringbuffer_callback, NULL);
    if (res < 0) {
        exception_handler_trigger_manually("Ringbuffer Construction Failure", res);
        return res;
    }

    res = sceMpegCreate(&d->mpeg, d->mpeg_mem_base, mpeg_size,
                        &d->ringbuffer, FRAME_STRIDE, 0, 0);
    if (res < 0) {
        exception_handler_trigger_manually("sceMpegCreate Failure", res);
        return res;
    }

    d->videoStream = sceMpegRegistStream(&d->mpeg, 0, 0);
    if (!d->videoStream) {
        exception_handler_trigger_manually("sceMpegRegistStream Failure", 0);
        return -1;
    }

    d->avcEsBuf = sceMpegMallocAvcEsBuf(&d->mpeg);
    if (!d->avcEsBuf) {
        exception_handler_trigger_manually("sceMpegMallocAvcEsBuf Failure", 0);
        return -1;
    }

    res = sceMpegInitAu(&d->mpeg, d->avcEsBuf, &d->avcAu);
    if (res < 0) {
        exception_handler_trigger_manually("sceMpegInitAu Failure", res);
        return res;
    }

    /* RGB 565 output mode — 50% memory saving vs 8888 */
    SceMpegAvcMode mode;
    mode.iUnk0 = -1;
    mode.iPixelFormat = SCE_MPEG_AVC_FORMAT_5650;
    res = sceMpegAvcDecodeMode(&d->mpeg, &mode);
    if (res < 0) return res;

    /* Defaults - set to 480x272 to match Moonlight standard */
    d->output_width  = 480;
    d->output_height = 272;

    /* Allocate decode target buffer for MAX size (512x272 @ 16-bit) to avoid reallocs */
    int frame_bytes = FRAME_STRIDE * 272 * 2;
    d->decode_target = (unsigned char*)memalign(64, frame_bytes);
    if (!d->decode_target) return -1;
    memset(d->decode_target, 0, frame_bytes);

    d->me_output_frame = d->decode_target;

    /* Staging buffer for raw H.264 - 64-byte aligned for Absolute Perfection */
    d->staging_buf = (unsigned char*)memalign(64, MAX_FRAME_SIZE);
    if (!d->staging_buf) return -1;

    LOG_INFO(COMPONENT_VIDEO, "ME init OK: ES=%p target=%p %dx%d stride=%d",
             d->avcEsBuf, d->decode_target, d->output_width, d->output_height, FRAME_STRIDE);
    return 0;
}

static void term_media_engine(VideoDecoder* d) {
    if (d->avcEsBuf) { sceMpegFreeAvcEsBuf(&d->mpeg, d->avcEsBuf); d->avcEsBuf = NULL; }
    if (d->videoStream) { sceMpegUnRegistStream(d->mpeg, d->videoStream); d->videoStream = NULL; }
    sceMpegDelete(&d->mpeg);
    sceMpegRingbufferDestruct(&d->ringbuffer);
    sceMpegFinish();
    sceUtilityUnloadModule(PSP_MODULE_AV_MPEGBASE);
    sceUtilityUnloadModule(PSP_MODULE_AV_AVCODEC);
}

void video_decoder_set_stream_resolution(VideoDecoder* d, unsigned int width, unsigned int height) {
    if (d) {
        if (width > 480 || height > 272) {
            LOG_ERROR(COMPONENT_VIDEO, "Unsupported resolution: %dx%d (MAX 480x272)", width, height);
            return;
        }
        
        if (d->output_width != width || d->output_height != height) {
            LOG_INFO(COMPONENT_VIDEO, "Resolution change: %dx%d -> %dx%d", d->output_width, d->output_height, width, height);
            
            d->output_width = width;
            d->output_height = height;

            /* Reconfigure Media Engine output mode if initialized */
            if (d->initialized) {
                SceMpegAvcMode mode;
                mode.iUnk0 = -1;
                mode.iPixelFormat = SCE_MPEG_AVC_FORMAT_5650;
                sceMpegAvcDecodeMode(&d->mpeg, &mode);
            }
        }
    }
}
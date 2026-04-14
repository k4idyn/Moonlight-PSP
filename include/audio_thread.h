/*
 * audio_thread.h - Moonlight Audio Receiver and Playback Thread
 *
 * Receives RTP/UDP audio from the Sunshine host on port 48000,
 * decodes PCM frames, and outputs them via sceAudio at 48000 Hz stereo.
 *
 * Architecture:
 *   Network Thread → AudioRingBuffer → Audio Thread → sceAudioOutputBlocking
 *
 * Thread priority: 0x1C (between ME decoder 0x1A and main thread 0x30).
 *
 * NOTE: sceAudioOutputBlocking() must NEVER be called from the main thread;
 *       it blocks until DMA completes (~11.6 ms at 44100/512 samples).
 */

#ifndef AUDIO_THREAD_H
#define AUDIO_THREAD_H

#include <psptypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/*--------------------------------------------------------------------------
 * Audio Configuration
 *--------------------------------------------------------------------------*/

/** Host UDP port for audio RTP stream (Moonlight default) */
#define AUDIO_PORT              48000

/** Sample rate expected from Sunshine (Hz) */
#define AUDIO_SAMPLE_RATE       48000

/** Stereo pairs */
#define AUDIO_CHANNELS          2

/**
 * PSP sceAudio DMA chunk size in stereo samples.
 * sceAudioChReserve() requires power-of-2 values in [64, 65536].
 * 512 samples / 48000 Hz = 10.67 ms per chunk — low enough for smooth audio.
 */
#define AUDIO_CHUNK_SAMPLES     512

/**
 * Maximum Opus frame size in samples per channel.
 * Sunshine may send 5 ms (240), 10 ms (480), or 20 ms (960) frames.
 * 960 = 20 ms @ 48 kHz — the largest standard Opus frame duration.
 */
#define AUDIO_MAX_FRAME_SAMPLES 960

/** Maximum RTP payload (Moonlight uses up to 1400 bytes of PCM/OPUS) */
#define AUDIO_MAX_RTP_SIZE      1400

/**
 * Number of audio ring-buffer slots.
 * 64 × 512 samples / 48000 Hz ≈ 682 ms of pre-buffer — absorbs WiFi
 * jitter and bursty packet arrivals on 802.11b.  Doubled from 32 to
 * reduce ring-full drops and underruns caused by recv/decode bursts.
 */
#define AUDIO_RING_SLOTS        64

/*--------------------------------------------------------------------------
 * Audio Ring Buffer (lock-free SPSC)
 *--------------------------------------------------------------------------*/
typedef struct {
    /* Each slot holds one decoded chunk: AUDIO_CHUNK_SAMPLES × 2 channels × 2 bytes */
    int16_t pcm[AUDIO_RING_SLOTS][AUDIO_CHUNK_SAMPLES * AUDIO_CHANNELS];
    volatile u32 head;   /* writer (network/decode) advances */
    volatile u32 tail;   /* reader (audio thread) advances */
} AudioRingBuffer;

/*--------------------------------------------------------------------------
 * Statistics
 *--------------------------------------------------------------------------*/
typedef struct {
    u32 frames_played;
    u32 frames_dropped;   /* ring full */
    u32 underruns;        /* ring empty when audio thread needed data */
} AudioStats;

/*--------------------------------------------------------------------------
 * Public API
 *--------------------------------------------------------------------------*/

/**
 * audio_thread_reserve_client_port - Pre-binds consecutive UDP sockets for audio
 *
 * Used by network_connect.c before RTSP SETUP to ensure we have a valid port.
 */
int audio_thread_reserve_client_port(unsigned short *out_port);

/**
 * audio_thread_start_ping_only - Start the audio ping thread before RTSP PLAY
 *
 * Moonlight-common-c brings up the audio socket and ping path before PLAY so
 * the host learns the client audio endpoint during session startup. This
 * starts only the ping thread on the already-reserved socket; full audio
 * decode/playback still starts later via audio_thread_init().
 *
 * Returns: 0 on success, negative on error
 */
int audio_thread_start_ping_only(void);

/**
 * audio_thread_init - Start the audio receive + playback thread
 *
 * @host_ip: IPv4 string of the Sunshine host (e.g. "192.168.1.100")
 *
 * Uses the already-reserved UDP sockets, reserves the PSP audio channel,
 * allocates the ring buffer, and starts the audio thread.
 *
 * Returns: 0 on success, negative on error
 */
int audio_thread_init(const char *host_ip);

/**
 * audio_thread_shutdown - Stop the audio thread and release resources
 *
 * Signals the thread to exit, waits for it, then closes socket and
 * releases the sceAudio channel.
 */
void audio_thread_shutdown(void);

/**
 * audio_thread_is_running - Check if the audio thread is active
 *
 * Returns: 1 if running, 0 if stopped or not started
 */
int audio_thread_is_running(void);

/**
 * audio_thread_send_ping_burst - Send a rapid burst of audio pings after PLAY
 *
 * Mirrors the video ping burst in network_me.  Sends 3 pings with 30 ms
 * spacing from the pre-bound audio socket to the server audio port so
 * Sunshine can lock onto the client audio endpoint immediately.
 *
 * Returns: 0 on success (at least one ping sent), negative on error
 */
int audio_thread_send_ping_burst(void);

/**
 * audio_thread_get_stats - Retrieve playback statistics
 */
void audio_thread_get_stats(AudioStats *out);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_THREAD_H */

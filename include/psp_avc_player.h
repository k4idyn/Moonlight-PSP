#ifndef MOONLIGHT_PSP_AVC_PLAYER_H
#define MOONLIGHT_PSP_AVC_PLAYER_H
#ifndef PSP_AVC_PROFILE_CHANGE_REQUIRED
#define PSP_AVC_PROFILE_CHANGE_REQUIRED 4
#endif
/* Experimental single-session player handoff. Decode/reset run on one worker.
 * take/release run on presenter. Join worker and release held pictures BEFORE
 * close. No custom ME worker may be initialized while this player is open. */
int psp_avc_player_open(unsigned int width,unsigned int height,const char *helper);
/* Selects the initial Sony mode. The first in-band PPS can request an idle
 * UI-thread switch before any IDR is submitted. */
int psp_avc_player_open_profile(unsigned int width,unsigned int height,
                                const char *helper,int main_profile);
int psp_avc_player_profile_change_pending(void);
int psp_avc_player_profile_change_ready(void);
int psp_avc_player_requested_profile(void);
int psp_avc_player_current_profile(void);
int psp_avc_player_apply_profile_change(void);
/* Open means the player owns resources; ready additionally requires a usable
 * Sony session.  Call open to repair retained-but-unready state before RTP. */
int psp_avc_player_is_open(void);
int psp_avc_player_is_ready(void);
void psp_avc_player_log_state(const char *phase);
/* 0=output published, 1=parameters only, 2=waiting IDR, 3=buffered.
 * -3=no writable slots: retain AU and retry (not consumed); other negatives
 * indicate failure. Output publication may replace an older queued picture. */
int psp_avc_player_decode(const unsigned char *data,unsigned int size);
/* Returns newest owned frame, or NULL. Must release after GPU completion.
 * sequence is a decoded-output ordinal, NOT the network input frame id;
 * decoded_at_us receives the PSP monotonic timestamp when output was ready. */
void *psp_avc_player_take(unsigned int *sequence,unsigned int *decoded_at_us);
int psp_avc_player_release(void *frame);
int psp_avc_player_reset(void);
/* Final process/module cleanup after stream workers have stopped. Ordinary
 * per-stream close intentionally retains reusable frame-pool memory. */
int psp_avc_player_destroy(void);
int psp_avc_player_close(void);
#endif

/*
 * upnp_client.h - Minimal UPnP IGD client for hotspot/remote streaming
 *
 * Best-effort helper that requests UDP port mappings on the local gateway
 * so incoming Moonlight RTP/RTCP packets can reach the PSP when needed.
 */

#ifndef UPNP_CLIENT_H
#define UPNP_CLIENT_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * upnp_prepare_stream_mappings - Best-effort UPnP mapping for active stream ports.
 *
 * target_host: remote Sunshine host address selected by the user.
 * video_port: local UDP video RTP port reserved during RTSP SETUP.
 * audio_enabled: non-zero when audio RTSP SETUP succeeded.
 * audio_port: local UDP audio RTP port (ignored when audio_enabled == 0).
 *
 * Returns:
 *  >0 : number of UDP ports mapped
 *   0 : intentionally skipped (for example private/LAN target)
 *  <0 : attempted but failed (stream should continue without UPnP)
 */
int upnp_prepare_stream_mappings(const char *target_host,
                                 unsigned short video_port,
                                 int audio_enabled,
                                 unsigned short audio_port);

/*
 * upnp_remove_stream_mappings - Remove previously created mappings.
 *
 * Safe to call repeatedly. Failures are logged but never fatal.
 */
void upnp_remove_stream_mappings(void);

#ifdef __cplusplus
}
#endif

#endif /* UPNP_CLIENT_H */

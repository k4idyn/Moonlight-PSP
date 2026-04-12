/*
 * moonlight_ports.h - Authoritative Moonlight/Sunshine port constants
 *
 * All port numbers used by the Moonlight protocol are defined here.
 * No other source file should hardcode port numbers — include this header
 * and reference the macros instead.
 */

#ifndef MOONLIGHT_PORTS_H
#define MOONLIGHT_PORTS_H

/* HTTP port — used for initial pairing handshake (getservercert, pair
 * challenge/response, serverinfo).  Plain HTTP, no TLS. */
#define MOONLIGHT_HTTP_PORT         47989

/* HTTPS port — used for launch, resume, cancel, unpair, applist,
 * appasset, and all requests requiring mutual TLS (client cert).
 * Sunshine default. */
#define MOONLIGHT_HTTPS_PORT        47984

/* RTSP port — used for the RTSP handshake that negotiates video,
 * audio, and control transport parameters (OPTIONS, DESCRIBE,
 * SETUP, ANNOUNCE, PLAY, TEARDOWN). */
#define MOONLIGHT_RTSP_PORT         48010

/* RTSP fallback — legacy NVIDIA GFE or older Sunshine versions might 
 * listen on 47990. Port 47998 is used specifically for VIDEO/UDP. */
#define MOONLIGHT_RTSP_PORT_LEGACY  47990

/* Control port — TCP connection for sending controller input packets
 * (Type-5 CONTROLLER_EVENT, Type-8 MOUSE_MOVE_EVENT, etc.). */
#define MOONLIGHT_CONTROL_PORT      47999

/* Discovery port — UDP broadcast/multicast for LAN discovery. */
#define MOONLIGHT_DISCOVERY_PORT    47998

/* Audio RTP port — UDP port for incoming Opus/PCM audio stream. */
#define MOONLIGHT_AUDIO_PORT        48000

/* Video RTP port — UDP port for incoming H.264 video stream.
 * Client binds to this port; advertised to Sunshine via SETUP Transport. */
#define MOONLIGHT_VIDEO_PORT        47998

#endif /* MOONLIGHT_PORTS_H */

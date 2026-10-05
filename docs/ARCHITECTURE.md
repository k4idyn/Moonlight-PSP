# Architecture

_PSP Moonlight v1.5.0_

PSP Moonlight connects to Sunshine hosts over Wi-Fi and handles discovery, pairing, streaming, controls, audio, video presentation, and the stream HUD on the PSP.

## Shared streaming path

The Main CPU manages host discovery, authenticated pairing, RTSP session setup, encrypted RTP traffic, packet reassembly, forward error correction, controller input, and Opus audio. The PSP graphics engine presents decoded video and the interface.

The network, control, audio and session-management components feed the Sony AVC decoder and PSP presenter.

## Host connection and transport

The client discovers Sunshine hosts, completes certificate-based pairing, and establishes an RTSP stream session. Encrypted H.264 video and Opus audio arrive over UDP. The video path reassembles RTP access units and uses forward error correction when packets are missing. Controller and keyboard input are sent through the Moonlight control channel.

When video data cannot be recovered, the client requests a new IDR and resumes presentation when a valid reference frame arrives. The stream exit path closes the network session, decoder, audio output, and helper resources before returning to the host list or PSP shell.

## Sony hardware AVC

The hardware build sends complete H.264 access units to Sony's sceMpeg AVC framework. It parses the in-band SPS and PPS and selects the matching firmware mode before decoding the first IDR:

- CAVLC uses Sony Baseline mode.
- CABAC uses Sony Main mode.

The Media Engine helper loads the Sony firmware providers and supports AVC mode setup. Decoded pictures are converted to RGBA output and passed to the PSP graphics engine.

The player uses six aligned output surfaces with a 512-pixel stride. Each surface can hold a 480x272 RGBA frame; the pool uses about 3.34 MB (3.19 MiB).

## Processor roles

| Component | Role |
|---|---|
| Main CPU | Host discovery, pairing, network transport, RTP/FEC, input, audio decoding, application UI, and stream coordination |
| Media Engine | Sony AVC decoding and picture conversion |
| PSP graphics engine | Video scaling, interface rendering, HUD composition, and display output |

## Source map

- src/network_connect.c — host connection, pairing, TLS, and RTSP setup
- src/network_me.c — video and audio packet receive
- src/rtp_reassembly.c and src/rtp_fec.c — packet assembly and recovery
- src/psp_avc_backend.c and src/psp_avc_session.c — Sony AVC session management
- src/psp_avc_submit.c and src/psp_avc_player.c — access-unit submission and frame ownership
- src/decoder_thread.c — shared decode dispatch and frame scheduling
- src/audio_thread.c — Opus decoding and PSP audio output
- src/display_gpu.c and src/hud.c — frame presentation and stream status
- moonlight_me_helper/ — kernel helper for Media Engine and Sony AVC setup

## Protocols

| Layer | Protocol or service |
|---|---|
| Pairing and session security | HTTPS, TLS, and Moonlight challenge-response pairing |
| Stream setup | RTSP |
| Video and audio | Encrypted RTP over UDP with FEC support |
| Host control and input | Moonlight control protocol over UDP |

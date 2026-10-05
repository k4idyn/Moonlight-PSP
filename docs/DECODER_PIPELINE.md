# H.264 Decoder Pipeline

_PSP Moonlight v1.5.0_

PSP Moonlight uses Sony hardware AVC decoding with its PSP-native network receive, packet assembly, control, audio and display systems.

## Sony hardware AVC

The hardware path prepares H.264 access units from the incoming RTP stream, reads the in-band sequence and picture parameter sets, and selects the matching Sony firmware mode before submitting the first IDR.

- CAVLC streams use Sony Baseline mode.
- CABAC streams use Sony Main mode.

The decoder submits access units through Sony's sceMpeg AVC interface. The resulting pictures are converted to RGBA and passed to the frame presenter. The Media Engine helper provides Sony firmware setup and import support.

## Frame presentation

The AVC player owns six aligned RGBA output surfaces with a 512-pixel stride. The pool supports decode, queued output, and display ownership without reusing a surface while the graphics engine is reading it. The display path scales frames up to the PSP's 480x272 screen.

## Shared stream path

The common stream path handles encrypted RTP, packet reassembly, FEC recovery, input, Opus audio, frame scheduling, and clean session teardown. The stream HUD reports current decoder, renderer, memory, bandwidth, audio, and battery information.

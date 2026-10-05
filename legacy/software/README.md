# Historical software decoder

This directory preserves the OpenH264 PSP decoder wrapper, Main CPU/Media Engine conversion implementation and bundled OpenH264 source from earlier versions. The v1.5 application uses Sony hardware AVC; these files are excluded from its build.

- `src/openh264_decode.cpp`: OpenH264 decoder wrapper.
- `src/me.c`: software frame conversion and Media Engine coordination.
- `third_party/openh264/`: bundled decoder source and its historical PSP Makefile.

The current shared stream interfaces are in `include/decoder_pipeline.h`. Other earlier software decoder implementations are preserved in the parent `legacy/` directory.

Retain the bundled [OpenH264 license](third_party/openh264/LICENSE) when redistributing these sources.

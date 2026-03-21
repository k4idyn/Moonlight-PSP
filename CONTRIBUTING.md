# Contributing to Moonlight PSP Core

Thank you for your interest in contributing to the Moonlight PSP client! As a project targeting the constraints of legacy hardware (the Sony PlayStation Portable), contributions must be highly optimized and mindful of memory/CPU limitations.

## Development Guidelines

- **Language:** Stick strictly to C99 and C++98/03 where applicable. Modern C++ features may not properly compile or optimize on the MIPS toolchain provided by `pspdev`.
- **Memory Management:** The PSP-1000 has a hard 32MB RAM limit.
  - Avoid dynamic allocations (`malloc`/`new`) during the active stream.
  - Always use pre-allocated buffers pool where possible.
- **Hardware Acceleration:** All modifications to video decoding should interface safely with the Media Engine (ME) via our abstracted VRAM/AVC pipeline. Fallback software decode should only be used defensively.

## Setting Up Your Environment

This project utilizes the standard `pspsdk` infrastructure. 
We strongly recommend using the official [pspdev Docker image](https://github.com/pspdev/pspdev) to ensure your toolchain exactly matches the CI environment:

```bash
docker run -it -v "/path/to/moonlight-psp-core:/src" pspdev/pspdev:latest /bin/bash
cd /src
make clean && make
```

## Pull Request Process

1. Fork the repo and create your feature branch (`git checkout -b feature/amazing-feature`).
2. Ensure your code compiles without warnings (`make`).
3. Test the built `EBOOT.PBP` in the **PPSSPP emulator** with hardware decoding forced ON and OFF.
4. If testing on genuine PSP hardware, test on a physical **PSP-1000** first, as it possesses the strictest memory limits.
5. Submit a Pull Request with a detailed summary of changes, linking any relevant issues.

## Testing

When altering protocol components (handshake, polling, encryption), you must verify connection stability against a live NVIDIA GameStream or Sunshine host. Ensure no memory leaks trigger an `EBOOT` crash after a 10-minute continuous stream.

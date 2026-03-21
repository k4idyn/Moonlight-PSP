# Release Notes v0.1.0.2-alpha

## Overview
This alpha release identifies and resolves the primary driver of the `0x80020320` (too many open files) error during the pairing and certificate loading process. 

## Technical Findings
1. **Socket Descriptor Leak**: The `libgamestream` HTTP implementation used standard POSIX `close()` for sockets. On the PSP, network sockets are managed by the `pspnet_inet` stack and require `closeSocket()` (mapped to `sceNetInetClose`) to be properly released to the system pool.
2. **FAT Driver Asynchronicity**: The PSP's Memory Stick driver (FAT/sceIo) handles file closures asynchronously. High-frequency logging (opening/writing/closing 50+ times per second) during the pairing handshake overwhelmed the system, leaking effective handles until the system reached the hard kernel limit.

## Fixes
- **MFILE Protection**: Rewrote `logger.c` to use a global persistent file handle initialized at boot. This bypasses the asynchronous close queue and significantly improves I/O performance.
- **Socket Integrity**: Patched `http.c` and associated TLS cleanup macros to use `closeSocket()`.

## Known Issues
- **0x80 (ENOTCONN)**: Connection to the host's control stream ends prematurely with error code 128 (ENOTCONN). This is currently being investigated as a possible ENet protocol handshake failure or Sunshine server-side rejection.

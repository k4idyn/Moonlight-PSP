# PSP Networking Stack Research: enet, mbedtls, and Alternatives

## Executive Summary
This research examines networking stack options for PlayStation Portable (PSP) homebrew development, focusing on porting enet (reliable UDP networking library) and mbedtls (TLS/SSL library) to the PSP platform, along with alternative networking solutions.

## 1. ENet Porting to PSP

### What's Needed
- ENet is built on top of the socket API, requiring a compatible socket implementation
- PSP provides sceNet libraries (part of PSPSDK) that wrap standard TCP/IP BSD sockets
- Porting would involve adapting ENet to work with PSP's sceNet socket API
- Need to handle PSP-specific networking initialization and cleanup

### Existing Ports
- No direct ENet port to PSP found in searches
- However, references indicate ENet has been used in PSP homebrew contexts:
  - GameDev.net post mentions "this other library I've seen in a few projects, notably Cube, called enet"
  - Indicates some homebrew projects have successfully integrated ENet-like functionality

### Challenges
- PSP networking requires initialization via sceNet functions before socket use
- Memory constraints (32MB total system memory)
- Need to adapt ENet's memory allocation to PSP's memory model
- Potential endianness considerations (PSP uses MIPS architecture)
- Limited documentation on PSP-specific socket behavior

## 2. mbedTLS Porting to PSP

### What's Needed
- Cross-compilation toolchain for PSP (MIPS architecture)
- Entropy source for cryptographic operations (PSP hardware RNG or software fallback)
- Network abstraction layer (mbedTLS needs to interface with sceNet sockets)
- Proper threading and timeout implementations compatible with PSP

### Existing Ports
- Interview on PSP Archive ("Getting the PSP back online: the first TLS app") confirms mbedTLS was used:
  - "The old version used mbedtls, a C library, which had to be cross-compiled with llvm/clang"
  - Developer reported "mysterious error" when trying to use it
- References to mbedTLS being used with cURL for TLS1.2 networking on PSP
- Homebrew Developer Conference talks mentioned "SSL with cURL (Bucanero); Getting secure TLS1.2 networking on PSP with cURL + mbedTLS"

### Performance Considerations
- mbedTLS is designed for embedded systems with configurable features
- Can be tuned for memory usage vs. performance trade-offs
- PSP's MIPS CPU may benefit from hardware acceleration if available
- Memory footprint can be reduced by disabling unused cipher suites and features

## 3. Alternative Networking Libraries for PSP Homebrew

### sceNet (Native PSP Networking)
- Part of the official PSPSDK
- Provides BSD socket-compatible interface
- Functions include: sceNetInit, sceNetTerm, sceNetSocket, sceNetConnect, etc.
- Well-documented in PSPSDK samples (e.g., pspsdk/src/samples/net/simple/main.c)
- Most straightforward option for PSP networking

### lwIP (Lightweight IP)
- No specific PSP ports found in searches
- lwIP is designed for embedded systems and could potentially be ported
- Would require adaptation to PSP's networking hardware and memory model
- More complex than using sceNet directly but offers more control

### OpenSSL PSP Port
- Found: "OpenSSL PSP Port by Raf." (PSP-Archive/openssl on GitHub)
- Provides TLS/SSL capabilities as alternative to mbedTLS
- May have different performance/memory characteristics than mbedTLS

## 4. TLS/SSL Options for PSP Beyond mbedTLS

### OpenSSL Port
- Direct port of OpenSSL to PSP available
- More feature-complete but heavier weight than mbedTLS
- Established library with broad compatibility

### drogue-tls
- Mentioned in PSP Archive interview: "more will come as drogue-tls becomes more feature-complete"
- Modern TLS 1.3-focused library designed for constrained devices
- Potentially better suited for PSP's limitations than traditional TLS libraries

### WolfSSL (formerly CyaSSL)
- Not found in specific PSP searches but known to support embedded platforms
- Designed for resource-constrained environments
- Commercial and open-source versions available

## 5. Performance Characteristics

### Memory Usage Considerations
- PSP has 32MB total system memory (shared between CPU and GPU)
- Networking libraries must be lightweight to leave room for application logic
- ENet: Minimal by design (typically <100KB RAM for basic usage)
- mbedTLS: Configurable; can be tuned to ~50-100KB RAM with minimal features
- sceNet: Part of PSPSDK; memory usage depends on initialization but generally lightweight

### CPU Overhead
- ENet: Low CPU overhead for reliable UDP; designed for real-time applications
- mbedTLS: Moderate to high CPU overhead for TLS handshakes; symmetric encryption is faster
- PSP MIPS CPU (333MHz) can handle lightweight TLS but may struggle with heavy handshakes

### Latency
- ENet: Designed for low-latency reliable UDP; adds minimal latency over raw UDP
- TCP-based solutions (including TLS over TCP): Higher latency due to TCP handshakes and congestion control
- For real-time applications (gaming, video streaming), ENet may be preferable

## 6. Integration with pspsdk

### Linking Libraries
- PSPSDK uses standard GCC-based toolchain for MIPS
- Libraries can be linked using standard `-l` flags in Makefile
- Example linking for networking: `-lpspnet -lpspnet_adhoc -lpspnet_adhocmatching -lpspnet_adhocctl -lpspnet_inet -lpspnet_resolver -lpspnet_apctl`

### Usage Pattern
1. Initialize networking: `sceNetInit()`
2. Create sockets using standard BSD socket API via sceNet wrappers
3. For ENet: Wrap sceNet socket functions in ENet's socket callbacks
4. For mbedTLS: Implement mbedTLS net_send/net_recv callbacks using sceNet sockets
5. Proper cleanup: `sceNetTerm()` on exit

### Sample Integration Approach
```c
// ENet with PSP sceNet
ENetSocket enet_socket_wrap(void *address) {
    int sock = sceNetSocket(AF_INET, SOCK_DGRAM, 0);
    // ... configure socket ...
    return sock;
}

int enet_socket_send(ENetSocket socket, const void *data, size_t dataSize, const ENetAddress *address) {
    // ... convert ENetAddress to sockaddr_in ...
    return sceNetSendto(socket, data, dataSize, 0, (struct sockaddr *)&addr, sizeof(addr));
}

// Similar wrappers for recv, etc.
```

## 7. Existing PSP Homebrew Projects Using Networking

### Confirmed Networking Homebrew
- cURL with mbedTLS for HTTPS connections (mentioned in homebrew conference)
- Various homebrew applications using sceNet directly for TCP/UDP communication
- Homebrew web browsers and downloaders requiring TLS/SSL

### ENet-Specific Projects
- No specific ENet-based PSP homebrew found in searches
- However, the GameDev.net reference to Cube using ENet suggests at least one project has attempted or succeeded in porting/using ENet

## 8. Memory Footprint Considerations for 32MB Limit

### Critical Constraints
- PSP allocates memory between CPU (main RAM) and GPU (VRAM)
- Typical homebrew gets ~20-24MB for code/data after system reservations
- Networking libraries must be conservative in memory usage

### Recommendations
- **ENet**: Excellent choice for memory-constrained environments
  - Static memory allocation options available
  - Peer count and packet limits directly control memory usage
  - Typical usage: 20-50KB RAM for modest peer counts
  
- **mbedTLS**: Requires careful configuration
  - Disable unused features (MBEDTLS_XXX_C flags)
  - Reduce buffer sizes (MBEDTLS_SSL_IN_CONTENT_LEN, etc.)
  - Consider using asymmetric crypto only for handshake, symmetric for data
  - Target: 50-100KB RAM for basic TLS client
  
- **sceNet**: Minimal overhead
  - Most memory used by socket buffers (configurable)
  - Base networking stack: ~10-20KB

## 9. Reliable UDP Implementation Options for PSP

### ENet
- Primary choice for reliable UDP
- Features: automatic retransmission, sequencing, bandwidth limiting, connection management
- Well-suited for real-time applications like game networking

### Custom Reliable UDP
- Could implement lightweight reliable UDP on top of sceNet UDP sockets
- Simpler than full ENet but lacks advanced features
- Viable for simple applications with few peers

### Alternative Libraries
- RakNet: Not found in PSP searches but used in game development
- Lidgren Network: C# library, not suitable for PSP native development
- enet remains the best C-based reliable UDP library for porting

## 10. Recommendations

### For Real-Time Applications (Gaming, Video Chat)
1. **Primary**: Port ENet to PSP using sceNet socket wrappers
2. **Alternative**: Implement custom lightweight reliable UDP if ENet porting proves difficult
3. **Avoid**: TCP-based solutions due to higher latency

### For Secure Communications (HTTPS, API Calls)
1. **Primary**: Use mbedTLS with careful configuration for minimal footprint
2. **Alternative**: Use OpenSSL PSP port if features needed outweigh size concerns
3. **Emerging**: Monitor drogue-tls development for potential better fit

### For General Networking
1. **Primary**: Use sceNet directly (BSD sockets) for maximum compatibility and control
2. **Consider**: lwIP port if needing more control than sceNet provides but wanting lighter weight than full TCP/IP stack

## Implementation Roadmap

### Phase 1: ENet Porting Investigation
1. Examine ENet source to identify socket abstraction layer
2. Create PSP-specific socket adapter using sceNet functions
3. Test basic connectivity with simple client/server
4. Optimize memory usage for PSP constraints

### Phase 2: Security Layer Evaluation
1. Test mbedTLS cross-compilation for PSP
2. Implement net callbacks using sceNet sockets
3. Benchmark handshake and data transfer performance
4. Compare with OpenSSL port if mbedTLS proves problematic

### Phase 3: Integration Testing
1. Combine ENet and security layers for secure reliable UDP (if needed)
2. Test with actual PSP homebrew application
3. Document memory usage and performance characteristics
4. Create sample project demonstrating usage

## Conclusion
While no turnkey ENet or mbedTLS ports for PSP were found in searches, the platform provides sufficient networking foundation (sceNet) to port these libraries. ENet appears particularly well-suited for PSP's constraints given its lightweight nature and focus on reliable UDP. mbedTLS is viable for TLS needs but requires careful configuration to fit within PSP's memory limits. The native sceNet libraries offer the most straightforward path for basic networking needs.
# Technical Handbook Dictionary - PSP Moonlight

This document contains relational dictionary entries cross-referencing psp_moonlight source code analysis with antigravity research legacy.

## Main.c Entries

## libgamestream/client.h Entries

### SERVER_DATA: (Structure)
Type: Code Logic
Definition: Structure containing server information including GPU type, pairing status, version information, display modes, and network ports.
Antigravity Context: From comparison_report.md - "libgamestream/client.c" shows mismatches where Resources version has Sunshine compatibility fixes missing in Repository. This structure is defined in client.h and used throughout the client.c implementation.
Dependencies:
- gs_init() function
- gs_cleanup() function
- gs_start_app() function
- gs_applist() function
- gs_unpair() function
- gs_pair() function
- gs_quit_app() function
- load_serverinfo() function
- load_server_status() function
Evolution Note:
- Before (Research): Repository version lacked Sunshine compatibility fixes
- Current (Code): Includes antigravity research fixes for Sunshine detection and version handling

### gs_error: (Global Variable)
Type: Global Variable
Definition: Pointer to a string containing the last error message from the GameStream client library.
Antigravity Context: Used throughout client.c for error reporting, particularly important for debugging connection and pairing issues identified in antigravity research.
Dependencies:
- Set in various functions when errors occur (load_cert, load_serverinfo, gs_pair, etc.)
- Read by UI layer to display error messages to user
Evolution Note:
- Before (Research): Basic error reporting
- Current (Code): Enhanced with specific Sunshine error messages and countdown displays

### MIN_SUPPORTED_GFE_VERSION / MAX_SUPPORTED_GFE_VERSION: (Constants)
Type: Code Logic
Definition: Constants defining the minimum and maximum GeForce Experience versions supported by the client (3 and 7 respectively).
Antigravity Context: From comparison_report.md - "libgamestream/client.c" shows mismatches where Resources version has Sunshine compatibility fixes missing in Repository. These constants are used in load_server_status() to validate server version.
Dependencies:
- load_server_status() function
- gs_init() function
Evolution Note:
- Before (Research): Basic version checking
- Current (Code): Enhanced with Sunshine detection logic that forces version compatibility

### gs_init(): (Function)
Type: Code Logic
Definition: Initializes the GameStream client with server address, port, key directory, and logging level. Sets up unique ID, device name, certificates, and server status.
Antigravity Context: From task.md - "Fix Pairing Persistence & Device Identity" work, ensuring proper initialization sequence for reliable connections.
Dependencies:
- gs_cleanup() function
- mkdirtree() function
- load_unique_id() function
- load_device_name() function
- load_cert() function
- http_init() function
- LiInitializeServerInformation()
- load_server_status() function
Evolution Note:
- Before (Research): Basic initialization without persistence guarantees
- Current (Code): Includes antigravity research improvements for device identity and pairing persistence

### gs_cleanup(): (Function)
Type: Code Logic
Definition: Cleans up resources allocated by gs_init(), preserving certificates and private keys for Sunshine compatibility.
Antigravity Context: Critical for maintaining pairing persistence across reinitializations, as noted in antigravity research about Sunshine recognizing the same TLS identity.
Dependencies:
- All gs_* functions that allocate resources
- http_cleanup() function
- xml_free_* functions for serverInfo fields
Evolution Note:
- Before (Research): Potentially freed certificates causing pairing issues
- Current (Code): Preserves cert and privateKey across gs_init retries for Sunshine compatibility

### gs_pair(): (Function)
Type: Code Logic
Definition: Implements the detailed pairing protocol with the GameStream server, including certificate exchange, challenge-response sequence, and manual approval handling for Sunshine.
Antigravity Context: From task.md - "Fix Pairing Persistence & Device Identity" work and comparison_report.md showing Sunshine compatibility fixes. This function contains the complex 5-step pairing process with special handling for Sunshine manual approval.
Dependencies:
- All helper functions (RAND_bytes, bytes_to_hex, hex_to_bytes, etc.)
- mbedtls cryptographic functions
- http_request() function
- xml_search() and xml_status() functions
- sign_it() and verifySignature() functions
- IS_SUNSHINE() macro
Evolution Note:
- Before (Research): Basic pairing without Sunshine manual approval handling
- Current (Code): Includes antigravity research improvements for Sunshine compatibility and manual approval workflows

### gs_start_app(): (Function)
Type: Code Logic
Definition: Launches or resumes an application on the GameStream server, handling video format negotiation and session establishment.
Antigravity Context: From comparison_report.md - "libgamestream/client.c" shows mismatches where Resources version has Sunshine compatibility fixes missing in Repository. This function handles the launch/resume process and session URL extraction.
Dependencies:
- Helper functions for AES key generation
- http_request() function
- xml_search() function for parsing response
- Server mode validation logic
Evolution Note:
- Before (Research): Standard application launching
- Current (Code): Enhanced with Sunshine compatibility and arbitrary resolution support for Virtual Display feature

### gs_applist(): (Function)
Type: Code Logic
Definition: Retrieves the list of available applications from the GameStream server.
Antigravity Context: Used in main.c's STATE_APP_SELECT to populate the application list for user selection.
Dependencies:
- http_request() function
- xml_status() and xml_applist() functions
Evolution Note:
- Before (Research): Basic application listing
- Current (Code): Includes Sunshine compatibility handling in XML status checks

### gs_quit_app(): (Function)
Type: Code Logic
Definition: Instructs the GameStream server to quit the currently running application.
Antigravity Context: Used in main.c's STATE_QUIT_CONFIRM when user confirms quitting current app to start a new one.
Dependencies:
- http_request() function
- xml_status() and xml_search() functions
Evolution Note:
- Before (Research): Basic quit application functionality
- Current (Code): Maintains compatibility with Sunshine server variations

### Helper Functions: (Functions)
Type: Code Logic
Definition: Various cryptographic and utility functions used throughout the pairing and communication process (RAND_bytes, mbedtls_entropy_rand, bytes_to_hex, hex_to_bytes, mkdirtree, load_unique_id, load_device_name, load_cert, load_serverinfo, load_server_status, sign_it, verifySignature, encrypt, decrypt).
Antigravity Context: These functions implement the security and reliability improvements from antigravity research, particularly in the pairing process and certificate handling.
Dependencies:
- Interdependent helper functions
- mbedtls library functions
- PSP Io functions (sceIoOpen, sceIoRead, sceIoWrite, sceIoMkdir, sceIoSync)
Evolution Note:
- Before (Research): Basic implementations without PSP-specific optimizations
- Current (Code): Includes antigravity research improvements like internal entropy source for PSP compatibility and robust directory creation

## libgamestream/http.c Entries

### http_raw_request(): (Function)
Type: Code Logic
Definition: Sends an HTTP request over a TCP socket with PSP-specific optimizations and retry logic for transient errors.
Antigravity Context: From comparison_report.md - "libgamestream/http.c" shows mismatches where Resources version includes recursive TCP_NODELAY and SO_REUSEADDR settings, a 100ms hardware flush delay, and a 3-attempt retry loop for transient socket errors. This function implements the core HTTP communication.
Dependencies:
- tcp_connect() function
- tcp_close() function
- http_create_data()/http_free_data() functions
- PSP socket functions (sceNetSocket, sceNetConnect, sceNetSend, sceNetRecv)
- PSP socket options (TCP_NODELAY, SO_REUSEADDR, increased buffers)
Evolution Note:
- Before (Research): Susceptible to "Connection Reset" and "0-byte receive" errors due to lack of Nagle's algorithm disabling and retry logic
- Current (Code): Includes antigravity research fixes for WiFi stability and latency

### https_raw_request(): (Function)
Type: Code Logic
Definition: Sends an HTTPS request over a TCP socket using MbedTLS for encryption, with PSP-specific optimizations.
Antigravity Context: From comparison_report.md - "libgamestream/http.c" shows mismatches where Resources version includes recursive TCP_NODELAY and SO_REUSEADDR settings, a 100ms hardware flush delay, and a 3-attempt retry loop for transient socket errors. This function implements the core HTTPS communication.
Dependencies:
- tcp_connect() function
- tcp_close() function
- http_create_data()/http_free_data() functions
- MbedTLS functions (mbedtls_ssl_init, mbedtls_ssl_setup, mbedtls_ssl_handshake, etc.)
- PSP socket functions and options
Evolution Note:
- Before (Research): Susceptible to HTTPS connection issues due to lack of PSP-specific optimizations
- Current (Code): Includes antigravity research fixes for WiFi stability and latency in HTTPS requests

### http_request(): (Function)
Type: Code Logic
Definition: Main entry point for HTTP/HTTPS requests that determines whether to use HTTP or HTTPS based on the URL and calls the appropriate raw request function.
Antigravity Context: From comparison_report.md - "libgamestream/http.c" shows mismatches where Resources version includes recursive TCP_NODELAY and SO_REUSEADDR settings, a 100ms hardware flush delay, and a 3-attempt retry loop for transient socket errors.
Dependencies:
- http_raw_request() function
- https_raw_request() function
- URL parsing logic (http vs https)
Evolution Note:
- Before (Research): Basic HTTP/HTTPS routing without PSP-specific optimizations
- Current (Code): Includes antigravity research improvements for reliable communication

### tcp_connect(): (Function)
Type: Code Logic
Definition: Establishes a TCP connection to a server with PSP-specific socket optimizations.
Antigravity Context: From comparison_report.md - "libgamestream/http.c" shows mismatches where Resources version includes recursive TCP_NODELAY and SO_REUSEADDR settings for improved WiFi performance.
Dependencies:
- PSP socket functions (sceNetSocket, sceNetConnect)
- Socket options setting (TCP_NODELAY, SO_REUSEADDR, increased send/recv buffers)
Evolution Note:
- Before (Research): Basic TCP connection without PSP-specific optimizations
- Current (Code): Includes antigravity research fixes for TCP_NODELAY (disables Nagle's algorithm) and SO_REUSEADDR (allows address reuse) plus increased buffer sizes

### tcp_close(): (Function)
Type: Code Logic
Definition: Closes a TCP socket connection.
Antigravity Context: Part of the HTTP communication infrastructure improved per antigravity research.
Dependencies:
- PSP socket close function (sceNetSocketClose)
Evolution Note:
- Before (Research): Basic TCP socket closing
- Current (Code): Proper resource cleanup as part of stable communication

### http_create_data(): (Function)
Type: Code Logic
Definition: Creates an HTTP_DATA structure for holding request/response data.
Antigravity Context: Supporting function for HTTP communication improved per antigravity research.
Dependencies:
- malloc() for data allocation
Evolution Note:
- Before (Research): Basic data structure creation
- Current (Code): Part of reliable HTTP infrastructure

### http_free_data(): (Function)
Type: Code Logic
Definition: Frees an HTTP_DATA structure and its associated memory.
Antigravity Context: Supporting function for HTTP communication improved per antigravity research.
Dependencies:
- free() for memory deallocation
Evolution Note:
- Before (Research): Basic data structure freeing
- Current (Code): Proper memory management as part of stable communication

### url_parse(): (Function)
Type: Code Logic
Definition: Parses a URL into its components (host, port, path, etc.) for HTTP requests.
Antigravity Context: Supporting function for HTTP communication improved per antigravity research.
Dependencies:
- String manipulation functions
Evolution Note:
- Before (Research): Basic URL parsing
- Current (Code): Part of reliable HTTP infrastructure

## modules/video_decoder.c Entries

### VideoDecoder: (Structure)
Type: Code Logic
Definition: Structure representing the video decoder state, including pointers to Media Engine-owned buffers and configuration information.
Antigravity Context: From comparison_report.md - "modules/video_decoder.c" shows Resources version correctly handles Media Engine's pointer ownership where sceMpegAvcDecode returns an ME-owned pointer. Resources tracks this separately (me_output_frame), while Repository tries to manage its own decode_target.
Dependencies:
- video_decoder_create() function
- video_decoder_init() function
- video_decoder_update() function
- submit_frame() function (internal)
- return_frame() function (internal)
Evolution Note:
- Before (Research): Repository version tried to manage its own decode_target causing memory corruption or blank screen issues
- Current (Code): Uses me_output_frame pattern to correctly handle ME-owned pointers from sceMpegAvcDecode

### me_output_frame: (Pointer)
Type: Code Logic
Definition: Pointer to the output frame buffer owned by the Media Engine (ME) after decoding with sceMpegAvcDecode.
Antigravity Context: From comparison_report.md - "modules/video_decoder.c" shows Resources version correctly handles Media Engine's pointer ownership where sceMpegAvcDecode returns an ME-owned pointer. Resources tracks this separately (me_output_frame), while Repository tries to manage its own decode_target.
Dependencies:
- VideoDecoder structure
- sceMpegAvcDecode() function
- return_frame() function
Evolution Note:
- Before (Research): Repository version attempted to manage its own decode_target buffer
- Current (Code): Properly tracks ME-owned output frame pointer

### submit_frame(): (Function)
Type: Code Logic
Definition: Submits an MPEG-PS packet to the Media Engine for H.264 decoding, handling ringbuffer wrapping and timestamp management.
Antigravity Context: From implementation_plan.md - "Add micro-second tracing for AvcDecode calls" and "Implement a watchdog to detect if the decoder has been blocked for >500ms (Build 20: Mutex Watchdog)".
Dependencies:
- VideoDecoder structure
- ringbuffer callback mechanism
- sceMpegAvcDecode() function
- me_output_frame pointer
- Presentation Timestamp (PTS) handling
Evolution Note:
- Before (Research): Basic frame submission without tracing or watchdog protection
- Current (Code): Includes antigravity research improvements for micro-second tracing and 500ms video pipeline watchdog

### return_frame(): (Function)
Type: Code Logic
Definition: Returns the decoded frame buffer from the Media Engine for display, handling ME-owned pointer management.
Antigravity Context: From comparison_report.md - "modules/video_decoder.c" shows Resources version correctly handles Media Engine's pointer ownership where sceMpegAvcDecode returns an ME-owned pointer.
Dependencies:
- VideoDecoder structure
- me_output_frame pointer
- Video display/output mechanisms
Evolution Note:
- Before (Research): Repository version tried to manage its own decode_target causing memory issues
- Current (Code): Properly handles ME-owned pointer return for display

### init_media_engine(): (Function)
Type: Code Logic
Definition: Initializes the PSP Media Engine for H.264 decoding, setting up necessary buffers and callbacks.
Antigravity Context: From task.md - items related to "Achieving Absolute Perfection" and "Fix 10-Second Streaming Freeze" work, particularly Build 30: "2048-byte cl_log buffer + Synchronized ME Resolution".
Dependencies:
- sceMpegInit() function
- sceMpegAvcInit() function
- Ringbuffer setup for MPEG-PS packet wrapping
- VideoDecoder structure initialization
Evolution Note:
- Before (Research): Basic Media Engine initialization
- Current (Code): Includes antigravity research improvements for synchronized ME resolution and enhanced logging

### terminate_media_engine(): (Function)
Type: Code Logic
Definition: Shuts down the PSP Media Engine and releases associated resources.
Antigravity Context: Part of the video decoder lifecycle management improved per antigravity research.
Dependencies:
- sceMpegAvcFinish() function
- sceMpegFinish() function
- Resource cleanup
Evolution Note:
- Before (Research): Basic Media Engine termination
- Current (Code): Proper resource cleanup as part of stable video decoding

### Ringbuffer Callback: (Function)
Type: Code Logic
Definition: Callback function that wraps MPEG-PS packets with proper headers for input to the Media Engine decoder.
Antigravity Context: Critical for MPEG-PS packet preparation before sending to sceMpegAvcDecode(). From comparison_report.md - Resources version correctly handles this wrapping for ME input.
Dependencies:
- MPEG-PS packet structure
- Media Engine input requirements
- VideoDecoder ringbuffer management
Evolution Note:
- Before (Research): Basic packet wrapping without proper ME formatting
- Current (Code): Correctly formats MPEG-PS packets with headers for ME consumption

### video_decoder_update(): (Function)
Type: Code Logic
Definition: Updates the video decoder state, called periodically during streaming to check for new frames and handle decoder status.
Antigravity Context: From implementation_plan.md - "Add micro-second tracing for AvcDecode calls" and "Implement a watchdog to detect if the decoder has been blocked for >500ms (Build 20: Mutex Watchdog)".
Dependencies:
- VideoDecoder structure
- submit_frame() function
- return_frame() function
- Frame timing and synchronization logic
Evolution Note:
- Before (Research): Basic decoder update without tracing or watchdog
- Current (Code): Includes antigravity research improvements for micro-second tracing and 500ms video pipeline watchdog

## modules/audio_decoder.c Entries

### AudioDecoder: (Structure)
Type: Code Logic
Definition: Structure representing the audio decoder state, including buffers, mixing state, and Opus decoding configuration.
Antigravity Context: From comparison_report.md - "modules/audio_decoder.c" shows Resources version explicitly bypasses Opus decoding (failing to silence) to prevent the kernel panics caused by libopus on real hardware. It also uses a more efficient mix_buffer management.
Dependencies:
- audio_decoder_create() function
- audio_decoder_init() function
- audio_decoder_update() function
- Internal buffer management functions
Evolution Note:
- Before (Research): Repository version attempted full Opus decoding causing immediate crashes on real hardware
- Current (Code): Implements decoding bypass and improved buffer management from antigravity research

### mix_buffer: (Pointer)
Type: Code Logic
Definition: Buffer used for audio mixing operations before output to the PSP audio subsystem.
Antigravity Context: From comparison_report.md - "modules/audio_decoder.c" shows Resources version uses a more efficient mix_buffer management to prevent audio issues.
Dependencies:
- AudioDecoder structure
- Audio output functions
- PSP audio subsystem interfaces
Evolution Note:
- Before (Research): Basic mix buffer management
- Current (Code): Improved buffer management from antigravity research for efficiency and stability

### audio_decoder_update(): (Function)
Type: Code Logic
Definition: Updates the audio decoder state, called periodically during streaming to decode audio packets and manage audio output.
Antigravity Context: From implementation_plan.md - "Implement aggressive packet dropping for Audio packet queue overflow to prevent main-loop starvation" and "Ensure audio sync doesn't wait indefinitely for missing packets".
Dependencies:
- AudioDecoder structure
- Opus decoding functions (when not bypassed)
- Packet queue management
- Audio output timing and synchronization
Evolution Note:
- Before (Research): Basic audio update that could cause main-loop starvation during packet queue overflow
- Current (Code): Includes antigravity research improvements for aggressive packet dropping and non-blocking audio sync

### Opus Decoding Bypass: (Logic)
Type: Code Logic
Definition: Logic that explicitly bypasses Opus decoding to prevent kernel panics caused by libopus on real PSP hardware, instead sending silence or neutral audio data.
Antigravity Context: From comparison_report.md - "modules/audio_decoder.c" shows Resources version explicitly bypasses Opus decoding (failing to silence) to prevent the kernel panics caused by libopus on real hardware.
Dependencies:
- AudioDecoder structure
- Audio output functions
- Packet queue management
Evolution Note:
- Before (Research): Repository version attempted full Opus decoding causing immediate crashes
- Current (Code): Implements decoding bypass to prevent kernel panics while maintaining audio output

### Packet Queue Management: (Logic)
Type: Code Logic
Definition: Logic for managing incoming audio packet queues, including overflow handling and synchronization.
Antigravity Context: From implementation_plan.md - "Implement aggressive packet dropping for Audio packet queue overflow to prevent main-loop starvation".
Dependencies:
- AudioDecoder structure
- audio_decoder_update() function
- Network receiver audio packet delivery
Evolution Note:
- Before (Research): Basic packet queue that could overflow and cause main-loop starvation
- Current (Code): Includes antigravity research improvements for aggressive packet dropping to prevent main-loop starvation

### Audio Sync Logic: (Logic)
Type: Code Logic
Definition: Logic for synchronizing audio output with video presentation, managing timing and drift correction.
Antigravity Context: From implementation_plan.md - "Ensure audio sync doesn't wait indefinitely for missing packets".
Dependencies:
- AudioDecoder structure
- audio_decoder_update() function
- Video decoder timing references
Evolution Note:
- Before (Research): Audio sync that could wait indefinitely for missing packets
- Current (Code): Includes antigravity research improvements to prevent indefinite waiting on missing packets

## modules/network_receiver.c Entries

### NetworkReceiver: (Structure)
Type: Code Logic
Definition: Structure representing the network receiver state, handling RTSP communication, application management, and media streaming.
Antigravity Context: From comparison_report.md - "modules/network_receiver.c" shows mismatches where Resources version has RTSP Encryption bypass missing in Repository. This structure manages the core networking functionality for PSP Moonlight.
Dependencies:
- network_receiver_create() function
- network_receiver_init() function
- network_receiver_connect() function
- network_receiver_is_connected() function
- network_receiver_get_app_list() function
- network_receiver_start_app() function
- network_receiver_quit_app() function
- network_receiver_disconnect() function
- network_receiver_get_status() function
- network_receiver_send_key() function
- network_receiver_send_input() function
- network_receiver_set_video_decoder() function
- network_receiver_set_audio_decoder() function
Evolution Note:
- Before (Research): Repository version lacked RTSP encryption bypass causing connection issues
- Current (Code): Includes antigravity research fixes for RTSP handling and thread safety

### RTSP Connection Handling: (Logic)
Type: Code Logic
Definition: Logic for establishing and maintaining Real-Time Streaming Protocol (RTSP) connections for video and audio streaming from GameStream servers.
Antigravity Context: From comparison_report.md - "modules/network_receiver.c" shows mismatches where Resources version has RTSP Encryption bypass missing in Repository. From task.md - "Fix RTSP Handshake & Thread Creation" work including detailed RTSP logging and thread-safe logging.
Dependencies:
- NetworkReceiver structure
- RTSP request/response parsing
- Socket communication functions
- Encryption handling (with bypass per antigravity research)
Evolution Note:
- Before (Research): Repository version lacked RTSP encryption bypass causing connection instability
- Current (Code): Includes antigravity research fixes for RTSP encryption bypass and improved handshake handling

### Application Management: (Logic)
Type: Code Logic
Definition: Logic for retrieving application lists from GameStream servers and managing application launching/quitting.
Antigravity Context: From task.md - "Fix RTSP Handshake & Thread Creation" work and general networking improvements in antigravity research.
Dependencies:
- NetworkReceiver structure
- RTSP communication for applist and launch/resume requests
- XML parsing for application data
Evolution Note:
- Before (Research): Basic application management without proper error handling
- Current (Code): Includes antigravity research improvements for reliable application listing and control

### Key Delivery Mechanism: (Logic)
Type: Code Logic
Definition: Logic for delivering encryption keys to the video decoder for secure stream decoding.
Antigravity Context: Part of the secure streaming infrastructure improved per antigravity research.
Dependencies:
- NetworkReceiver structure
- VideoDecoder interface
- RTSP key exchange mechanisms
Evolution Note:
- Before (Research): Basic key delivery without optimization
- Current (Code): Includes antigravity research improvements for reliable key delivery

### network_receiver_create(): (Function)
Type: Code Logic
Definition: Creates and initializes a NetworkReceiver instance.
Antigravity Context: From task.md - "Fix Pairing Persistence & Device Identity" work, ensuring proper initialization sequence for reliable connections.
Dependencies:
- malloc() for structure allocation
- Initialization of internal state and buffers
Evolution Note:
- Before (Research): Basic instance creation
- Current (Code): Proper initialization as part of antigravity research stability improvements

### network_receiver_init(): (Function)
Type: Code Logic
Definition: Initializes a NetworkReceiver instance, setting up internal state and preparing for connections.
Antigravity Context: From task.md - "Fix Pairing Persistence & Device Identity" work and "Fix RTSP Handshake & Thread Creation" work.
Dependencies:
- NetworkReceiver structure
- Internal buffer and state initialization
- Thread creation for RTSP handling
Evolution Note:
- Before (Research): Basic initialization without thread safety considerations
- Current (Code): Includes antigravity research improvements for thread-safe initialization

### network_receiver_connect(): (Function)
Type: Code Logic
Definition: Establishes a connection to a GameStream server for streaming initialization.
Antigravity Context: From comparison_report.md - "modules/network_receiver.c" shows mismatches where Resources version has RTSP Encryption bypass missing in Repository. This function handles the initial connection setup.
Dependencies:
- NetworkReceiver structure
- RTSP SETUP and PLAY requests
- Server information from gs_start_app()
Evolution Note:
- Before (Research): Basic connection without RTSP encryption bypass
- Current (Code): Includes antigravity research fixes for RTSP encryption bypass and improved connection stability

### network_receiver_is_connected(): (Function)
Type: Code Logic
Definition: Checks the connection status of the NetworkReceiver, returning specific states for disconnected, connecting, waiting, streaming, or application switching.
Antigravity Context: Used throughout main.c's STATE_CONNECTING, STATE_WAITING, and STATE_STREAMING to determine connection progress.
Dependencies:
- NetworkReceiver structure
- Internal connection state tracking
Evolution Note:
- Before (Research): Basic connection status checking
- Current (Code): Enhanced with detailed state reporting for better UI feedback

### network_receiver_get_app_list(): (Function)
Type: Code Logic
Definition: Retrieves the list of available applications from the connected GameStream server.
Antigravity Context: Used in main.c's STATE_APP_SELECT to populate the application list for user selection.
Dependencies:
- NetworkReceiver structure
- RTSP DESCRIBE or equivalent request for application list
- XML parsing for application data
Evolution Note:
- Before (Research): Basic application listing without error handling
- Current (Code): Includes antigravity research improvements for reliable application retrieval

### network_receiver_start_app(): (Function)
Type: Code Logic
Definition: Instructs the GameStream server to launch a specific application for streaming.
Antigravity Context: Used in main.c's STATE_APP_SELECT when user selects an application to start.
Dependencies:
- NetworkReceiver structure
- RTSP PLAY request with application ID
- Server state management
Evolution Note:
- Before (Research): Basic application start without proper state handling
- Current (Code): Includes antigravity research improvements for reliable application launching

### network_receiver_quit_app(): (Function)
Type: Code Logic
Definition: Instructs the GameStream server to quit the currently running application.
Antigravity Context: Used in main.c's STATE_QUIT_CONFIRM when user confirms quitting current app to start a new one.
Dependencies:
- NetworkReceiver structure
- RTSP TEARDOWN or equivalent request
- Server state management
Evolution Note:
- Before (Research): Basic application quit without proper cleanup
- Current (Code): Includes antigravity research improvements for clean application termination

### network_receiver_disconnect(): (Function)
Type: Code Logic
Definition: Terminates the connection to the GameStream server and cleans up resources.
Antigravity Context: Used throughout main.c when returning to menu or handling connection errors.
Dependencies:
- NetworkReceiver structure
- RTSP TEARDOWN request
- Resource cleanup (buffers, threads, etc.)
Evolution Note:
- Before (Research): Basic disconnection without proper resource cleanup
- Current (Code): Includes antigravity research improvements for clean resource release

### network_receiver_get_status(): (Function)
Type: Code Logic
Definition: Retrieves a human-readable status message from the NetworkReceiver for UI display.
Antigravity Context: Used in main.c's draw_connecting_logic() to show connection progress and errors.
Dependencies:
- NetworkReceiver structure
- Internal status message tracking
Evolution Note:
- Before (Research): Basic status reporting
- Current (Code): Enhanced with detailed messages for better user feedback

### network_receiver_send_key(): (Function)
Type: Code Logic
Definition: Sends keyboard input events to the GameStream server for remote PC control.
Antigravity Context: Used in main.c's STATE_STREAMING for special shortcuts like Win Key and Alt-Tab.
Dependencies:
- NetworkReceiver structure
- RTSP SET_PARAMETER request for key events
- Input state from input_mapper
Evolution Note:
- Before (Research): Basic key sending without optimization
- Current (Code): Includes antigravity research improvements for reliable input transmission

### network_receiver_send_input(): (Function)
Type: Code Logic
Definition: Sends controller input events to the GameStream server for remote PC control.
Antigravity Context: Used in main.c's STATE_STREAMING for regular game controller input.
Dependencies:
- NetworkReceiver structure
- RTSP SET_PARAMETER request for input events
- Input state from input_mapper
Evolution Note:
- Before (Research): Basic input sending without optimization
- Current (Code): Includes antigravity research improvements for reliable input transmission

### network_receiver_set_video_decoder(): (Function)
Type: Code Logic
Definition: Sets the video decoder instance on the NetworkReceiver for key delivery and frame timing coordination.
Antigravity Context: Part of the module interconnection setup in main.c's initialize_modules().
Dependencies:
- NetworkReceiver structure
- VideoDecoder interface
Evolution Note:
- Before (Research): Basic decoder setting without coordination
- Current (Code): Proper interconnection for key delivery and timing

### network_receiver_set_audio_decoder(): (Function)
Type: Code Logic
Definition: Sets the audio decoder instance on the NetworkReceiver for key delivery and frame timing coordination.
Antigravity Context: Part of the module interconnection setup in main.c's initialize_modules().
Dependencies:
- NetworkReceiver structure
- AudioDecoder interface
Evolution Note:
- Before (Research): Basic decoder setting without coordination
- Current (Code): Proper interconnection for key delivery and timing

## modules/exception_handler.c Entries

### exception_handler_init(): (Function)
Type: Code Logic
Definition: Initializes the Blue Screen of Death (BSOD) exception handler for crash forensics on PSP.
Antigravity Context: From implementation_plan.md - "Enable Hardware Exception Handler for crash forensics" and task.md items related to stability improvements.
Dependencies:
- PSP exception handling functions
- Logging infrastructure for crash reporting
- File system access for writing exception.log
Evolution Note:
- Before (Research): Basic or no exception handling
- Current (Code): Includes antigravity research improvements for crash forensics with detailed logging to ms0:/exception.log

### Exception Handling Logic: (Logic)
Type: Code Logic
Definition: Logic for capturing and displaying detailed crash information when exceptions occur on PSP.
Antigravity Context: From implementation_plan.md - "Enable Hardware Exception Handler for crash forensics".
Dependencies:
- PSP exception registration functions
- Display functions for BSOD rendering
- Logging functions for crash details
Evolution Note:
- Before (Research): Minimal or no crash information available
- Current (Code): Detailed BSOD with register dumps and stack traces for forensic analysis

## kernel_exception.c Entries

### exception_handler(): (Function)
Type: Code Logic
Definition: Kernel-mode exception handler that traps CPU exceptions and displays a custom BSOD with register state.
Antigravity Context: From implementation_plan.md - "Enable Hardware Exception Handler for crash forensics" and task.md items related to stability improvements.
Dependencies:
- pspDebugScreenInit()/pspDebugScreenSet*() functions
- PspDebugStackTrace structure
- sceKernelDelayThread() for idle loop
Evolution Note:
- Before (Research): Basic or no exception handling for kernel-mode faults
- Current (Code): Detailed BSOD output including exception cause, EPC, VADDR, and full register dump

### module_start(): (Function)
Type: Code Logic
Definition: Entry point for the kernel exception handler module that installs the custom exception handler.
Antigravity Context: Part of the exception handling infrastructure improvements per antigravity research.
Dependencies:
- pspDebugInstallErrorHandler() function
- exception_handler() function
Evolution Note:
- Before (Research): No custom kernel exception handler installed
- Current (Code): Custom handler installed for detailed crash forensics

### module_stop(): (Function)
Type: Code Logic
Definition: Exit point for the kernel exception handler module.
Antigravity Context: Part of the module lifecycle management.
Dependencies:
- None (simple return)
Evolution Note:
- Before (Research): No specific cleanup needed
- Current (Code): Proper module exit point

## discover.c Entries

### gs_discover_server(): (Function)
Type: Code Logic
Definition: Discovers GameStream servers on the local network using mDNS/DNS-SD (Avahi) protocol.
Antigravity Context: While not explicitly detailed in the antigravity research files examined, service discovery is implied in the pairing and connection workflow.
Dependencies:
- Avahi library functions (avahi_simple_poll_new, avahi_client_new, etc.)
- gs_error global for error reporting
- Simple poll loop for asynchronous operation
Evolution Note:
- Before (Research): Basic or manual server discovery
- Current (Code): Automatic mDNS/DNS-SD based service discovery

### Client/Resolve/Browse Callbacks: (Functions)
Type: Code Logic
Definition: Callback functions for handling Avahi client events, service resolution, and service browsing.
Antigravity Context: Part of the automatic service discovery infrastructure.
Dependencies:
- Avahi service browser and resolver objects
- gs_error global for error reporting
- avahi_simple_poll_quit() for terminating discovery
Evolution Note:
- Before (Research): Manual server IP entry required
- Current (Code): Automatic discovery of _nvstream._tcp services

## mkcert.c Entries

### better_entropy_mkcert(): (Function)
Type: Code Logic
Definition: Entropy function that combines system time and random values for improved randomness in certificate generation.
Antigravity Context: From implementation_plan.md - context around certificate handling and security improvements.
Dependencies:
- sceKernelGetSystemTimeWide() for high-resolution time
- rand() for random values
- Bitwise operations for entropy mixing
Evolution Note:
- Before (Research): Basic entropy sources for certificate generation
- Current (Code): Improved entropy combining time and random values

### mkcert_generate(): (Function)
Type: Code Logic
Definition: Generates a new RSA certificate key pair for PSP Moonlight identity.
Antigravity Context: From task.md - "Fix Pairing Persistence & Device Identity" work and certificate handling improvements.
Dependencies:
- mbedtls library functions for RSA key generation and X.509 certificate creation
- moonlight_debug.log for forensic tracing
- Device name for certificate subject/issuer
Evolution Note:
- Before (Research): Basic certificate generation without forensic enhancements
- Current (Code): Includes 10-year validity, 16-byte random serial, and standard extensions for GFE/Apollo compatibility

### mkcert_free(): (Function)
Type: Code Logic
Definition: Frees resources allocated by mkcert_generate().
Antigravity Context: Part of the certificate lifecycle management.
Dependencies:
- mbedtls library free functions
- free() for allocated structures
Evolution Note:
- Before (Research): Basic resource cleanup
- Current (Code): Proper cleanup of all mbedtls and allocated resources

### mkcert_save(): (Function)
Type: Code Logic
Definition: Saves the generated certificate, private key, and creates a DER version for Sunshine/GFE compatibility.
Antigravity Context: From implementation_plan.md - "Absolute Perfection for Sunshine/GFE" comment on DER certificate saving.
Dependencies:
- mbedtls write functions for PEM and DER formats
- sceIo functions for file operations
- sceIoSync() for filesystem consistency
Evolution Note:
- Before (Research): Basic PEM-only certificate saving
- Current (Code): Saves PEM certificate, private key, and DER version for maximum compatibility

## sps.c Entries

### NAL Unit Handling Functions: (Functions)
Type: Code Logic
Definition: Functions for parsing and extracting SPS/PPS NAL units from H.264 byte stream for Media Engine configuration.
Antigravity Context: Implied by video decoder functionality and H.264 stream processing requirements.
Dependencies:
- Bit stream parsing functions
- SPS/PPS structure handling
- VideoDecoder interface for configuration
Evolution Note:
- Before (Research): Basic NAL unit handling
- Current (Code): Proper SPS/PPS extraction for ME initialization

### SPS/PPS Tracking: (Logic)
Type: Code Logic
Definition: Logic for tracking and managing SPS/PPS NAL units to ensure proper video decoder configuration.
Antigravity Context: Part of the H.264 stream processing infrastructure for hardware decoding.
Dependencies:
- NAL unit parsing functions
- SPS/PPS storage and validation
- Video decoder reconfiguration on SPS/PPS changes
Evolution Note:
- Before (Research): Basic SPS/PPS handling without change detection
- Current (Code): Includes tracking and handling of SPS/PPS changes during streaming

## xml.c Entries

### XML Parsing Functions: (Functions)
Type: Code Logic
Definition: Functions for parsing XML responses from GameStream servers using mbedtls or custom parsing.
Antigravity Context: From comparison_report.md - "Refactor xml module signatures to use const char* for type safety" and general XML processing in client-server communication.
Dependencies:
- XML string input and parsing
- Value extraction for key tags (paired, currentgame, etc.)
- Memory management for returned strings
Evolution Note:
- Before (Research): Basic XML parsing without type safety considerations
- Current (Code): Refactored signatures for improved type safety and error handling

### Specific Tag Search Functions: (Functions)
Type: Code Logic
Definition: Functions for searching specific XML tags like paired, currentgame, PairStatus, etc.
Antigravity Context: From comparison_report.md showing specific xml_search() calls for various tags in server responses.
Dependencies:
- XML parsing infrastructure
- String comparison and extraction
- Memory allocation for returned values
Evolution Note:
- Before (Research): Basic tag searching without robust error handling
- Current (Code): Improved search functions with proper error returns

## Additional Main.c Elements

### g_frame_counter: (Global Variable) - ASCII Animation Use
Type: Global Variable
Definition: Counter used for ASCII animations in the UI background (starfield twinkle/movement).
Antigravity Context: While not explicitly called out, UI enhancements are part of overall stability and polish improvements.
Dependencies:
- ui_draw_background() function for starfield rendering
- Main loop increment
Evolution Note:
- Before (Research): Static background or basic animation
- Current (Code): Dynamic starfield with twinkle/movement effects

### UI Layout and Color Scheme: (Logic)
Type: Code Logic
Definition: Specific UI layout coordinates, dimensions, and color scheme for the premium space-themed interface.
Antigravity Context: From implementation_plan.md comments about "premium UI overhead" and "space-aware" designs.
Dependencies:
- ui_draw_* functions for rendering
- SCR_WIDTH/SCR_HEIGHT constants
- Color values in ABGR format
Evolution Note:
- Before (Research): Basic functional UI without premium styling
- Current (Code): Space-themed gradient background, starfield, and coordinated color scheme

### Shortcut Handling Enhancements: (Logic)
Type: Code Logic
Definition: Additional shortcut handling beyond Win Key and Alt-Tab, including control mode toggling.
Antigravity Context: From main.c implementation showing control mode toggling via SELECT+RTRIGGER.
Dependencies:
- input_mapper_get_state() for control mode selection
- UI state for displaying current mode
- network_receiver_send_key() for sending key events
Evolution Note:
- Before (Research): No shortcut handling
- Current (Code): Includes Win Key (SELECT+UP), Alt-Tab (SELECT+LTRIGGER), and Control Mode (SELECT+RTRIGGER) shortcuts

## modules/logger.c Entries

### logger_init(): (Function)
Type: Code Logic
Definition: Initializes the thread-safe logging system for PSP Moonlight.
Antigravity Context: From task.md - "Implement thread-safe logging in logger.c with Mutex (Build 6)" and implementation_plan.md - "Expand Log Buffers to 4096 bytes to prevent stack corruption".
Dependencies:
- Mutex creation for thread safety
- Log buffer allocation (expanded to 4096 bytes per antigravity research)
- Output redirection to console and/or file
Evolution Note:
- Before (Research): Basic logging without thread safety or sufficient buffer size
- Current (Code): Includes antigravity research improvements for thread-safe logging and expanded log buffers

### logger_shutdown(): (Function)
Type: Code Logic
Definition: Shuts down the logging system and releases associated resources.
Antigravity Context: Part of the logging lifecycle management improved per antigravity research.
Dependencies:
- Mutex destruction
- Log buffer deallocation
- Output stream closing
Evolution Note:
- Before (Research): Basic logging shutdown
- Current (Code): Proper resource cleanup as part of stable logging infrastructure

### LOG_* Macros: (Macros)
Type: Code Logic
Definition: Macros for logging at different levels (LOG_INFO, LOG_ERROR, etc.) with thread safety and buffer management.
Antigravity Context: From task.md - "Implement thread-safe logging in logger.c with Mutex (Build 6)" and implementation_plan.md - "Expand Log Buffers to 4096 bytes to prevent stack corruption".
Dependencies:
- logger_init() function
- Thread-safe logging implementation
- Expanded log buffers (4096 bytes)
Evolution Note:
- Before (Research): Basic logging macros without thread safety
- Current (Code): Includes antigravity research improvements for thread-safe logging and prevention of stack corruption

## modules/input_mapper.c Entries

### InputMapper: (Structure)
Type: Code Logic
Definition: Structure representing the input mapper state, translating PSP controller inputs to GameStream-compatible signals.
Antigravity Context: While not explicitly detailed in the antigravity research files examined, this module is critical for input handling and benefited from general stability improvements.
Dependencies:
- input_mapper_create() function
- input_mapper_init() function
- input_mapper_update() function
- input_mapper_get_state() function
Evolution Note:
- Before (Research): Basic input mapping implementation
- Current (Code): Enhanced with control mode switching (Xbox/Browser) and shortcut handling per main.c implementation

### input_mapper_create(): (Function)
Type: Code Logic
Definition: Creates and initializes an InputMapper instance.
Antigravity Context: Part of the input mapping infrastructure improvements.
Dependencies:
- malloc() for structure allocation
- Initialization of input state and mapping tables
Evolution Note:
- Before (Research): Basic instance creation
- Current (Code): Proper initialization as part of input mapping improvements

### input_mapper_init(): (Function)
Type: Code Logic
Definition: Initializes an InputMapper instance, preparing it for input translation.
Antigravity Context: Part of the input mapping infrastructure improvements.
Dependencies:
- InputMapper structure
- Input state initialization
- Mapping table setup for PSP to GameStream translation
Evolution Note:
- Before (Research): Basic initialization without comprehensive mapping
- Current (Code): Includes improvements for accurate input translation

### input_mapper_update(): (Function)
Type: Code Logic
Definition: Updates the input mapper state with current PSP controller readings.
Antigravity Context: Used in main.c's STATE_STREAMING to update input state for sending to GameStream server.
Dependencies:
- InputMapper structure
- PSP controller reading (sceCtrlReadBufferPositive)
- Input state processing and filtering
Evolution Note:
- Before (Research): Basic input state updating
- Current (Code): Enhanced with debouncing and filtering for reliable input

### input_mapper_get_state(): (Function)
Type: Code Logic
Definition: Retrieves the current processed input state from the input mapper in GameStream-compatible format.
Antigravity Context: Used in main.c's STATE_STREAMING to get input state for sending to GameStream server.
Dependencies:
- InputMapper structure
- Processed input state
- Control mode selection (Xbox/Browser)
Evolution Note:
- Before (Research): Basic input state retrieval
- Current (Code): Includes control mode switching and shortcut handling

### Control Mode Switching: (Logic)
Type: Code Logic
Definition: Logic for switching between Xbox and Browser control modes for different input mapping preferences.
Antigravity Context: From main.c implementation showing control mode toggling via SELECT+RTRIGGER and pause menu options.
Dependencies:
- InputMapper structure
- input_mapper_get_state() function
- UI state for displaying current mode
Evolution Note:
- Before (Research): Single fixed control mode
- Current (Code): Includes antigravity research improvements for flexible control mode switching

### Shortcut Handling: (Logic)
Type: Code Logic
Definition: Logic for handling special input shortcuts like Win Key (SELECT+UP) and Alt-Tab (SELECT+LTRIGGER).
Antigravity Context: From main.c implementation showing shortcut handling in STATE_STREAMING and pause menu.
Dependencies:
- InputMapper structure
- input_mapper_update() function
- network_receiver_send_key() function for sending key events to server
Evolution Note:
- Before (Research): No special shortcut handling
- Current (Code): Includes antigravity research improvements for Win Key and Alt-Tab shortcuts

## modules/render_pipeline.c Entries

### RenderPipeline: (Structure)
Type: Code Logic
Definition: Structure representing the render pipeline state, handling video frame presentation to the display.
Antigravity Context: From comparison_report.md - "modules/render_pipeline.c" shows Resources uses sceKernelDcacheWritebackInvalidateAll() during frame presentation.
Dependencies:
- render_pipeline_create() function
- render_pipeline_init() function
- render_pipeline_draw_video() function
- render_pipeline_set_video_decoder() function
Evolution Note:
- Before (Research): Repository version used InvalidateRange which often missed stale cache lines
- Current (Code): Uses global cache flush from Resources for proper ME buffer rotation handling

### render_pipeline_create(): (Function)
Type: Code Logic
Definition: Creates and initializes a RenderPipeline instance.
Antigravity Context: Part of the render pipeline infrastructure improvements.
Dependencies:
- malloc() for structure allocation
- Initialization of rendering state and buffers
Evolution Note:
- Before (Research): Basic instance creation
- Current (Code): Proper initialization as part of render pipeline improvements

### render_pipeline_init(): (Function)
Type: Code Logic
Definition: Initializes a RenderPipeline instance, preparing it for video frame presentation.
Antigravity Context: Part of the render pipeline infrastructure improvements.
Dependencies:
- RenderPipeline structure
- Graphics initialization (GU setup)
- Display list and buffer setup
Evolution Note:
- Before (Research): Basic initialization without complete graphics setup
- Current (Code): Includes antigravity research improvements for proper GU initialization

### render_pipeline_draw_video(): (Function)
Type: Code Logic
Definition: Draws a video frame to the display using the PSP Graphics Unit (GU).
Antigravity Context: From comparison_report.md - "modules/render_pipeline.c" shows Resources uses sceKernelDcacheWritebackInvalidateAll() during frame presentation for proper cache coherency.
Dependencies:
- RenderPipeline structure
- Video frame data from video_decoder
- GU drawing commands
- Cache coherency operations
Evolution Note:
- Before (Research): Repository version used InvalidateRange which often missed stale cache lines
- Current (Code): Uses global cache flush (sceKernelDcacheWritebackInvalidateAll()) from Resources for proper ME buffer rotation handling

### render_pipeline_set_video_decoder(): (Function)
Type: Code Logic
Definition: Sets the video decoder instance on the RenderPipeline for frame retrieval and timing.
Antigravity Context: Part of the module interconnection setup in main.c's initialize_modules().
Dependencies:
- RenderPipeline structure
- VideoDecoder interface
Evolution Note:
- Before (Research): Basic decoder setting without coordination
- Current (Code): Proper interconnection for frame retrieval and timing

## Cache Coherency Logic: (Logic)
Type: Code Logic
Definition: Logic for ensuring proper cache coherency between CPU and Media Engine when handling video frames.
Antigravity Context: From comparison_report.md - "modules/render_pipeline.c" shows Resources uses sceKernelDcacheWritebackInvalidateAll() during frame presentation.
Dependencies:
- RenderPipeline structure
- render_pipeline_draw_video() function
- PSP cache control functions (sceKernelDcacheWritebackInvalidateAll)
Evolution Note:
- Before (Research): Repository version used InvalidateRange which often missed stale cache lines when ME rotates output buffers
- Current (Code): Uses global cache flush from Resources for proper handling of ME buffer rotation

### g_network_receiver: (Global Variable)
Type: Global Variable
Definition: Pointer to the NetworkReceiver module instance responsible for handling network connections, RTSP communication, and app management.
Antigravity Context: From comparison_report.md - "modules/network_receiver.c" shows mismatches where Resources version has RTSP Encryption bypass missing in Repository. This variable is initialized in main.c's initialize_modules() function and used throughout the main loop for network operations.
Dependencies: 
- NetworkReceiver module initialization
- network_receiver_create() function
- network_receiver_init() function
- network_receiver_connect() function
- network_receiver_is_connected() function
- network_receiver_get_app_list() function
- network_receiver_start_app() function
- network_receiver_quit_app() function
- network_receiver_disconnect() function
- network_receiver_get_status() function
- network_receiver_send_key() function
- network_receiver_send_input() function
Evolution Note: 
- Before (Research): Network receiver lacked RTSP encryption bypass causing connection issues
- Current (Code): Includes antigravity research fixes for RTSP handling and thread safety

### g_video_decoder: (Global Variable)
Type: Global Variable
Definition: Pointer to the VideoDecoder module instance responsible for hardware H.264 decoding using PSP's Media Engine.
Antigravity Context: From comparison_report.md - "modules/video_decoder.c" shows Resources version correctly handles Media Engine's pointer ownership where sceMpegAvcDecode returns an ME-owned pointer. This variable is initialized in main.c's initialize_modules() function.
Dependencies:
- VideoDecoder module initialization
- video_decoder_create() function
- video_decoder_init() function
- video_decoder_update() function
- render_pipeline_set_video_decoder() function
- network_receiver_set_video_decoder() function
Evolution Note:
- Before (Research): Repository version tried to manage its own decode_target causing memory corruption
- Current (Code): Uses me_output_frame pattern to correctly handle ME-owned pointers from sceMpegAvcDecode

### g_audio_decoder: (Global Variable)
Type: Global Variable
Definition: Pointer to the AudioDecoder module instance responsible for Opus audio decoding.
Antigravity Context: From comparison_report.md - "modules/audio_decoder.c" shows Resources version explicitly bypasses Opus decoding to prevent kernel panics caused by libopus on real hardware.
Dependencies:
- AudioDecoder module initialization
- audio_decoder_create() function
- audio_decoder_init() function
- audio_decoder_update() function
- render_pipeline_set_audio_decoder() function (implied)
- network_receiver_set_audio_decoder() function
Evolution Note:
- Before (Research): Repository version attempted full Opus decoding causing immediate crashes
- Current (Code): Implements decoding bypass and improved buffer management from antigravity research

### g_input_mapper: (Global Variable)
Type: Global Variable
Definition: Pointer to the InputMapper module instance responsible for translating PSP controller inputs to GameStream-compatible signals.
Antigravity Context: While not explicitly mentioned in the antigravity research files examined, this module is critical for translating PSP controls (buttons, analog sticks) to PC game inputs sent via GameStream.
Dependencies:
- InputMapper module initialization
- input_mapper_create() function
- input_mapper_init() function
- input_mapper_update() function
- input_mapper_get_state() function
Evolution Note:
- Before (Research): Basic input mapping implementation
- Current (Code): Enhanced with control mode switching (Xbox/Browser) and shortcut handling

### g_render_pipeline: (Global Variable)
Type: Global Variable
Definition: Pointer to the RenderPipeline module instance responsible for presenting decoded video frames to the display.
Antigravity Context: From comparison_report.md - "modules/render_pipeline.c" shows Resources uses sceKernelDcacheWritebackInvalidateAll() during frame presentation for proper cache coherency.
Dependencies:
- RenderPipeline module initialization
- render_pipeline_create() function
- render_pipeline_init() function
- render_pipeline_draw_video() function
- render_pipeline_set_video_decoder() function
Evolution Note:
- Before (Research): Repository version used InvalidateRange which often missed stale cache lines
- Current (Code): Uses global cache flush from Resources for proper ME buffer rotation handling

### g_ui_sema: (Global Variable)
Type: Global Variable
Definition: Semaphore used to prevent overlapping UI access from multiple threads.
Antigravity Context: This synchronization primitive ensures thread-safe UI operations, particularly important given the antigravity research focus on stability and preventing race conditions.
Dependencies:
- ui_lock() function
- ui_unlock() function
- SceUID creation via sceKernelCreateSema()
Evolution Note:
- Before (Research): Potential UI thread safety issues
- Current (Code): Explicit semaphore protection for UI access

### g_running: (Global Variable)
Type: Global Variable
Definition: Flag controlling the main application loop execution.
Antigravity Context: Related to the antigravity research on application stability and clean shutdown procedures.
Dependencies:
- main_loop() function
- STATE_STREAMING case handling
- Exit callback handling
Evolution Note:
- Before (Research): Basic running flag implementation
- Current (Code): Integrated with network state monitoring for auto-recovery

### g_frame_counter: (Global Variable)
Type: Global Variable
Definition: Counter used for ASCII animations and periodic stability checks.
Antigravity Context: Used in the antigravity research for "Absolute Perfection: Periodic Stability Logs (every ~60 seconds at 60fps)".
Dependencies:
- main_loop() increment
- STATE_STREAMING stability check logic
Evolution Note:
- Before (Research): Basic frame counter for animations
- Current (Code): Enhanced with stability monitoring functionality

### AppState: (Enum)
Type: Code Logic
Definition: Enumeration defining the application states: STATE_MAIN_MENU, STATE_ENTER_IP, STATE_CONNECTING, STATE_WAITING, STATE_APP_SELECT, STATE_STREAMING, STATE_PAUSE_MENU, STATE_QUIT_CONFIRM.
Antigravity Context: State machine design enables the antigravity research features like auto-recovery logic and state-specific UI handling.
Dependencies:
- main_loop() switch statement
- STATE_MAIN_MENU: draw_main_menu_logic()
- STATE_ENTER_IP: draw_enter_ip_logic()
- STATE_CONNECTING/STATE_WAITING: draw_connecting_logic()
- STATE_APP_SELECT: draw_app_select_logic()
- STATE_STREAMING: streaming logic with pause handling
- STATE_QUIT_CONFIRM: draw_quit_confirm_logic()
- STATE_PAUSE_MENU: draw_pause_overlay_logic()
Evolution Note:
- Before (Research): Basic state management
- Current (Code): Enhanced with antigravity research improvements like auto-recovery and stability checks

### current_state: (Global Variable)
Type: Global Variable
Definition: Tracks the current application state from the AppState enumeration.
Antigravity Context: Central to implementing the antigravity research features that are state-dependent (e.g., stability checks only in STATE_STREAMING).
Dependencies:
- All state drawing logic functions
- State transition logic in main_loop()
Evolution Note:
- Before (Research): Basic state tracking
- Current (Code): Integrated with antigravity research features like network auto-recovery

### save_hosts(): (Function)
Type: Code Logic
Definition: Saves the configured host IP addresses to persistent storage (ms0:/moonlight/hosts.txt).
Antigravity Context: From task.md - "Fix Pairing Persistence & Device Identity" work, ensuring host configurations persist across reboots.
Dependencies:
- HOSTS_FILE definition
- sceIoMkdir() for directory creation
- sceIoOpen()/sceIoWrite()/sceIoClose() for file operations
- load_hosts() function (complementary)
Evolution Note:
- Before (Research): Host configuration lost on reboot
- Current (Code): Persistent host storage implemented per antigravity research

### load_hosts(): (Function)
Type: Code Logic
Definition: Loads previously saved host IP addresses from persistent storage.
Antigravity Context: From task.md - "Fix Pairing Persistence & Device Identity" work, ensuring host configurations persist across reboots.
Dependencies:
- HOSTS_FILE definition
- sceIoOpen()/sceIoRead()/sceIoClose() for file operations
- save_hosts() function (complementary)
Evolution Note:
- Before (Research): No persistent host storage
- Current (Code): Host loading from ms0:/moonlight/hosts.txt

### exit_callback(): (Function)
Type: Code Logic
Definition: Handles the PSP Home button exit request by calling sceKernelExitGame().
Antigravity Context: Related to antigravity research on clean application termination and resource management.
Dependencies:
- CallbackThread() function
- SetupCallbacks() function
- sceKernelExitGame() call
Evolution Note:
- Before (Research): Potential improper cleanup on exit
- Current (Code): Direct kernel exit as recommended for PSP homebrew to avoid module cleanup issues

### CallbackThread(): (Function)
Type: Code Logic
Definition: Creates and registers the exit callback for handling PSP Home button presses.
Antigravity Context: Part of the application lifecycle management enhanced per antigravity research.
Dependencies:
- exit_callback() function
- sceKernelCreateCallback()/sceKernelRegisterExitCallback()
- sceKernelSleepThreadCB()
Evolution Note:
- Before (Research): Basic callback implementation
- Current (Code): Proper PSP homebrew exit handling

### SetupCallbacks(): (Function)
Type: Code Logic
Definition: Initializes the callback thread for handling system events like PSP Home button presses.
Antigravity Context: Ensures proper application lifecycle management as part of antigravity research stability improvements.
Dependencies:
- CallbackThread() function
- sceKernelCreateThread()/sceKernelStartThread()
Evolution Note:
- Before (Research): Basic callback setup
- Current (Code): Robust thread creation for system event handling

### draw_wrapped_text(): (Function)
Type: Code Logic
Definition: Renders text with word wrapping within a specified width, used for status messages and UI elements.
Antigravity Context: From implementation_plan.md - "Absolute Perfection: Space-aware word wrap for premium UI aesthetics" in draw_connecting_logic().
Dependencies:
- ui_draw_text() function
- String manipulation logic for word wrapping
Evolution Note:
- Before (Research): Basic text rendering without wrapping
- Current (Code): Implements space-aware word wrap for premium UI aesthetics per antigravity research

### initialize_modules(): (Function)
Type: Code Logic
Definition: Initializes all PSP Moonlight modules in the correct order: UI Renderer first, then network receiver, video/audio decoders, input mapper, and render pipeline.
Antigravity Context: From implementation_plan.md comments - "Absolute Perfection: UI Renderer MUST be first for any screen output" and "Ensure folder exists for pairing persistence before any GS operations".
Dependencies:
- All module init functions (ui_renderer_init, network_receiver_create/init, etc.)
- Folder creation for ms0:/moonlight and ms0:/moonlight/keys
- Module interconnection setup (setting video/audio decoders on network receiver and render pipeline)
Evolution Note:
- Before (Research): Potential initialization order issues
- Current (Code): Strict initialization order per antigravity research for reliability

### is_button_pressed(): (Function)
Type: Code Logic
Definition: Detects button press events (transition from not pressed to pressed) for PSP controller input.
Antigravity Context: Used throughout main_loop() for state transitions and special feature activation.
Dependencies:
- PSP_CTRL_* constants
- pad_data and old_pad SceCtrlData structures
Evolution Note:
- Before (Research): Basic button state checking
- Current (Code): Proper edge detection for reliable input handling

### State Drawing Logic Functions: (Functions)
Type: Code Logic
Definition: Functions responsible for rendering the UI for each application state (draw_main_menu_logic, draw_enter_ip_logic, etc.).
Antigravity Context: Enhanced per antigravity research for UI stability and aesthetics, particularly the space-aware word wrap and status bar improvements.
Dependencies:
- UI renderer functions (ui_draw_background, ui_draw_header, etc.)
- State-specific logic and data
Evolution Note:
- Before (Research): Basic UI rendering
- Current (Code): Enhanced with antigravity research improvements like word wrapping and status-aware layouts

### main_loop(): (Function)
Type: Code Logic
Definition: The main application loop that handles input, state transitions, module updates, and rendering.
Antigravity Context: Central to implementing all antigravity research features including stability checks, shortcuts, and auto-recovery logic.
Dependencies:
- All state handling logic
- Module update functions (video_decoder_update, audio_decoder_update, input_mapper_update)
- Special feature handling (shortcuts, stability logs, message overlays)
Evolution Note:
- Before (Research): Basic main loop with state handling
- Current (Code): Enhanced with antigravity research features:
  * Periodic stability logs every ~60 seconds
  * Win Key and Alt-Tab shortcuts
  * Control mode toggling
  * Sunshine message overlay (toast)
  * Pause menu with extended options

### main(): (Function)
Type: Code Logic
Definition: Application entry point that initializes logging, exception handling, clock frequency, callbacks, modules, and starts the main loop.
Antigravity Context: Incorporates multiple antigravity research improvements for system stability and reliability.
Dependencies:
- logger_init()/logger_shutdown()
- exception_handler_init()
- scePowerSetClockFrequency()
- SetupCallbacks()
- initialize_modules()
- sceKernelCreateSema() for g_ui_sema
- load_hosts()
- main_loop()
- Clean exit handling
Evolution Note:
- Before (Research): Basic application startup
- Current (Code): Enhanced with antigravity research:
  * Thread-safe logging initialization
  * Blue Screen Exception Handler for crash forensics
  * Fixed clock frequency to prevent stream lag
  * Proper semaphore creation for UI thread safety
  * Persistent host loading
  * Clean exit without problematic module cleanup

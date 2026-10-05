# Media Engine and Sony AVC

The Media Engine performs Sony hardware AVC decoding. The kernel helper loads the firmware providers, binds the Sony imports and supports AVC mode setup.

The hardware decoder submits assembled H.264 access units to Sony's sceMpeg AVC framework. The in-band PPS selects the firmware mode before the first IDR: CAVLC uses Baseline mode; CABAC uses Main mode.

The AVC player manages decoded pictures and RGBA output surfaces. The PSP graphics engine scales and presents the frames while the Main CPU handles network traffic, controls and audio.

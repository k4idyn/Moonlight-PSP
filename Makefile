TARGET = moonlight-psp-core
OBJS = \
    src/main.o \
    src/wpa2.o \
    src/exports.o \
    src/test_errno.o \
    src/libgamestream/client.o \
    src/libgamestream/discover.o \
    src/libgamestream/http.o \
    src/libgamestream/mkcert.o \
    src/libgamestream/sps.o \
    src/libgamestream/xml.o \
    src/modules/audio_decoder.o \
    src/modules/exception_handler.o \
    src/modules/input_mapper.o \
    src/modules/kernel_exception.o \
    src/modules/logger.o \
    src/modules/network_receiver.o \
    src/modules/render_pipeline.o \
    src/modules/ui_renderer.o \
    src/modules/video_decoder.o

INCDIR = include
CFLAGS = -O2 -G0 -Wall
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)

LIBDIR =
LDFLAGS =
LIBS = 

EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = Moonlight PSP

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak

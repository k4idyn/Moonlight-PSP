TARGET = moonlight-psp-core
OBJS = \
    src/main.o \
    src/libgamestream/client.o \
    src/libgamestream/discover.o \
    src/libgamestream/http.o \
    src/libgamestream/mkcert.o \
    src/libgamestream/sps.o \
    src/libgamestream/xml.o \
    src/modules/audio_decoder.o \
    src/modules/exception_handler.o \
    src/modules/input_mapper.o \
    src/modules/logger.o \
    src/modules/network_receiver.o \
    src/modules/render_pipeline.o \
    src/modules/ui_renderer.o \
    src/modules/video_decoder.o \
    src/common/AudioStream.o \
    src/common/ByteBuffer.o \
    src/common/Connection.o \
    src/common/ConnectionTester.o \
    src/common/ControlStream.o \
    src/common/FakeCallbacks.o \
    src/common/InputStream.o \
    src/common/LinkedBlockingQueue.o \
    src/common/Misc.o \
    src/common/Platform.o \
    src/common/PlatformCrypto.o \
    src/common/PlatformSockets.o \
    src/common/RecorderCallbacks.o \
    src/common/RtpAudioQueue.o \
    src/common/RtpVideoQueue.o \
    src/common/RtspConnection.o \
    src/common/RtspParser.o \
    src/common/SdpGenerator.o \
    src/common/SimpleStun.o \
    src/common/VideoDepacketizer.o \
    src/common/VideoStream.o \
    src/common/rswrapper.o

INCDIR = include include/libgamestream include/modules include/common include/ark4
CFLAGS = -O2 -g -G0 -Wall -Wextra -Werror -D_PSP -D__psp__ -DUSE_MBEDTLS -std=gnu99 -I$(PSPDEV)/psp/include
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)

LIBDIR = lib
LDFLAGS = -L$(PSPDEV)/psp/lib
LIBS = -lenet -lmbedtls -lmbedx509 -lmbedcrypto -leverest -lp256m -lopus -lpthread-psp -lmxml -lz -lm -lpspvfpu
LIBS += -lpspaudio -lpspmpeg -lpspgum -lpspgu -lpsprtc -lpspwlan -lpsppower -lpspdebug -lpspdisplay -lpspge -lpspctrl -lc -lpspnet -lpspnet_inet -lpspnet_apctl -lpspnet_resolver -lpsputility -lpspsdk -lpspuser

BUILD_PRX = 1
USE_USER_LIBS = 1
PRX_EXPORTS = exports.exp

EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = Moonlight PSP

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak

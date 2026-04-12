# MediaEngine kernel PRX — exports InitME/KillME
# User-side functions (CallME/BeginME/WaitME etc) live in src/me.c

TARGET = moonlight_me_helper
OBJS = me_stub.o main.o

INCDIR =
CFLAGS = -O2 -G0 -Wall -std=gnu99
CXXFLAGS = $(CFLAGS)
ASFLAGS = $(CFLAGS)

BUILD_PRX = 1
PRX_EXPORTS = exports.exp

USE_KERNEL_LIBC = 1
USE_KERNEL_LIBS = 1

PSP_FW_VERSION = 660

LDFLAGS = -nostartfiles
LIBS = -lpspkernel

EXTRA_TARGETS = MediaEngine.S MediaEngine.o

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak

MediaEngine.S: $(PRX_EXPORTS)
	psp-build-exports --build-stubs $(PRX_EXPORTS)

MediaEngine.o: MediaEngine.S
	$(CC) $(CFLAGS) -c -o $@ $<

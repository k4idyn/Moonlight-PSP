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
PSPDEV = $(shell psp-config --pspdev-path)
PSP_TOOL_BIN = $(PSPDEV)/bin
export PATH := $(PSP_TOOL_BIN):$(PATH)
CC = $(PSP_TOOL_BIN)/psp-gcc
CXX = $(PSP_TOOL_BIN)/psp-g++

LDFLAGS = -nostartfiles
LIBS = -lpspkernel

EXTRA_TARGETS = MediaEngine.S MediaEngine.o

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak

override CC := $(PSP_TOOL_BIN)/psp-gcc.exe
override CXX := $(PSP_TOOL_BIN)/psp-g++.exe
override AS := $(PSP_TOOL_BIN)/psp-gcc.exe
override LD := $(PSP_TOOL_BIN)/psp-gcc.exe
override AR := $(PSP_TOOL_BIN)/psp-ar.exe
override RANLIB := $(PSP_TOOL_BIN)/psp-ranlib.exe
override STRIP := $(PSP_TOOL_BIN)/psp-strip.exe
override MKSFO := $(PSP_TOOL_BIN)/mksfo.exe
override PACK_PBP := $(PSP_TOOL_BIN)/pack-pbp.exe
override FIXUP := $(PSP_TOOL_BIN)/psp-fixup-imports.exe

MediaEngine.S: $(PRX_EXPORTS)
	$(PSP_TOOL_BIN)/psp-build-exports --build-stubs $(PRX_EXPORTS)

MediaEngine.o: MediaEngine.S
	$(CC) $(CFLAGS) -c -o $@ $<

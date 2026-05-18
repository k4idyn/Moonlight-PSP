# PSP Homebrew Makefile for PSP Moonlight
# Asymmetric dual-core software H.264 decode:
#   Main CPU: Network + CAVLC entropy decode
#   Media Engine: VFPU reconstruction (IDCT + MotComp + YUV→RGBA)
# Targets PSP with MIPS Allegrex core

# ============================================================================
# PSPSDK Configuration
# ============================================================================
PSPSDK   = $(shell psp-config --pspsdk-path)
PSP_PREFIX = $(shell psp-config --psp-prefix)
PSPDEV   = $(shell psp-config --pspdev-path)
PSP_TOOL_BIN := $(PSPDEV)/bin
export PATH := $(PSP_TOOL_BIN):$(PATH)

# ============================================================================
# Build Target Configuration
# ============================================================================
TARGET          = moonlight
PSP_EBOOT_TITLE = PSP Moonlight
BUILD_PRX       = 1
PSP_FW_VERSION  = 660
EXTRA_TARGETS   = EBOOT.PBP
.DEFAULT_GOAL   = all

# ============================================================================
# Source Files
# ============================================================================
OBJS = src/main.o src/network_connect.o src/network_me.o \
       src/net_send.o \
       src/sw_decoder_thread.o src/stream_resolution.o \
       src/upnp_client.o \
       src/display_gpu.o src/input.o src/rtp_reassembly.o src/rtp_fec.o src/rs.o src/host_discovery.o \
       src/settings_menu.o src/config.o src/hud.o src/stream_session.o src/game_list_parser.o \
       src/signal_strength.o src/safety_buffer.o src/audio_thread.o src/power_handler.o \
       src/moonlight_stubs.o src/ui_manager.o src/button_mapping_ui.o src/game_grid_ui.o src/pairing_pin_ui.o \
       src/netconf_ui.o src/osk_input.o src/stream_connect_ui.o src/wol.o src/exit_dialog.o \
       src/diag_log.o src/runtime_telemetry.o \
       src/crypto_lite.o src/psp_mbedtls_entropy.o \
       src/stream_crypto.o src/client_identity.o src/icon_cache.o src/control_stream.o \
       src/opus_decode_psp.o \
       src/me.o moonlight_me_helper/MediaEngine.o \
       src/openh264_decode.o \
       $(MBEDTLS_OBJS) $(OPUS_ALL_OBJS)

# ============================================================================
# mbedTLS
# ============================================================================
MBEDTLS_ROOT    = third_party/mbedtls
MBEDTLS_LIBDIR  = $(MBEDTLS_ROOT)/library
MBEDTLS_INCDIR  = $(MBEDTLS_ROOT)/include
MBEDTLS_CFLAGS  = -I$(MBEDTLS_ROOT)/include \
                  -DMBEDTLS_CONFIG_FILE='"mbedtls_psp_config.h"' \
                  -include limits.h

MBEDTLS_NAMES = aes asn1parse asn1write base64 bignum cipher cipher_wrap \
                constant_time ctr_drbg ecdh ecp ecp_curves entropy error gcm md md5 oid pem pk pk_wrap pkparse pkwrite \
                platform platform_util memory_buffer_alloc rsa rsa_internal sha1 sha256 \
                ssl_ciphersuites ssl_cli ssl_cookie ssl_msg ssl_ticket ssl_tls \
                x509 x509_create x509_crl x509_crt x509_csr x509write_crt x509write_csr

MBEDTLS_OBJS  = $(addprefix mbedtls_, $(addsuffix .o, $(MBEDTLS_NAMES)))

# ============================================================================
# Opus Codec
# ============================================================================
OPUS_ROOT   = third_party/opus
OPUS_CFLAGS = -DOPUS_BUILD -include opus_psp_config.h \
              -I$(OPUS_ROOT)/include -I$(OPUS_ROOT)/celt \
              -I$(OPUS_ROOT)/silk -I$(OPUS_ROOT)/silk/fixed \
              -I$(OPUS_ROOT)/src

OPUS_SRC_NAMES = opus opus_decoder opus_encoder extensions \
                 opus_multistream opus_multistream_encoder \
                 opus_multistream_decoder repacketizer \
                 opus_projection_encoder opus_projection_decoder \
                 mapping_matrix
OPUS_CELT_NAMES = bands celt celt_encoder celt_decoder cwrs \
                  entcode entdec entenc kiss_fft laplace \
                  mathops mdct modes pitch celt_lpc \
                  quant_bands rate vq
OPUS_SILK_NAMES = CNG code_signs init_decoder decode_core \
                  decode_frame decode_parameters decode_indices \
                  decode_pulses decoder_set_fs dec_API enc_API \
                  encode_indices encode_pulses gain_quant interpolate \
                  LP_variable_cutoff NLSF_decode NSQ NSQ_del_dec PLC \
                  shell_coder tables_gain tables_LTP tables_NLSF_CB_NB_MB \
                  tables_NLSF_CB_WB tables_other tables_pitch_lag \
                  tables_pulses_per_block VAD control_audio_bandwidth \
                  quant_LTP_gains VQ_WMat_EC HP_variable_cutoff \
                  NLSF_encode NLSF_VQ NLSF_unpack NLSF_del_dec_quant \
                  process_NLSFs stereo_LR_to_MS stereo_MS_to_LR \
                  check_control_input control_SNR init_encoder control_codec \
                  A2NLSF ana_filt_bank_1 biquad_alt bwexpander_32 bwexpander \
                  debug decode_pitch inner_prod_aligned lin2log log2lin \
                  LPC_analysis_filter LPC_inv_pred_gain table_LSF_cos \
                  NLSF2A NLSF_stabilize NLSF_VQ_weights_laroia \
                  pitch_est_tables resampler resampler_down2_3 resampler_down2 \
                  resampler_private_AR2 resampler_private_down_FIR \
                  resampler_private_IIR_FIR resampler_private_up2_HQ \
                  resampler_rom sigm_Q15 sort sum_sqr_shift \
                  stereo_decode_pred stereo_encode_pred stereo_find_predictor \
                  stereo_quant_pred LPC_fit
OPUS_SILK_FIX_NAMES = LTP_analysis_filter_FIX LTP_scale_ctrl_FIX \
                      corrMatrix_FIX encode_frame_FIX find_LPC_FIX \
                      find_LTP_FIX find_pitch_lags_FIX find_pred_coefs_FIX \
                      noise_shape_analysis_FIX process_gains_FIX \
                      regularize_correlations_FIX residual_energy16_FIX \
                      residual_energy_FIX warped_autocorrelation_FIX \
                      apply_sine_window_FIX autocorr_FIX burg_modified_FIX \
                      k2a_FIX k2a_Q16_FIX pitch_analysis_core_FIX \
                      vector_ops_FIX schur64_FIX schur_FIX

OPUS_SRC_OBJS      = $(addprefix opus_s_, $(addsuffix .o, $(OPUS_SRC_NAMES)))
OPUS_CELT_OBJS     = $(addprefix opus_c_, $(addsuffix .o, $(OPUS_CELT_NAMES)))
OPUS_SILK_OBJS     = $(addprefix opus_k_, $(addsuffix .o, $(OPUS_SILK_NAMES)))
OPUS_SILK_FIX_OBJS = $(addprefix opus_f_, $(addsuffix .o, $(OPUS_SILK_FIX_NAMES)))
OPUS_ALL_OBJS      = $(OPUS_SRC_OBJS) $(OPUS_CELT_OBJS) $(OPUS_SILK_OBJS) $(OPUS_SILK_FIX_OBJS)

# ============================================================================
# OpenH264 (PSP decoder-only port)
# ============================================================================
OPENH264_ROOT    = third_party/openh264
OPENH264_LIB     = $(OPENH264_ROOT)/libopenh264_dec_psp.a
OPENH264_INCDIR  = -I$(OPENH264_ROOT)
CORE_HEADERS     = $(wildcard include/*.h)

# ============================================================================
# Build Mode
# ============================================================================
# RETAIL_BUILD=1 disables diagnostic file writes and debug telemetry work.
# This is the default for public release packaging.
# For local hardware debugging/validation, use RETAIL_BUILD=0.
RETAIL_BUILD ?= 1
PSP_VIDEO_FEC_PERCENT ?= 35
PSP_VIDEO_FEC_MIN_REQUIRED ?= 1
PSP_AUDIO_PACKET_DURATION_MS ?= 60
ifeq ($(RETAIL_BUILD),1)
BUILD_MODE_DEFINES = -DRETAIL_BUILD
else
BUILD_MODE_DEFINES =
endif

PSP_TUNE_DEFINES = -DPSP_VIDEO_FEC_PERCENT=$(PSP_VIDEO_FEC_PERCENT) \
                   -DPSP_VIDEO_FEC_MIN_REQUIRED=$(PSP_VIDEO_FEC_MIN_REQUIRED) \
                   -DPSP_AUDIO_PACKET_DURATION_MS=$(PSP_AUDIO_PACKET_DURATION_MS)
BUILD_DIR = .build
TUNE_STAMP = $(BUILD_DIR)/tune.stamp

# ============================================================================
# Compiler Flags
# ============================================================================
CFLAGS  = -O2 -G0 -Wall -Werror -DPSP $(BUILD_MODE_DEFINES) $(PSP_TUNE_DEFINES) $(MBEDTLS_CFLAGS) \
           -I$(PSPSDK)/include -I$(PSP_PREFIX)/include
CXXFLAGS = -O2 -G0 -Wall -Werror -DPSP $(BUILD_MODE_DEFINES) $(PSP_TUNE_DEFINES) -fno-exceptions -fno-rtti $(MBEDTLS_CFLAGS) \
           $(OPENH264_INCDIR) \
           -I$(PSPSDK)/include -I$(PSP_PREFIX)/include

CC  = $(PSP_TOOL_BIN)/psp-gcc
CXX = $(PSP_TOOL_BIN)/psp-g++

INCDIR  = include $(PSP_PREFIX)/include/oslib/intraFont $(PSP_PREFIX)/include
LIBDIR  = lib $(PSP_PREFIX)/lib

LIBS = $(OPENH264_LIB) \
       -lintraFont -lpsprtc -lpspwlan \
       -lpspgum -lpspgu -lpspge -lpsppower \
       -lpspdebug -lpspdisplay -lpspctrl -lpspsdk -lc -lpng -lz -lm \
       -lpspnet -lpspnet_inet -lpspnet_apctl -lpspnet_resolver \
       -lpsputility -lpspuser -lpspaudio -lpsphttp -lstdc++

# ============================================================================
# Targets
# ============================================================================
all: me_helper $(TARGET).prx $(EXTRA_TARGETS)

.PHONY: FORCE_TUNE_STAMP

$(TUNE_STAMP): FORCE_TUNE_STAMP
	@mkdir -p $(BUILD_DIR)
	@printf "RETAIL_BUILD=%s\nPSP_VIDEO_FEC_PERCENT=%s\nPSP_VIDEO_FEC_MIN_REQUIRED=%s\nPSP_AUDIO_PACKET_DURATION_MS=%s\n" "$(RETAIL_BUILD)" "$(PSP_VIDEO_FEC_PERCENT)" "$(PSP_VIDEO_FEC_MIN_REQUIRED)" "$(PSP_AUDIO_PACKET_DURATION_MS)" > $@.tmp
	@if test -f $@ && cmp -s $@.tmp $@; then rm -f $@.tmp; else mv -f $@.tmp $@; fi

.PHONY: smoke
smoke:
	bash scripts/smoke_checks.sh

# Build the Media Engine kernel PRX helper (provides InitME/KillME)
.PHONY: me_helper
me_helper:
	$(MAKE) -C moonlight_me_helper
	@cp -f moonlight_me_helper/moonlight_me_helper.prx .

$(MBEDTLS_OBJS): mbedtls_%.o: $(MBEDTLS_LIBDIR)/%.c
	$(CC) -std=gnu99 $(CFLAGS) -c -o $@ $<

src/psp_mbedtls_entropy.o: src/psp_mbedtls_entropy.c $(TUNE_STAMP)
	$(CC) $(CFLAGS) -c -o $@ $<

src/opus_decode_psp.o: src/opus_decode_psp.c $(TUNE_STAMP)
	$(CC) $(CFLAGS) $(OPUS_CFLAGS) -c -o $@ $<

# OpenH264 decoder wrapper (C++ TU, needs openh264 include paths)
src/openh264_decode.o: src/openh264_decode.cpp $(OPENH264_LIB) $(CORE_HEADERS) $(TUNE_STAMP)
	$(CXX) $(CXXFLAGS) $(OPENH264_INCDIR) -Iinclude -I$(PSPSDK)/include -I$(PSP_PREFIX)/include/oslib/intraFont -I$(PSP_PREFIX)/include -c -o $@ $<

# Build OpenH264 PSP static library if not already built
$(OPENH264_LIB):
	$(MAKE) -C $(OPENH264_ROOT) -f Makefile.psp

$(OPUS_SRC_OBJS): opus_s_%.o: $(OPUS_ROOT)/src/%.c
	$(CC) $(CFLAGS) $(OPUS_CFLAGS) -c -o $@ $<

$(OPUS_CELT_OBJS): opus_c_%.o: $(OPUS_ROOT)/celt/%.c
	$(CC) $(CFLAGS) $(OPUS_CFLAGS) -c -o $@ $<

$(OPUS_SILK_OBJS): opus_k_%.o: $(OPUS_ROOT)/silk/%.c
	$(CC) $(CFLAGS) $(OPUS_CFLAGS) -c -o $@ $<

$(OPUS_SILK_FIX_OBJS): opus_f_%.o: $(OPUS_ROOT)/silk/fixed/%.c
	$(CC) $(CFLAGS) $(OPUS_CFLAGS) -c -o $@ $<

# C-only standard — inject -std=gnu99 only for .c files.
# build.mak passes $(CFLAGS) to psp-g++ too, so -std=gnu99 must NOT be in CFLAGS.
# This pattern rule takes precedence over build.mak's suffix rule for src/*.c files.
src/%.o: src/%.c $(CORE_HEADERS) $(TUNE_STAMP)
	$(CC) -std=gnu99 $(CFLAGS) -c -o $@ $<

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

EXPORT_OBJ_RSP := $(subst \,/,$(EXPORT_OBJ))

# Workaround for CreateProcess Windows character limits on the PSP-1000
# release build. Keep PSPSDK's normal object order, but pass the long
# object/library list through a GCC response file.
$(TARGET).elf: $(OBJS) $(EXPORT_OBJ)
	printf "%s\n" $(OBJS) $(EXPORT_OBJ_RSP) $(LIBS) > _link.rsp
	$(LINK.c) -B$(PSP_TOOL_BIN)/ @_link.rsp -o $@
	$(PSP_TOOL_BIN)/psp-fixup-imports.exe $@

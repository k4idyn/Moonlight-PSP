# build_psp.ps1 - Build libopenh264_dec_psp.a for PSP (Windows)
# Uses pspdev native Windows toolchain at C:\pspdev\pspsdk\bin

$TOOLCHAIN  = "C:\pspdev\pspsdk\bin"
$PSP_GXX    = "$TOOLCHAIN\psp-g++.exe"
$PSP_AR     = "$TOOLCHAIN\psp-ar.exe"
$PSP_RANLIB = "$TOOLCHAIN\psp-ranlib.exe"

$SDK_INCLUDE = "C:/pspdev/pspsdk/psp/include"

$CXXFLAGS = @(
    "-march=allegrex", "-mabi=eabi", "-G0",
    "-Wall", "-fno-strict-aliasing",
    "-DPSP", "-DGENERATED_VERSION_HEADER",
    "-DDISABLE_ENCODER_SIDE", "-DDISABLE_DECODER_MT",
    "-D_POSIX_THREADS",
    "-O3", "-DNDEBUG",
    "-I$SDK_INCLUDE",
    "-I./codec/api/wels",
    "-I./codec/common/inc",
    "-I./codec/decoder/core/inc",
    "-I./codec/decoder/plus/inc"
)

$SOURCES = @(
    "codec/common/src/common_tables.cpp",
    "codec/common/src/copy_mb.cpp",
    "codec/common/src/cpu.cpp",
    "codec/common/src/crt_util_safe_x.cpp",
    "codec/common/src/deblocking_common.cpp",
    "codec/common/src/expand_pic.cpp",
    "codec/common/src/intra_pred_common.cpp",
    "codec/common/src/mc.cpp",
    "codec/common/src/memory_align.cpp",
    "codec/common/src/sad_common.cpp",
    "codec/common/src/utils.cpp",
    "codec/common/src/welsCodecTrace.cpp",
    "codec/common/src/WelsTaskThread.cpp",
    "codec/common/src/WelsThread.cpp",
    "codec/common/src/WelsThreadLib.cpp",
    "codec/common/src/WelsThreadPool.cpp",
    "codec/decoder/core/src/au_parser.cpp",
    "codec/decoder/core/src/bit_stream.cpp",
    "codec/decoder/core/src/cabac_decoder.cpp",
    "codec/decoder/core/src/deblocking.cpp",
    "codec/decoder/core/src/decode_mb_aux.cpp",
    "codec/decoder/core/src/decode_slice.cpp",
    "codec/decoder/core/src/decoder.cpp",
    "codec/decoder/core/src/decoder_core.cpp",
    "codec/decoder/core/src/decoder_data_tables.cpp",
    "codec/decoder/core/src/error_concealment.cpp",
    "codec/decoder/core/src/fmo.cpp",
    "codec/decoder/core/src/get_intra_predictor.cpp",
    "codec/decoder/core/src/manage_dec_ref.cpp",
    "codec/decoder/core/src/memmgr_nal_unit.cpp",
    "codec/decoder/core/src/mv_pred.cpp",
    "codec/decoder/core/src/parse_mb_syn_cabac.cpp",
    "codec/decoder/core/src/parse_mb_syn_cavlc.cpp",
    "codec/decoder/core/src/pic_queue.cpp",
    "codec/decoder/core/src/rec_mb.cpp",
    "codec/decoder/core/src/wels_decoder_thread.cpp",
    "codec/decoder/plus/src/welsDecoderExt.cpp"
)

$OUTPUT_LIB = "libopenh264_dec_psp.a"

$errors = 0
$objects = @()

Write-Host "=== Building OpenH264 PSP (GCC 4.3.5) ===" -ForegroundColor Cyan
Write-Host "Compiling $($SOURCES.Count) source files..." -ForegroundColor Yellow

foreach ($src in $SOURCES) {
    $obj = $src -replace "\.cpp$", ".o"
    $objects += $obj
    Write-Host "  CC $src" -ForegroundColor Gray
    $result = & $PSP_GXX @CXXFLAGS -c $src -o $obj 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host "FAILED: $src" -ForegroundColor Red
        $result | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
        $errors++
    } elseif ($result) {
        $result | ForEach-Object { Write-Host "  WARN: $_" -ForegroundColor Yellow }
    }
}

if ($errors -gt 0) {
    Write-Host "`n$errors compile error(s). Aborting." -ForegroundColor Red
    exit 1
}

Write-Host "`nArchiving -> $OUTPUT_LIB ..." -ForegroundColor Yellow
& $PSP_AR cr $OUTPUT_LIB @objects 2>&1
if ($LASTEXITCODE -ne 0) { Write-Host "ar failed" -ForegroundColor Red; exit 1 }

& $PSP_RANLIB $OUTPUT_LIB 2>&1
if ($LASTEXITCODE -ne 0) { Write-Host "ranlib failed" -ForegroundColor Red; exit 1 }

$size = (Get-Item $OUTPUT_LIB).Length
Write-Host "`n=== SUCCESS: $OUTPUT_LIB ($size bytes) ===" -ForegroundColor Green

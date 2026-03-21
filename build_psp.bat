@echo off
set "PSPDEV=C:\bin_root\psp_sdk_root"
set "PSPSDK=C:\bin_root\psp_sdk_root\psp\sdk"
set "PATH=%PSPDEV%\bin;%PATH%"

if not exist "%PSPDEV%\bin\psp-gcc.exe" (
    copy "%PSPDEV%\bin\psp-gcc-4.3.5.exe" "%PSPDEV%\bin\psp-gcc.exe"
)

echo Building with PSPSDK=%PSPSDK%
make clean
make V=1 2>&1

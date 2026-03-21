@echo off
set PSPDEV=C:\Users\beelink\Desktop\moonlight3\pspsdk
set PATH=%PSPDEV%\bin;%PSPDEV%\libexec\gcc\psp\4.3.5;%PATH%
cd moonlight-psp-core
make clean
make
if %ERRORLEVEL% equ 0 (
    echo BUILD SUCCESSFUL
) else (
    echo BUILD FAILED
    exit /b %ERRORLEVEL%
)

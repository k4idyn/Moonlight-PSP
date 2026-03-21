@echo off
echo --- Building Moonlight PSP Core (Windows) ---
:: Ensure PSPSDK bin is in PATH
make clean
make -j%NUMBER_OF_PROCESSORS%
if %ERRORLEVEL% NEQ 0 (
    echo Build FAILED!
    pause
    exit /b %ERRORLEVEL%
)
echo Build Successful! EBOOT.PBP generated.
pause

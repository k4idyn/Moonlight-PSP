#!/bin/bash
echo "--- Building Moonlight PSP Core (Linux) ---"
# Ensure PSPSDK bin is in PATH
make clean
make -j$(nproc)
if [ $? -ne 0 ]; then
    echo "Build FAILED!"
    exit 1
fi
echo "Build Successful! EBOOT.PBP generated."

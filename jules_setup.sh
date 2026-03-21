#!/bin/bash
# jules_setup.sh - Automated Jules VM Setup
set -e

echo "--- Starting Moonlight PSP Environment Setup ---"

# 0. Sync and reset the current repository to ensure a clean state
if [ -d ".git" ]; then
    echo "Resetting repository to clean state..."
    git pull
    git reset --hard HEAD
fi

# 1. Install System Dependencies
sudo apt-get update
sudo apt-get install -y \
    build-essential cmake git python3 python3-pip \
    autoconf automake bison flex libncurses5-dev libreadline-dev \
    libusb-dev libgmp3-dev libmpfr-dev libmpc-dev libisl-dev \
    zlib1g-dev libtool libtool-bin libncurses-dev jq curl

# 2. Install PSPSDK (Prebuilt for Ubuntu)
if [ ! -d "/usr/local/pspdev" ]; then
    echo "Installing PSPSDK Toolchain..."
    # Using a specific known-good release to avoid API rate limits or missing assets
    PSPDEV_VER="v20260301"
    FILENAME="pspdev-ubuntu-latest-x86_64.tar.gz"
    URL="https://github.com/pspdev/pspdev/releases/download/${PSPDEV_VER}/${FILENAME}"
    
    echo "Downloading from: $URL"
    curl -L "$URL" -o pspdev.tar.gz
    
    # Check if the file is valid
    if ! file pspdev.tar.gz | grep -q "gzip compressed data"; then
        echo "Error: Downloaded pspdev.tar.gz is not a valid gzip file. Check the URL or GitHub status."
        head -c 100 pspdev.tar.gz
        exit 1
    fi
    
    sudo mkdir -p /usr/local/pspdev
    sudo tar -xzf pspdev.tar.gz -C /usr/local/pspdev --strip-components=1
    rm pspdev.tar.gz
fi

export PSPDEV=/usr/local/pspdev
export PATH=$PATH:$PSPDEV/bin
export ROOT_DIR=$(pwd)

# Helper function to find or clone a dependency
# Usage: find_or_clone <dir_name> <git_url> <branch/tag>
find_or_clone() {
    if [ -d "$ROOT_DIR/$1" ]; then
        echo "Found local dependency: $1"
        cd "$ROOT_DIR/$1"
    elif [ -d "$ROOT_DIR/../$1" ]; then
         echo "Found parent-level dependency: $1"
         cd "$ROOT_DIR/../$1"
    else
        echo "Cloning dependency: $1 from $2..."
        git clone --depth 1 -b "$3" "$2" "$ROOT_DIR/$1"
        cd "$ROOT_DIR/$1"
    fi
}

# 3. Build & Install Project Dependencies
# We install to $PSPDEV/psp to match the toolchain's search paths

echo "Building ENet..."
# Clone the 'moonlight' branch which has the initial PSP support
find_or_clone "enet-cgutman" "https://github.com/cgutman/enet.git" "moonlight"
# Apply our custom Error 116 timeout fixes
if [ -f "$ROOT_DIR/patches/enet_psp.patch" ]; then
    echo "Applying ENet PSP patches..."
    git apply --ignore-whitespace "$ROOT_DIR/patches/enet_psp.patch"
fi
rm -rf build-linux-psp && mkdir build-linux-psp && cd build-linux-psp
cmake .. -DCMAKE_TOOLCHAIN_FILE="$ROOT_DIR/generic-psp-toolchain.cmake" -G "Unix Makefiles"
make -j$(nproc)
make install

echo "Building Mini-XML (mxml)..."
find_or_clone "mxml-4.0.4" "https://github.com/michaelrsweet/mxml.git" "v4.0.4"
CC=psp-gcc ./configure --host=mipsel-pspe-elf --prefix="$PSPDEV/psp" --disable-shared
make -j$(nproc)
make install

echo "Building Opus..."
find_or_clone "opus-1.5.2" "https://github.com/xiph/opus.git" "v1.5.2"
rm -rf build-linux-psp && mkdir build-linux-psp && cd build-linux-psp
cmake .. -DCMAKE_TOOLCHAIN_FILE="$ROOT_DIR/generic-psp-toolchain.cmake" \
    -DOPUS_FIXED_POINT=ON -DOPUS_ENABLE_FLOAT_API=OFF -DOPUS_ASSERTIONS=OFF \
    -DOPUS_CUSTOM_MODES=OFF -DOPUS_INSTALL_PKG_CONFIG_MODULE=OFF \
    -DOPUS_INSTALL_CMAKE_CONFIG_MODULE=OFF
make -j$(nproc)
make install

echo "Building mbedTLS..."
find_or_clone "mbedtls-3.6.2" "https://github.com/Mbed-TLS/mbedtls.git" "mbedtls-3.6.2"
git submodule update --init --recursive
# Jules: Disable platform entropy and timing as they are not supported on PSP
sed -i 's/\/\/#define MBEDTLS_NO_PLATFORM_ENTROPY/#define MBEDTLS_NO_PLATFORM_ENTROPY/' include/mbedtls/mbedtls_config.h
sed -i 's/\/\/#define MBEDTLS_NO_DEFAULT_ENTROPY_SOURCES/#define MBEDTLS_NO_DEFAULT_ENTROPY_SOURCES/' include/mbedtls/mbedtls_config.h
printf "\n#undef MBEDTLS_TIMING_C\n#undef MBEDTLS_NET_C\n" >> include/mbedtls/mbedtls_config.h
# Jules: Enable platform millisecond time alternative and provide implementation
sed -i 's/\/\/#define MBEDTLS_PLATFORM_MS_TIME_ALT/#define MBEDTLS_PLATFORM_MS_TIME_ALT/' include/mbedtls/mbedtls_config.h
printf "\n#if defined(__psp__)\n#include <pspthreadman.h>\nmbedtls_ms_time_t mbedtls_ms_time(void)\n{\n    return (mbedtls_ms_time_t)(sceKernelGetSystemTimeWide() / 1000);\n}\n#endif\n" >> library/platform_util.c
rm -rf build-linux-psp && mkdir build-linux-psp && cd build-linux-psp
cmake .. -DCMAKE_TOOLCHAIN_FILE="$ROOT_DIR/generic-psp-toolchain.cmake" \
    -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF \
    -DMBEDTLS_FATAL_WARNINGS=OFF \
    -DGEN_FILES=OFF
make -j$(nproc)
make install
cd "$ROOT_DIR"

echo "Building Moonlight Common C..."
find_or_clone "moonlight-common-c" "https://github.com/moonlight-stream/moonlight-common-c.git" "master"
# Moonlight-common-c now includes PSP support in master. 
# We only apply the patch if it hasn't been applied yet (though usually it is already there).
if [ -f "$ROOT_DIR/patches/common_c_psp.patch" ]; then
    if grep -q "_PSP" src/PlatformSockets.h; then
        echo "Moonlight Common C already contains PSP support. Skipping patch."
    else
        echo "Applying Common C PSP patches..."
        git apply --ignore-whitespace "$ROOT_DIR/patches/common_c_psp.patch"
    fi
fi
rm -rf build-linux-psp && mkdir build-linux-psp && cd build-linux-psp
cmake .. -DCMAKE_TOOLCHAIN_FILE="$ROOT_DIR/generic-psp-toolchain.cmake" -DUSE_MBEDTLS=ON
make -j$(nproc)
make install

echo "--- Setup Complete! ---"
echo "You can now run 'make' inside the root directory!"

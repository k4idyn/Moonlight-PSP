#!/bin/bash
# jules_setup.sh - Automated Jules VM Setup
set -e

echo "--- Starting Moonlight PSP Environment Setup ---"

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
    LATEST_TAG=$(curl -s https://api.github.com/repos/pspdev/pspdev/releases/latest | jq -r .tag_name)
    curl -L "https://github.com/pspdev/pspdev/releases/download/${LATEST_TAG}/pspdev-linux-amd64.tar.gz" -o pspdev.tar.gz
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
find_or_clone "enet-cgutman" "https://github.com/cgutman/enet.git" "master"
# We should apply psp patches if needed, but for now we build the master
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
rm -rf build-linux-psp && mkdir build-linux-psp && cd build-linux-psp
cmake .. -DCMAKE_TOOLCHAIN_FILE="$ROOT_DIR/generic-psp-toolchain.cmake" \
    -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF \
    -DMBEDTLS_TEST_NULL_ENTROPY=ON -DMBEDTLS_NO_DEFAULT_ENTROPY_SOURCES=ON
make -j$(nproc)
make install

echo "Building Moonlight Common C..."
find_or_clone "moonlight-common-c" "https://github.com/moonlight-stream/moonlight-common-c.git" "master"
rm -rf build-linux-psp && mkdir build-linux-psp && cd build-linux-psp
cmake .. -DCMAKE_TOOLCHAIN_FILE="$ROOT_DIR/generic-psp-toolchain.cmake" -DUSE_MBEDTLS=ON
make -j$(nproc)
make install

echo "--- Setup Complete! ---"
echo "You can now run 'make' inside the root directory!"

#!/bin/bash
# setup_linux.sh - Automated Environment Setup
set -e

echo "--- Starting Moonlight PSP Environment Setup ---"

# 0. Sync and reset the current repository to ensure a clean state
if [ -d ".git" ]; then
    echo "Resetting repository to clean state..."
    git fetch origin
    git reset --hard origin/main
    git clean -fd
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

# 3. Environment Summary
echo "--- Setup Complete! ---"
echo "PSPSDK is installed at /usr/local/pspdev"
echo "Add these to your .bashrc or .zshrc:"
echo "  export PSPDEV=/usr/local/pspdev"
echo "  export PATH=\$PATH:\$PSPDEV/bin"
echo ""
echo "This repository includes all necessary pre-compiled dependencies in the 'lib/' directory."
echo "You can now run 'make' inside the root directory!"

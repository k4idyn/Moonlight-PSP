# PSP Toolchain Installation Guide for Windows

## Overview
This guide provides detailed instructions for installing the PSP toolchain (psp-gcc, psp-config) and dependencies (ENet, Opus, mbedTLS) on Windows systems without a Unix-like subsystem.

## Prerequisites
- Windows 10 or 11 (64-bit recommended)
- Administrative privileges
- Internet connection
- Approximately 2-3 GB of free disk space

## RECOMMENDED: Automated Setup (Linux / Jules VM)

If you have access to an Ubuntu environment or a VM (e.g., WSL2 or Jules VM), it is **highly recommended** to use the automated setup script. This script handles the toolchain and all dependencies in a single step:

```bash
cd moonlight-psp-core
./jules_setup.sh
```

---

## Step 1: Install MSYS2 (Alternative Approach for Native Windows)

Since you don't have a Unix-like environment, we'll install MSYS2 which provides a Unix-like environment on Windows.

### 1.1 Download and Install MSYS2
1. Go to https://www.msys2.org/
2. Download the latest MSYS2 installer (msys2-x86_64-*.exe)
3. Run the installer as administrator
4. Follow the installation wizard (default settings are fine)
5. **Important**: When installation completes, check the box to "Run MSYS2 now" and click Finish

### 1.2 Update MSYS2 Core Components
In the MSYS2 MSYS terminal that opens:
```bash
pacman -Syuu
```
If prompted to proceed, type `Y` and press Enter.
If the update requires restarting MSYS2, close the terminal and reopen it, then run:
```bash
pacman -Su
```

### 1.3 Install Required Development Tools
```bash
pacman -S base-devel mingw-w64-x86_64-toolchain git wget unzip
```

## Step 2: Install PSP Toolchain via PSPDev

The PSP toolchain is not available in standard MSYS2 repositories, so we'll install it from source.

### 2.1 Install Dependencies for Building PSP Toolchain
```bash
pacman -S subversion texinfo
```

### 2.2 Set Environment Variables
Add these to your MSYS2 environment (you can add them to ~/.bashrc):
```bash
export PSPDEV=/opt/pspsdk
export PSPSDK=$PSPDEV/psp/sdk
export PATH=$PATH:$PSPDEV/bin:$PSPSDK/bin
```

To make these permanent, add them to your ~/.bashrc file:
```bash
echo 'export PSPDEV=/opt/pspsdk' >> ~/.bashrc
echo 'export PSPSDK=$PSPDEV/psp/sdk' >> ~/.bashrc
echo 'export PATH=$PATH:$PSPDEV/bin:$PSPSDK/bin' >> ~/.bashrc
source ~/.bashrc
```

### 2.3 Create Installation Directory
```bash
sudo mkdir -p /opt/pspsdk
sudo chown $(whoami):$(whoami) /opt/pspsdk
```

### 2.4 Download and Build PSP Toolchain
```bash
# Create a working directory
mkdir -p ~/psp-toolchain
cd ~/psp-toolchain

# Download the PSP toolchain source
wget https://github.com/pspdev/pspsdk/archive/refs/heads/master.zip
unzip master.zip
cd pspsdk-master

# Build and install
./toolchain-sudo.sh
```

**Note**: The build process may take 30-60 minutes depending on your system.

### 2.5 Verify Installation
After installation completes, verify the tools are available:
```bash
psp-gcc --version
psp-config --version
```

## Step 3: Install PSP Dependencies

### 3.1 Install ENet
```bash
cd ~/psp-toolchain
git clone https://github.com/lsalzman/enet.git
cd enet
mkdir build && cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=$PSPSDK/lib/cmake/PSP.cmake
make
make install
```

### 3.2 Install Opus
```bash
cd ~/psp-toolchain
wget https://archive.mozilla.org/pub/opus/opus-1.3.1.tar.gz
tar -xzf opus-1.3.1.tar.gz
cd opus-1.3.1
./configure --host=psp --prefix=$PSPSDK --disable-shared --enable-static
make
make install
```

### 3.3 Install mbedTLS
```bash
cd ~/psp-toolchain
git clone https://github.com/Mbed-TLS/mbedtls.git
cd mbedtls
cmake . -DCMAKE_TOOLCHAIN_FILE=$PSPSDK/lib/cmake/PSP.cmake -DENABLE_TESTING=OFF
make
make install
```

## Alternative: Pre-built PSP Toolchain for Windows

If building from source is too time-consuming, you can use a pre-built toolchain:

### 3.4 Download Pre-built Toolchain
1. Visit https://github.com/pspdev/pspsdk/releases
2. Download the latest release for Windows (look for .zip file)
3. Extract to a location like `C:\pspsdk`
4. Add to your PATH:
   - `C:\pspsdk\bin`
   - `C:\pspsdk\psp\sdk\bin`

### 3.5 Install Dependencies for Pre-built Toolchain
You'll still need to build the dependencies (ENet, Opus, mbedTLS) using the pre-built toolchain:
```bash
# Open MSYS2 MinGW 64-bit terminal
export PSPSDK=/c/pspsdk/psp/sdk
export PATH=$PATH:/c/pspsdk/bin:/c/pspsdk/psp/sdk/bin

# Then follow steps 3.1-3.3 above to build dependencies
```

## Step 4: Verification

After installation, verify everything works:
```bash
psp-gcc --version
psp-config --version
psp-config --psp-libdir
```

You should see version information and library paths.

## Troubleshooting

### Common Issues

1. **Permission Errors**: Run MSYS2 as administrator
2. **Missing Dependencies**: Ensure you ran `pacman -S base-devel` 
3. **Path Issues**: Double-check your PSPDEV and PSPSDK environment variables
4. **Build Failures**: Clean the build directory and try again

### Verification Commands
```bash
# Check if tools are in path
which psp-gcc
which psp-config

# Check library directories
psp-config --psp-libdir
psp-config --psp-includedir

# Test compilation
echo "int main() { return 0; }" > test.c
psp-gcc test.c -o test.elf
```

## Additional Resources

- PSPDev Wiki: https://wiki.pspdev.org/
- PSP SDK Documentation: https://github.com/pspdev/pspsdk
- ENet: https://github.com/lsalzman/enet
- Opus: https://opus-codec.org/
- mbedTLS: https://tls.mbed.org/

## Notes for Moonlight PSP Development

For Moonlight PSP development specifically, you may need additional libraries:
- SDL2: https://www.libsdl.org/
- FFmpeg: https://ffmpeg.org/
- zlib: Usually included with PSP toolchain

These can be installed similarly using the PSP toolchain:
```bash
# Example for zlib (usually already included)
# Example for SDL2
cd ~/psp-toolchain
wget https://www.libsdl.org/release/SDL2-2.0.22.tar.gz
tar -xzf SDL2-2.0.22.tar.gz
cd SDL2-2.0.22
./configure --host=psp --prefix=$PSPSDK
make
make install
```

## Conclusion

Once the PSP toolchain and dependencies are installed, you'll be able to compile PSP homebrew applications including Moonlight PSP ports. The installation process is involved but provides a complete development environment for PSP programming.

Remember to source your ~/.bashrc or restart your MSYS2 terminal after making changes to environment variables.
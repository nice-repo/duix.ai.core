#!/bin/bash

# Exit immediately if a command fails.
set -e

# --- 1. SETUP THE WORKING DIRECTORY ---
APP_DIR="/app"
mkdir -p "$APP_DIR"
cd "$APP_DIR" # Still good practice to start here

echo "Working directory set to $(pwd)"

# --- 2. INSTALL DEPENDENCIES ---
echo "Updating package lists and installing dependencies..."
sudo apt update
sudo apt install build-essential cmake wget curl ffmpeg git libopencv-dev libcurl4-openssl-dev libboost-all-dev -y

# --- 3. BUILD THE APPLICATION ---
# This part still requires changing directories for cmake to work correctly.
echo "Building the application..."
mkdir -p build
cd build
cmake ..
make -j$(($(nproc) - 1))
cp ../conf . -r
mkdir -p audio video

# --- 4. DOWNLOAD RESOURCES (ROBUST METHOD) ---
# This section now uses absolute paths for all checks and file operations,
# making it independent of which directory we are currently in.

echo "Downloading resources using absolute paths..."

if [ ! -d "$APP_DIR/gj_dh_res" ]; then
    echo "Downloading gj_dh_res..."
    # Download the file to a temporary name in the main app directory
    wget -O "$APP_DIR/gj_dh_res.zip" https://cdn.guiji.ai/duix/location/gj_dh_res.zip
    # Unzip the file directly into the main app directory
    unzip "$APP_DIR/gj_dh_res.zip" -d "$APP_DIR"
    rm "$APP_DIR/gj_dh_res.zip" # Clean up the archive
else
    echo "gj_dh_res directory already exists at $APP_DIR/gj_dh_res."
fi

mkdir -p "$APP_DIR/roles"
if [ ! -d "$APP_DIR/roles/SiYao" ]; then
    echo "Downloading SiYao role..."
    wget -O "$APP_DIR/siyao.zip" https://digital-public.obs.cn-east-3.myhuaweicloud.com/duix/digital/model/1719194450521/siyao_20240418.zip
    unzip "$APP_DIR/siyao.zip" -d "$APP_DIR"
    # Move the extracted folder to its final destination using absolute paths
    mv "$APP_DIR/siyao_20240418" "$APP_DIR/roles/SiYao"
    rm "$APP_DIR/siyao.zip" # Clean up
else
    echo "SiYao role already exists at $APP_DIR/roles/SiYao."
fi

if [ ! -d "$APP_DIR/roles/DearSister" ]; then
    echo "Downloading DearSister role..."
    wget -O "$APP_DIR/dear_sister.zip" https://digital-public.obs.cn-east-3.myhuaweicloud.com/duix/digital/model/1719194007931/bendi1_0329.zip
    unzip "$APP_DIR/dear_sister.zip" -d "$APP_DIR"
    mv "$APP_DIR/bendi1_0329" "$APP_DIR/roles/DearSister"
    rm "$APP_DIR/dear_sister.zip" # Clean up
else
    echo "DearSister role already exists at $APP_DIR/roles/DearSister."
fi

# --- 5. RUN THE SERVER ---
echo "Starting the server..."
# We are still in the /app/build directory from step 3, which is correct for running the server.
./ws_server

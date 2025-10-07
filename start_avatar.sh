#!/bin/bash

# Exit immediately if a command fails, making the script more robust.
set -e

# --- 1. SETUP THE WORKING DIRECTORY ---
# Define the main application directory and change into it.
# This is the most critical fix. All subsequent relative paths now start from /app.
APP_DIR="/app"
mkdir -p "$APP_DIR"
cd "$APP_DIR"

echo "Working directory set to $(pwd)"

# --- 2. INSTALL DEPENDENCIES ---
echo "Updating package lists and installing dependencies..."
sudo apt update
sudo apt install build-essential cmake wget curl ffmpeg git libopencv-dev libcurl4-openssl-dev libboost-all-dev -y

# --- 3. BUILD THE APPLICATION ---
# This part assumes your source code (e.g., from git clone) is in /app.
# If not, you should add your `git clone` command here.
echo "Building the application..."
mkdir -p build
cd build
cmake ..
make -j$(($(nproc) - 1))
cp ../conf . -r
mkdir -p audio video
cd "$APP_DIR" # IMPORTANT: Return to the main /app directory after building.

# --- 4. DOWNLOAD RESOURCES ---
# Now that we are back in /app, all downloads will go to the correct place.
echo "Downloading resources..."

if [ ! -d "gj_dh_res" ]; then
    echo "Downloading gj_dh_res..."
    wget https://cdn.guiji.ai/duix/location/gj_dh_res.zip
    unzip gj_dh_res.zip
    rm gj_dh_res.zip # Clean up the archive
else
    echo "gj_dh_res directory already exists."
fi

mkdir -p roles
if [ ! -d "roles/SiYao" ]; then
    echo "Downloading SiYao role..."
    wget https://digital-public.obs.cn-east-3.myhuaweicloud.com/duix/digital/model/1719194450521/siyao_20240418.zip
    unzip siyao_20240418.zip
    mv siyao_20240418 roles/SiYao
    rm siyao_20240418.zip # Clean up
else
    echo "SiYao role already exists."
fi

if [ ! -d "roles/DearSister" ]; then
    echo "Downloading DearSister role..."
    wget https://digital-public.obs.cn-east-3.myhuaweicloud.com/duix/digital/model/1719194007931/bendi1_0329.zip
    unzip bendi1_0329.zip
    mv bendi1_0329 roles/DearSister
    rm bendi1_0329.zip # Clean up
else
    echo "DearSister role already exists."
fi

# --- 5. RUN THE SERVER ---
echo "Starting the server..."
# Change into the build directory to run the compiled executable.
cd build
./ws_server

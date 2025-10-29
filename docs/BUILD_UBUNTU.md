# Build Guide for Ubuntu

## Requirements
bash
Skopiuj kod
sudo apt-get update
sudo apt-get install -y build-essential git cmake ninja-build zlib1g-dev libsodium-dev
Build
bash
Skopiuj kod
cp assembly/native/build-ubuntu-shared.sh .
chmod +x build-ubuntu-shared.sh
./build-ubuntu-shared.sh

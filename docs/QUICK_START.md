# TON Quick Start Guide

To build TON on Ubuntu 22.04:

```bash
sudo apt-get update
sudo apt-get install -y build-essential git cmake ninja-build libsodium-dev
cp assembly/native/build-ubuntu-shared.sh .
chmod +x build-ubuntu-shared.sh
./build-ubuntu-shared.sh

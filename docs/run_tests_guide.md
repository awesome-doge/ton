# TON Build & Test Guide

This guide describes how to build and test TON locally on Ubuntu or macOS.

## Build the project (Ubuntu 22.04+)
```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build zlib1g-dev libsodium-dev
cp assembly/native/build-ubuntu-shared.sh .
chmod +x build-ubuntu-shared.sh
./build-ubuntu-shared.sh

# TON Quick Build Guide

This short guide explains how to build TON from source on Ubuntu, macOS and Windows.

---

## 🐧 Ubuntu 20.04–24.04

Install required packages:
```bash
sudo apt-get update
sudo apt-get install -y build-essential git cmake ninja-build zlib1g-dev libsecp256k1-dev libmicrohttpd-dev libsodium-dev
wget https://apt.llvm.org/llvm.sh
chmod +x llvm.sh
sudo ./llvm.sh 16 all

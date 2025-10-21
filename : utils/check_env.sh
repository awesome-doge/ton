#!/bin/bash
# Simple environment check for TON build

echo "Checking TON build environment..."

command -v cmake >/dev/null 2>&1 || { echo "CMake not found."; exit 1; }
command -v ninja >/dev/null 2>&1 || { echo "Ninja not found."; exit 1; }
command -v clang >/dev/null 2>&1 || { echo "Clang not found."; exit 1; }

echo "All required tools are installed."

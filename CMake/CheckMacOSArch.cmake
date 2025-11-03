# CMake/CheckMacOSArch.cmake
# Utility script to enforce correct architecture flags when building TON on macOS
# Author: Marta Nowak
# Date: 2025-11-01
#
# This module checks the detected processor architecture and ensures
# that both Intel (x86_64) and Apple Silicon (arm64) are correctly set.
# Prevents build failures when cross-compiling dependencies like libsodium or secp256k1.

if(APPLE)
    execute_process(COMMAND uname -m OUTPUT_VARIABLE ARCH OUTPUT_STRIP_TRAILING_WHITESPACE)
    message(STATUS "Detected macOS architecture: ${ARCH}")

    if(NOT CMAKE_OSX_ARCHITECTURES)
        if(ARCH STREQUAL "arm64")
            set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "Build architecture" FORCE)
        else()
            set(CMAKE_OSX_ARCHITECTURES "x86_64" CACHE STRING "Build architecture" FORCE)
        endif()
    endif()

    message(STATUS "CMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}")
endif()

#!/bin/bash
BUILD_TYPE=${1:-Debug}
TARGET=${2:-linux}  # linux or windows

cd $(pwd)

if [ "$TARGET" = "windows" ]; then
    cmake -S . \
        -D CMAKE_BUILD_TYPE=$BUILD_TYPE \
        -D CMAKE_TOOLCHAIN_FILE=cmake/windows-mingw.cmake \
        -G Ninja \
        -B build-windows && \
    cmake --build build-windows --parallel
else
    cmake -S . \
        -D CMAKE_BUILD_TYPE=$BUILD_TYPE \
        -G Ninja \
        -B build && \
    cmake --build build --parallel
fi
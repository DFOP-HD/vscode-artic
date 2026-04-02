#!/bin/bash
BUILD_TYPE=${1:-Debug}

cd $(pwd)

cmake -S . \
    -D CMAKE_BUILD_TYPE=$BUILD_TYPE \
    -G Ninja \
    -B build && \
cmake --build build --parallel

exit


cmake -S . -D CMAKE_BUILD_TYPE=$BUILD_TYPE-G Ninja -B build && cmake --build build --parallel
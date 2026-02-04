#!/bin/bash
BUILD_TYPE=${1:-Debug}
cd $(pwd)
cmake -S . -D CMAKE_BUILD_TYPE=$BUILD_TYPE -D CMAKE_CXX_COMPILER=/usr/bin/clang++ -G Ninja -B build && cmake --build build --parallel
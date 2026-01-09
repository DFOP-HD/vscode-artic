#!/bin/bash
cd $(pwd)
cmake -S . -D CMAKE_CXX_COMPILER=/usr/bin/clang++ -G Ninja -B build && cmake --build build --parallel
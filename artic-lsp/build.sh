#!/bin/bash
cd $(pwd)
cmake -S . -D CMAKE_CXX_COMPILER=/usr/bin/clang++ -D CMAKE_EXPORT_COMPILE_COMMANDS=ON -G Ninja -B build && cmake --build build --parallel
#!/bin/bash
# Configures and builds the language server (and the standalone `artic` compiler) with
# Ninja. Usage: ./build.sh [Debug|Release]
set -e

BUILD_TYPE=${1:-Debug}

# Resolve relative to this script rather than the caller's working directory: package.sh
# and build-lsp.sh both invoke it from elsewhere.
cd "$(cd "$(dirname "$0")" && pwd)"

cmake -S . -B build -G Ninja -D CMAKE_BUILD_TYPE="$BUILD_TYPE"
cmake --build build --parallel

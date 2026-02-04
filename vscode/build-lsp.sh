#!/bin/bash
set -e

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
# Navigate to artic directory
cd "$SCRIPT_DIR/../artic-lsp"
./build.sh Release
mkdir -p "$SCRIPT_DIR/build/bin"
cp -f "build/bin/artic-lsp" "$SCRIPT_DIR/build/bin/artic-lsp"

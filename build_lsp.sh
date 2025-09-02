#!/bin/bash
set -e

echo "Building Artic Language Server..."
echo "=================================="

# Navigate to artic directory
cd "$(dirname "$0")/artic"

# Create build directory
mkdir -p build
cd build

# Configure with LSP support
echo "Configuring CMake with LSP support..."
cmake \
  .. \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=1 \
  -DARTIC_BUILD_LSP=ON \
  -DCMAKE_BUILD_TYPE=Debug \
  -DThorin_DIR=$HOME/repos/upstream-anydsl/thorin/build/share/anydsl/cmake

# Build
echo "Building..."
make -j$(nproc)

echo ""
echo "Build complete!"
echo "Binary location: $(pwd)/bin/artic"
echo ""

echo ""
echo "You can now test the language server with:"
echo "  ./bin/artic --lsp"
echo ""
echo "Or use it as a regular compiler:"
echo "  ./bin/artic <file.art>"

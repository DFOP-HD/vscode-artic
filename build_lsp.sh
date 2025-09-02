#!/bin/bash
set -e

# Navigate to artic directory
cd "$(dirname "$0")/artic-lsp" && ./build.sh

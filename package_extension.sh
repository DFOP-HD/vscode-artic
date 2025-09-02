#!/usr/bin/env bash

# Package the Artic VS Code extension with a bundled Artic LSP binary
# - Builds the C++ language server
# - Copies the binary into the extension under vscode-client/artic/build/bin/
# - Compiles the extension
# - Packages a .vsix using vsce

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "$0")" && pwd)
EXT_DIR="$ROOT_DIR"
ARTIC_BIN_SRC="$ROOT_DIR/artic/build/bin/artic"
ARTIC_BIN_DST_DIR="$EXT_DIR/artic/build/bin"
ARTIC_BIN_DST="$ARTIC_BIN_DST_DIR/artic"

echo "==> Building Artic LSP server"
"$ROOT_DIR/build_lsp.sh"

if [[ ! -x "$ARTIC_BIN_SRC" ]]; then
  echo "Error: Built Artic binary not found at $ARTIC_BIN_SRC"
  exit 1
fi

echo "==> Preparing extension bundle location: $ARTIC_BIN_DST"
mkdir -p "$ARTIC_BIN_DST_DIR"
cp -f "$ARTIC_BIN_SRC" "$ARTIC_BIN_DST"
chmod +x "$ARTIC_BIN_DST"

echo "==> Installing extension dependencies"
pushd "$EXT_DIR" >/dev/null
# Use npm install to update lockfile if needed (ci would fail without matching lock)
npm install

echo "==> Compiling extension"
npm run compile

echo "==> Packaging extension (.vsix)"
npm run package

VSIX_FILE=$(ls -1 artic-language-server-*.vsix | tail -n 1 || true)
popd >/dev/null

echo ""
if [[ -n "${VSIX_FILE:-}" ]]; then
  echo "✅ Package created: $EXT_DIR/$VSIX_FILE"
  echo "To install locally: code --install-extension \"$EXT_DIR/$VSIX_FILE\""
else
  echo "⚠ Could not detect output .vsix. Check the packaging logs above."
fi

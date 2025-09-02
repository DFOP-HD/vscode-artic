#!/usr/bin/env bash

# Package the Artic VS Code extension with a bundled Artic LSP binary
# - Builds the C++ language server
# - Copies the binary into the extension under vscode-client/artic-lsp/build/bin/
# - Compiles the extension
# - Packages a .vsix using vsce

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "$0")" && pwd)
EXT_DIR="$ROOT_DIR"
ARTIC_BIN="$ROOT_DIR/artic-lsp/build/bin/artic"

echo "==> Building Artic LSP server"
"$ROOT_DIR/build_lsp.sh"

if [[ ! -x "$ARTIC_BIN" ]]; then
  echo "Error: Built Artic binary not found at $ARTIC_BIN"
  exit 1
fi

echo "==> Preparing extension bundle location: $ARTIC_BIN"
chmod +x "$ARTIC_BIN"

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

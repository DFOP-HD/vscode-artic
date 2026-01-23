#!/usr/bin/env bash

# Package the Artic VS Code extension with a bundled Artic LSP binary
#!/usr/bin/env bash

# Package the Artic VS Code extension with a bundled Artic LSP binary
# - Builds the C++ language server
# - Copies the binary into the extension under vscode-client/artic-lsp/build/bin/
# - Compiles the extension
# - Packages a .vsix using vsce

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "$0")" && pwd)
EXT_DIR="$ROOT_DIR"
ARTIC_BIN="$ROOT_DIR/../artic-lsp/build/bin/artic"

echo "==> Building Artic LSP server"
"$ROOT_DIR/build-lsp.sh"

if [[ ! -x "$ARTIC_BIN" ]]; then
  echo "Error: Built Artic binary not found at $ARTIC_BIN"
  exit 1
fi

echo "==> Preparing extension bundle location: $ARTIC_BIN"
chmod +x "$ARTIC_BIN"

mkdir -p "$EXT_DIR/build/bin"
cp "$ARTIC_BIN" "$EXT_DIR/build/bin/artic"

echo "==> Auditing and fixing vulnerabilities"
npm audit fix

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

# Optional install step
if [[ "${1:-}" == "install" ]]; then
  VSIX_FILE=$(ls *.vsix | tail -n1)
  if [[ -f "$VSIX_FILE" ]]; then
    echo "Installing extension: $VSIX_FILE"
    code --install-extension "$VSIX_FILE"
  else
    echo "VSIX package not found. Please run the packaging step first."
    exit 1
  fi
fi

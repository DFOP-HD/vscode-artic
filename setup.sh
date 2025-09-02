#!/bin/bash

echo "Setting up Artic VS Code Extension..."
echo "====================================="

# Navigate to the vscode-client directory
cd "$(dirname "$0")"

# Install dependencies
echo "Installing dependencies..."
npm install

# Compile TypeScript
echo "Compiling TypeScript..."
npm run compile

# Check if compilation was successful
if [ $? -eq 0 ]; then
    echo "✓ Extension compiled successfully!"
    echo ""
    echo "To use the extension:"
    echo "1. Open VS Code"
    echo "2. Go to Extensions view (Ctrl+Shift+X)"
    echo "3. Click '...' menu and select 'Install from VSIX...'"
    echo "4. Or copy this folder to your VS Code extensions directory"
    echo ""
    echo "Alternative: Run 'npm run package' to create a VSIX file"
    echo "Then run 'npm run install-local' to install it"
else
    echo "✗ Compilation failed. Please check the errors above."
    exit 1
fi

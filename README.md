# Artic Language Server Extension

This VS Code extension provides language support for the Artic programming language through the Language Server Protocol (LSP).

## Features

- **Syntax Highlighting**: Basic syntax highlighting for `.art` files
- **Language Server Integration**: Connects to the Artic LSP server for:
  - Error diagnostics
  - Code completion (when implemented)
  - Hover information (when implemented)
  - Go to definition (when implemented)

## Installation and Setup

1. **Build the Artic LSP Server**: Make sure you have built the Artic compiler with LSP support:
   ```bash
   cd artic-language-server-plugin
   ./build_lsp.sh
   ```

2. **Install the Extension**: 
   - Copy this folder to your VS Code extensions directory, or
   - Use "Install from VSIX" if you package it as a VSIX file

3. **Configure the Server Path** (Optional):
   - Open VS Code settings
   - Search for "artic"
   - Set `artic.serverPath` to the full path of your `artic` binary if it's not in PATH

## Configuration

### Settings

- `artic.serverPath`: Path to the Artic language server binary
- `artic.trace.server`: Enable tracing of LSP communication for debugging

### Commands

- **Artic: Restart Language Server**: Restart the Artic language server if it stops responding

## Usage

1. Open any `.art` file in VS Code
2. The extension will automatically start the Artic language server
3. You should see syntax highlighting and error diagnostics (if any)

## Development

To develop this extension:

```bash
cd vscode-client
npm install
npm run compile
# Or for watch mode:
npm run watch
```

Press F5 in VS Code to launch the extension in a new Extension Development Host window.

## Troubleshooting

- **Server not starting**: Check that the `artic` binary is accessible and built with LSP support
- **No syntax highlighting**: Verify that files have the `.art` extension
- **Server crashes**: Check the "Artic Language Server" output channel in VS Code for error messages

## Requirements

- VS Code 1.75.0 or higher
- Artic compiler built with LSP support (`artic --lsp` should work)

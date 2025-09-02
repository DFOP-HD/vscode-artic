
# Artic Language Server VS Code Extension

This extension adds language support for the Impala programming language in Visual Studio Code, including syntax highlighting and LSP-based features such as diagnostics and go-to-definition.

## Features

- Syntax highlighting for `.art` and `.impala` files
- Language Server Protocol (LSP) integration with the Artic compiler
- Error diagnostics, go to definition, and more

## Installation

### 1. Build the Artic LSP Server

You must build the Artic compiler with LSP support. From the root of this repository:

```bash
cd artic-lsp
./build.sh
```

This will build the `artic` binary with LSP support and place it in `artic-lsp/build/bin/artic`.

### 2. Package the Extension

To build and package the VS Code extension as a `.vsix` file:

```bash
./package_extension.sh
```

This will compile the TypeScript sources and create a `.vsix` package in the current directory.

### 3. Install the Extension

In VS Code, run `Extensions: Install from VSIX...` and select the generated `.vsix` file, or run:

```bash
code --install-extension artic-language-server-*.vsix
```

## Configuration

Open VS Code settings and search for "artic":

- `artic.serverPath`: Path to the Artic language server binary. Leave empty to use `artic` from your `PATH`.
- `artic.trace.server`: Set the trace level for LSP communication (`off`, `messages`, `verbose`).

## Usage

1. Open a `.art` file in VS Code.
2. The extension will automatically start the Artic language server.
3. You will see syntax highlighting and LSP features such as diagnostics.

## Development

To develop or modify the extension:

```bash
npm install
npm run compile
# Or for watch mode:

```

To test the extension, press F5 in VS Code to open a new Extension Development Host window.

## Troubleshooting

- **Server not starting**: Ensure the `artic` binary is built and accessible, and has LSP support (`artic --lsp`).
- **No syntax highlighting**: Make sure your files have the `.art` extension.
- **Server crashes**: Check the "Artic Language Server" output channel in VS Code for error messages.

## Requirements

- VS Code 1.75.0 or higher
- Artic compiler built with LSP support (`artic --lsp` should work)

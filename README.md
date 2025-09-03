
# Artic Language Server VS Code Extension

This extension adds language support for the Impala programming language in Visual Studio Code

## Features

- Syntax highlighting for `.art` and `.impala` files
- Language Server Protocol (LSP) integration with the Artic compiler
- Error diagnostics
- Go to definition for functions / structs (currently only works when there are no errors your files)

## Dependencies

- Thorin (could be possible to remove this in the future, but that would require larger changes to artic)
  - You may need to adjust the path to thorin in `build.sh`

## Installation

### Build the Artic LSP Server

Build the Artic compiler with LSP support:

```bash
cd artic-lsp
./build.sh
```

### Build and Package the Extension

To build Artic and package the VS Code extension as a `.vsix` file:

```bash
./package.sh
```

If you also want to immediately install the extension, use:

```bash
./package.sh install
```

### Install the Extension

In VS Code, run `Extensions: Install from VSIX...` and select the generated `.vsix` file, or run:

```bash
code --install-extension artic-language-server-<version>.vsix
```

## Usage

1. Open a `.art` file in VS Code.
2. The extension will automatically start the Artic language server.
3. You will see syntax highlighting and LSP features such as diagnostics.

## Development

To test the extension, press F5 in VS Code to open a new Extension Development Host window.

## Troubleshooting

- **Server not starting**: Ensure the `artic` binary is built and accessible, and has LSP support (`artic --lsp`).
- **No syntax highlighting**: Make sure your files have the `.art` extension.
- **Server crashes**: Check the "Artic Language Server" output channel in VS Code for error messages.

## Requirements

- VS Code 1.75.0 or higher
- Artic compiler built with LSP support (`artic --lsp` should work)

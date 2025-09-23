
# AnyDSL - Artic Language Server

Visual Studio Code Language support for AnyDSL's Impala programming language.\
The language server is based on a fork of the Artic compiler frontend and continuously compiles your code as you write it.

Note: The language server is currently in an alpha stage and only supports a limited feature set. Please report any technical or non-technical problems.

## Features

- Syntax highlighting
- Diagnostics (errors, warnings, hints)
- Go to definition (for variables, functions, structs, ...)

## Limitations

- Only supports x86_64 Linux
- Does not support the legacy Impala syntax
- Go to definition feature:
    - Not available when your code has syntax errors
    - Not availalbe for projection expressions (e.g. `my_struct.field` -> cannot find definition for `field`)
    - Not available for path expressions (e.g. `my_module::func()` -> goes to definition of `my_module`)

## Usage

1. [Install extension](#installation)
2. Open a `.impala` or `.art` file in VS Code.
    - The extension will automatically start the Artic language server.
    - You will see syntax highlighting and LSP features such as diagnostics.
3. Create a [workspace configuration file](#workspace-configuration-file) `artic.json`
4. Create a [global configuration file](#global-configuration-file) `artic-global.json`

## Installation

1. Download the latest release of the extension [here](https://github.com/DFOP-HD/vscode-artic/releases).

2. Install the extension using one of these options:
    - a) Open the command palette in VS Code with `Ctrl+P`, select `Extensions: Install from VSIX...` and then select the downloaded `.vsix` file.
    - b) Run this command in the terminal: `code --install-extension artic-language-server-<version>.vsix`

## Workspace Configuration File

Create a workspace configuration file `artic.json` at the root of your workspace.

This configuration file tells the language server which files are associated with your project and should therefore be compiled together.
This is essential to give you good diagnostics and 'go to definition' functionality

Example:

```json
{
    // configuration header (includes version in case the config file format changes in the future)
    "artic-config": "1.0",

    // all projects defined in this workspace
    "projects": [ 
        {
            "name": "my project",     // name of the project (must be unique)
            "folder": "",             // root folder of the project (optional, defaults to location of the configuration file)
            "dependencies": [
                "anydsl.runtime",     // include all files of the project 'anydsl.runtime'     (and it's dependencies)
                "anydsl.artic-utils"  // include all files of the project 'anydsl.artic-utils' (and it's dependencies)
            ],
            "files": [
                "/home/gruen/absolute.art", // include single file (absolute path)
                "relative.art",             // include single file (relative to project folder)
                "!src/exclude.impala",      // exclude file(s) with '!' prefix
                "wi?d*rd.art",              // wildcard '?' substitute a single character, '*' substitutes multiple characters
                "**/*.impala"               // include files recursively with '**'
            ]
        }
    ],

    // recursively include projects from other configuration files (paths do not support wildcards)
    "include": [
        // include projects from global config 'artic-global.json' (path specified in extension settings). 
        // also active even when "<global>" is not explicitly specified
        "<global>",                           

        // mark include as optional with '?' postfix 
        // (useful as a fallback for projects assumed to be included by 'artic-global.json') 
        "~/repos/anydsl/optional/artic.json?" 

        "../anydsl/runtime/artic.json",       // here: defines project anydsl.runtime
        "../anydsl/artic-utils/artic.json",   // here: defines project anydsl.artic-utils
    ],
    
    // default project (usually only defined in 'artic-global.json'):
    // When you open a file that does not belong to any known project 
    // (i.e. projects that are defined or recursively included in your global or workspace config),
    // the language server will compile that file along with the files of the default project
    "default-project": {
        "name": "default project",
        "dependencies": [
            "anydsl.runtime" // usually includes default dependencies like the runtime library
        ],
        "files": []
    },
}
```

## Global Configuration File
Create a global configuration file `artic-global.json` (e.g. in `HOME`) and specify the path to the file in the extension settings

Example:

```json
{
    "artic-config": "1.0",

    // the global configuration file typically includes a default project definition
    "default-project": {
        "name": "default project",
        "dependencies": [
            "anydsl.runtime"
        ],
        "files": []
    },

    // defined projects are globally available
    "projects": [],

    // included projects are globally available
    "include": [
        "repos/anydsl/runtime/artic.json" // here: defines project 'anydsl.runtime'
    ]
}
```

## Hints
The language server will also provide information and diagnostics for your configuration files.

Example:






![config diagnostics](docs/media/config.png)






# Development

## Repository

vscode-artic
| artic-lsp                   // Language server - fork of artic - c++
| src
| | extension.ts              // Language client (vscode)
| syntaxes
| | artic.tmGrammar.json      // TextMate grammar for syntax highlighting
| language-configuration.json // Brackets and indentation rules
|
| build_lsp.sh                // builds artic-lsp
| package.sh                  // builds artic-lsp and extension, packages the extension
| publish.sh                  // builds and packages everything, publishes a new release (internal)
|
| LICENSE.md                  
| README.md                   

## Build Requirements

- Clang compiler
- AnyDSL (specifically Thorin)
  - Set the path to thorin in `artic-lsp/build.sh`
  - Note: Not directly used by the language server, could be removed in the future, but that would require larger changes to artic

## Build and Start Extension Development Host

Open the project in vscode and press `F5`

## Build artic-lsp

Build the Artic compiler with LSP support:

```bash
cd ./build_lsp.sh
```
or
```bash
cd artic-lsp && ./build.sh
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
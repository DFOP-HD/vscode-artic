# AnyDSL - Artic Language Server

Visual Studio Code language support for [AnyDSL](https://anydsl.github.io/)'s Artic language.

The server is built on a fork of the Artic compiler frontend and recompiles your project as you
type, so diagnostics and navigation come from the real type checker rather than from a heuristic
parser.

Note: The language server is currently in an alpha stage. Please report any technical or
non-technical problems.

![demo](docs/media/demo.gif)

## Features

- Diagnostics (errors, warnings and hints) from the real lexer, parser, name binder and type checker
- Hover: declaration signature and inferred type
- Go to definition and find references
- Go to type definition
- Document highlight: every occurrence of the symbol under the cursor
- Document symbols: outline view, breadcrumbs and Ctrl+Shift+O
- Expand and shrink selection along the syntax tree (Shift+Alt+Right / Left)
- Rename
- Code completion for the symbols in scope
- Signature help while typing a call
- Inlay hints for inferred types
- Semantic highlighting on top of the TextMate grammar
- Snippets for loops, function declarations and other common constructs
- Diagnostics for the `artic.json` configuration file itself

## Limitations

- x86_64 only
- The legacy Impala syntax is not supported, even in `.impala` files
- The workspace configuration file must be in the root workspace folder
- `artic.json` is plain JSON: comments and trailing commas are rejected
- Releases contain the Linux and Windows server binaries; on any other platform, build the server
  from source and point `artic.serverPath` at it.

## Usage

1. [Install the extension](#installation).
2. Open a `.art` or `.impala` file. The extension starts the language server automatically.
3. Create a [workspace configuration file](#workspace-configuration-file) so the server knows which
   files belong together. Without one, a file is compiled on its own and everything it expects from
   another file is reported as unknown.

## Installation

1. Download the latest release [from GitHub](https://github.com/DFOP-HD/vscode-artic/releases).
2. Install the `.vsix`:
    - VS Code: `Ctrl+Shift+P`, then `Extensions: Install from VSIX...`, or run
      `code --install-extension artic-language-server-<version>.vsix`
    - Cursor: `cursor --install-extension artic-language-server-<version>.vsix`

The extension is not published to the Visual Studio Marketplace or to Open VSX, so it cannot be
installed from the extensions panel.

## Commands

| Command | Description |
| ------- | ----------- |
| `Artic: Restart Artic Language Server` | Restarts the server process. |
| `Artic: Detect workspace configuration` | Scans the workspace for `.sln`, `build.ninja` and `.vcxproj` files that invoke artic and adds them to `artic.json`. |

## Settings

| Setting | Default | Description |
| ------- | ------- | ----------- |
| `artic.serverPath` | `""` | Path to the `artic-lsp` binary. When empty, the bundled binary is used, falling back to `artic-lsp` on `PATH`. |
| `artic.trace.server` | `"off"` | Traces the communication between the editor and the server. One of `off`, `messages`, `verbose`. |

## Workspace Configuration File

Create `artic.json` in the root of your workspace. It tells the language server which files belong
to which project and are therefore compiled together, which is what makes diagnostics and
go-to-definition correct across file boundaries.

If your project is already built with CMake, run **Artic: Detect workspace configuration** from the
command palette. It scans for `.sln`, `build.ninja` and `.vcxproj` files that invoke artic and adds
them as optional includes, so the configuration stays valid before the first build.

```json
{
    "artic-config": "2.0",
    "projects": [
        {
            "name": "my_project",
            "folder": "",
            "dependencies": ["runtime", "artic-utils"],
            "files": [
                "src/**/*.art",
                "!src/experimental.art"
            ]
        }
    ],
    "include": [
        "../anydsl/runtime/artic.json",
        "../anydsl/artic-utils/artic.json",
        "../build/build.ninja?"
    ],
    "default-project": {
        "name": "default project",
        "dependencies": ["runtime"],
        "files": []
    }
}
```

### Top-level keys

| Key | Description |
| --- | ----------- |
| `artic-config` | Format version. Required, and must be `"2.0"`. |
| `projects` | The projects defined by this file. |
| `include` | Other configuration or build files to take projects from. Paths do not support wildcards. |
| `default-project` | Compiled together with any file that belongs to no known project. |

### Project keys

| Key | Description |
| --- | ----------- |
| `name` | Unique project name. Required. |
| `folder` | Project root. Optional; defaults to the directory containing the configuration file. |
| `dependencies` | Names of other projects whose files are compiled together with this one, transitively. |
| `files` | File patterns, relative to `folder` unless absolute. |

File patterns support `?` for a single character, `*` for multiple characters and `**` for
recursion. A `!` prefix excludes matches, for example `"!src/exclude.impala"`.

### Include entries

| Entry | Effect |
| ----- | ------ |
| `../other/artic.json` | Adds every project defined in that configuration file. |
| `../build/my_kernel.vcxproj` | A Visual Studio project. Its source files are taken from the `artic.exe` build command. |
| `../build/anydsl.sln` | A whole solution. Expands to the `.vcxproj` files it lists; projects that do not invoke artic are skipped silently. |
| `../build/build.ninja` | A ninja build file. Every target whose command line invokes artic becomes a project named after the file it generates. |
| `../build/build.ninja?` | A trailing `?` marks the include optional, which is useful for a build directory that does not exist on a fresh checkout. An optional include that exists but is broken is still reported. |

Earlier versions supported a separate global configuration file, referenced as `"<global>"`. It has
been removed; use `include` instead.

## Configuration diagnostics

The configuration files are validated as well, and problems are reported in the same place as
source errors.

![config diagnostics](docs/media/config.png)

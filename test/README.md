# Tests

Black-box tests that drive the real `artic-lsp` binary over stdio with a
dependency-free LSP client, so they exercise the protocol surface that ships.

## Running

```powershell
node --test 'test/*.test.mjs'
```

Node >= 20 is required; there are no npm dependencies.

The binary is discovered automatically in `artic-lsp/build*/bin`. Override with:

```powershell
$env:ARTIC_LSP_BIN = 'path/to/artic-lsp.exe'   # language server
$env:ARTIC_BIN     = 'path/to/artic.exe'       # compiler, for fixture validation
```

Build them first — see [AGENTS.md](../AGENTS.md):

```powershell
cmake --build artic-lsp/buildGcc --parallel
```

Also run the artic compiler's own suite when touching the submodule:

```powershell
ctest --test-dir artic-lsp/buildGcc -E "^thorin_"
```

## Layout

| Path | Purpose |
| ---- | ------- |
| `lsp-client.mjs` | Minimal JSON-RPC/LSP client. Handles framing, requests, notifications and URI normalisation. |
| `helpers.mjs` | Binary discovery, staging of fixtures into temp directories, and `locate()` for turning source text into LSP positions. |
| `fixtures/` | Self-authored `.art` sources and `artic.json` configs. |
| `server-lifecycle.test.mjs` | Handshake and advertised capabilities. |
| `source-diagnostics.test.mjs` | Diagnostics for `.art` sources: position, URI shape, attribution, clearing. |
| `config-diagnostics.test.mjs` | Diagnostics for `artic.json` / `.artic-lsp`. |
| `language-features.test.mjs` | Semantic tokens, inlay hints, go-to-definition, find-references. |
| `hover.test.mjs` | `textDocument/hover`: the rendering of every declaration kind, the reported range, and the null cases. |
| `path-identity.test.mjs` | The same features when the file is reached through a `.vcxproj` that spells the path differently from the editor. |
| `sln-config.test.mjs` | `.sln` files listed in `include`, including the noise a CMake-generated solution brings with it. |
| `ninja-config.test.mjs` | `build.ninja` files listed in `include`: artic targets become projects, other custom commands are ignored. |
| `optional-includes.test.mjs` | Includes marked with a trailing `?`: absent is fine, broken is not, and the `?` is not part of the path. |
| `detect-config.test.mjs` | Which build files the "Detect workspace configuration" command writes into `artic.json`. Bundles `vscode/src/detect.ts` with esbuild, so it needs no VS Code instance. |
| `server-path.test.mjs` | How the extension picks the server binary: the `artic.serverPath` setting, the bundled binary, then `PATH`. Bundles `vscode/src/server-path.ts` with esbuild. |
| `fixtures.test.mjs` | Compiles the fixtures with the real `artic` binary. |

Fixtures are copied to a temp directory before each suite, so tests may freely
edit config files and sources without dirtying the repository.

## Writing tests

`waitForDiagnostics(uri)` resolves when the server publishes diagnostics for a
document. To assert something is *not* published, use `settle()` and then check
`diagnosticsFor(uri)`.

URIs must be compared with `normalizeUri` (or via `diagnosticsFor`): the server
emits VS Code's encoding (`file:///C%3A/...`) while Node produces `file:///C:/...`.

Semantic tokens and inlay hints deliberately refuse to trigger a compile, so a
document must be opened *and* its diagnostics awaited before requesting them —
otherwise the server legitimately answers `null` and the test proves nothing.

Do not hard-code line/column numbers. Use `locate(text, 'fn dot')`, which keeps a
test readable and stops it rotting when a fixture gains a comment line.

## Fixture rules

All `.art` / `.impala` fixture code **must be written by us**. Never copy sources
from other AnyDSL repositories — read them for reference only.

Every fixture that is meant to be valid must be proven valid with the real
compiler; `fixtures.test.mjs` enforces this. Negative fixtures must fail with
exactly the diagnostic being asserted.

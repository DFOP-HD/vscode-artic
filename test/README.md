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
| `config-hints.test.mjs` | Inlay hints on `artic.json`: per-project and per-pattern file counts, and that a healthy config stays out of the Problems panel. |
| `config-positions.test.mjs` | Where a config diagnostic or hint lands when the value it is about occurs more than once in the document, and where a JSON syntax error lands. |
| `language-features.test.mjs` | Semantic tokens, type and parameter inlay hints, go-to-definition, find-references. |
| `completion.test.mjs` | `textDocument/completion`: field and enum-option projection, the `detail` of a generic function, local-scope visibility, and module paths. |
| `signature-help.test.mjs` | `textDocument/signatureHelp`: the rendered label, the parameter spans inside it, the active parameter, and the half-written calls that never reach the AST. |
| `hover.test.mjs` | `textDocument/hover`: the rendering of every declaration kind, the reported range, and the null cases. |
| `document-symbols.test.mjs` | `textDocument/documentSymbol`: the outline tree, the symbol kind of each declaration kind, and the two ranges. |
| `workspace-symbols.test.mjs` | `workspace/symbol` across every project in the config, and the reference-count code lens. |
| `navigation.test.mjs` | `textDocument/definition` (on a declaration as well as a reference), `documentHighlight`, `typeDefinition`, `implementation` and `selectionRange`. |
| `incomplete-code.test.mjs` | The same features while the buffer does not compile: go-to-definition and completion on a half-written call, and the outline, hover and semantic tokens of a partially parsed file. |
| `latency.test.mjs` | How long the requests an editor issues while typing take, against a generated multi-file project. |
| `did-close.test.mjs` | `textDocument/didClose`: the closed document's diagnostics are withdrawn and its unsaved buffer is discarded. |
| `path-identity.test.mjs` | The same features when the file is reached through a `.vcxproj` that spells the path differently from the editor. |
| `sln-config.test.mjs` | `.sln` files listed in `include`, including the noise a CMake-generated solution brings with it. |
| `ninja-config.test.mjs` | `build.ninja` files listed in `include`: artic targets become projects, other custom commands are ignored. |
| `optional-includes.test.mjs` | Includes marked with a trailing `?`: absent is fine, broken is not, and the `?` is not part of the path. |
| `paths-with-spaces.test.mjs` | Build commands that quote a source path containing a space, in both `build.ninja` and `.vcxproj`, including the `cmd.exe /C "..."` wrapper and XML-escaped quotes. |
| `circular-dependencies.test.mjs` | Cycles between projects are reported once and then broken, so the projects still compile. |
| `project-provenance.test.mjs` | The `artic.projectForFile` command: which project a file is compiled in, and whether that came from a config, a `default-project` or the single-file fallback. |
| `project-status.test.mjs` | How that answer is worded in the status bar. Bundles `vscode/src/project-status.ts` with esbuild. |
| `zero-config.test.mjs` | A workspace with build files but no `artic.json`: the sources still compile together, a config the user wrote wins over a detected build file, and the scan does not descend into hidden or output directories. |
| `detect-config.test.mjs` | Which build files the "Detect workspace configuration" command writes into `artic.json`. Bundles `vscode/src/detect.ts` with esbuild, so it needs no VS Code instance. |
| `server-path.test.mjs` | How the extension picks the server binary: the `artic.serverPath` setting, the bundled binary, then `PATH`. Bundles `vscode/src/server-path.ts` with esbuild. |
| `fixtures.test.mjs` | Compiles the fixtures with the real `artic` binary. |

Fixtures are copied to a temp directory before each suite, so tests may freely
edit config files and sources without dirtying the repository.

## Writing tests

`waitForDiagnostics(uri)` resolves when the server publishes diagnostics for a
document. To assert something is *not* published, use `settle()` and then check
`diagnosticsFor(uri)`.

One action can publish for a document **more than once** — closing a file with
unsaved edits recompiles the project first and withdraws the diagnostics after.
`waitForDiagnostics()` resolves on the *first* of those and would assert against
an intermediate state, so anything that reads a final value must `settle()` and
then use `diagnosticsFor(uri)`, which holds the latest.

URIs must be compared with `normalizeUri` (or via `diagnosticsFor`): the server
emits VS Code's encoding (`file:///C%3A/...`) while Node produces `file:///C:/...`.

Semantic tokens and inlay hints deliberately refuse to trigger a compile, so a
document must be opened *and* its diagnostics awaited before requesting them —
otherwise the server legitimately answers `null` and the test proves nothing.

Do not hard-code line/column numbers. Use `locate(text, 'fn dot')`, which keeps a
test readable and stops it rotting when a fixture gains a comment line.

The server keeps **one project registry per session** and ignores a duplicate
project name with a warning. Tests in the same suite that stage different
workspaces must therefore use distinct project names, or the second workspace's
projects silently do not exist.

A config diagnostic parsed out of a JSON document is published **once**, at the
exact value it is about — nlohmann records a range for every parsed value. Only a
message from a build file, which has no positions, falls back to a text search and
can therefore appear at several textually identical strings.

### Testing against code that does not compile

An editor spends most of its time on source that is mid-edit, so a feature that
only works on valid code is broken in practice. Drive the broken state through
`changeDocument()` rather than committing an invalid fixture — the fixture stays
valid (and therefore stays covered by `fixtures.test.mjs`), and the test shows
exactly which keystroke it is simulating.

Such a suite must **assert that the buffer really is broken**, otherwise it can
silently degrade into a second copy of the valid-source tests.

### Latency

`latency.test.mjs` is a guard, not a benchmark. Absolute budgets are loose enough
that only an order-of-magnitude regression trips them; the assertion with teeth is
the relative one, comparing a warm request against a `didChange` round trip
**measured in the same run**. A request answered from the last compile must not
recompile, and that ratio holds on any machine. `ARTIC_LSP_WARM_BUDGET_MS` and
`ARTIC_LSP_EDIT_BUDGET_MS` raise the absolute ceilings if a runner needs it.

## Fixture rules

All `.art` / `.impala` fixture code **must be written by us**. Never copy sources
from other AnyDSL repositories — read them for reference only.

Every fixture that is meant to be valid must be proven valid with the real
compiler; `fixtures.test.mjs` enforces this. Negative fixtures must fail with
exactly the diagnostic being asserted.

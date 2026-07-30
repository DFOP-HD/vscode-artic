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
cmake --build artic-lsp/buildGcc --target artic-lsp artic --parallel
```

Also run the artic compiler's own suite when touching the submodule:

```powershell
ctest --test-dir artic-lsp/buildGcc -E "^thorin_"
```

## Layout

| Path | Purpose |
| ---- | ------- |
| `lsp-client.mjs` | Minimal JSON-RPC/LSP client. Handles framing, requests, notifications and URI normalisation. |
| `helpers.mjs` | Binary discovery and staging of fixtures into temp directories. |
| `fixtures/` | Self-authored `.art` sources and `artic.json` configs. |
| `*.test.mjs` | The suites. |

Fixtures are copied to a temp directory before each suite, so tests may freely
edit config files and sources without dirtying the repository.

## Writing tests

`waitForDiagnostics(uri)` resolves when the server publishes diagnostics for a
document. To assert something is *not* published, use `settle()` and then check
`diagnosticsFor(uri)`.

URIs must be compared with `normalizeUri` (or via `diagnosticsFor`): the server
emits VS Code's encoding (`file:///C%3A/...`) while Node produces `file:///C:/...`.

## Fixture rules

All `.art` / `.impala` fixture code **must be written by us**. Never copy sources
from other AnyDSL repositories — read them for reference only.

Every fixture that is meant to be valid must be proven valid with the real
compiler; `fixtures.test.mjs` enforces this. Negative fixtures must fail with
exactly the diagnostic being asserted.

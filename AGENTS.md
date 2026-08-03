# AGENTS.md

Working notes for AI agents and new contributors. Keep this file current — it is the
handover document between sessions.

## What this repo is

A VS Code language server for AnyDSL's Artic/Impala language.

| Path | What it is |
| ---- | ---------- |
| `artic/` | **Git submodule** — fork of AnyDSL/artic (`DFOP-HD/artic-with-lsp`). Provides `libartic` (lexer/parser/binder/typechecker) used as a library. |
| `artic-lsp/` | The language server (C++20). Links `libartic` + `lsp-framework`. |
| `vscode/` | The VS Code extension (TypeScript) that launches the server over stdio. |
| `test/` | Automated tests (see [Testing](#testing)). |
| `docs/` | [Implementation notes](docs/implementation-notes.md) — how each feature works and what it has to get right. Feature-level detail belongs there, not here. |

Compile pipeline used by the server: `Lexer -> Parser -> NameBinder -> TypeChecker -> Summoner`,
driven by `Compiler::compile_files()` in [artic-lsp/src/compile.cpp](artic-lsp/src/compile.cpp).

### Inside `artic-lsp/`

`server.cpp` is deliberately one large file — all LSP request handlers live there, grouped
by feature in anonymous namespaces. Everything that is *not* a request handler belongs in
one of the utility modules, so a handler stays readable:

| Module | Namespace | What belongs in it |
| ------ | --------- | ------------------ |
| `paths.h` / `paths.cpp` | `artic::ls::paths` | Everything that turns a path or URI into a file identity: `canonical_path`, `lookup_key`, `to_absolute_path`, `from_msbuild_path`, `read_file`. **The only legitimate source of a lookup key** — see the file-identity gotcha below. |
| `text.h` | `artic::ls::text` | Header-only string helpers: `to_lower`, `trim_left`, `strip_quotes`, `split_whitespace`, `split_command_line`, `quote`. |
| `lsp_convert.h` / `.cpp` | `artic::ls` | Conversions between artic's `Loc`/`Severity`/`Diagnostic` and the `lsp::` protocol types, plus `contains(range, position)`. |
| `ast_render.h` / `.cpp` | `artic::ls` | Turning AST nodes into display strings: `print_to_string`, `print_param_list`, `render_decl`, and the `symbol_kind_of` mapping. Hover, document symbols, workspace symbols, code lens and completion all render through here so they cannot disagree. |
| `symbol_index.{h,cpp}` | `artic::ls` | The parse-only, per-project declaration index behind `workspace/symbol`. |
| `json_source.{h,cpp}` | `artic::ls` | Turns nlohmann's `start_pos()`/`end_pos()` byte offsets into `lsp::Range`, so a config diagnostic or hint can point at the value it is about. Keeps the document text, because that is what the offsets index. |
| `workspace.{h,cpp}` | `artic::ls::workspace` | Config discovery, the project registry, file tracking. |
| `config.{h,cpp}` | `artic::ls::config` | Parsing `artic.json`, `.artic-lsp`, `.vcxproj`, `.sln`, `build.ninja`, the bounded workspace scan behind zero-config projects, and `ConfigLog`. |
| `compile.{h,cpp}` | `artic::ls` | Driving `libartic` and holding the resulting AST, `Locator` and `NameMap`. |

The `artic-lsp` target is built with `-Wall` (`/W3` on MSVC). The flags are scoped to that
target only, because `libartic` and the fetched dependencies are not warning-clean and are
not ours to fix. `-Wno-deprecated-declarations` / `/wd4996` are set because `lsp-framework`
uses `std::codecvt`.

## Build

Dependencies (thorin, lsp-framework, nlohmann_json, half) are fetched by CMake via
`FetchContent` on first configure — the first build is slow, later ones are not.

### Fast local loop (recommended)

No Visual Studio environment needed:

```powershell
cmake -S artic-lsp -B artic-lsp/buildGcc -G Ninja -D CMAKE_BUILD_TYPE=Release `
      -D CMAKE_C_COMPILER=gcc -D CMAKE_CXX_COMPILER=g++
cmake --build artic-lsp/buildGcc --parallel
```

Outputs `artic-lsp/buildGcc/bin/artic-lsp.exe` and `artic-lsp/buildGcc/bin/artic.exe` — the
default target builds both, because the standalone compiler is needed to validate `.art`
fixtures and to run the compiler's own ctest suite. **Do not ask for `artic` by name**
(`--target artic-lsp artic`): the Visual Studio generator cannot resolve a target defined in
a subdirectory and fails with `MSB1009: Project file does not exist. Switch: artic.vcxproj`,
because the project is really at `<build>/artic/src/artic.vcxproj`.

### Verified toolchains

All of these build on Windows x64. Do not reintroduce compiler-specific flags without guards.

| Compiler | Generator |
| -------- | --------- |
| Clang 19 | Ninja |
| MSVC 19.43 | Ninja |
| MSVC 19.43 | Visual Studio 17 2022 |
| GCC 13.2 (MinGW-w64) | Ninja |

### Windows + MSVC/Clang caveat

On this machine VS 2022 does **not** register the Windows SDK: `vcvars64.bat` leaves
`WindowsSdkDir` empty and omits the SDK from `INCLUDE`/`LIB`, so linking fails with
`lld-link: error: could not open 'kernel32.lib'`. The SDK is present at
`C:\Program Files (x86)\Windows Kits\10` (10.0.22621.0). After running `vcvars64.bat`, append:

```powershell
$sdk='C:\Program Files (x86)\Windows Kits\10'; $v='10.0.22621.0'
$env:INCLUDE="$env:INCLUDE;$sdk\Include\$v\ucrt;$sdk\Include\$v\um;$sdk\Include\$v\shared;$sdk\Include\$v\winrt"
$env:LIB="$env:LIB;$sdk\Lib\$v\ucrt\x64;$sdk\Lib\$v\um\x64"
```

The Visual Studio generator (`-G "Visual Studio 17 2022"`) needs none of this — MSBuild
locates the SDK itself.

### Debugging the extension (F5)

`Launch Client` in [.vscode/launch.json](.vscode/launch.json) runs the
`Build Extension with LSP Server` task chain: build the server, `npm install`, `npm run compile`.
The extension loads `vscode/out/extension.js` and launches `vscode/build/bin/artic-lsp[.exe]`,
so both must exist before the extension host starts.

Staging the server is done by [vscode/build-lsp.sh](vscode/build-lsp.sh) on Linux/macOS and
[vscode/build-lsp.ps1](vscode/build-lsp.ps1) on Windows; the latter reuses an existing
`artic-lsp/build*` tree rather than configuring a new one. Do not let the tasks run the `.sh`
on Windows — `bash.exe` there is usually the WSL stub, which silently builds Linux binaries.

**The shell scripts must keep `set -e`.** [artic-lsp/build.sh](artic-lsp/build.sh) once did not,
so a failed CMake exited 0 and `vscode/build-lsp.sh` copied a *stale* binary into the extension:
a VSIX that ships the previous build with no error anywhere.

## Testing

See [test/README.md](test/README.md) for details.

```powershell
node --test 'test/*.test.mjs'                      # LSP protocol suite
ctest --test-dir artic-lsp/buildGcc -E "^thorin_"  # artic compiler suite (145 tests)
```

The LSP tests drive the real `artic-lsp` binary over stdio using a dependency-free client,
so they exercise the shipped protocol surface rather than internals. The CTest suite comes
from the submodule and is the regression guard for any change to the artic fork; it needs
the `artic` target built.

Diagnostics are *not* enough coverage. A regression that broke semantic tokens, inlay hints
and go-to-definition for every `.vcxproj`-derived project shipped unnoticed because only
diagnostics were tested. Any change touching path handling, the workspace or the compile
cache must be covered by [test/language-features.test.mjs](test/language-features.test.mjs)
and [test/path-identity.test.mjs](test/path-identity.test.mjs).

**Valid source is not enough coverage either.** The editor spends most of its time on a
buffer that does not parse, and the compiler is error-tolerant on purpose:
`Compiler::compile_files` keeps the partially parsed AST (`exclude_non_parsed_files` is
`false` outside safe mode) and still runs the name binder, the type checker and the
summoner over it, so `name_map` is populated for everything that did parse. Go-to-definition,
completion, hover, the outline and semantic tokens all depend on that and are guarded by
[test/incomplete-code.test.mjs](test/incomplete-code.test.mjs) — flipping
`exclude_non_parsed_files` to `true` fails seven of its ten cases. The broken state is
injected with `changeDocument()` so the fixture on disk stays valid and keeps its
`fixtures.test.mjs` coverage.

[test/latency.test.mjs](test/latency.test.mjs) is a guard against complexity regressions,
not a benchmark: it generates a multi-file project and asserts that a warm request stays
far below a `didChange` round trip measured in the same run. On this machine an edit costs
~19 ms and every warm request 1–3 ms. That ratio is what catches a request that starts
recompiling — the double-compile bug in the file-identity gotcha below was invisible to
every correctness test.

**This machine has WSL2 enabled but no distribution installed.** Nothing can be verified on
real Linux locally; the Linux leg of CI is the only evidence, so do not claim otherwise.

**All `.art` / `.impala` fixture code must be written by us** — never copied from other
AnyDSL repos (licensing). Sample projects such as `D:/anydsl-metaproject` may be read for
reference only. Every fixture that is supposed to be valid must be proven valid with the real
compiler before being relied on in a test:

```powershell
artic-lsp/buildGcc/bin/artic.exe test/fixtures/<project>/*.art
```

When writing Artic code, follow [.github/skills/artic-language/SKILL.md](.github/skills/artic-language/SKILL.md).

## Packaging and CI

One VSIX carries **both** server binaries (`vscode/build/bin/artic-lsp` and
`artic-lsp.exe`); `package.json`'s `files` list includes `build/bin/**`, and
[vscode/src/server-path.ts](vscode/src/server-path.ts) picks the right one at startup.
A universal package was chosen over `vsce package --target <platform>` because it is one
file to publish, and because it survives Remote-WSL, where the extension host is Linux while
the editor is Windows.

- **`resolveServerPath()` order is: `artic.serverPath` setting, bundled binary, `PATH`.**
  A setting that points at a file that no longer exists must *not* shadow the bundled
  binary. Guarded by [test/server-path.test.mjs](test/server-path.test.mjs), which fakes the
  host so no filesystem or platform is needed.
- **`vsce` drops POSIX permissions from every packaged file when it runs on Windows.**
  A VSIX built on Windows therefore ships the Linux binary without its executable bit.
  The release workflow packages on `ubuntu-latest` for that reason, and
  `resolveServerPath()` re-adds the bit defensively.
- **`actions/upload-artifact` also loses the executable bit** (it zips its input), so the
  package job re-runs `chmod +x` on the downloaded Linux binary.
- Local `vscode/package.sh` / `build-lsp.ps1` stage the **host** platform's binary only.
  Neither deletes the other platform's file, so running both leaves a universal
  `vscode/build/bin/` behind. `vscode/publish.sh` no longer builds anything: it bumps the
  version, tags and pushes, and [.github/workflows/release.yml](.github/workflows/release.yml)
  does the rest.
- **To try a change in the editor: `powershell -File vscode/install.ps1`** (Windows) or
  `vscode/package.sh install` (Linux/macOS). [vscode/install.ps1](vscode/install.ps1) rebuilds
  the server, stages it, packages the VSIX and installs it with the first `code`/`cursor` CLI
  on `PATH` — in a remote window that is the *server* CLI, i.e. the host holding the workspace,
  which is the one that matters. It skips `npm install` when `node_modules` already exists.
  Reload the window afterwards; the old server process keeps running otherwise.
- **Cursor is a supported target and must stay one.** `engines.vscode: ^1.75.0` is satisfied by
  every current Cursor, and the extension uses no proprietary or proposed API (only `workspace`,
  `window`, `commands`, `Uri`, `RelativePattern`, `FileSystemError`). Do not raise the engine
  floor without a reason that survives that constraint. The owner declined publishing to the
  Visual Studio Marketplace and to Open VSX, so the documented install route is
  `cursor --install-extension artic-language-server-<version>.vsix`; Cursor's `product.json`
  points `extensionsGallery` at Open VSX, so a listing there would need an Eclipse account and a
  signed Publisher Agreement.
- [.github/workflows/ci.yml](.github/workflows/ci.yml) runs the whole Definition of Done on
  push and PR: build + `ctest` + `node --test` + `npm audit` on Linux (Ninja/GCC) *and*
  Windows (whichever Visual Studio the runner image has), plus the no-`ENABLE_LSP` build on
  Linux. The Linux leg also runs `vscode/build-lsp.sh` and asserts the staged binary is
  executable — that packaging path is the one thing CI covers that nothing else does. The two
  toolchains are the point: the `u8string()` and drive-letter bugs below were both
  single-toolchain bugs that a one-OS CI would have missed.
- **Do not pin a Visual Studio generator in CI.** `-G "Visual Studio 17 2022"` failed with
  `could not find any instance of Visual Studio` once the `windows-latest` image moved past
  VS 2022. The Windows leg passes no `-G` at all, so CMake selects the newest Visual Studio
  present; MSBuild still finds the Windows SDK by itself. Locally, pinning is fine.
- **Nor name build targets there.** The build step is a plain
  `cmake --build artic-lsp/build --config Release --parallel`; see
  [Fast local loop](#fast-local-loop-recommended) for why `--target artic` cannot work under
  MSBuild.
- **The test step runs under `shell: bash`, with the glob unquoted.** `node --test` only
  expands glob patterns itself on newer Node, so `node --test "test/*.test.mjs"` failed on
  the runner with `Could not find '.../test/*.test.mjs'`. Letting bash (Git Bash on the
  Windows image) expand it removes the Node-version dependency. The paths it produces are
  relative, so MSYS path translation does not touch them.
- **`npm ci` must run before `node --test`** in CI: `detect-config.test.mjs` and
  `server-path.test.mjs` bundle TypeScript with the esbuild in `vscode/node_modules`.
- `half` is fetched from an unpinned SourceForge `files/latest/download` URL — the least
  reproducible step in the build. CI caches `.deps/*-src` and `.deps/*-subbuild` keyed on the
  hash of `artic-lsp/cmake/Dependencies.cmake` to keep it off the critical path. If CI starts
  failing at configure time, suspect that URL first.

## Working with the `artic/` submodule

- It is intentionally checked out on branch `master`, not detached, because we commit to it.
- The parent repo records only a commit SHA, so this produces no parent diff.
  ` m artic` in `git status` means "submodule worktree is dirty", not "new commits".
- **Two-commit rule:** commit inside `artic/` first, then commit the moved gitlink in the parent.
- Keep the diff against upstream small and reviewable. Check it with:

```powershell
git -C artic diff --stat upstream/master...HEAD
```

  (`upstream` = `https://github.com/AnyDSL/artic.git`; add it if missing.) It has been through
  one deliberate reduction pass already — do not let it drift back.
- **When a fork change is acceptable at all:** an LSP-only addition is fine behind
  `ENABLE_LSP` as long as it does not change actual compiler behaviour. If it cannot be
  guarded without becoming too intrusive or too unreadable — and dropping the guard would be
  bad practice — then do not make the change; find a server-side approach, or drop the
  feature.
- LSP-only additions to artic must be guarded with `#ifdef ENABLE_LSP`. The define is applied
  in [artic/src/CMakeLists.txt](artic/src/CMakeLists.txt) when the `artic-lsp` target exists:

```cmake
if (TARGET artic-lsp)
    target_compile_definitions(libartic PUBLIC -DENABLE_LSP)
endif()
```

- Whole files that exist only for the LSP (`include/artic/lsp.h`, `include/artic/name_map.h`,
  `src/name_map.cpp`) guard their **entire body** instead of being added conditionally to the
  target. They are listed in `src/CMakeLists.txt` unconditionally and compile to nothing
  without the define, which keeps the file list identical in both configurations.
  `lsp.h` holds the plain data types (`ls::Severity`, `ls::Diagnostic`) and must stay
  dependency-light: `log.h` includes it, and `ast.h` includes `log.h`, so anything needing
  `ast.h` (i.e. `NameMap`) has to live in `name_map.h` instead or the include graph cycles.
- **A host class gets a guarded, default-initialised member — never a second constructor.**
  `Log::diagnostics`, `NameBinder::name_map` and `TypeChecker::name_map` are all
  `#ifdef ENABLE_LSP` members defaulting to `nullptr`, assigned by `ls::Compiler` in
  [artic-lsp/include/compile.h](artic-lsp/include/compile.h). Threading them through the
  constructor instead needs an `#ifdef/#else/#endif` pair per class, which triples the
  upstream diff and touches a signature upstream also owns.
- **Do not thread an LSP flag through a hot path.** `Printer::print_additional_node_info`
  wrapped every printed node in `<NodeName>` tags for an `artic/debugAst` request; paying
  for it meant a `NodeScope` line in all 54 `print()` overrides — the second-largest fork
  footprint after `ast.h`, for a debug aid. Both it and the request were removed. If an AST
  dump is wanted again, build it in `artic-lsp/` on top of `traverse()`.
- **Documented exception:** the AST traversal in [artic/include/artic/ast.h](artic/include/artic/ast.h)
  (`Node::TraverseFn`, `traverse()`, and 54 one-line `traverse_children()` overrides) is
  LSP-only but deliberately **not** guarded. Guarding it needs an `ARTIC_TRAVERSE_CHILDREN(...)`
  macro at all 54 sites, which makes the upstream diff harder to review rather than easier,
  and buys only a vtable slot per node type. The rule exists to keep the standalone compiler
  clean and buildable — it is, in both configurations.
- The standalone `artic` compiler must keep building **with and without** `ENABLE_LSP`.
  It previously did not — `main.cpp` included `server.h`, which only exists in `artic-lsp/`.
  Verify both, every time:

```powershell
cmake -S artic-lsp/nolsp -B artic-lsp/build-nolsp -G Ninja -D CMAKE_BUILD_TYPE=Release `
      -D CMAKE_C_COMPILER=gcc -D CMAKE_CXX_COMPILER=g++ `
      -D Thorin_DIR="$PWD/artic-lsp/buildGcc/share/anydsl/cmake" `
      -D Half_DIR="$PWD/artic-lsp/buildGcc/_deps/half-src/include"
cmake --build artic-lsp/build-nolsp --parallel
ctest --test-dir artic-lsp/build-nolsp -E "^thorin_"
```

  [artic-lsp/nolsp/CMakeLists.txt](artic-lsp/nolsp/CMakeLists.txt) is a throwaway project that
  does `add_subdirectory(../../artic artic)` **without** an `artic-lsp` target, so the define
  is never applied. It fetches nothing — point `Thorin_DIR`/`Half_DIR` at an already
  configured `artic-lsp` build tree. It is tracked (an earlier `build-nolsp-src/` was not:
  `.gitignore` has `build*/`), so CI runs exactly the same check —
  see the *Build artic without ENABLE_LSP* step in
  [.github/workflows/ci.yml](.github/workflows/ci.yml).
- Changes to the fork fall into two sets, and they must be kept distinguishable:
  **LSP-only** (guarded, will never be upstreamed) and **upstreamable**. The latter is
  currently: the `err.stream` fix in `log.h`, uninitialised `Loc::Pos` members, the
  `std::is_pod` → `is_standard_layout && is_trivial` fix in `hash.h` (`is_pod` is deprecated
  in C++20), lexer/parser/`ast.cpp` error tolerance, the bounded parse-error recovery in
  `parser.h`/`parser.cpp` (non-consuming `expect()`, `skip_to_decl()`, `reported_at()`), the
  type-checker error tolerance in `check.cpp` including the `infer` rule the four error nodes
  were missing, the `pop_scope` warning fix, `Node::dump()`/`Type::dump()` writing to
  `log::err` instead of `log::out`, the `usage()` text in `main.cpp`, and the
  `file(row, col)` → `file:row:col` location format in `loc.h` (a separate, purely cosmetic
  change — terminal-clickable, but user-visible). The unmerged `origin/error-tolerance`
  branch is relevant prior art.

## Definition of Done

A larger change is not done until all of these hold. State explicitly which ones you
verified, and say so if you deliberately skipped one.

1. **Scoped** — the task is written in the [Plan](#plan) with an owner-visible outcome
   before implementation starts.
2. **Builds** — clean build on the fast loop *and* at least one other toolchain from the
   verified matrix. No new compiler warnings.
3. **Regression-guarded** — an automated test exists that **fails before** the change and
   **passes after**. Demonstrate both directions rather than asserting it.
4. **Suite green** — `node --test 'test/*.test.mjs'` passes in full, not just the new test,
   and `ctest --test-dir artic-lsp/buildGcc -E "^thorin_"` stays at 145/145.
5. **Fixtures validated** — any new `.art`/`.impala` sample compiles under `artic.exe`
   (or, if it is a negative fixture, fails with exactly the diagnostic being asserted).
6. **Upstream-diff reviewed** — for submodule changes, `git -C artic diff --stat upstream/master...HEAD`
   was inspected and the growth is justified. Unrelated reformatting is reverted.
7. **Dependencies audited** — `npm audit` in `vscode/` reports **0 vulnerabilities**. Never
   accept a finding silently: fix it, or record the justification here. See
   [Dependency security](#dependency-security) before reaching for `npm audit fix --force`.
8. **Docs updated** — user-facing behaviour in [README.md](README.md), how a feature works in
   [docs/implementation-notes.md](docs/implementation-notes.md), and how to work on the repo here.
9. **Tree clean** — `git status` shows only intended files; no build output, no scratch dirs.

## Dependency security

`npm audit` must be clean. The npm-suggested fix is frequently *not* the right one, so verify
before applying:

- **`npm audit fix --force` is not a solution on its own.** For the `brace-expansion` DoS
  advisory it wanted `vscode-languageclient@8 -> 10`, which raises `engines.vscode` from
  `^1.75.0` to `^1.91.0`. That is a user-visible platform bump, and the engine floor is what keeps
  the extension installable in Cursor. Prefer a scoped `overrides` entry.
- **Prove the override at the API level, not just via the audit output.** A blanket
  `"overrides": { "brace-expansion": "^5.0.9" }` silences the audit but *breaks the extension
  at runtime*: `brace-expansion@5` exports `{ expand }` where v1/v2 exported a bare function,
  so `minimatch@5` throws `TypeError: expand is not a function` on any pattern containing
  braces. Audit-clean and working are different things.
- What is actually in place:

  ```json
  "overrides": { "vscode-languageclient": { "minimatch": "^10.2.6" } }
  ```

  `vscode-languageclient@8` only ever calls `new minimatch.Minimatch(pattern, opts)`, which
  `minimatch@10` still exports, and `minimatch@10` depends on the patched `brace-expansion@5`.
  This keeps the client at v8 and the engine floor at `^1.75.0`.
- **`--target=node16` in `esbuild-base` is load-bearing.** `minimatch@10`/`brace-expansion@5`
  declare `engines.node >= 20`, but we still support VS Code 1.75 (Electron 19 / Node 16).
  esbuild downlevels the syntax; drop the target and the bundle can break on older VS Code.
- After any dependency change, re-verify: `npm run compile`, `npx vsce package`, and
  `node --test 'test/*.test.mjs'`. `@vscode/vsce` is dev-only (it just builds the VSIX), but
  it was the source of 8 of the original 11 findings — keep it current.

## Plan

Only what is **not** done is tracked here. Everything already shipped is described in
[docs/implementation-notes.md](docs/implementation-notes.md); if you want to know how a
feature works, read that, not a changelog.

**The language server itself is feature-complete.** Every LSP capability that artic's frontend
can support is implemented and tested; the two that are not are parked below for reasons that
are not about effort. What is left is one piece of hygiene with an external dependency, and a
short list of things that were measured and found not to be worth doing yet.

### 1. Upstream the upstreamable fork set — *not started, and the only open work*

Open a PR against AnyDSL/artic with the non-LSP fixes listed under
[Working with the artic/ submodule](#working-with-the-artic-submodule). The unmerged
`origin/error-tolerance` branch is prior art. The longer this waits the more expensive it gets
— parse-error recovery and the error nodes' missing `infer` rule both landed in exactly this
set. It is also the only item whose outcome is not ours to decide, so starting it early is
worth more than finishing it fast.

### Deferred — measured, and not worth doing at the current scale

- **Performance.** Text sync is `Full`, and every `didChange` recompiles the whole project and
  rebuilds the `NameMap` from scratch. Measured on this machine against generated projects and
  against the largest real artic library in `D:/anydsl-metaproject` (`runtime/platforms/artic`,
  29 files, 7 208 lines):

  | Project | Cold open | `didChange` → diagnostics | Warm request |
  | ------- | --------: | ------------------------: | -----------: |
  | generated, 360 decls | 52 ms | 17 ms | 1.6 ms |
  | generated, 3 600 decls | 136 ms | 50 ms | 2.4 ms |
  | generated, 10 800 decls | 190 ms | 104 ms | 4.9 ms |
  | generated, 54 000 decls | 607 ms | 471 ms | 9.7 ms |
  | **real `runtime` library** | **83 ms** | **43 ms** | **3.4 ms** |

  It is linear in project size, at roughly 9 µs per declaration, and an edit only becomes
  user-visibly slow (>100 ms) at about **1.5× the size of the largest artic project that
  exists**. So incremental parsing is not the thing to reach for.

  **The one number that is not fine is queueing.** Notifications are handled synchronously on
  the message loop, so a request issued behind a burst of edits waits for every one of them:
  `documentSymbol` behind 10 queued `didChange`s took **429 ms** — ten compiles, not one.
  Latency while typing is `pending_edits × compile_time`, and that is the term that would bite
  first on a large project. The fix is to coalesce edits rather than to make a compile faster.
  It is still deferred, because `lsp-framework` has no way to peek at the input queue and
  `processIncomingMessages()` reads one message at a time, so coalescing means a timer thread
  and locking around compile state that today has no concurrency at all. **Trigger for
  revisiting: a real project where the edit round trip exceeds ~100 ms, or a report of the
  editor feeling laggy while typing.** Re-measure with
  [test/latency.test.mjs](test/latency.test.mjs) before believing anything here.
- **Protocol modernisation.** Both remaining pieces were costed against the numbers above and
  neither buys anything: **pull diagnostics** (`textDocument/diagnostic`) would still trigger
  the same compile, so it saves the publish and nothing else; **`$/progress` during a compile**
  is free of risk but invisible at 83 ms. Incremental text sync is in the same bucket — the
  server re-lexes the whole buffer regardless, so it would only shrink a payload that is not
  the bottleneck, in exchange for range arithmetic that corrupts a buffer silently when wrong.
- **Doc comments** — the lexer discards `///`, so nothing can surface documentation in hover
  or completion. The highest-value item left after item 1, and the only deferred entry a user
  would actually notice. Capturing them is a fork change under the `ENABLE_LSP` policy, and it
  must not alter tokenisation for the standalone compiler.
- **Minor gaps** — semantic tokens have no delta support and never emit the `deprecated` or
  `defaultLibrary` modifiers; call hierarchy is derivable from `references_of` plus an
  enclosing-function lookup; rename performs no collision, shadowing or valid-identifier check.
  Of these only rename validation is arguably a correctness bug rather than a gap: renaming to
  a keyword or to a name already bound in scope produces a broken program with no warning.
- **Comments in `artic.json`** — the config is read with `nlohmann::json::parse`, i.e. nlohmann's
  defaults, so `//` is a parse error. The README examples were written with comments for years and
  were therefore not copy-pasteable. Enabling `ignore_comments` is one line, but the document
  selector in [vscode/src/extension.ts](vscode/src/extension.ts) matches `language: 'json'`, so
  the file would also need a `jsonc` filename association to stop VS Code's own validator
  flagging them, and the selector would have to accept both languages or config diagnostics
  stop arriving. Documented as a limitation in the README instead. Zero-config lowers the value
  of this further: the best config is the one nobody has to write.

### Declined — considered, evidenced, and rejected by the owner

- **Inferring a project from a directory**, and **project globs from a setting**. The evidence
  is in [Zero-config projects](docs/implementation-notes.md#zero-config-projects): artic has no
  import mechanism, ~550 of the 690 real-world `.art`/`.impala` files are independent
  single-file programs the fallback already handles correctly, a directory rule turns 71 clean
  files into 141 redefinition errors, and a real project is combinatorial in a way only the
  build system knows. Do not re-propose either without new evidence against that table.

### Parked — do not start without explicit approval

- **Formatting** (`textDocument/formatting` and friends). The obvious basis,
  [artic/src/print.cpp](artic/src/print.cpp), is a debug printer, not a formatter: it does
  **not preserve comments**, and it is suspected to change which types end up implicitly
  declared or inferred. A formatter that silently rewrites the meaning of a program is worse
  than no formatter. Worth doing eventually, but **only once the owner explicitly approves
  starting it.**
- **Code actions / quick fixes** — the highest-polish feature and the highest effort: it
  requires diagnostics to carry structured fix data out of the type checker, which is a
  substantial fork change.

## Gotchas

- **A completion the client never asks for looks exactly like a server that returns nothing.**
  Only `.` and `:` were declared as `triggerCharacters`, so `let a = ` opened no widget while
  `let a = .` opened a full one — reported as "completion does not work". Probing the protocol
  at that cursor showed the server had always returned every item, including the wanted one.
  Before debugging a language feature that "does nothing", establish which side is silent.
- **An item without a `sortText` is sorted by its label, so any ordering the handler builds is
  thrown away.** The completion handler reversed its item list twice to put the nearest scope
  first; both calls were dead code, and the wanted declaration sat at rank 22 of 31 behind
  `addrspace(...)`, `asm`, `bool`, `break` — off-screen in a twelve-row widget, so even an open
  window looked empty. `finish()` in
  [artic-lsp/src/server.cpp](artic-lsp/src/server.cpp) now ranks every path's items.
- **`isa<T>()` returns null, and the completion handler used to dereference it.** The
  projection branch tested `type->isa<StructType>()` and then read `struct_type->decl.fields`
  from *both* the struct and the enum path, so typing `.` after an enum value dereferenced a
  null `StructType` and **killed the server process** — every subsequent request in the
  session timed out, which looks like a hang rather than a crash. Guarded by
  [test/completion.test.mjs](test/completion.test.mjs).
- **A generic function's `Node::type` is a `ForallType`, not a `FnType`.** Asking
  `fn->type->isa<FnType>()` therefore fails for every `fn f[T](...)`, and the completion item
  silently lost its `detail`. Unwrap through `ForallType::body` first. Same trap applies
  anywhere a signature is read off a declaration.
- **An artic `FnType` is a tuple type and carries no parameter names.** Anything that needs
  them has to read the declaration's `Ptrn`, unwrapping `TypedPtrn` recursively.
- **Cycle detection must run one DFS over all roots, not one per dependency edge.** The
  original loop seeded `detect_cycle(project.name, dep)` with the arguments swapped relative
  to the parameter list, cleared `visited`/`rec_stack` per edge, and erased the offending
  entry with `std::remove` while a range-for over that same vector was live — undefined
  behaviour whenever a project depends on itself. Guarded by
  [test/circular-dependencies.test.mjs](test/circular-dependencies.test.mjs).
- **The project registry is per server session, not per workspace.** A duplicate project
  name is warned about and *ignored*, so two staged workspaces in one test suite must not
  reuse a name — the second one's projects silently do not exist.
- **`lsp::FileUri::fromPath()` renders the path with `u8string()`, not `generic_u8string()`.**
  On **MSVC** that keeps native backslashes, which get percent-encoded as `%5C`, so **no
  diagnostic ever reached the editor**. MinGW's libstdc++ keeps forward slashes, which is why
  the fast loop never showed it. Worked around with `to_file_uri()` in
  [artic-lsp/src/server.cpp](artic-lsp/src/server.cpp); the real bug is upstream in
  lsp-framework and is worth reporting.
- **`Parser::expect()` no longer consumes on a mismatch, so any new loop must not rely on
  it for progress.** Leaving the offending token in place is what lets the parser resume at
  the next declaration instead of eating it, but it means a `while` whose only advance came
  from a failing `expect()` now spins. The existing ones are safe and were audited:
  `parse_list` breaks on a token that is neither separator nor terminator, and the block,
  module and top-level loops dispatch on the token themselves. `parse_error_decl()` is the
  guarantee at the declaration level — it always consumes at least one token before
  `skip_to_decl()` runs.
- **`ConfigLog::error("...{}", x)` does not format.** The `{}` reaches the user verbatim and
  `x` is silently treated as the search context.
- **`JSON_DIAGNOSTIC_POSITIONS` changes the layout of `basic_json`, so it is a build-wide
  setting, not a per-file one.** It is forced on in
  [artic-lsp/cmake/Dependencies.cmake](artic-lsp/cmake/Dependencies.cmake) *before*
  `FetchContent_MakeAvailable`, and nlohmann's own CMake turns it into an `INTERFACE` compile
  definition on `nlohmann_json::nlohmann_json` so every consumer agrees. Because
  `JSON_Diagnostic_Positions` is a cached option on a `FetchContent` subproject, a stale build
  tree can silently keep the old value — `artic-lsp/src/json_source.cpp` carries an `#error`
  guard so that fails the build instead of quietly losing every position. Confirm with
  `Diagnostic positions enabled (JSON_DIAGNOSTIC_POSITIONS=1)` in the configure output.
- **`end_pos()` is exclusive; `parse_error::byte` is not.** A value's range is
  `[start_pos(), end_pos())`, but the byte a parse error reports is 1-based *and* one past the
  offending character — hence the separate `position_of_byte()`. Pinned by the exact-span
  assertions in [test/config-positions.test.mjs](test/config-positions.test.mjs).
- **A `catch` block runs after the RAII file-context scope has already unwound.**
  `ConfigParser::parse()` wraps its whole body in `try`, and `ConfigLog::scoped_file()` restores
  the previous context from its destructor — which fires while the exception propagates, before
  the handler. So the `log.error()` in the handler had no file to report against and the server
  quietly logged `Dropping config message with no reportable file`: **a plain JSON syntax error in
  `artic.json` produced no diagnostic at all.** The handler re-establishes the scope now. Guarded
  by the `rejects a config file containing comments` case in
  [test/config-diagnostics.test.mjs](test/config-diagnostics.test.mjs).
- **`NameBinder::pop_scope()` is where "unused identifier" is reported, so a wrong early-out
  there disables the warning globally and silently.** The fork suppressed it for function
  prototypes (which bind parameters but have no body to use them in) with
  `if (auto fn = current_node->isa<ast::FnDecl>(); !fn || !fn->fn->body) ...`. `isa<>` returns
  null for every node that is *not* an `FnDecl`, so `!fn` was true for block scopes, loops,
  case arms and structs — i.e. the warning was suppressed everywhere except in real function
  prototypes, the one place it was meant to be suppressed. Nothing failed; the diagnostic
  just stopped existing. Guarded by the `unused local binding` case in
  [test/source-diagnostics.test.mjs](test/source-diagnostics.test.mjs) with
  [test/fixtures/diagnostics/src/unused_local.art](test/fixtures/diagnostics/src/unused_local.art).
- **`publish_config_diagnostics()` may only clear what the current pass evaluated.**
  Every compile calls it with a fresh `ConfigLog`, and configs are cached, so that log is
  almost always empty. Clearing everything it does not mention meant that opening any `.art`
  file wiped every config diagnostic milliseconds after it appeared — the Problems panel
  looked clean while the config was broken. `ConfigLog::evaluated_files` (filled by
  `scoped_file()`) records which configs a pass actually looked at; only those may be
  cleared. Guarded by [test/optional-includes.test.mjs](test/optional-includes.test.mjs),
  which opens a source file after the config and then asserts the error is still there.
- **A file's identity is its canonicalised path string.** `paths::canonical_path()` in
  [artic-lsp/src/paths.cpp](artic-lsp/src/paths.cpp) is the single place that
  produces it; everything that turns a path or URI into a lookup key must go through it.
  `File::path` is handed to the lexer and the locator, so every `Loc::file` string, every
  `name_map` key and every diagnostic URI derives from it.
  Producers disagree about spelling: VS Code always sends `file:///d:/...` (lower-case
  drive), CMake-generated `.vcxproj` files contain `D:\...`. `std::filesystem::weakly_canonical`
  normalises **neither case nor drive letter** on Windows (verified on MinGW *and* MSVC),
  so without the fold the same file gets two identities depending on which producer
  registered it first. Symptom: diagnostics still work, but semantic tokens, inlay hints
  and go-to-definition all silently return nothing, and every request recompiles the whole
  project twice because `already_compiled` is never true. Guarded by
  [test/path-identity.test.mjs](test/path-identity.test.mjs).
  Only the drive letter is folded — lowercasing the whole path would stop diagnostic URIs
  matching the document VS Code opened.
- **`lsp::Uri::path()` keeps the leading slash of a Windows drive path; `lsp::FileUri::path()`
  strips it.** A URI arriving as a plain `Uri` — a command argument parsed with `Uri::parse`,
  or `WorkspaceFolder::uri` and `rootUri` in `initialize` — yields `/D:/ws/main.art`, which
  canonicalises to something no config matches. It bit twice: `artic.projectForFile` reported
  `single-file` for every file, and the detected workspace roots matched nothing so zero-config
  detection silently never ran. Both look exactly like discovery bugs. Wrap it:
  `lsp::FileUri(uri).path()`. Note `path()` borrows from the object, so the `FileUri` has to
  outlive the use.
- `artic-lsp/src/main.cpp` ignores `argv`; the extension still passes `--lsp`, which is a no-op.
- `libartic` exports `ENABLE_LSP` as a **PUBLIC** compile definition, so it leaks to every
  consumer of the library, including the `artic` executable.
- Build directories are ignored via `build*/`. Keep scratch builds under that pattern.
- **Test on more than the fast loop.** The MinGW and MSVC standard libraries disagree about
  `std::filesystem::path::u8string()` on Windows, which hid a bug that broke diagnostics
  entirely on MSVC. Run the suite against a second binary with `ARTIC_LSP_BIN` before
  calling protocol-level work done.
- **`ARTIC_LSP_BIN` outlives the command that set it.** The terminal session is persistent,
  so a leftover `$env:ARTIC_LSP_BIN` from an earlier cross-toolchain run silently points the
  whole suite at a stale binary. A brand-new handler then fails with `Method not found` and
  the capability reads `undefined`, which looks exactly like a registration bug in code that
  is actually fine. `Remove-Item Env:ARTIC_LSP_BIN` when done, and check it first when a
  freshly built feature appears to be missing.
- **The CI runner's `%TEMP%` is an 8.3 short path, and MSVC's `weakly_canonical` expands it.**
  `os.tmpdir()` on `windows-latest` is `C:\Users\RUNNER~1\AppData\Local\Temp`, because
  `runneradmin` exceeds eight characters. A test that staged a workspace there sent
  `file:///C:/Users/RUNNER%7E1/...` while the server published diagnostics under
  `c:/Users/runneradmin/...`, so `waitForDiagnostics()` timed out and 13 tests failed with
  27 cancelled — while `Compile success` and the diagnostics themselves were plainly visible
  in the server's stderr. `stageFixture()`/`stageFiles()` run the temp directory through
  `realpathSync.native()` now. Two things make this hard to hit locally: a user name of
  eight characters or fewer produces no short name at all, and **MinGW's `weakly_canonical`
  does not expand 8.3 names while MSVC's does** — reproducing it needs a short `TMP` *and*
  `ARTIC_LSP_BIN` pointed at an MSVC build.
- **A backslash is a separator only on Windows, and `.sln`/`.vcxproj` always use one.**
  `to_absolute_path(dir, "src\\main.art")` yields a single filename containing a backslash on
  Linux, so the file "does not exist" and the project silently expands to nothing. Both
  MSBuild parsers in [artic-lsp/src/config.cpp](artic-lsp/src/config.cpp) go through
  `from_msbuild_path()` now. `build.ninja` is deliberately *not* converted: it is generated
  per platform and a literal backslash there is only ever a separator on Windows, where it
  already works.
- **A build command must be unwrapped before it can be tokenised.** A generated command is
  `cmd.exe /C "<real command>"`, one quote pair around the whole line including its ` && `
  separators, so quote-aware splitting returns it as a *single* token — the parser then finds
  no artic invocation and the project silently expands to nothing, which looks exactly like a
  detection bug. `unwrap_shell_command()` in [artic-lsp/src/config.cpp](artic-lsp/src/config.cpp)
  strips it first. The old code split on whitespace and stripped stray quotes per token, which
  needed no unwrapping but lost every path containing a space. Guarded by
  [test/paths-with-spaces.test.mjs](test/paths-with-spaces.test.mjs).
- **Writing to a server that is exiting raises EPIPE on Linux and is silent on Windows.**
  `LspClient.stop()` sends `exit` right after `shutdown`, so the write races the server
  closing stdin. Unhandled, it surfaced as `write EPIPE` in an `after` hook *after the test
  had ended*, failing six suites that were otherwise green. `#send()` now bails out when the
  child has exited or stdin is no longer writable, and `stdin` has an `error` listener.
- **`node_modules/esbuild/bin/esbuild` is a JS shim on Windows and a native ELF binary on
  Linux.** `execFileSync(process.execPath, [esbuild, ...])` therefore works locally and dies
  with `SyntaxError: Invalid or unexpected token` (on the literal text `ELF`) in CI. Use the
  JS API via `importExtensionModule()` in [test/helpers.mjs](test/helpers.mjs), which picks
  esbuild's own platform binary.

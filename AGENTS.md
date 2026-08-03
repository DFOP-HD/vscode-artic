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

Compile pipeline used by the server: `Lexer -> Parser -> NameBinder -> TypeChecker -> Summoner`,
driven by `Compiler::compile_files()` in [artic-lsp/src/compile.cpp](artic-lsp/src/compile.cpp).

### Inside `artic-lsp/`

`server.cpp` is deliberately one large file — all LSP request handlers live there, grouped
by feature in anonymous namespaces. Everything that is *not* a request handler belongs in
one of the utility modules, so a handler stays readable:

| Module | Namespace | What belongs in it |
| ------ | --------- | ------------------ |
| `paths.h` / `paths.cpp` | `artic::ls::paths` | Everything that turns a path or URI into a file identity: `canonical_path`, `lookup_key`, `to_absolute_path`, `from_msbuild_path`, `read_file`. **The only legitimate source of a lookup key** — see the file-identity gotcha below. |
| `text.h` | `artic::ls::text` | Header-only string helpers: `to_lower`, `trim_left`, `strip_quotes`, `split_whitespace`, `quote`. |
| `lsp_convert.h` / `.cpp` | `artic::ls` | Conversions between artic's `Loc`/`Severity`/`Diagnostic` and the `lsp::` protocol types, plus `contains(range, position)`. |
| `ast_render.h` / `.cpp` | `artic::ls` | Turning AST nodes into display strings: `print_to_string`, `print_param_list`, `render_decl`, and the `symbol_kind_of` mapping. Hover, document symbols, workspace symbols, code lens and completion all render through here so they cannot disagree. |
| `symbol_index.{h,cpp}` | `artic::ls` | The parse-only, per-project declaration index behind `workspace/symbol`. |
| `workspace.{h,cpp}` | `artic::ls::workspace` | Config discovery, the project registry, file tracking. |
| `config.{h,cpp}` | `artic::ls::config` | Parsing `artic.json`, `.artic-lsp`, `.vcxproj`, `.sln`, `build.ninja`, and `ConfigLog`. |
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

**All `.art` / `.impala` fixture code must be written by us** — never copied from other
AnyDSL repos (licensing). Sample projects such as `D:/anydsl-metaproject/stincilla` may be
read for reference only. Every fixture that is supposed to be valid must be proven valid
with the real compiler before being relied on in a test:

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
- [.github/workflows/ci.yml](.github/workflows/ci.yml) runs the whole Definition of Done on
  push and PR: build + `ctest` + `node --test` + `npm audit` on Linux (Ninja/GCC) *and*
  Windows (whichever Visual Studio the runner image has), plus the no-`ENABLE_LSP` build on
  Linux. The two toolchains are the point — the `u8string()` and drive-letter bugs below
  were both single-toolchain bugs that a one-OS CI would have missed.
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

  (`upstream` = `https://github.com/AnyDSL/artic.git`; add it if missing.)
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
  in C++20), lexer/parser/`ast.cpp` error tolerance, the type-checker error tolerance in
  `check.cpp`, the `pop_scope` warning fix, `Node::dump()`/`Type::dump()` writing to
  `log::err` instead of `log::out`, the `usage()` text in `main.cpp`, and the
  `file(row, col)` → `file:row:col` location format in `loc.h` (a separate, purely cosmetic
  change — terminal-clickable, but user-visible). The unmerged `origin/error-tolerance`
  branch is relevant prior art.

## Definition of Done

A larger change is not done until all of these hold. State explicitly which ones you
verified, and say so if you deliberately skipped one.

1. **Scoped** — the task is written in the [Backlog](#backlog) with an owner-visible outcome
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
8. **Docs updated** — user-facing behaviour in [README.md](README.md), agent-facing facts here.
9. **Tree clean** — `git status` shows only intended files; no build output, no scratch dirs.

## Dependency security

`npm audit` must be clean. The npm-suggested fix is frequently *not* the right one, so verify
before applying:

- **`npm audit fix --force` is not a solution on its own.** For the `brace-expansion` DoS
  advisory it wanted `vscode-languageclient@8 -> 10`, which raises `engines.vscode` from
  `^1.75.0` to `^1.91.0`. That is a user-visible platform bump and works against
  backlog item 5 (Cursor support). Prefer a scoped `overrides` entry.
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

## Backlog

Ordered. Keep status markers current.

1. **Test harness** — *done*. Dependency-free LSP protocol tests + self-authored fixtures
   in `test/`, plus the artic CTest suite wired up via `include(CTest)`.
2. **Config diagnostics are unreliable** — *done, with one follow-up*. Fixed:
   - Diagnostic URIs were built with `FileUri::fromPath()`, which renders the path with
     `u8string()` instead of `generic_u8string()`. On **MSVC** that keeps native backslashes,
     which get percent-encoded as `%5C`, so **no diagnostic ever reached the editor**.
     MinGW's libstdc++ keeps forward slashes, which is why the fast loop never showed it.
     Worked around with `to_file_uri()` in [artic-lsp/src/server.cpp](artic-lsp/src/server.cpp);
     the real bug is upstream in lsp-framework and is worth reporting.
   - Source diagnostics were published under a lowercased path (`tracked_file()` used the
     lowercased string as the file's identity).
   - `.artic-lsp` was never recognised: it is a dotfile, so `path::extension()` is empty and
     both `get_file_type()` and `instantiate_config()` fell through. Matched on filename now.
   - Diagnostics were never cleared once a config was fixed; `published_config_diagnostics_`
     now tracks what was sent and clears it.
   - `ConfigLog::file_context` was a single mutable field that recursive includes overwrote
     without restoring. Replaced with the RAII `ConfigLog::scoped_file()`.
   - `ConfigLog::error("...{}", x)` does not format — the `{}` reached the user and `x` was
     silently treated as the search context.
   - Removed the dead `propagate_to_file` branch in `publish_config_diagnostics()`.

   Follow-up: a valid project config always emits an Information-severity message like
   `+ 1 files | total matches: 1 files:` against its `files` pattern. Folded into item 20.
3. **Support `.sln` files in the config** — *done*. `"include": ["../build/x.sln"]` expands to
   the `.vcxproj` files the solution lists. Solution folders (which reuse the `Project(...)`
   syntax) are skipped, and so is any project without an `artic.exe` build command — a
   CMake-generated solution is mostly `ZERO_CHECK`/`ALL_BUILD` noise, so those must not
   produce diagnostics. That distinction is `ConfigPath::is_implicit`: an include the user
   wrote is reported on, one derived from a solution is not. Projects are instantiated
   lazily and the result is cached even when empty, so a solution with hundreds of entries
   parses each `.vcxproj` at most once. See `parse_sln()` in
   [artic-lsp/src/config.cpp](artic-lsp/src/config.cpp) and `instantiate_config_sln()` in
   [artic-lsp/src/workspace.cpp](artic-lsp/src/workspace.cpp); guarded by
   [test/sln-config.test.mjs](test/sln-config.test.mjs).
4. **Support `build.ninja` and detect the configuration automatically** — *done*.
   `parse_ninja()` in [artic-lsp/src/config.cpp](artic-lsp/src/config.cpp) turns every ninja
   target whose `COMMAND =` line invokes artic into a project named after the generated file.
   The command is split on ` && ` so the `cd /D <dir>` of CMake's `cmd.exe /C "..."` wrapper
   becomes the base directory for relative source paths; the first artic invocation wins, and
   arguments are taken until the first one starting with `-`. Paths containing spaces are not
   supported, exactly as in `.vcxproj` files.
   The extension command is now `artic.detectWorkspaceConfiguration` ("Detect workspace
   configuration"). It scans for `.sln`, then `build.ninja`, then `.vcxproj`, and skips
   anything under a directory already covered by a stronger match — otherwise a solution and
   the projects it lists both get included and every project is reported as a duplicate.
   **A `.sln` never contains the word "artic"** — it holds nothing but project names and
   GUIDs — so it cannot be filtered by content like a `.vcxproj` or a `build.ninja` can. It
   qualifies when one of the projects it references does. Getting this wrong made stincilla
   detect all 57 of its `.vcxproj` files and miss `STINCILLA.sln`. The selection logic lives
   in [vscode/src/detect.ts](vscode/src/detect.ts) so it can be tested without VS Code.
   Detected entries are written as **optional** includes (trailing `?`) because a build
   directory does not exist on a fresh checkout.
   Guarded by [test/ninja-config.test.mjs](test/ninja-config.test.mjs),
   [test/optional-includes.test.mjs](test/optional-includes.test.mjs) and
   [test/detect-config.test.mjs](test/detect-config.test.mjs).
5. **Clean up the artic fork** — *done*. Divergence from the merge base went from
   **+645 / −89 across 19 files** to **+332 / −65 across 16 files** plus three fully
   self-guarded new files. What was done:
   - Deleted fork noise: `.gitmodules` (referenced a `lsp-framework` submodule that does not
     exist) and `build.sh` (hardcoded `$HOME/repos/...`).
   - Reverted pure churn: `include/artic/parser.h`, the `main.cpp` usage-text rewrite,
     whitespace-only edits in `bind.cpp` and `ast.h`, and debug leftovers (`// fn->dump();`).
   - Extracted the LSP types into `include/artic/lsp.h`, `include/artic/name_map.h` and
     `src/name_map.cpp`, all guarded in their entirety. `include/artic/print.h` is back to
     upstream and `src/print.cpp` is down from +99 lines to four.
   - Guarded the remaining LSP-only code in `bind.h`, `check.h`, `log.h`, `bind.cpp` and
     `check.cpp`. See [Working with the artic/ submodule](#working-with-the-artic-submodule)
     for the conventions and the one documented exception (`ast.h`).
   - Removed dead code: the `Node::print_node()` declaration (never defined, never called),
     an unused `const artic::Type* t` in `LetDecl::infer`, and restored upstream's
     `ModDecl::members` that the fork had deleted.
   - Fixed two real regressions the fork had introduced: the `pop_scope` gotcha below, and
     `ContinueExpr::infer` reporting `"break expression"` (copy-paste from `BreakExpr`).
   The upstreamable set is listed under the submodule section; PR'ing it to AnyDSL/artic is
   still open.
6. **Cursor editor support** — *done, except for a marketplace listing*. The blocker was never
   an API one: `engines.vscode: ^1.75.0` is satisfied by every current Cursor, and the
   extension uses no proprietary or proposed API (only `workspace`, `window`, `commands`,
   `Uri`, `RelativePattern`, `FileSystemError`). **The real bug was that the published VSIX
   contained a Linux ELF binary only, and the PATH fallback shelled out to `which` — so on
   Windows the extension has never worked, in Cursor *or* VS Code.** Fixed by
   [vscode/src/server-path.ts](vscode/src/server-path.ts) plus the cross-platform release
   build; see [Packaging and CI](#packaging-and-ci). The owner declined publishing to the
   Visual Studio Marketplace and to Open VSX, so the documented install route is
   `cursor --install-extension artic-language-server-<version>.vsix`, which works.
   Whether Cursor can reach the Microsoft Marketplace at all is answered by its
   `product.json` `extensionsGallery.serviceUrl`; it points at Open VSX, so a listing would
   require an Eclipse account and a signed Publisher Agreement.
7. **Restore Linux support** — development has moved to Windows and Linux has regressed.
   Re-verify the build (`artic-lsp/build.sh`), the packaging scripts (`vscode/build-lsp.sh`,
   `vscode/package.sh`), path handling, and run the test suite there.

### Language features

Ordered by value/effort. Items 8–12 need no submodule change and reuse infrastructure that
already exists: `ls::NameMap` (`find_decl_at`, `find_ref_at`, `find_decl`, `find_refs` in
[artic/include/artic/name_map.h](artic/include/artic/name_map.h)),
`ast::Node::traverse_children()` with `TraverseFn`, and the type printer the inlay-hint
handler already uses. The [LSP specification](https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/)
is the reference for what else a server can advertise.

8. **Hover** — *done*. `TextDocument_Hover` is registered in `setup_events_definitions()` and
   `hoverProvider` is advertised. It resolves the cursor with `find_decl_at`, falling back to
   `find_ref_at` + `find_decl`, and renders the declaration as a fenced ```artic``` block via
   `render_decl()` in [artic-lsp/src/server.cpp](artic-lsp/src/server.cpp). **The renderer
   must never call `decl.print()`** — every `Decl::print` overload emits the body (and
   `StructDecl`'s emits every field), which is a wall of text in a hover popup. It prints the
   keyword and identifier itself and delegates only to the sub-nodes that belong in a
   signature (`type_params`, `fn->param`, `fn->ret_type`, `aliased_type`), falling back to the
   inferred `Node::type` where the source has no annotation (`let` bindings, inferred return
   types). `fn->param` brings its own parentheses only when it is a tuple pattern — that is
   what upstream's `print_parens()` handles, and it is `static` in `print.cpp`, so the check
   is duplicated rather than exported. Guarded by [test/hover.test.mjs](test/hover.test.mjs)
   against [test/fixtures/hover/src/shapes.art](test/fixtures/hover/src/shapes.art), which
   holds one declaration of every kind the renderer branches on.
9. **Document symbols** — *done*. `TextDocument_DocumentSymbol` is registered in
   `setup_events_definitions()` and `documentSymbolProvider` is advertised. One pass over
   `compile->program->decls` builds a hierarchical `DocumentSymbol` tree via
   `make_document_symbol()` / `collect_document_symbols()` in
   [artic-lsp/src/server.cpp](artic-lsp/src/server.cpp); `detail` reuses the hover renderer,
   so the outline and the hover popup can never disagree. **`program` is the concatenation
   of every file in the project**, so each entry is filtered on `decl.loc.file` — without
   that the outline of one file lists the whole project. Fields of a tuple-like struct are
   skipped: they are named `0`, `1`, … and say nothing. `implicit` is the one declaration
   with no identifier; it is named `"implicit"` and detailed with its type. Guarded by
   [test/document-symbols.test.mjs](test/document-symbols.test.mjs) against
   [test/fixtures/document-symbols/src/outline.art](test/fixtures/document-symbols/src/outline.art).
10. **Document highlight, and fix `definition` on a declaration** — *done*. `definitionProvider`
    used to answer with *all references* when the cursor sat on a declaration, so
    Go-to-Definition on `fn scale` jumped into whichever file happened to call it. It now
    returns the declaration itself, and `find_refs(decl)` became
    `TextDocument_DocumentHighlight` instead, so no capability was lost. **Highlights are
    filtered to the requested document**: the whole project is compiled at once, and a
    range is only meaningful in the file it was asked for — without the filter the client
    receives ranges that point into a different buffer. The declaration is reported as
    `Write` and every reference as `Read`. Both handlers resolve the cursor through the
    shared `decl_at()` helper in [artic-lsp/src/server.cpp](artic-lsp/src/server.cpp)
    (`find_decl_at`, falling back to `find_ref_at` + `find_decl`), which hover, type
    definition and highlight now share. Guarded by
    [test/navigation.test.mjs](test/navigation.test.mjs).
11. **`didClose` is a no-op** — *done*. The handler now withdraws the document's diagnostics
    and discards the editor buffer it carried. **`File::text` alone does not mean "unsaved
    edits"** — `File::read()` fills the very same field from disk and never clears it, so
    the first attempt (`if (text) reset`) recompiled the project on every close and
    republished the errors it had just withdrawn. `File::text_from_editor` in
    [artic-lsp/include/workspace.h](artic-lsp/include/workspace.h) records the difference;
    only `set_file_content()` sets it, and `Workspace::discard_editor_buffer()` reports
    whether there was anything to drop. When there was, the project is recompiled from disk
    **before** the diagnostics are withdrawn — other documents may have been reported broken
    because of edits that no longer exist, and the recompile republishes for every file in
    the project, including the one being closed. Guarded by
    [test/did-close.test.mjs](test/did-close.test.mjs).
12. **Selection range** — *done*. `TextDocument_SelectionRange` is registered in
    `setup_events_selection_range()` and `selectionRangeProvider` is advertised.
    `spine_at()` in [artic-lsp/src/server.cpp](artic-lsp/src/server.cpp) walks
    `program->decls` with a `TraverseFn` that returns `false` for any node not covering the
    cursor, so one path is visited rather than the whole program. Two details the LSP does
    not spell out but clients depend on: **every requested position needs an answer**
    (`params.positions` is plural, and a client lines the results up with its requests by
    index), so a position no node covers still gets an empty range at the cursor; and
    **consecutive ranges must differ**, because many nodes wrap a child of exactly the same
    extent and a duplicate makes the user press Shift+Alt+Right twice for one visible step.
    The chain is built outermost-first so each node becomes the `parent` of the previous.
    Guarded by [test/navigation.test.mjs](test/navigation.test.mjs).
13. **Signature help** — *done*. `TextDocument_SignatureHelp` is registered in
    `setup_events_signature_help()` and `signatureHelpProvider` is advertised with `(` and
    `,` as trigger characters. **The call site is found in the source text, not in the AST**:
    signature help fires on a half-written call like `dot(a,`, where the parser has produced
    an error node rather than a `CallExpr`, so an AST walk finds nothing exactly when the
    feature is wanted. `enclosing_bracket()` scans backwards for the innermost unclosed
    bracket — skipping comments and string/char literals so a `(` inside one does not open a
    frame — and counts the commas at that bracket's own nesting level, which is the active
    parameter index. `callee_before()` then takes the identifier in front of the `(`, skipping
    a `[...]` type-argument list, and the declaration is resolved through
    `name_map.find_ref_at` + `find_decl` at the identifier's **first** character, so the
    lookup does not depend on how the lexer spells the end of a token.
    `render_signature()` prefers the *declaration* over the type, because only a declaration
    carries parameter names; an `OptionDecl` has to be rendered from the declaration in any
    case, since an enum option's type is its payload rather than a function type. Generic
    functions are unwrapped through `ForallType::body`, exactly as in completion.
    `activeParameter` is clamped to the last parameter — an out-of-range index makes VS Code
    highlight the *first* one, which is worse than sticking to the last. Guarded by
    [test/signature-help.test.mjs](test/signature-help.test.mjs) against
    [test/fixtures/signature-help/src/calls.art](test/fixtures/signature-help/src/calls.art).
14. **Go to type definition** — *done*. `TextDocument_TypeDefinition` is registered in
    `setup_events_definitions()` and `typeDefinitionProvider` is advertised. The cursor is
    resolved with `decl_at()`, and `declaring_type_decl()` in
    [artic-lsp/src/server.cpp](artic-lsp/src/server.cpp) unwraps the type until a
    user-written declaration is reached: `TypeApp` (a generic instantiation points at
    `applied`), `AddrType` (both `PtrType` and `RefType`), `ArrayType` and
    `ImplicitParamType`, then `StructType` / `EnumType` / `TypeAlias` / `TypeVar` /
    `ModType`, each of which carries a `decl`. A primitive resolves to nothing and the
    handler answers `null` rather than guessing. Guarded by
    [test/navigation.test.mjs](test/navigation.test.mjs).
15. **Go to implementation for implicits / `summon`** — *done, with no fork change at all*.
    `TextDocument_Implementation` is registered in `setup_events_definitions()` and
    `implementationProvider` is advertised. The item was scoped on the assumption that the
    Summoner would have to be taught to record its choice; it already does. `ast::SummonExpr`
    carries an **upstream, unguarded** `const Expr* resolved`, assigned in
    `SummonExpr::resolve_summons` from `Summoner::resolve`, and `emit.cpp` and `print.cpp`
    both rely on it — so the `ENABLE_LSP` question never arose.
    `summon_at()` in [artic-lsp/src/server.cpp](artic-lsp/src/server.cpp) finds the innermost
    `SummonExpr` covering the cursor and answers with `resolved->loc`. Two things worth
    knowing: **an omitted implicit argument is a `SummonExpr` too** — `TypeChecker::coerce`
    synthesises one per `ImplicitParamType` in the callee's parameter tuple, so `apply(v)`
    resolves even though nothing in it is written down; and that synthesised node **inherits
    the argument's location**, so the cursor has to be inside the parentheses rather than on
    the callee. An implicit *parameter* resolves to the `PathExpr` that `IdPtrn::to_expr()`
    synthesises, which carries the pattern's own location, so that case lands on real source
    too. Guarded by [test/navigation.test.mjs](test/navigation.test.mjs) against
    [test/fixtures/navigation/src/implicits.art](test/fixtures/navigation/src/implicits.art).
16. **Workspace symbols (Ctrl+T) and code lens** — *done*. `workspaceSymbolProvider` and
    `codeLensProvider` are advertised and handled in `setup_events_symbols()`.
    The blocker was the server holding a single `std::optional<Compiler>`. It is sidestepped
    rather than removed: **the index parses, it does not compile.** `SymbolIndex` in
    [artic-lsp/src/symbol_index.cpp](artic-lsp/src/symbol_index.cpp) runs `Lexer` + `Parser`
    over each project's own files and copies out name, container path, kind and location.
    Name binding, type checking and summoning are what make a compile expensive and none of
    them contributes anything a symbol picker shows.
    Three things it has to get right. **Everything the harvest sees dies with it**: the
    `Arena` holding the AST and the `Locator` owning every `Loc::file` string are both local
    to `harvest_file()`, so an `IndexedSymbol` may keep no pointer into either — the
    `lsp::Location` is built from `File::path` while they are still alive. **Parse errors go
    to a `std::ostream(nullptr)`**, because the index runs over files that are usually
    mid-edit and their diagnostics are the compile's business. And **the result is cached per
    project**, because a client re-issues `workspace/symbol` on every keystroke;
    `SymbolIndex::invalidate()` drops every project containing a changed file, and any
    `on_config_changed` / `reload_workspace` throws the whole index away, since it is keyed
    by project name and those names have just been re-instantiated.
    Only a project's **own** files are indexed — a dependency is a project in its own right
    and is indexed there, so following the edges would report every shared symbol twice.
    **Code lens** counts `name_map.find_refs(decl)` above each `fn`, `struct`, `enum`,
    `type`, `static` and `mod`. Fields and enum options are deliberately left out: a lens per
    struct field turns a record into a ladder of grey text. The count is scoped to the
    declaration's own project, because that is what the file's compile covers.
    **The lens command carries a URI string and two integers, not LSP objects**, because
    `vscode-languageclient` passes command arguments through unconverted;
    `artic.showReferences` in [vscode/src/extension.ts](vscode/src/extension.ts) rebuilds a
    `vscode.Uri`/`vscode.Position` and forwards to `editor.action.showReferences`. It is
    registered in code only, not in `contributes.commands`, so it stays out of the palette.
    `symbol_kind_of()` moved from `server.cpp` into
    [artic-lsp/src/ast_render.cpp](artic-lsp/src/ast_render.cpp) so document symbols,
    workspace symbols and code lens cannot disagree about what a declaration is.
    Guarded by [test/workspace-symbols.test.mjs](test/workspace-symbols.test.mjs).
17. **Parameter-name inlay hints** — *done*. `collect_parameter_hints()` in
    [artic-lsp/src/server.cpp](artic-lsp/src/server.cpp) walks the program with a `TraverseFn`
    that prunes anything outside the requested document, resolves every `CallExpr`'s callee
    through `name_map.find_ref_at` + `find_decl`, and labels each argument with the parameter
    it binds. Three things it must get right:
    **the names come from the declaration's `Ptrn`, not from the `FnType`** — a `FnType` in
    artic is a tuple type and carries no parameter names at all; and `fn dot(a: Vec2, b: Vec2)`
    parses as a `TuplePtrn` of **`TypedPtrn`s**, each wrapping the `IdPtrn` that holds the
    name, so `parameter_name()` has to unwrap `TypedPtrn` recursively. Reading `IdPtrn`
    directly yields an empty name for every annotated parameter, i.e. for every parameter
    anyone writes — the feature silently produces nothing.
    **An `ImplicitParamPtrn` yields no name**: it is summoned rather than written, so it can
    never be labelled.
    **Only a positional match is labelled.** `tuple->args.size() != names.size()` bails out,
    because a function whose single parameter is a tuple receives the whole tuple as one
    argument and lining names up with elements would be a guess.
    `argument_repeats_name()` suppresses the hint when the argument is a plain path or
    projection spelling the parameter's own name — `add(a, b)` gets no hints, which is what
    every other language server does. Guarded by
    [test/language-features.test.mjs](test/language-features.test.mjs).
18. **Completion polish** — *done, with the rest reclassified*. The `TODO` in
    [artic-lsp/src/server.cpp](artic-lsp/src/server.cpp) is fixed: a `for a in ...` binding
    was offered for the whole enclosing function, and so was every `match` arm binding.
    The default branch collects declarations by walking each `local_scopes` entry, and it
    pruned only **nested `BlockExpr`s**, so any other scope-introducing node was walked in
    full regardless of where the cursor was. It now prunes nested `FnExpr`, `CaseExpr` and
    `LoopExpr` as well — every scope that encloses the cursor is a `local_scopes` entry in
    its own right, so pruning loses nothing and descending only ever duplicates bindings or
    invents ones that are out of scope.
    **`ForExpr::traverse_children` bypasses the desugared closure**: it yields
    `lambda->param`, `iter`, `call->arg` and `lambda->body` directly, so the `FnExpr` node is
    never visited and pruning `FnExpr` alone does nothing for a `for` loop — `LoopExpr` has
    to be pruned too. To keep the loop variable available *inside* the loop, the outer walk
    now registers `loop_binding()` (which unwraps `for_expr->call->callee` → `CallExpr` →
    `arg` → `FnExpr` → `param` with `isa`, never `as`) and a `CaseExpr`'s pattern as scopes,
    and `FnExpr::param` replaces the old `FnDecl`-only parameter push.
    `use`-path completion turned out to work already, through the same `ast::Path` branch as
    `a::b`; it is now guarded rather than assumed. What is left — a resolve handler and item
    documentation — has no content to serve until doc comments exist, and those are
    **Deferred**, so nothing further is scheduled here.
    Guarded by [test/completion.test.mjs](test/completion.test.mjs).
19. **Project overview in the config file** — *done*. The `+ N files | total matches:`
    Information diagnostic is gone, and `artic.json` / `.artic-lsp` are annotated with inlay
    hints instead: a project's name carries `N files` (plus `M with dependencies` when it
    inherits any), each include pattern carries what it matched, and each exclude pattern
    what it removed. **A working configuration is not a problem and must not appear in the
    Problems panel** — that is the whole point of the move, and
    [test/config-hints.test.mjs](test/config-hints.test.mjs) asserts the document publishes
    no diagnostics at all.
    Two things this needs that are easy to get wrong. **The counts cannot be recomputed when
    the editor asks**: patterns are expanded once and the resulting `ConfigFile` is cached, so
    `evaluate_patterns()` records them on `Project::pattern_matches` as it goes. And **there
    is no position information in a parsed config** — it is plain nlohmann JSON — so
    `ConfigDocument` in [artic-lsp/src/server.cpp](artic-lsp/src/server.cpp) locates each
    literal by scanning the file, exactly as `publish_config_diagnostics()` does, and marks
    every occurrence it has used so two projects sharing a `files` pattern each get their own
    hint instead of both landing on the first line.
    `Workspace::projects_of_config()` deliberately never parses anything: `configs_` is keyed
    by canonical path and a config nothing has opened yet simply yields no hints.

### Deferred

- **Doc comments** — the lexer discards `///`, so nothing can surface documentation in
    hover or completion. Capturing them is a fork change under the `ENABLE_LSP` policy, and
    it must not alter tokenisation for the standalone compiler.
- **Performance and protocol modernisation** — text sync is `Full` (the whole buffer on every
  keystroke), every change recompiles the entire project and rebuilds the `NameMap` from
  scratch, diagnostics are push-only, and there is no `$/progress` during a compile.
  **No performance problem has actually been observed**, so this is not scheduled. Revisit
  only if a change here is non-intrusive and carries no crash risk.
- **Minor gaps** — semantic tokens have no delta support and never emit the `deprecated` or
  `defaultLibrary` modifiers; call hierarchy is derivable from `references_of` plus an
  enclosing-function lookup; rename performs no collision, shadowing or valid-identifier
  check.
- **Comments in `artic.json`** — the config is read with `is >> j`, i.e. nlohmann's defaults, so
  `//` is a parse error. The README examples were written with comments for years and were
  therefore not copy-pasteable. Enabling `ignore_comments` is one line, but the document selector
  in [vscode/src/extension.ts](vscode/src/extension.ts) matches `language: 'json'`, so the file
  would also need a `jsonc` filename association to stop VS Code's own validator flagging them,
  and the selector would have to accept both languages or config diagnostics stop arriving.
  Documented as a limitation in the README instead.

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
- **Cycle detection must run one DFS over all roots, not one per dependency edge.** The
  original loop seeded `detect_cycle(project.name, dep)` with the arguments swapped relative
  to the parameter list, cleared `visited`/`rec_stack` per edge, and erased the offending
  entry with `std::remove` while a range-for over that same vector was live — undefined
  behaviour whenever a project depends on itself. Guarded by
  [test/circular-dependencies.test.mjs](test/circular-dependencies.test.mjs).
- **The project registry is per server session, not per workspace.** A duplicate project
  name is warned about and *ignored*, so two staged workspaces in one test suite must not
  reuse a name — the second one's projects silently do not exist.
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
- **Optional includes (`"path?"`) mean "may be absent", not "ignore errors".** A missing
  optional include is silent; one that exists but is broken is reported normally. The single
  place that reports a missing include is the eager loop in `instantiate_config_json()` —
  `Workspace::instantiate_config()` returns `nullptr` silently for a path that does not
  exist, because the lazy lookup in `find_project_in_config_using_file()` reaches it again
  later with no idea whether the include was optional.

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

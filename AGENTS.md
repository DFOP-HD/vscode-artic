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

Outputs `artic-lsp/buildGcc/bin/artic-lsp.exe`.

Also build the standalone compiler — needed to validate `.art` fixtures:

```powershell
cmake --build artic-lsp/buildGcc --target artic --parallel   # -> buildGcc/bin/artic.exe
```

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

**All `.art` / `.impala` fixture code must be written by us** — never copied from other
AnyDSL repos (licensing). Sample projects such as `D:/anydsl-metaproject/stincilla` may be
read for reference only. Every fixture that is supposed to be valid must be proven valid
with the real compiler before being relied on in a test:

```powershell
artic-lsp/buildGcc/bin/artic.exe test/fixtures/<project>/*.art
```

When writing Artic code, follow [.github/skills/artic-language/SKILL.md](.github/skills/artic-language/SKILL.md).

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
- LSP-only additions to artic must be guarded with `#ifdef ENABLE_LSP`. The define is applied
  in [artic/src/CMakeLists.txt](artic/src/CMakeLists.txt) when the `artic-lsp` target exists.
- The standalone `artic` compiler must keep building **with and without** `ENABLE_LSP`.
  It previously did not — `main.cpp` included `server.h`, which only exists in `artic-lsp/`.

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
7. **Docs updated** — user-facing behaviour in [README.md](README.md), agent-facing facts here.
8. **Tree clean** — `git status` shows only intended files; no build output, no scratch dirs.

## Backlog

Ordered. Keep status markers current.

1. **Test harness** — *done*. Dependency-free LSP protocol tests + self-authored fixtures
   in `test/`, plus the artic CTest suite wired up via `include(CTest)`.
2. **Config diagnostics are unreliable** — *in progress*. Fixed so far: source diagnostics
   were published under a lowercased path on Windows (`tracked_file()` used the lowercased
   string as the file's identity), so the editor could not match them to the open document.
   Still open: `ConfigLog::error("...{}", x)` does not format — the `{}` reaches the user and
   `x` is silently treated as the search context; `ConfigLog::file_context` is a single
   mutable field that recursive config includes overwrite without restoring, misattributing
   messages to the wrong config file; messages with an empty `file_context` are dropped by
   `publish_config_diagnostics()`; and diagnostics are never cleared for a file that no
   longer has messages. See `publish_config_diagnostics()` in
   [artic-lsp/src/server.cpp](artic-lsp/src/server.cpp).
3. **Support `.sln` files in the config** — today only `.vcxproj` can be listed; a solution
   should be expandable to its projects. See `instantiate_config()` in
   [artic-lsp/src/workspace.cpp](artic-lsp/src/workspace.cpp) and `parse_vcxproj()` in
   [artic-lsp/src/config.cpp](artic-lsp/src/config.cpp).
4. **Clean up the artic fork** — guard all LSP-only additions behind `ENABLE_LSP`, and split
   out the genuinely upstreamable fixes (chiefly the type-checker error tolerance that lets
   later stages survive a failed earlier stage) so they can be PR'd to AnyDSL/artic.
   The unmerged `origin/error-tolerance` branch is relevant prior art.
5. **Cursor editor support** — make the extension installable and functional in Cursor
   (Open VSX packaging, `engines.vscode` range, avoid VS Code proprietary APIs).
6. **Restore Linux support** — development has moved to Windows and Linux has regressed.
   Re-verify the build (`artic-lsp/build.sh`), the packaging scripts (`vscode/build-lsp.sh`,
   `vscode/package.sh`), path handling, and run the test suite there.

## Gotchas

- `artic-lsp/src/main.cpp` ignores `argv`; the extension still passes `--lsp`, which is a no-op.
- `libartic` exports `ENABLE_LSP` as a **PUBLIC** compile definition, so it leaks to every
  consumer of the library, including the `artic` executable.
- Build directories are ignored via `build*/`. Keep scratch builds under that pattern.

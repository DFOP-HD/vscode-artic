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
   `+ 1 files | total matches: 1 files:` against its `files` pattern. It is deliberate
   (an "N files matched" annotation) but it renders as noise in the Problems panel and the
   wording looks malformed. Decide whether it should be an inlay hint instead.
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
   configuration"). It scans for `.sln`, then `build.ninja`, then `.vcxproj`, keeping only
   files that mention artic and skipping anything under a directory already covered by a
   stronger match — otherwise a solution and the projects it lists both get included and every
   project is reported as a duplicate. Detected entries are written as **optional** includes
   (trailing `?`) because a build directory does not exist on a fresh checkout.
   Guarded by [test/ninja-config.test.mjs](test/ninja-config.test.mjs) and
   [test/optional-includes.test.mjs](test/optional-includes.test.mjs).
5. **Clean up the artic fork** — guard all LSP-only additions behind `ENABLE_LSP`, and split
   out the genuinely upstreamable fixes (chiefly the type-checker error tolerance that lets
   later stages survive a failed earlier stage) so they can be PR'd to AnyDSL/artic.
   The unmerged `origin/error-tolerance` branch is relevant prior art.
6. **Cursor editor support** — make the extension installable and functional in Cursor
   (Open VSX packaging, `engines.vscode` range, avoid VS Code proprietary APIs).
7. **Restore Linux support** — development has moved to Windows and Linux has regressed.
   Re-verify the build (`artic-lsp/build.sh`), the packaging scripts (`vscode/build-lsp.sh`,
   `vscode/package.sh`), path handling, and run the test suite there.

## Gotchas

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

- **A file's identity is its canonicalised path string.** `workspace::canonical_path()` in
  [artic-lsp/src/workspace.cpp](artic-lsp/src/workspace.cpp) is the single place that
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

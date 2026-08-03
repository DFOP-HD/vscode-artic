# Implementation notes

How the server actually works, and what each part has to get right. This is the
contributor-facing counterpart to the two other documents in this repo:

| Document | Audience | Answers |
| -------- | -------- | ------- |
| [README.md](../README.md) | users | what the extension does and how to configure it |
| [AGENTS.md](../AGENTS.md) | contributors and agents | how to build, test, ship and change the repo |
| this file | contributors and agents | how a feature is implemented and why |

Everything below describes shipped behaviour. Plans live in
[AGENTS.md](../AGENTS.md#plan); nothing here is a proposal.

---

## Compilation

`Compiler::compile_files()` in [artic-lsp/src/compile.cpp](../artic-lsp/src/compile.cpp) drives
`Lexer -> Parser -> NameBinder -> TypeChecker -> Summoner` over **every file of the project at
once**. Artic has no import mechanism — files are concatenated on the command line, there is no
`#include`, and `use` only aliases a module that is already in the program. That is why a
configuration file exists at all, and it bounds every feature here: the set of files is an input,
never something the server can infer from the source.

Two consequences that most handlers have to deal with:

- **`program` is the concatenation of every file in the project.** Anything that answers a
  question about *one document* — document symbols, document highlight, inlay hints — has to
  filter on `loc.file`, or it reports the whole project.
- **The compile is error-tolerant on purpose.** `exclude_non_parsed_files` is `false` outside
  safe mode, so the partially parsed AST survives and the name binder, type checker and summoner
  still run over it. `name_map` is therefore populated for everything that did parse, which is
  what keeps navigation, hover, completion and the outline alive while the user is mid-edit.
  Guarded by [test/incomplete-code.test.mjs](../test/incomplete-code.test.mjs).

### Recovering from a parse error

Surviving a parse error is not the same as staying useful after one. A break in the *middle* of a
file — which is where an editor puts one — used to cost an error per token to the end of the file,
and every declaration below the break was lost: for `fn a`, an unfinished
`fn broken(x: i32) -> i32 { x +`, then `fn b` and `fn c`, the parser reported **eight** errors, all
but the first pointing at `fn b`, and neither `b` nor `c` reached the AST. In the editor that is
the outline, hover, completion and go-to-definition disappearing for everything below the line
being typed.

Three amplifiers, fixed in [artic/include/artic/parser.h](../artic/include/artic/parser.h) and
[artic/src/parser.cpp](../artic/src/parser.cpp):

- **`expect()` consumed on mismatch.** It called `next()` unconditionally, so a missing token also
  ate the token that would have resynchronised the parse — usually the `fn` opening the next
  declaration. It now leaves a token that is not what was expected in place. Every loop that could
  have depended on it consuming was audited: `parse_list` still terminates because a token that is
  neither a separator nor a terminator breaks it, and the block, module and top-level loops all
  dispatch on the token themselves.
- **`parse_error_decl()` skipped exactly one token**, so it re-reported on every token of whatever
  followed. It now consumes one and then calls `skip_to_decl()`, which runs to the next token that
  can begin a declaration (`fn struct enum type mod static implicit use let #`). Braces are
  counted, so a declaration nested inside the body being skipped does not end the skip early, and
  a `}` at depth zero stops it because it belongs to an enclosing block.
- **Nothing suppressed a re-report about a token already complained about.** `expect()` failing at
  a token and then the recovery path reporting the same token says nothing the first message did
  not. `reported_at()` records the last position reported and every recovery point checks it.

Eight errors become three, each on a distinct token, and the declarations below the break are
parsed at top level again. This is an upstreamable change rather than an LSP-only one, so it takes
no `ENABLE_LSP` guard — and it moved no expected-output file in the compiler's own suite, which
stays at 145/145. Guarded by the *a break in the middle of a file* block in
[test/incomplete-code.test.mjs](../test/incomplete-code.test.mjs), which bounds the parse errors,
requires them to be at distinct positions, and asserts the declarations below the break are still
in the outline and still reachable by go-to-definition.

The type errors that follow a parse error were a **separate** amplifier, downstream of this one.
An `ErrorExpr`, `ErrorType`, `ErrorDecl` or `ErrorPtrn` had no inference rule of its own, so it
fell through to `Node::infer` and got "cannot infer type for expression" — a message about a node
whose only problem had already been reported, and one that said "expression" even for a broken
type or pattern. All four now infer to the error type in
[artic/src/check.cpp](../artic/src/check.cpp), which is enough on its own: `should_report_error()`
already suppresses any message about a type containing the error type, so the silence propagates
outward without a flag having to be threaded anywhere. That is an inference rule the nodes were
missing, not a suppression, so it is upstreamable and unguarded, and it moved no expected-output
file either.

The case above therefore went 13 diagnostics → 6 (parse recovery) → **4**. The one error left over
the parser's own three is the *name binder*: `expected ';', but got 'cascade_b'` is followed by
`unknown identifier 'cascade_b'`, because the parser mis-read the name of the next function as an
identifier expression and the binder then resolved it honestly. Suppressing that would mean not
binding names in a subtree that follows a parse error, and the binder is what fills `name_map` —
i.e. it would cost go-to-definition exactly where the editor needs it most. Left alone
deliberately.

`ls::NameMap` ([artic/include/artic/name_map.h](../artic/include/artic/name_map.h)) is the index
every navigation feature resolves through: `find_decl_at`, `find_ref_at`, `find_decl`, `find_refs`.

---

## Configuration

### Discovery

`find_config_recursive` walks up from the file's directory calling `find_config_in_dir`, which
looks for `.artic-lsp` first and then `artic.json`. It stops at the filesystem root.

Inside a config, `find_project_in_config_using_file` tries the config's own projects, then
recurses into its includes, then falls back to `default-project`. When no config is found at all,
`Workspace::collect_project_files` falls through to `{tracked_file(file)}` — the file is compiled
alone.

**`.artic-lsp` is a dotfile, so `path::extension()` is empty for it.** Both `get_file_type()` and
`instantiate_config()` originally keyed off the extension and silently never recognised the file.
They match on the filename now.

### Formats

`artic.json` and `.artic-lsp` share the JSON schema documented in the
[README](../README.md#workspace-configuration-file). Three build-system formats are also parsed
directly, all in [artic-lsp/src/config.cpp](../artic-lsp/src/config.cpp):

- **`.vcxproj`** — `parse_vcxproj()` reads the artic invocation out of the `<Command>` element.
  The element holds a shell command line wrapped in XML, so each line is run through `xml_text()`
  (tags dropped first, then `&quot;` and friends decoded — the other order would mistake a `&lt;`
  in the command for a tag) before it is tokenised. The first token that names an artic executable
  wins, and arguments are taken until the first one starting with `-`.
- **`.sln`** — `parse_sln()` expands a solution to the `.vcxproj` files it lists. Solution
  *folders* reuse the same `Project(...)` syntax and are skipped, and so is any project without an
  `artic.exe` build command: a CMake-generated solution is mostly `ZERO_CHECK`/`ALL_BUILD` noise
  and must not produce diagnostics. That distinction is `ConfigPath::is_implicit` — an include the
  user wrote is reported on, one derived from a solution is not. Projects are instantiated lazily
  by `instantiate_config_sln()` in [artic-lsp/src/workspace.cpp](../artic-lsp/src/workspace.cpp)
  and the result is cached **even when empty**, so a solution with hundreds of entries parses each
  `.vcxproj` at most once.
- **`build.ninja`** — `parse_ninja()` turns every target whose `COMMAND =` line invokes artic into
  a project named after the generated file. The command is split on ` && ` so the `cd /D <dir>` of
  CMake's `cmd.exe /C "..."` wrapper becomes the base directory for relative source paths. The
  first artic invocation wins, and arguments are taken until the first one starting with `-`.

**A build command is a shell command line, so a path containing a space is quoted.** All three
formats therefore tokenise with `text::split_command_line()`, which treats a quoted run as one
token, rather than splitting on whitespace and stripping stray quotes afterwards. The old
behaviour turned `"C:\my sources\main.art"` into two paths that do not exist, so the project
expanded to nothing and every cross-file reference became an unknown identifier.

That only works once the shell wrapper is off: a generated command is usually
`cmd.exe /C "<real command>"`, whose outer quote pair spans the whole line — including the ` && `
separators — so a quote-aware split would return it as a single token. `unwrap_shell_command()`
removes it first, and only when the remainder after a `/C`, `/c` or `-c` token is *fully* quoted,
so a real argument is never mistaken for a wrapper. Guarded by
[test/paths-with-spaces.test.mjs](../test/paths-with-spaces.test.mjs).

**An executable or source path is recognised by its own base name, not `fs::path`.** A build file
spells paths with the separator of the platform that generated it, which is not necessarily the
one we are parsing on — `fs::path("D:\\any dsl\\bin\\artic.exe").stem()` is the entire string on
Linux. `is_artic_executable()` and `is_artic_source()` split on both separators themselves.

**A backslash is a separator only on Windows, and `.sln`/`.vcxproj` always use one.** Both MSBuild
parsers go through `paths::from_msbuild_path()`. `build.ninja` deliberately does **not**: it is
generated per platform, so a literal backslash there is only ever a separator on Windows, where it
already works.

### Optional includes

A trailing `?` on an include path means **"may be absent"**, not "ignore errors". A missing
optional include is silent; one that exists but is broken is reported normally. The single place
that reports a missing include is the eager loop in `instantiate_config_json()` —
`Workspace::instantiate_config()` returns `nullptr` silently for a path that does not exist,
because the lazy lookup in `find_project_in_config_using_file()` reaches it again later with no
idea whether the include was optional.

### Detecting a configuration from the workspace

`Artic: Detect workspace configuration` scans for `.sln`, then `build.ninja`, then `.vcxproj`, and
skips anything under a directory already covered by a stronger match — otherwise a solution and
the projects it lists both get included and every project is reported as a duplicate.

**A `.sln` never contains the word "artic".** It holds nothing but project names and GUIDs, so it
cannot be filtered by content the way a `.vcxproj` or a `build.ninja` can; it qualifies when one of
the projects it references does. Getting this wrong made stincilla detect all 57 of its `.vcxproj`
files and miss `STINCILLA.sln`.

The selection logic is pure and lives in [vscode/src/detect.ts](../vscode/src/detect.ts)
(`solutionProjectPaths`, `selectWorkspaceConfigFiles`) so it can be tested without VS Code.
Detected entries are written as **optional** includes, because a build directory does not exist on
a fresh checkout.

### Reporting on a config document

- **Errors** are published as diagnostics by `publish_config_diagnostics()`. It may only clear what
  the current pass evaluated — see the gotcha in [AGENTS.md](../AGENTS.md#gotchas).
- **A working configuration produces no diagnostics at all.** The project overview is delivered as
  inlay hints instead: a project's name carries `N files` (plus `M with dependencies` when it
  inherits any), each include pattern carries what it matched, and each exclude pattern what it
  removed.

Two things this needs. **The counts cannot be recomputed on demand** — patterns are expanded once
and the resulting `ConfigFile` is cached, so `evaluate_patterns()` records them on
`Project::pattern_matches` as it goes. And **a value has to be findable in the document again**,
which is what `JsonSource` is for.

#### Getting back to where a value was written

A parsed value has to be able to say where it came from, or a message about it can only be placed
by searching the document text — which reports *every* textually identical string:

- A project named after the folder it lives in (`"name": "lib", "folder": "lib"`) produced **two**
  identical "folder does not exist" diagnostics, one of them on the perfectly correct name.
- A project referenced as a dependency before it is declared had its file-count hint placed on the
  reference, because that occurrence comes first in the file.
- A syntax error had no context at all and landed at `0:0`.

nlohmann/json records exactly that, as [diagnostic positions](https://json.nlohmann.me/api/macros/json_diagnostic_positions/):
with `JSON_DIAGNOSTIC_POSITIONS` defined, every value parsed out of a document carries
`start_pos()` and `end_pos()`. The option is turned on for the whole build in
[artic-lsp/cmake/Dependencies.cmake](../artic-lsp/cmake/Dependencies.cmake), because it changes the
layout of `basic_json` and every translation unit has to agree; nlohmann's own CMake turns it into
an `INTERFACE` compile definition on `nlohmann_json::nlohmann_json`, so every consumer of the
target sees it. [artic-lsp/src/json_source.cpp](../artic-lsp/src/json_source.cpp) has an `#error`
guard so a build where it failed to propagate fails loudly instead of silently losing positions.

Those positions are byte offsets into the input, so `JsonSource` keeps the text and converts:
`position_of()` maps an offset through a line-start table, `value_range()` turns a value into an
`lsp::Range`, and `member_range()` extends that range back over the `"name":` that introduced it —
skipping whitespace, then a `:`, then the closing quote, then back to the opening quote — so
`"folder": "src"` is reported rather than a bare `"src"`. An array element has no such prefix and
is returned unchanged. `position_of_byte()` handles the one position nlohmann reports differently:
`parse_error::byte` is 1-based and one past the offending character.

`ConfigParser` builds one `JsonSource` per config and hands it to `ConfigLog::scoped_file()`, so
`log.error_at(log.member_range(value), ...)` resolves **while the text that produced it is still in
hand** — a config edited after being parsed cannot move a message onto the wrong line.
`Project::name_range` and `PatternMatch::range` carry the same information through to the inlay
hints. Text search is kept as the fallback, for build files (which have no positions at all) and
for a range that no longer describes the value it was recorded for; `ConfigDocument::take()`
re-checks the range against the current text before trusting it.
`Workspace::projects_of_config()` never parses anything: `configs_` is keyed by canonical path, so
a config nothing has opened yet simply yields no hints.

### Which project a file is in

A file with no configuration above it is compiled **alone** — `collect_project_files()` falls
through to `{tracked_file(file)}`. That is often the right answer (most `.art` files in the AnyDSL
checkouts really are independent single-file programs), but it used to be completely silent: every
cross-file reference became "unknown identifier" and nothing said why.

`Workspace::project_of_file()` answers the same question `collect_project_files()` acts on, in a
form that can be shown to a user: it goes through `discover_project_for_file()` — the same
discovery a compile does — and returns a `FileProject` with

| Provenance | Meaning |
| ---------- | ------- |
| `Config` | A project lists this file. `file_count` is the project's own files plus its dependencies'. |
| `DefaultProject` | The file is listed nowhere, but a `default-project` applies; a compile adds this file to that project, so `file_count` includes it. |
| `DetectedBuildFile` | No configuration was found, but a build file in the workspace lists the file — see [Zero-config projects](#zero-config-projects). |
| `SingleFile` | Nothing claims the file. It is compiled on its own. |

**It is exposed through `workspace/executeCommand`, not a request of our own.** A custom method
would need a client that speaks it; every LSP client already speaks `executeCommand`. The command
is `artic.projectForFile`, it takes a single document URI string, and it answers a JSON object
(`file`, `provenance`, `name`, `origin`, `fileCount`) or `null`. It triggers no compile.

The argument arrives as a URI *string* and has to be turned into a path with
`lsp::FileUri(lsp::Uri::parse(...)).path()`. Plain `Uri::path()` keeps the leading slash a Windows
drive path carries (`/D:/...`), which canonicalises to something that matches no config — the
symptom is every file reporting `single-file`.

On the editor side, [vscode/src/extension.ts](../vscode/src/extension.ts) queries this on every
active-editor change and after every save, and renders it in a status bar item; the wording lives
in [vscode/src/project-status.ts](../vscode/src/project-status.ts) so it can be unit tested without
a VS Code instance. A single-file compile gets `statusBarItem.warningBackground`. That entry is the
whole UI: detection deliberately raises no notification, because it applies to every file and a
popup per file is not a feature.

### Zero-config projects

A CMake- or MSBuild-driven checkout needs no `artic.json`. When the walk up the directory tree
finds no configuration, `Workspace::find_detected_project()` scans the workspace roots for build
files and treats what it finds as a configuration that was never written down.

**Order matters and is not negotiable: a configuration the user wrote always wins.** Detection only
runs after `find_config_recursive()` has returned nothing, so adding an `artic.json` can never be
overridden by a build file sitting in the same tree.

**The roots come from `initialize`.** `workspaceFolders`, falling back to the deprecated `rootUri`,
which every client still sends. Both are plain `Uri`s, so the same `FileUri` conversion applies —
without it every root is `/D:/...`, matches no file, and detection silently never happens. With no
root at all (a single file opened outside a folder) there is nothing to scan and the fallback stays
single-file, which is also why a file outside every root is never swept into a project.

**`config::detect_build_files()` mirrors `selectWorkspaceConfigFiles` in
[vscode/src/detect.ts](../vscode/src/detect.ts)**, which does the same job for the explicit
"Detect workspace configuration" command: strongest match first, a `.sln` supersedes the projects
it lists and a `build.ninja` supersedes the projects next to it, because including both would
define every project twice and the duplicate is dropped rather than merged. A `.sln` never mentions
artic itself, so it qualifies when one of the `.vcxproj` files it lists does.

**The scan is bounded and cached**, because it runs on the miss path of *every* file that has no
config above it, and an AnyDSL checkout with LLVM in it is hundreds of thousands of files. It caps
depth, directory count and file count, skips hidden directories and the usual output directories,
and `detected_config_for_root_` caches the **miss** as well as the hit — otherwise a workspace with
no build files would be walked again for every file opened in it.

The result is a synthetic `ConfigFile` whose includes are the detected build files, marked
`is_implicit` so a build file that turns out not to build artic after all is not reported as a
problem the user can act on, and named after a path that does not exist so no diagnostic can ever
be attributed to it. The projects reached through it are recorded in `detected_projects_`, which is
the only reason `project_of_file()` can tell `DetectedBuildFile` apart from `Config`.

**What is deliberately *not* done: inferring a project from a directory, or from a glob in a
setting.** Both were proposed and both were declined by the owner; the evidence is below so nobody
proposes them a third time.

**What bounds the whole question: artic has no import mechanism.** Files are concatenated on the
command line, there is no `#include`, and `use` only aliases a module that is already in the
program. So a project cannot be inferred from the source the way it can in Rust or Cargo — that is
precisely why a config exists. What *can* be recovered is the build system's own answer, and that
is what detection does.

Measured against `D:/anydsl-metaproject`, 690 `.art`/`.impala` files outside the LLVM trees:

| Tree | Files | Shape |
| ---- | ----: | ----- |
| `impala/test/**` | 395 | one independent program per file, many deliberately failing |
| `artic/test/**` | 151 | same — `test/simple` and `test/failure` are 71 files each |
| `stincilla/**` | 75 | combinatorial: one application × one mapping × one of 7 `backend_*.impala` |
| `runtime/platforms/artic/**` | 39 | one library, listed file by file in `artic.json` |
| `srl-cross-compile-toolchain/tests/src` | 16 | |
| `spmv-benchmarks/artic-sources` | 12 | |
| `artic-utils/artic` | 2 | library, depends on `runtime` |

Two findings, both hard:

- **A directory is not a project.** 70 of the 71 files in `artic/test/simple` compile cleanly on
  their own; compiled together they produce **141 errors**, all redefinitions (`test`, `foo`, `E`,
  …). The same holds for `impala/test/**`. That is ~550 of the 690 files, and for every one of them
  **the single-file fallback is already the correct answer.**
- **A real project is combinatorial, and only the build system knows the combination.**
  `stincilla/` holds `jacobi.impala`, `gaussian.impala`, `bilateral.impala` and `matmul.impala`
  side by side with seven mutually exclusive `backend_*.impala` and two mutually exclusive
  `mapping_*.impala`. No directory rule can pick one of each; `stincilla/build/*.vcxproj` does.

So neither *whole workspace* nor *nearest source root* is a safe implicit project — both are wrong
for the two dominant layouts. The owner's own config (`D:/anydsl-metaproject/artic.json`) confirms
it from the other side: it declares `runtime` and `artic-utils` as **explicit file lists** with a
`folder` root and a dependency edge, includes `stincilla/build/jacobi.vcxproj` for the application,
and gives everything else a `default-project` that just inherits the two libraries. A glob forwarded
through `initializationOptions` was considered as an escape hatch and declined too: it is a config
file with a worse name, and it cannot express the dependency edge that made the real config work.

---

## Language features

### Shared cursor resolution

`decl_at()` in [artic-lsp/src/server.cpp](../artic-lsp/src/server.cpp) resolves the position with
`find_decl_at`, falling back to `find_ref_at` + `find_decl`. Hover, definition, type definition and
document highlight all go through it.

Rendering goes through [artic-lsp/src/ast_render.cpp](../artic-lsp/src/ast_render.cpp)
(`print_to_string`, `print_param_list`, `render_decl`, `symbol_kind_of`), so hover, document
symbols, workspace symbols, code lens and completion cannot disagree about what a declaration is
or how it reads.

### Hover

`render_decl()` **must never call `decl.print()`** — every `Decl::print` overload emits the body,
and `StructDecl`'s emits every field, which is a wall of text in a popup. It prints the keyword and
identifier itself and delegates only to the sub-nodes that belong in a signature (`type_params`,
`fn->param`, `fn->ret_type`, `aliased_type`), falling back to the inferred `Node::type` where the
source has no annotation (`let` bindings, inferred return types).

`fn->param` brings its own parentheses only when it is a tuple pattern — that is what upstream's
`print_parens()` handles, and it is `static` in `print.cpp`, so the check is duplicated rather than
exported.

### Document symbols

One pass over `compile->program->decls` builds a hierarchical tree via `make_document_symbol()` /
`collect_document_symbols()`; `detail` reuses the hover renderer. Entries are filtered on
`decl.loc.file`. Fields of a tuple-like struct are skipped — they are named `0`, `1`, … and say
nothing. `implicit` is the one declaration with no identifier; it is named `"implicit"` and
detailed with its type.

### Definition, document highlight, type definition, implementation

- **Definition** on a declaration returns *the declaration itself*. It used to answer with all
  references, so Go-to-Definition on `fn scale` jumped into whichever file happened to call it.
- **Document highlight** is where `find_refs(decl)` went instead. Results are filtered to the
  requested document — the whole project is compiled at once and a range is only meaningful in the
  file it was asked for. The declaration is reported as `Write`, every reference as `Read`.
- **Type definition** unwraps through `declaring_type_decl()` until a user-written declaration is
  reached: `TypeApp` (a generic instantiation points at `applied`), `AddrType` (both `PtrType` and
  `RefType`), `ArrayType`, `ImplicitParamType`, then `StructType` / `EnumType` / `TypeAlias` /
  `TypeVar` / `ModType`, each of which carries a `decl`. A primitive resolves to nothing and the
  handler answers `null` rather than guessing.
- **Implementation** answers for `summon`. It needed no fork change: `ast::SummonExpr` already
  carries an upstream, unguarded `const Expr* resolved`, assigned in `SummonExpr::resolve_summons`
  and relied on by `emit.cpp` and `print.cpp`. `summon_at()` finds the innermost `SummonExpr`
  covering the cursor and answers with `resolved->loc`.

  Two things worth knowing: **an omitted implicit argument is a `SummonExpr` too** —
  `TypeChecker::coerce` synthesises one per `ImplicitParamType` in the callee's parameter tuple, so
  `apply(v)` resolves even though nothing in it is written down — and that synthesised node
  **inherits the argument's location**, so the cursor has to be inside the parentheses rather than
  on the callee. An implicit *parameter* resolves to the `PathExpr` that `IdPtrn::to_expr()`
  synthesises, which carries the pattern's own location, so that case lands on real source too.

### Selection range

`spine_at()` walks `program->decls` with a `TraverseFn` that returns `false` for any node not
covering the cursor, so one path is visited rather than the whole program. Two details the LSP does
not spell out but clients depend on:

- **Every requested position needs an answer.** `params.positions` is plural and a client lines the
  results up with its requests by index, so a position no node covers still gets an empty range at
  the cursor.
- **Consecutive ranges must differ.** Many nodes wrap a child of exactly the same extent, and a
  duplicate makes the user press Shift+Alt+Right twice for one visible step.

The chain is built outermost-first so each node becomes the `parent` of the previous.

### Signature help

**The call site is found in the source text, not in the AST.** Signature help fires on a
half-written call like `dot(a,`, where the parser has produced an error node rather than a
`CallExpr` — an AST walk finds nothing exactly when the feature is wanted.

`enclosing_bracket()` scans backwards for the innermost unclosed bracket, skipping comments and
string/char literals so a `(` inside one does not open a frame, and counts the commas at that
bracket's own nesting level to get the active parameter index. `callee_before()` takes the
identifier in front of the `(`, skipping a `[...]` type-argument list, and the declaration is
resolved at the identifier's **first** character so the lookup does not depend on how the lexer
spells the end of a token.

`render_signature()` prefers the *declaration* over the type, because only a declaration carries
parameter names; an `OptionDecl` has to be rendered from the declaration in any case, since an enum
option's type is its payload rather than a function type. Generic functions are unwrapped through
`ForallType::body`. `activeParameter` is clamped to the last parameter — an out-of-range index
makes VS Code highlight the *first* one, which is worse than sticking to the last.

### Completion

The default branch collects declarations by walking each `local_scopes` entry and pruning any
nested scope-introducing node: `BlockExpr`, `FnExpr`, `CaseExpr` and `LoopExpr`. Every scope that
encloses the cursor is a `local_scopes` entry in its own right, so pruning loses nothing and
descending only ever duplicates bindings or invents ones that are out of scope.

**`ForExpr::traverse_children` bypasses the desugared closure**: it yields `lambda->param`, `iter`,
`call->arg` and `lambda->body` directly, so the `FnExpr` node is never visited and pruning `FnExpr`
alone does nothing for a `for` loop — `LoopExpr` has to be pruned too. To keep the loop variable
available *inside* the loop, the outer walk registers `loop_binding()` (which unwraps
`for_expr->call->callee` → `CallExpr` → `arg` → `FnExpr` → `param` with `isa`, never `as`) and a
`CaseExpr`'s pattern as scopes, and `FnExpr::param` replaces the old `FnDecl`-only parameter push.

`use`-path completion goes through the same `ast::Path` branch as `a::b`.

**A completion the editor never asks for does not exist.** The widget opens either on a declared
trigger character or when the client decides on its own — and only `.` and `:` were declared, so
`let a = ` produced nothing while `let a = .` produced a full list. The user-visible symptom was
"completion does not work", but the server had always been answering correctly: probing the
protocol at that cursor returned all 31 items including the function the user was after. The
trigger set is now every character after which an expression, a type or a name can begin —
including a space — and [vscode/package.json](../vscode/package.json) adds a `configurationDefaults`
block enabling `editor.quickSuggestions` for the `artic` language, because a trigger character
cannot cover typing the first letters of an identifier. Both halves are needed: the setting is only
a *default* and a user's global value outranks it, so the trigger characters are the load-bearing
part. Note `(` and `,` are also signature-help triggers; a character may belong to both providers.

**`isIncomplete` was `false`, and the list is position-dependent.** `only_show_types`,
`inside_block_expr`, `top_level` and `local_scopes` are all computed from the cursor, so a client
that cached the first response and only re-filtered it client-side was showing a list built for an
earlier position. It is `true` now, and the handler is cheap enough for that — a warm request costs
1–3 ms, well inside the budget for re-asking on every keystroke.

**Nothing carried a `sortText`, so the client sorted by label.** That threw away the order the
handler had gone to some trouble to build, and left `my_function()` at rank 22 of 31 — behind
`addrspace(...)`, `asm`, `bool`, `break`, `continue`, `else`, `f16`, `f32`, `f64`, `for` — i.e.
off-screen in a twelve-row widget. A window that *did* open therefore still looked like it was
offering nothing. `finish()` assigns a zero-padded rank in three groups: bindings from the scopes
enclosing the cursor, then the module's own declarations, then keywords and primitive types.
Insertion order is preserved within a group by a `stable_sort`, and the locals span is reversed
first because the traversal fills `local_scopes` outermost-first while the binding that shadows is
the innermost one. Every return path goes through `finish()` — the two `std::reverse` calls it
replaced were dead code, because without a `sortText` the client re-sorted the result anyway.

### Inlay hints

Type hints and parameter-name hints share the handler.
`collect_parameter_hints()` walks the program with a `TraverseFn` that prunes anything outside the
requested document, resolves every `CallExpr`'s callee, and labels each argument with the parameter
it binds. Three things it must get right:

- **The names come from the declaration's `Ptrn`, not from the `FnType`.** A `FnType` in artic is a
  tuple type and carries no parameter names at all.
- **`fn dot(a: Vec2, b: Vec2)` parses as a `TuplePtrn` of `TypedPtrn`s**, each wrapping the
  `IdPtrn` that holds the name, so `parameter_name()` unwraps `TypedPtrn` recursively. Reading
  `IdPtrn` directly yields an empty name for every annotated parameter, i.e. for every parameter
  anyone writes.
- **Only a positional match is labelled.** `tuple->args.size() != names.size()` bails out, because
  a function whose single parameter is a tuple receives the whole tuple as one argument and lining
  names up with elements would be a guess.
- **A one-argument call has no element location to hang the hint on.** `parse_tuple_expr()`
  collapses a one-element tuple into the expression it contains but overwrites that expression's
  location with the parenthesised range, so `call->arg->loc.begin` is the `(` and not the argument.
  Placing the hint there renders it *outside* the call — `length_squared` `v:` `(c)`, and with an
  explicit type argument it lands between the `]` and the `(`, which is where it was reported. The
  sole-argument branch therefore steps one column past the location it is given. The multi-argument
  branch must not: a `TupleExpr`'s elements keep their own locations, which is why every existing
  test passed while every single-argument call was wrong.

An `ImplicitParamPtrn` yields no name — it is summoned rather than written.
`argument_repeats_name()` suppresses the hint when the argument is a plain path or projection
spelling the parameter's own name, so `add(a, b)` gets no hints.

### Workspace symbols

**The index parses, it does not compile.** `SymbolIndex` in
[artic-lsp/src/symbol_index.cpp](../artic-lsp/src/symbol_index.cpp) runs `Lexer` + `Parser` over
each project's own files and copies out name, container path, kind and location. Name binding, type
checking and summoning are what make a compile expensive and none of them contributes anything a
symbol picker shows. This is also what sidesteps the server holding a single
`std::optional<Compiler>`.

Three things it has to get right:

- **Everything the harvest sees dies with it.** The `Arena` holding the AST and the `Locator`
  owning every `Loc::file` string are both local to `harvest_file()`, so an `IndexedSymbol` may
  keep no pointer into either — the `lsp::Location` is built from `File::path` while they are still
  alive.
- **Parse errors go to a `std::ostream(nullptr)`.** The index runs over files that are usually
  mid-edit and their diagnostics are the compile's business.
- **The result is cached per project.** A client re-issues `workspace/symbol` on every keystroke.
  `SymbolIndex::invalidate()` drops every project containing a changed file, and any
  `on_config_changed` / `reload_workspace` throws the whole index away, since it is keyed by
  project name and those names have just been re-instantiated.

Only a project's **own** files are indexed — a dependency is a project in its own right and is
indexed there, so following the edges would report every shared symbol twice.

### Code lens

A reference count above each `fn`, `struct`, `enum`, `type`, `static` and `mod`, from
`name_map.find_refs(decl)`, scoped to the declaration's own project. Fields and enum options are
deliberately left out: a lens per struct field turns a record into a ladder of grey text.

**The lens command carries a URI string and two integers, not LSP objects**, because
`vscode-languageclient` passes command arguments through unconverted. `artic.showReferences` in
[vscode/src/extension.ts](../vscode/src/extension.ts) rebuilds a `vscode.Uri`/`vscode.Position` and
forwards to `editor.action.showReferences`. It is registered in code only, not in
`contributes.commands`, so it stays out of the palette.

### didClose

The handler withdraws the document's diagnostics and discards the editor buffer it carried.

**`File::text` alone does not mean "unsaved edits"** — `File::read()` fills the very same field
from disk and never clears it, so `if (text) reset` recompiled the project on every close and
republished the errors it had just withdrawn. `File::text_from_editor` in
[artic-lsp/include/workspace.h](../artic-lsp/include/workspace.h) records the difference; only
`set_file_content()` sets it, and `Workspace::discard_editor_buffer()` reports whether there was
anything to drop.

When there was, the project is recompiled from disk **before** the diagnostics are withdrawn —
other documents may have been reported broken because of edits that no longer exist, and the
recompile republishes for every file in the project, including the one being closed.

### Files that change on disk

A config or build file is the source of truth for a project, and it is the one file the editor
never opens: a `git checkout` that rewrites `artic.json`, or a build regenerating `build.ninja`,
happens entirely behind the server's back. `workspace/didChangeWatchedFiles` is the only
notification that reports it, and its `Changed` events used to be discarded outright — which
mattered more once zero-config detection made a build file authoritative for a workspace that has
no config at all.

The three file events are answered differently, because they mean different things:

| Event | Response | Why |
| ----- | -------- | --- |
| `Created`, `Deleted` (any watched file) | reload the whole workspace | a source file appearing or disappearing changes what a `files` glob expands to, so no single project can be updated in isolation |
| `Changed` on a config or build file | `Server::reload_config()` | that one file's projects are stale; nothing else is |
| `Changed` on a source file | ignored | `didChange` carries the buffer, and the editor's copy wins over what is on disk |

`reload_config()` is the body `didOpen` and `didSave` already ran, extracted: re-read the config,
throw away the symbol index (project instances are recreated, so anything cached under a project
name now describes a project that no longer exists), and drop the compile. It adds one thing none
of the three callers did — **recompiling the active file afterwards**. Diagnostics are push-only,
so fixing a config used to leave the old errors on screen until the user touched a source file.
The active file has to be captured *before* `compile.reset()`, or there is nothing left to ask.

A config the editor currently has open is skipped: `open_configs_` is maintained by `didOpen` and
`didClose`, and a watcher event for one of those files is the echo of the save that `didSave` has
already handled. Guarded by [test/watched-files.test.mjs](../test/watched-files.test.mjs), which
rewrites an `artic.json` and a `build.ninja` on disk without ever opening them.

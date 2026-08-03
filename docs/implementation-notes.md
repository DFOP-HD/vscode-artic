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

- **`.vcxproj`** — `parse_vcxproj()` reads the artic invocation out of the build command.
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

Paths containing spaces are not supported in any of the three.

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
`Project::pattern_matches` as it goes. And **there is no position information in a parsed config**
(it is plain nlohmann JSON), so `ConfigDocument` in
[artic-lsp/src/server.cpp](../artic-lsp/src/server.cpp) locates each literal by scanning the file
and marks every occurrence it has used, so two projects sharing a `files` pattern each get their
own hint instead of both landing on the first line.
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
| `SingleFile` | No configuration was found. The file is compiled on its own. |

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
a VS Code instance. A single-file compile gets `statusBarItem.warningBackground` and, when the item
is clicked, offers to run **Artic: Detect workspace configuration**. Detection stays a status bar
affordance rather than a notification: it is shown on every file, and a popup on every file is not
a feature.

`Initialize` also records `workspaceFolders` (falling back to the deprecated `rootUri`) in
`Server::workspace_roots_`. Nothing consumes it yet — it is what bounds the upward search when the
server starts looking for build files itself.

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

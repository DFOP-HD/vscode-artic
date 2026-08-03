#include "symbol_index.h"

#include "ast_render.h"
#include "text.h"

#include "artic/arena.h"
#include "artic/ast.h"
#include "artic/locator.h"
#include "artic/log.h"
#include "artic/parser.h"

#include <algorithm>
#include <sstream>

namespace artic::ls {

namespace {

/// Discards everything written to it. Indexing parses files that are very often mid-edit,
/// and their syntax errors are the compile's business to report, not the index's.
struct NullOutput {
    std::ostream stream { nullptr };
    log::Output output { stream, false };
};

void harvest(const ast::Decl& decl, const lsp::FileUri& uri, const std::string& container,
    std::vector<IndexedSymbol>& out);

template <typename Decls>
void harvest_all(const Decls& decls, const lsp::FileUri& uri, const std::string& container,
    std::vector<IndexedSymbol>& out)
{
    for (const auto& decl : decls)
        if (decl) harvest(*decl, uri, container, out);
}

void harvest(const ast::Decl& decl, const lsp::FileUri& uri, const std::string& container,
    std::vector<IndexedSymbol>& out)
{
    auto kind = symbol_kind_of(decl);
    auto named = decl.isa<ast::NamedDecl>();
    // An `implicit` has no identifier - the summoner picks it by type - so there is nothing
    // for a name-based picker to match it on.
    if (kind && named) {
        out.push_back(IndexedSymbol{
            .name = named->id.name,
            .container = container,
            .kind = *kind,
            .location = lsp::Location{ .uri = uri, .range = to_range(decl.loc) },
        });
    }

    auto nested = named ? (container.empty() ? named->id.name : container + "::" + named->id.name)
                        : container;
    if (auto mod_decl = decl.isa<ast::ModDecl>()) {
        harvest_all(mod_decl->decls, uri, nested, out);
    } else if (auto struct_decl = decl.isa<ast::StructDecl>()) {
        // A tuple-like struct numbers its fields, which no one searches for.
        if (!struct_decl->is_tuple_like) harvest_all(struct_decl->fields, uri, nested, out);
    } else if (auto enum_decl = decl.isa<ast::EnumDecl>()) {
        harvest_all(enum_decl->options, uri, nested, out);
    } else if (auto option_decl = decl.isa<ast::OptionDecl>()) {
        harvest_all(option_decl->fields, uri, nested, out);
    }
}

void harvest_file(workspace::Workspace& workspace, const fs::path& path,
    std::vector<IndexedSymbol>& out)
{
    const auto* text = workspace.file_text(path);
    if (!text) return;

    // Everything below dies at the end of this function, including the `Locator` that owns
    // every `Loc::file` string, so the symbols must be fully copied out before it does.
    NullOutput sink;
    Locator locator;
    Log log(sink.output, &locator);
    Arena arena;

    auto name = path.generic_string();
    locator.register_file(name, *text);
    std::istringstream is(*text);
    Lexer lexer(log, name, is);
    Parser parser(log, lexer, arena);
    auto module = parser.parse();
    if (!module) return;

    harvest_all(module->decls, to_file_uri(path), std::string(), out);
}

} // anonymous namespace

const SymbolIndex::Entry& SymbolIndex::entry_for(
    workspace::Workspace& workspace, const workspace::Project& project)
{
    if (auto it = projects_.find(project.name); it != projects_.end()) return it->second;

    Entry entry;
    // Only the project's own files: a dependency is a project in its own right and is
    // indexed there, so following the edges would report every shared symbol twice.
    entry.files = project.files;
    for (const auto& file : entry.files) harvest_file(workspace, file, entry.symbols);
    return projects_.emplace(project.name, std::move(entry)).first->second;
}

std::vector<const IndexedSymbol*> SymbolIndex::find(
    workspace::Workspace& workspace, const std::string& query, size_t limit)
{
    auto needle = text::to_lower(query);
    std::vector<const IndexedSymbol*> result;
    for (const auto* project : workspace.all_projects()) {
        for (const auto& symbol : entry_for(workspace, *project).symbols) {
            if (result.size() >= limit) return result;
            if (!needle.empty() && text::to_lower(symbol.name).find(needle) == std::string::npos)
                continue;
            result.push_back(&symbol);
        }
    }
    return result;
}

void SymbolIndex::invalidate(const fs::path& file) {
    auto key = paths::lookup_key(file);
    std::erase_if(projects_, [&](const auto& entry) {
        return std::any_of(entry.second.files.begin(), entry.second.files.end(),
            [&](const fs::path& f) { return paths::lookup_key(f) == key; });
    });
}

} // namespace artic::ls

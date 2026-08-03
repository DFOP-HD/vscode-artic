#ifndef ARTIC_LS_SYMBOL_INDEX_H
#define ARTIC_LS_SYMBOL_INDEX_H

#include "workspace.h"
#include "lsp_convert.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace artic::ls {

/// One declaration, as a symbol picker needs it.
///
/// The AST it was harvested from lives in an arena that is destroyed as soon as the file
/// has been walked, and every `Loc::file` points into a `Locator` that dies with it, so
/// nothing here may refer back to either.
struct IndexedSymbol {
    std::string name;
    /// Enclosing declaration path (`shapes::Circle`), empty at file scope.
    std::string container;
    lsp::SymbolKind kind = lsp::SymbolKind::Function;
    lsp::Location location;
};

/// Every declaration of every project the workspace knows about.
///
/// The index parses; it does not compile. Name binding, type checking and summoning are
/// what make a compile expensive, and none of them contributes anything a symbol picker
/// shows, so the whole workspace costs about as much to index as it costs to read. The
/// result is cached per project, because a client re-issues `workspace/symbol` on every
/// keystroke.
class SymbolIndex {
public:
    /// Symbols of every known project whose name contains `query`, case-insensitively.
    /// An empty query matches everything, which is what a client sends when it opens the
    /// picker; `limit` keeps that from becoming unbounded.
    std::vector<const IndexedSymbol*> find(
        workspace::Workspace& workspace, const std::string& query, size_t limit);

    /// Forgets every project that indexed `file`, so the next query re-harvests it.
    void invalidate(const fs::path& file);

private:
    struct Entry {
        std::vector<IndexedSymbol> symbols;
        std::vector<fs::path> files;
    };

    const Entry& entry_for(workspace::Workspace& workspace, const workspace::Project& project);

    std::unordered_map<workspace::Project::Identifier, Entry> projects_;
};

} // namespace artic::ls

#endif // ARTIC_LS_SYMBOL_INDEX_H

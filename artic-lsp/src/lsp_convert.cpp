#include "lsp_convert.h"

#include "paths.h"

#include <lsp/error.h>

#include <memory>

namespace artic::ls {

lsp::FileUri to_file_uri(const fs::path& path) {
    auto generic = fs::absolute(path).generic_string();
#ifdef _WIN32
    generic.insert(generic.begin(), '/');
#endif
    lsp::Uri uri;
    uri.setScheme("file");
    uri.setAuthority({});
    uri.setPath(generic);
    return lsp::FileUri(uri);
}

lsp::Position to_position(const Loc::Pos& pos) {
    return lsp::Position {
        static_cast<lsp::uint>(pos.row - 1),
        static_cast<lsp::uint>(pos.col - 1)
    };
}

lsp::Range to_range(const Loc& loc) {
    return lsp::Range { .start = to_position(loc.begin), .end = to_position(loc.end) };
}

lsp::Location to_location(const Loc& loc) {
    if (!loc.file) throw lsp::RequestError(lsp::Error::InternalError, "Cannot convert location with undefined file");
    return lsp::Location { .uri = to_file_uri(*loc.file), .range = to_range(loc) };
}

static std::shared_ptr<std::string> file_of(const lsp::TextDocumentIdentifier& file) {
    return std::make_shared<std::string>(paths::canonical_path(fs::path(file.uri.path())).generic_string());
}

Loc to_loc(const lsp::TextDocumentIdentifier& file, const lsp::Position& pos) {
    return Loc(
        file_of(file),
        Loc::Pos { .row = static_cast<int>(pos.line + 1), .col = static_cast<int>(pos.character + 1) }
    );
}

Loc to_loc(const lsp::TextDocumentIdentifier& file, const lsp::Range& range) {
    return Loc(
        file_of(file),
        Loc::Pos { .row = static_cast<int>(range.start.line + 1), .col = static_cast<int>(range.start.character + 1) },
        Loc::Pos { .row = static_cast<int>(range.end.line + 1),   .col = static_cast<int>(range.end.character + 1) }
    );
}

lsp::DiagnosticSeverity to_severity(Severity severity) {
    switch (severity) {
        case Severity::Warning: return lsp::DiagnosticSeverity::Warning;
        case Severity::Info:    return lsp::DiagnosticSeverity::Information;
        case Severity::Hint:    return lsp::DiagnosticSeverity::Hint;
        case Severity::Error:   break;
    }
    return lsp::DiagnosticSeverity::Error;
}

lsp::Diagnostic to_diagnostic(const Diagnostic& diag) {
    lsp::Diagnostic lsp_diag;
    lsp_diag.message = diag.message;
    lsp_diag.range = to_range(diag.loc);
    lsp_diag.severity = to_severity(diag.severity);
    return lsp_diag;
}

bool contains(const lsp::Range& range, const lsp::Position& pos) {
    if (pos.line < range.start.line || pos.line > range.end.line) return false;
    if (pos.line == range.start.line && pos.character < range.start.character) return false;
    if (pos.line == range.end.line && pos.character > range.end.character) return false;
    return true;
}

} // namespace artic::ls

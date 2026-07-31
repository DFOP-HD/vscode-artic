#ifndef ARTIC_LS_LSP_CONVERT_H
#define ARTIC_LS_LSP_CONVERT_H

#include "artic/lsp.h"
#include "artic/loc.h"

#include <lsp/types.h>

#include <filesystem>

namespace artic::ls {

namespace fs = std::filesystem;

// lsp-framework's FileUri::fromPath() renders the path with u8string() rather than
// generic_u8string(). On MSVC that keeps the native backslashes, which then get
// percent-encoded as %5C and no editor can match the URI to the open document.
// (MinGW's libstdc++ happens to keep forward slashes, so the bug is invisible there.)
lsp::FileUri to_file_uri(const fs::path& path);

lsp::Position to_position(const Loc::Pos& pos);
lsp::Range to_range(const Loc& loc);

// Throws lsp::RequestError when the location has no file.
lsp::Location to_location(const Loc& loc);

// Artic counts rows and columns from one, LSP from zero.
Loc to_loc(const lsp::TextDocumentIdentifier& file, const lsp::Position& pos);
Loc to_loc(const lsp::TextDocumentIdentifier& file, const lsp::Range& range);

lsp::DiagnosticSeverity to_severity(Severity severity);
lsp::Diagnostic to_diagnostic(const Diagnostic& diag);

bool contains(const lsp::Range& range, const lsp::Position& pos);

} // namespace artic::ls

#endif // ARTIC_LS_LSP_CONVERT_H

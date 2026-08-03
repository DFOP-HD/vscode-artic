#ifndef ARTIC_LS_AST_RENDER_H
#define ARTIC_LS_AST_RENDER_H

#include "artic/ast.h"
#include "artic/log.h"
#include "artic/print.h"

#include <lsp/types.h>

#include <optional>
#include <sstream>
#include <string>

namespace artic::ls {

/// Prints an AST or type node the way the source spells it.
template <typename T>
std::string print_to_string(const T& node) {
    std::ostringstream oss;
    log::Output output(oss, false);
    Printer printer(output);
    node.print(printer);
    return oss.str();
}

/// Prints a parameter list, adding the parentheses that a non-tuple does not bring itself.
/// Mirrors upstream's `print_parens()`, which is static in print.cpp.
void print_param_list(Printer& printer, const ast::Ptrn& param);
void print_param_list(Printer& printer, const Type& dom);

/// Renders a declaration the way it is written in source, but without its body.
std::string render_decl(const ast::NamedDecl& decl);

/// The outline kind of a declaration, or nothing for declarations that do not belong in
/// an outline at all (`let`, `use`, and anything the parser could not make sense of).
/// Shared by document symbols, workspace symbols and code lens, so they cannot disagree.
std::optional<lsp::SymbolKind> symbol_kind_of(const ast::Decl& decl);

} // namespace artic::ls

#endif // ARTIC_LS_AST_RENDER_H

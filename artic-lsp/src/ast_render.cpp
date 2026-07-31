#include "ast_render.h"

#include "artic/types.h"

namespace artic::ls {

void print_param_list(Printer& printer, const ast::Ptrn& param) {
    if (param.is_tuple()) { param.print(printer); return; }
    printer << '(';
    param.print(printer);
    printer << ')';
}

void print_param_list(Printer& printer, const Type& dom) {
    if (dom.isa<TupleType>()) { dom.print(printer); return; }
    printer << '(';
    dom.print(printer);
    printer << ')';
}

std::string render_decl(const ast::NamedDecl& decl) {
    std::ostringstream oss;
    log::Output output(oss, false);
    Printer printer(output);

    auto print = [&](const ast::Node* node) { if (node) node->print(printer); };
    auto print_declared_type = [&](const ast::Type* declared) {
        if (declared) declared->print(printer);
        else if (decl.type) decl.type->print(printer);
        else oss << '?';
    };

    if (auto fn_decl = decl.isa<ast::FnDecl>()) {
        const auto& fn = *fn_decl->fn;
        oss << "fn " << decl.id.name;
        print(fn_decl->type_params.get());
        if (fn.param) print_param_list(printer, *fn.param);
        if (fn.ret_type) {
            oss << " -> ";
            fn.ret_type->print(printer);
        } else if (auto fn_type = decl.type ? decl.type->isa<artic::FnType>() : nullptr) {
            oss << " -> ";
            fn_type->codom->print(printer);
        }
    } else if (auto struct_decl = decl.isa<ast::StructDecl>()) {
        oss << "struct " << decl.id.name;
        print(struct_decl->type_params.get());
    } else if (auto enum_decl = decl.isa<ast::EnumDecl>()) {
        oss << "enum " << decl.id.name;
        print(enum_decl->type_params.get());
    } else if (auto option_decl = decl.isa<ast::OptionDecl>()) {
        if (option_decl->parent) oss << option_decl->parent->id.name << "::";
        oss << decl.id.name;
        if (option_decl->param) {
            oss << '(';
            option_decl->param->print(printer);
            oss << ')';
        }
    } else if (auto type_decl = decl.isa<ast::TypeDecl>()) {
        oss << "type " << decl.id.name;
        print(type_decl->type_params.get());
        oss << " = ";
        print(type_decl->aliased_type.get());
    } else if (auto static_decl = decl.isa<ast::StaticDecl>()) {
        oss << "static ";
        if (static_decl->is_mut) oss << "mut ";
        oss << decl.id.name << ": ";
        print_declared_type(static_decl->type.get());
    } else if (auto field_decl = decl.isa<ast::FieldDecl>()) {
        oss << decl.id.name << ": ";
        print_declared_type(field_decl->type.get());
    } else if (auto ptrn_decl = decl.isa<ast::PtrnDecl>()) {
        if (ptrn_decl->is_mut) oss << "mut ";
        oss << decl.id.name << ": ";
        print_declared_type(nullptr);
    } else if (decl.isa<ast::ModDecl>()) {
        oss << "mod " << decl.id.name;
    } else if (decl.isa<ast::TypeParam>()) {
        oss << "type " << decl.id.name;
    } else {
        oss << decl.id.name;
        if (decl.type) {
            oss << ": ";
            decl.type->print(printer);
        }
    }

    return oss.str();
}

} // namespace artic::ls

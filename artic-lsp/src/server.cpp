#include "server.h"

#include "ast_render.h"
#include "compile.h"
#include "config.h"
#include "lsp_convert.h"
#include "paths.h"
#include "workspace.h"
#include "artic/log.h"
#include "artic/ast.h"
#include "artic/print.h"
#include "artic/types.h"
#include "lsp/error.h"

#include <filesystem>
#include <limits>
#include <lsp/types.h>
#include <lsp/io/standardio.h>
#include <lsp/messages.h>
#include <lsp/jsonrpc/jsonrpc.h>

#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <cctype>
#include <sstream>
#include <algorithm>

namespace reqst = lsp::requests;
namespace notif = lsp::notifications;
namespace fs = std::filesystem;

namespace artic::ls {
namespace {

// Answered through `workspace/executeCommand` rather than a request of our own: a custom
// method would need the client to speak it, and every LSP client already speaks this one.
constexpr std::string_view project_for_file_command = "artic.projectForFile";

fs::path absolute_path(std::string_view file) {
    return paths::canonical_path(fs::path(file));
}

void log_request(std::string_view name, const lsp::TextDocumentIdentifier& doc) {
    log::info("\n[LSP] <<< {} {}", name, doc.uri.path());
}

void log_request(std::string_view name, const lsp::TextDocumentIdentifier& doc, const lsp::Position& pos) {
    log::info("\n[LSP] <<< {} {}:{}:{}", name, doc.uri.path(), pos.line + 1, pos.character + 1);
}

void log_request(std::string_view name, const lsp::TextDocumentIdentifier& doc, const lsp::Range& range) {
    log::info("\n[LSP] <<< {} {}:{}:{} to {}:{}", name, doc.uri.path(),
              range.start.line + 1, range.start.character + 1,
              range.end.line + 1, range.end.character + 1);
}

} // anonymous namespace

// Server ---------------------------------------------------------------------

Server::Server() 
    : connection_(lsp::Connection(lsp::io::standardIO()))
    , message_handler_(this->connection_)
{
    setup_events();
}

Server::~Server() = default;

int Server::run() {
    log::info("LSP Server starting...");
    running_ = true;
    while (running_) {
        try {
            message_handler_.processIncomingMessages();
        } catch (const lsp::RequestError& e) {
            log::info("LSP Message processing error: {}", e.what());
        } catch (const std::runtime_error& e) {
            log::info("LSP Server fatal runtime error: {}", e.what());
            return 1;
        } catch (const std::exception& e) {
            log::info("LSP Server fatal exception: {}", e.what());
            return 1;
        } catch (...) {
            log::info("LSP Server unknown fatal error");
            return 1;
        }
    }

    log::info("LSP Server shutdown complete");
    return 0;
}

void Server::send_message(const std::string& message, lsp::MessageType type) {
    message_handler_.sendNotification<notif::Window_ShowMessage>({ .type = type, .message = message });
}

Server::FileType Server::get_file_type(const fs::path& file) {
    // `.artic-lsp` is a dotfile, so it has no extension as far as std::filesystem
    // is concerned and has to be matched on the filename instead.
    if (file.filename() == ".artic-lsp") return FileType::ConfigFile;
    auto ext = file.extension();
    return ext == ".json" || ext == ".ninja" || ext == ".vcxproj" || ext == ".sln"
        ? FileType::ConfigFile : FileType::SourceFile;
}

// -----------------------------------------------------------------------------
//
//
// Initialization
//
//
// -----------------------------------------------------------------------------


void Server::setup_events_initialization() {
    message_handler_.add<reqst::Initialize>([this](reqst::Initialize::Params&& params) -> reqst::Initialize::Result {
        log::info( "\n[LSP] <<< Initialize");
        
        bool restart_from_crash = false;
        // Parse init options
        if (auto init = params.initializationOptions; init.has_value() && init->isObject()) {
            const auto& obj = init->object();

            if (auto val = obj.find("restartFromCrash"); val && val->isBoolean())
                restart_from_crash = val->boolean();
        }

        // The folders the editor has open. `workspaceFolders` supersedes `rootUri`, which
        // the specification deprecated but every client still sends. Without either, the
        // server has no idea where the project ends and cannot look for a build file when
        // no configuration exists.
        //
        // Both are plain `Uri`s, whose `path()` keeps the leading slash of a Windows drive
        // path; only `FileUri::path()` strips it. Without the conversion every root is
        // `/D:/...`, which matches no file and silently disables detection.
        std::vector<std::filesystem::path> workspace_roots;
        auto to_path = [](const lsp::Uri& uri) {
            lsp::FileUri file_uri(uri);
            return absolute_path(file_uri.path());
        };
        if (auto& folders = params.workspaceFolders; folders.has_value() && !folders->isNull()) {
            for (const auto& folder : folders->value())
                workspace_roots.push_back(to_path(folder.uri));
        }
        if (workspace_roots.empty() && !params.rootUri.isNull())
            workspace_roots.push_back(to_path(*params.rootUri));
        for (const auto& root : workspace_roots)
            log::info("[LSP] workspace root: {}", root.generic_string());

        safe_mode_ = restart_from_crash;
        workspace_ = std::make_unique<workspace::Workspace>();
        workspace_->set_workspace_roots(std::move(workspace_roots));
        
        return reqst::Initialize::Result {
            .capabilities = lsp::ServerCapabilities{
                .textDocumentSync = lsp::TextDocumentSyncOptions{
                    .openClose = true,
                    .change    = lsp::TextDocumentSyncKind::Full,
                    .save      = lsp::SaveOptions{ .includeText = false },
                },
                .completionProvider = lsp::CompletionOptions{
                    .triggerCharacters = std::vector<std::string>{".", ":"}
                },
                .hoverProvider = true,
                .signatureHelpProvider = lsp::SignatureHelpOptions{
                    .triggerCharacters = std::vector<std::string>{"(", ","},
                    .retriggerCharacters = std::vector<std::string>{")"}
                },
                .definitionProvider = true,
                .typeDefinitionProvider = true,
                .implementationProvider = true,
                .referencesProvider = true,
                .documentHighlightProvider = true,
                .documentSymbolProvider = true,
                .codeLensProvider = lsp::CodeLensOptions{
                    .resolveProvider = false
                },
                .workspaceSymbolProvider = true,
                .renameProvider = lsp::RenameOptions {
                    .prepareProvider = true
                },
                .selectionRangeProvider = true,
                .executeCommandProvider = lsp::ExecuteCommandOptions {
                    .commands = { std::string(project_for_file_command) }
                },
                .semanticTokensProvider = lsp::SemanticTokensOptions{
                    .legend = lsp::SemanticTokensLegend{
                        .tokenTypes = {
                            "namespace", "type", "class", "enum", "interface", "struct", 
                            "typeParameter", "parameter", "variable", "property", "enumMember",
                            "event", "function", "method", "macro", "keyword",
                            "modifier", "comment", "string", "number", "regexp", "operator"
                        },
                        .tokenModifiers = {
                            "declaration", "definition", "readonly", "static", 
                            "deprecated", "abstract", "async", "modification", 
                            "documentation", "defaultLibrary"
                        }
                    },
                    .range = true,
                    .full = lsp::SemanticTokensOptionsFull{
                        .delta = false
                    }
                },
                .inlayHintProvider = lsp::InlayHintOptions {
                    .resolveProvider = false
                }
            },
            .serverInfo = lsp::InitializeResultServerInfo {
                .name    = "Artic Language Server",
                .version = "0.1.0"
            }
        };
    });

    message_handler_.add<notif::Initialized>([this](notif::Initialized::Params&&){
        log::info("\n[LSP] <<< Initialized");
        reload_workspace();
    });

    message_handler_.add<reqst::Shutdown>([this]() {
        log::info("\n[LSP] <<< Shutdown");
        running_ = false;
        return reqst::Shutdown::Result {};
    });
}


// -----------------------------------------------------------------------------
//
//
// Modifications (File changes)
//
//
// -----------------------------------------------------------------------------


void Server::setup_events_modifications() {

    // Textdocument ----------------------------------------------------------------------

    message_handler_.add<notif::TextDocument_DidClose>([this](notif::TextDocument_DidClose::Params&& params) {
        log::info("\n[LSP] <<< TextDocument DidClose");
        auto path = absolute_path(params.textDocument.uri.path());

        if(get_file_type(path) != FileType::SourceFile) {
            open_configs_.erase(path);
            return;
        }

        // Unsaved edits die with the buffer, so everything compiled from them is stale.
        // Other documents may still be reported broken because of edits that no longer
        // exist, so the project has to be rebuilt from what is on disk.
        if(workspace_->discard_editor_buffer(path)) {
            symbol_index_.invalidate(path);
            compile.reset();
            compile_this_and_related_files(path);
        }

        // The editor no longer owns the document, so its diagnostics have to go with it.
        // This has to come last: the recompile above republishes them.
        message_handler_.sendNotification<notif::TextDocument_PublishDiagnostics>(
            notif::TextDocument_PublishDiagnostics::Params {
                .uri = to_file_uri(path),
                .diagnostics = {}
            }
        );
    });
    message_handler_.add<notif::TextDocument_DidOpen>([this](notif::TextDocument_DidOpen::Params&& params) {
        log::info("\n[LSP] <<< TextDocument DidOpen");
        auto path = absolute_path(params.textDocument.uri.path());

        if(get_file_type(path) == FileType::SourceFile) {
            ensure_compile(path.string());
        } else {
            open_configs_.insert(path);
            reload_config(path);
        }
    });
    message_handler_.add<notif::TextDocument_DidChange>([this](notif::TextDocument_DidChange::Params&& params) {
        log::info("");
        log::info("--------------------------------");
        log::info("[LSP] <<< TextDocument DidChange");
        std::filesystem::path file = absolute_path(params.textDocument.uri.path());
        if(get_file_type(file) == FileType::ConfigFile) {
            // handled in didsave
            return;
        }

        auto& content = std::get<lsp::TextDocumentContentChangeEvent_Text>(params.contentChanges[0]).text;
        symbol_index_.invalidate(file);
        compile_this_and_related_files(file, &content);
    });

    message_handler_.add<notif::TextDocument_DidSave>([this](notif::TextDocument_DidSave::Params&& params) {
        log::info("\n[LSP] <<< TextDocument DidSave");
        std::filesystem::path file = absolute_path(params.textDocument.uri.path());
        if(get_file_type(file) == FileType::ConfigFile) {
            reload_config(file);
            return;
        }
    });

    // Workspace ----------------------------------------------------------------------

    message_handler_.add<notif::Workspace_DidChangeConfiguration>([this](notif::Workspace_DidChangeConfiguration::Params&& params) {
        log::info("\n[LSP] <<< Workspace DidChangeConfiguration");
        // Optionally, could inspect params.settings to override paths.
        reload_workspace();
    });
    message_handler_.add<notif::Workspace_DidChangeWatchedFiles>([this](notif::Workspace_DidChangeWatchedFiles::Params&& params) {
        // A source file appearing or disappearing changes what a `files` glob expands to,
        // so only those two need the whole workspace rebuilt. A config or build file whose
        // *content* changed invalidates itself and nothing else -- and it is the one thing
        // no editor event covers, because a `git checkout` or a build regenerating
        // `build.ninja` never opens it.
        std::vector<fs::path> changed_configs;
        for(auto& change : params.changes) {
            auto path = absolute_path(change.uri.path());

            switch(change.type.index()) {
                case lsp::FileChangeType::Created: 
                case lsp::FileChangeType::Deleted: {
                    reload_workspace();
                    return;
                }
                case lsp::FileChangeType::Changed: {
                    if(get_file_type(path) != FileType::ConfigFile) break;
                    // The editor owns this buffer, so didSave has already handled it.
                    if(open_configs_.count(path)) break;
                    changed_configs.push_back(path);
                    break;
                }
                case lsp::FileChangeType::MAX_VALUE: break;
            }
        }
        if(changed_configs.empty()) return;

        log::info("\n[LSP] <<< Workspace DidChangeWatchedFiles: {} config(s) changed on disk", changed_configs.size());
        for(const auto& config : changed_configs) reload_config(config);
    });
}


// -----------------------------------------------------------------------------
//
//
// Semantic Tokens
//
//
// -----------------------------------------------------------------------------


struct SemanticToken {
    uint32_t line;
    uint32_t start; 
    uint32_t length;
    uint32_t type;
    uint32_t modifiers;
};

namespace {

SemanticToken create_semantic_token(const Loc& loc, const ast::NamedDecl& decl, bool is_decl) {
    SemanticToken token {
        .line =   (uint32_t) loc.begin.row - 1,
        .start =  (uint32_t) loc.begin.col - 1,
        .length = (uint32_t) loc.end.col - loc.begin.col,
        .type = 0,
        .modifiers = 0,
    };
    using ty = lsp::SemanticTokenTypes;
    using md = lsp::SemanticTokenModifiers;

    auto flag = [](md mod) -> uint32_t  {
        uint32_t val = static_cast<uint32_t>(mod);
        return 1u << (val);
    };

    if (auto t = decl.isa<ast::StaticDecl>()) {
        token.type = (uint32_t) ty::Variable;
        token.modifiers |= flag(md::Static);
        if(!t->is_mut) token.modifiers |= flag(md::Readonly);
    } 
    else if (auto t = decl.isa<ast::LetDecl>()) {
        if(auto p = t->ptrn->isa<ast::PtrnDecl>()){
            token.type = (uint32_t) ty::Variable;
            if(!p->is_mut) token.modifiers |= flag(md::Readonly);
        }
    } 
    else if (auto t = decl.isa<ast::PtrnDecl>()) {
        token.type = (uint32_t) ty::Parameter;
        if(!t->is_mut) token.modifiers |= flag(md::Readonly);
    } 
    else if (decl.isa<ast::TypeParam>())  token.type = (uint32_t) ty::Type;
    else if (decl.isa<ast::FnDecl>())     token.type = (uint32_t) ty::Function;
    else if (decl.isa<ast::RecordDecl>()) token.type = (uint32_t) ty::Struct;
    else if (decl.isa<ast::EnumDecl>())   token.type = (uint32_t) ty::Enum;
    else if (decl.isa<ast::TypeDecl>())   token.type = (uint32_t) ty::Type;
    else if (decl.isa<ast::FieldDecl>())  token.type = (uint32_t) ty::Property;
    else if (decl.isa<ast::ModDecl>())    token.type = (uint32_t) ty::Namespace;
    else if (decl.isa<ast::UseDecl>())    token.type = (uint32_t) ty::Namespace;

    if(is_decl){
        token.modifiers |= flag(md::Definition);
        token.modifiers |= flag(md::Declaration);
    }
    auto type = decl.type;
    if(type) {
        if(auto addr = type->isa<AddrType>(); addr && addr->pointee) type = addr->pointee; // remove reference
        if(auto app = type->isa<TypeApp>(); app && app-> applied) type = app->applied; // collapse polymorphic type
        if(auto fn = type->isa<FnType>()){
            token.type = (uint32_t) ty::Function;
            if(fn->codom->isa<NoRetType>())
                token.type = (uint32_t) ty::Keyword; // continuation
        }
    }
    return token;
}

// Collect semantic tokens from the NameMap by iterating over declarations and references
lsp::SemanticTokens collect_semantic_tokens(
    const ls::NameMap& name_map, 
    const std::string& file, 
    int start_row = 0, 
    int end_row = std::numeric_limits<int>::max()
) {
    std::vector<SemanticToken> tokens;
    // Check if we have entries for this file
    if (!name_map.files.contains(file)) return {};
    
    auto& names = name_map.files.at(file);
    
    // Collect tokens from references (this is where we want semantic highlighting)
    for (const auto& [ref, decl] : names.declaration_of) {
        auto& loc = name_map.get_identifier(ref).loc;
        if(loc.begin.row >= start_row && loc.end.row <= end_row)
            tokens.push_back(create_semantic_token(loc, *decl, false));
    }
    
    // Collect tokens from declarations
    for (const auto& [decl, refs] : names.references_of) {
        auto& loc = decl->id.loc;
        if(loc.begin.row >= start_row && loc.end.row <= end_row)
            tokens.push_back(create_semantic_token(loc, *decl, true));
    }

    std::sort(tokens.begin(), tokens.end(), [](const SemanticToken& a, const SemanticToken& b) {
        if (a.line != b.line) return a.line < b.line;
        return a.start < b.start;
    });

    // Encode
    std::vector<uint32_t> data;
    data.reserve(tokens.size() * 5);
    uint32_t prev_line = 0;
    uint32_t prev_start = 0;
    
    for (const auto& token : tokens) {
        // Delta-encode the tokens as required by LSP spec
        uint32_t delta_line = token.line - prev_line;
        uint32_t delta_start = (delta_line == 0) ? token.start - prev_start : token.start;
        
        data.push_back(delta_line);
        data.push_back(delta_start);
        data.push_back(token.length);
        data.push_back(token.type);
        data.push_back(token.modifiers);
        
        prev_line = token.line;
        prev_start = token.start;
    }
    
    return lsp::SemanticTokens{
        .data = data
    };
}

} // anonymous namespace

void Server::setup_events_tokens() {
    // Semantic Tokens ----------------------------------------------------------------------
    message_handler_.add<reqst::TextDocument_SemanticTokens_Full>([this](lsp::SemanticTokensParams&& params) -> reqst::TextDocument_SemanticTokens_Full::Result {
        auto file = absolute_path(params.textDocument.uri.path());
        log_request("TextDocument SemanticTokens_Full", params.textDocument);
        
        if(!has_compiled(file)) return nullptr;
        auto tokens = collect_semantic_tokens(compile->name_map, file.generic_string());
        
        log::info("[LSP] >>> Returning {} semantic tokens", tokens.data.size());
        return tokens;
    });

    message_handler_.add<reqst::TextDocument_SemanticTokens_Range>([this](lsp::SemanticTokensRangeParams&& params) -> reqst::TextDocument_SemanticTokens_Range::Result {
        auto file = absolute_path(params.textDocument.uri.path());
        log_request("TextDocument SemanticTokens_Range", params.textDocument, params.range);

        if(!has_compiled(file)) return nullptr;
        auto tokens = collect_semantic_tokens(
            compile->name_map, file.generic_string(), 
            params.range.start.line + 1, 
            params.range.end.line + 1);
        
        log::info("[LSP] >>> Returning {} semantic tokens", tokens.data.size());
        return tokens;
    });
}


// -----------------------------------------------------------------------------
//
//
// Definitions
//
//
// -----------------------------------------------------------------------------

namespace {

struct IdentifierOccurrences {
    std::string name;
    std::vector<lsp::Location> all_occurences;

    // Additional info
    lsp::Location cursor_range;
    lsp::Location declaration_range;
};

template <typename Decls>
void collect_document_symbols(
    const Decls& decls,
    const std::string& file,
    lsp::Array<lsp::DocumentSymbol>& out);

/// Turns one declaration into an outline entry, recursing into whatever it contains.
std::optional<lsp::DocumentSymbol> make_document_symbol(const ast::Decl& decl, const std::string& file) {
    // A project spans several files, but the outline only ever describes the one asked for.
    if (!decl.loc.file || *decl.loc.file != file) return std::nullopt;
    auto kind = symbol_kind_of(decl);
    if (!kind) return std::nullopt;

    lsp::DocumentSymbol symbol;
    symbol.kind = lsp::SymbolKindEnum(*kind);
    symbol.range = to_range(decl.loc);

    if (auto named = decl.isa<ast::NamedDecl>()) {
        symbol.name = named->id.name;
        symbol.selectionRange = to_range(named->id.loc);
        symbol.detail = render_decl(*named);
    } else {
        // `implicit` declarations carry no identifier — the summoner picks them by type.
        auto implicit_decl = decl.as<ast::ImplicitDecl>();
        symbol.name = "implicit";
        symbol.selectionRange = symbol.range;
        if (implicit_decl->type) symbol.detail = print_to_string(*implicit_decl->type);
        else if (decl.type) symbol.detail = print_to_string(*decl.type);
    }

    lsp::Array<lsp::DocumentSymbol> children;
    if (auto mod_decl = decl.isa<ast::ModDecl>()) {
        collect_document_symbols(mod_decl->decls, file, children);
    } else if (auto struct_decl = decl.isa<ast::StructDecl>()) {
        // A tuple-like struct numbers its fields, which says nothing in an outline.
        if (!struct_decl->is_tuple_like) collect_document_symbols(struct_decl->fields, file, children);
    } else if (auto enum_decl = decl.isa<ast::EnumDecl>()) {
        collect_document_symbols(enum_decl->options, file, children);
    } else if (auto option_decl = decl.isa<ast::OptionDecl>()) {
        collect_document_symbols(option_decl->fields, file, children);
    }
    if (!children.empty()) symbol.children = std::move(children);

    return symbol;
}

template <typename Decls>
void collect_document_symbols(
    const Decls& decls,
    const std::string& file,
    lsp::Array<lsp::DocumentSymbol>& out)
{
    for (auto& decl : decls) {
        if (!decl) continue;
        if (auto symbol = make_document_symbol(*decl, file)) out.push_back(std::move(*symbol));
    }
}

/// The declaration the cursor names, whether it sits on the declaration itself or on a
/// reference to it.
const ast::NamedDecl* decl_at(const ls::NameMap& name_map, const Loc& cursor) {
    if(auto decl = name_map.find_decl_at(cursor)) return decl;
    if(auto ref = name_map.find_ref_at(cursor)) return name_map.find_decl(*ref);
    return nullptr;
}

/// The declaration a type originates from, looking through the wrappers that stand between
/// an expression's type and the `struct`/`enum`/`type` that declared it.
const ast::NamedDecl* declaring_type_decl(const Type* type) {
    while (type) {
        if (auto app = type->isa<TypeApp>())            { type = app->applied;        continue; }
        if (auto addr = type->isa<AddrType>())          { type = addr->pointee;       continue; }
        if (auto array = type->isa<ArrayType>())        { type = array->elem;         continue; }
        if (auto implicit = type->isa<ImplicitParamType>()) { type = implicit->underlying; continue; }
        break;
    }
    if (!type) return nullptr;
    if (auto struct_type = type->isa<StructType>()) return &struct_type->decl;
    if (auto enum_type = type->isa<EnumType>())     return &enum_type->decl;
    if (auto alias = type->isa<TypeAlias>())        return &alias->decl;
    if (auto var = type->isa<TypeVar>())            return &var->decl;
    if (auto mod = type->isa<ModType>())            return &mod->decl;
    return nullptr;
}

/// The innermost `SummonExpr` covering the cursor, or null. An implicit argument the caller
/// never wrote is a `SummonExpr` too, synthesised by the type checker with the argument
/// list's location, so a cursor inside `the_answer()` finds the implicit it receives.
const ast::SummonExpr* summon_at(const ast::ModDecl& program, const Loc& cursor) {
    const ast::SummonExpr* found = nullptr;
    ast::Node::TraverseFn visit([&](const ast::Node& node) {
        const auto& loc = node.loc;
        if (!loc.file || !cursor.file || *loc.file != *cursor.file) return false;
        if (cursor.begin < loc.begin || loc.end < cursor.begin) return false;
        if (auto summon = node.isa<ast::SummonExpr>()) found = summon;
        return true;
    });
    for (const auto& decl : program.decls) if (decl) decl->traverse(visit);
    return found;
}

std::optional<IdentifierOccurrences> find_occurrences_of_identifier(const ls::NameMap& name_map, const Loc& cursor) {
    Loc cursor_range;
    const ast::NamedDecl* target_decl = name_map.find_decl_at(cursor);
    if(target_decl) {
        cursor_range = target_decl->id.loc;
        log::info("found declaration at cursor '{}'", target_decl->id.name);
    } else {
        if(auto ref = name_map.find_ref_at(cursor)) {
            auto id = name_map.get_identifier(*ref);
            cursor_range = id.loc;
            target_decl = name_map.find_decl(*ref);
            if(target_decl) log::info("found reference at cursor '{}'", target_decl->id.name);
        }
    }
    // No symbol at cursor position
    if(!target_decl) return std::nullopt;

    std::vector<lsp::Location> locations;

    locations.push_back(to_location(target_decl->id.loc));

    // Find all references to this declaration
    for (auto ref : name_map.find_refs(target_decl)) {
        locations.push_back(to_location(name_map.get_identifier(ref).loc));
    }

    return IdentifierOccurrences {
        .name = target_decl->id.name,
        .all_occurences = std::move(locations),
        .cursor_range = to_location(cursor_range),
        .declaration_range = to_location(target_decl->id.loc),
    };
}

} // anonymous namespace

void Server::setup_events_definitions() {
    message_handler_.add<reqst::TextDocument_Hover>([this](reqst::TextDocument_Hover::Params&& params) -> reqst::TextDocument_Hover::Result {
        log_request("TextDocument Hover", params.textDocument, params.position);

        if(get_file_type(params.textDocument.uri.path()) != FileType::SourceFile) return nullptr;
        auto cursor = to_loc(params.textDocument, params.position);
        ensure_compile(params.textDocument.uri.path());
        auto& name_map = compile->name_map;

        // On a declaration the identifier itself is the range to highlight; on a reference
        // it is the occurrence under the cursor, not the declaration it points at.
        const ast::NamedDecl* decl = name_map.find_decl_at(cursor);
        Loc range = decl ? decl->id.loc : Loc();
        if (!decl) {
            auto ref = name_map.find_ref_at(cursor);
            if (!ref) {
                log::info("[LSP] >>> Hover found no symbol at cursor");
                return nullptr;
            }
            decl = name_map.find_decl(*ref);
            range = name_map.get_identifier(*ref).loc;
        }
        if (!decl) return nullptr;

        auto signature = render_decl(*decl);
        log::info("[LSP] >>> Hover '{}'", signature);
        return lsp::Hover {
            .contents = lsp::MarkupContent {
                .kind = lsp::MarkupKindEnum(lsp::MarkupKind::Markdown),
                .value = "```artic\n" + signature + "\n```"
            },
            .range = to_range(range)
        };
    });

    message_handler_.add<reqst::TextDocument_DocumentSymbol>([this](reqst::TextDocument_DocumentSymbol::Params&& params) -> reqst::TextDocument_DocumentSymbol::Result {
        log_request("TextDocument DocumentSymbol", params.textDocument);

        if (get_file_type(params.textDocument.uri.path()) != FileType::SourceFile) return nullptr;
        auto file = absolute_path(params.textDocument.uri.path()).generic_string();
        ensure_compile(params.textDocument.uri.path());
        if (!compile || !compile->program) return nullptr;

        lsp::Array<lsp::DocumentSymbol> symbols;
        collect_document_symbols(compile->program->decls, file, symbols);
        log::info("[LSP] >>> Returning {} document symbols", symbols.size());
        return symbols;
    });

    message_handler_.add<reqst::TextDocument_Definition>([this](lsp::TextDocumentPositionParams&& pos) -> reqst::TextDocument_Definition::Result {
        log_request("TextDocument Definition", pos.textDocument, pos.position);

        auto cursor = to_loc(pos.textDocument, pos.position);

        if(get_file_type(pos.textDocument.uri.path()) != FileType::SourceFile) return nullptr;
        ensure_compile(pos.textDocument.uri.path());
        auto& name_map = compile->name_map;
        
        // When on a reference try find declaration
        if(auto ref = name_map.find_ref_at(cursor)) {
            if(auto def = name_map.find_decl(*ref)) {
                auto loc = to_location(def->id.loc);
                log::info("[LSP] >>> return TextDocument Definition {}:{}:{}", loc.uri.path(), loc.range.start.line + 1, loc.range.start.character + 1);
                return { loc };
            }
            return nullptr;
        }
        // On a declaration the definition is the declaration itself. Returning its
        // references instead is what documentHighlight and references are for.
        if(auto decl = name_map.find_decl_at(cursor)) {
            log::info("[LSP] >>> TextDocument Definition is the declaration '{}' itself", decl->id.name);
            return { to_location(decl->id.loc) };
        }

        log::info("[LSP] >>> return TextDocument Definition <not found>");
        return nullptr;
    });

    message_handler_.add<reqst::TextDocument_TypeDefinition>([this](reqst::TextDocument_TypeDefinition::Params&& params) -> reqst::TextDocument_TypeDefinition::Result {
        log_request("TextDocument TypeDefinition", params.textDocument, params.position);

        if(get_file_type(params.textDocument.uri.path()) != FileType::SourceFile) return nullptr;
        ensure_compile(params.textDocument.uri.path());
        auto cursor = to_loc(params.textDocument, params.position);

        const ast::NamedDecl* decl = decl_at(compile->name_map, cursor);
        if(!decl) {
            log::info("[LSP] >>> TypeDefinition found no symbol at cursor");
            return nullptr;
        }

        const ast::NamedDecl* type_decl = declaring_type_decl(decl->type);
        if(!type_decl) {
            log::info("[LSP] >>> TypeDefinition: '{}' has no user-declared type", decl->id.name);
            return nullptr;
        }

        log::info("[LSP] >>> TypeDefinition '{}' -> '{}'", decl->id.name, type_decl->id.name);
        return lsp::Definition(to_location(type_decl->id.loc));
    });

    message_handler_.add<reqst::TextDocument_Implementation>([this](reqst::TextDocument_Implementation::Params&& params) -> reqst::TextDocument_Implementation::Result {
        log_request("TextDocument Implementation", params.textDocument, params.position);

        if(get_file_type(params.textDocument.uri.path()) != FileType::SourceFile) return nullptr;
        ensure_compile(params.textDocument.uri.path());
        if(!compile || !compile->program) return nullptr;
        auto cursor = to_loc(params.textDocument, params.position);

        // Implicits are the one thing in artic whose target is decided by the compiler
        // rather than written down, so this is where "go to implementation" earns its name.
        // `Summoner::resolve` already records its choice in `SummonExpr::resolved`.
        const ast::SummonExpr* summon = summon_at(*compile->program, cursor);
        if(!summon || !summon->resolved) {
            log::info("[LSP] >>> Implementation found no summoned implicit at cursor");
            return nullptr;
        }

        auto loc = to_location(summon->resolved->loc);
        log::info("[LSP] >>> Implementation {}:{}:{}", loc.uri.path(), loc.range.start.line + 1, loc.range.start.character + 1);
        return lsp::Definition(loc);
    });

    message_handler_.add<reqst::TextDocument_DocumentHighlight>([this](reqst::TextDocument_DocumentHighlight::Params&& params) -> reqst::TextDocument_DocumentHighlight::Result {
        log_request("TextDocument DocumentHighlight", params.textDocument, params.position);

        if(get_file_type(params.textDocument.uri.path()) != FileType::SourceFile) return nullptr;
        ensure_compile(params.textDocument.uri.path());
        auto cursor = to_loc(params.textDocument, params.position);
        auto& name_map = compile->name_map;

        const ast::NamedDecl* decl = decl_at(name_map, cursor);
        if(!decl) {
            log::info("[LSP] >>> DocumentHighlight found no symbol at cursor");
            return nullptr;
        }

        // The whole project is compiled at once, so occurrences in other files must be
        // dropped: a highlight range is only meaningful in the document that was asked for.
        lsp::Array<lsp::DocumentHighlight> highlights;
        auto add = [&](const Loc& loc, lsp::DocumentHighlightKind kind) {
            if(!loc.file || *loc.file != *cursor.file) return;
            highlights.push_back(lsp::DocumentHighlight{
                .range = to_range(loc),
                .kind = lsp::DocumentHighlightKindEnum(kind)
            });
        };

        add(decl->id.loc, lsp::DocumentHighlightKind::Write);
        for (auto ref : name_map.find_refs(decl)) add(name_map.get_identifier(ref).loc, lsp::DocumentHighlightKind::Read);

        if(highlights.empty()) return nullptr;
        log::info("[LSP] >>> Returning {} highlights of '{}'", highlights.size(), decl->id.name);
        return highlights;
    });

    message_handler_.add<reqst::TextDocument_References>([this](lsp::ReferenceParams&& params) -> reqst::TextDocument_References::Result {
        log_request("TextDocument References", params.textDocument, params.position);

        if(get_file_type(params.textDocument.uri.path()) != FileType::SourceFile) return {};
        ensure_compile(params.textDocument.uri.path());
        auto cursor = to_loc(params.textDocument, params.position);
        auto occurences = find_occurrences_of_identifier(compile->name_map, cursor);
        if(!occurences) return {};
        log::info("[LSP] >>> Found {} occurrences of identifier", occurences->all_occurences.size());
        return occurences->all_occurences;
    });

    message_handler_.add<reqst::TextDocument_PrepareRename>([this](lsp::TextDocumentPositionParams&& params) -> reqst::TextDocument_PrepareRename::Result {
        log_request("TextDocument PrepareRename", params.textDocument, params.position);

        if(get_file_type(params.textDocument.uri.path()) != FileType::SourceFile) return nullptr;
        ensure_compile(params.textDocument.uri.path());
        auto cursor = to_loc(params.textDocument, params.position);
        auto occurences = find_occurrences_of_identifier(compile->name_map, cursor);
        if(!occurences) {
            log::info("[LSP] >>> PrepareRename found no symbol at cursor");
            return nullptr;
        }

        // Success: return the range of the symbol to be renamed
        log::info("[LSP] >>> PrepareRename successful for symbol '{}'", occurences->name);
        auto res = lsp::PrepareRenameResult_Range_Placeholder {
            .range = occurences->cursor_range.range,
            .placeholder = occurences->name
        };
        return lsp::PrepareRenameResult(res);
    });

    message_handler_.add<reqst::TextDocument_Rename>([this](lsp::RenameParams&& params) -> reqst::TextDocument_Rename::Result {
        log_request("TextDocument Rename", params.textDocument, params.position);

        if(get_file_type(params.textDocument.uri.path()) != FileType::SourceFile) return nullptr;
        ensure_compile(params.textDocument.uri.path());
        auto cursor = to_loc(params.textDocument, params.position);
        auto occurences = find_occurrences_of_identifier(compile->name_map, cursor);
        if(!occurences) {
            log::info("[LSP] >>> Rename found no symbol at cursor");
            return nullptr;
        }

        // Convert to LSP WorkspaceEdit format
        lsp::WorkspaceEdit workspace_edit;
        auto& changes = workspace_edit.changes.emplace();
        size_t total_edits = 0;
        for (auto& loc : occurences->all_occurences) {
            changes[loc.uri].emplace_back(
                lsp::TextEdit {
                    .range = loc.range,
                    .newText = params.newName
                }
            );
            ++total_edits;
        }

        log::info("[LSP] >>> Rename operation will edit {} files with {} total edits", workspace_edit.changes->size(), total_edits);

        return workspace_edit;
    });
}


// -----------------------------------------------------------------------------
//
//
// Completion
//
//
// -----------------------------------------------------------------------------


// Completion Helper Functions
namespace {

lsp::CompletionItemKind get_completion_kind(const ast::NamedDecl* decl) {
    if (decl->isa<ast::FnDecl>()) return lsp::CompletionItemKind::Function;
    if (decl->isa<ast::StaticDecl>()) return lsp::CompletionItemKind::Variable;
    if (decl->isa<ast::PtrnDecl>()) return lsp::CompletionItemKind::Variable;
    if (decl->isa<ast::StructDecl>()) return lsp::CompletionItemKind::Struct;
    if (decl->isa<ast::EnumDecl>()) return lsp::CompletionItemKind::Enum;
    if (decl->isa<ast::TypeDecl>()) return lsp::CompletionItemKind::TypeParameter;
    if (decl->isa<ast::FieldDecl>()) return lsp::CompletionItemKind::Field;
    if (decl->isa<ast::ModDecl>()) return lsp::CompletionItemKind::Module;
    return lsp::CompletionItemKind::Text;
}

bool same_file(const Loc& a, const Loc& b) { return a.file && b.file && *a.file == *b.file; }
bool overlaps(const Loc& a, const Loc& b) { return a.end > /* important > */ b.begin && a.begin <= b.end; }

/// The pattern a loop binds, or null when it binds none. `for x in gen(..)` is stored
/// already desugared as `gen(|x| { .. })(..)`, so the loop variable is the parameter of a
/// closure the source never spells out.
const ast::Ptrn* loop_binding(const ast::LoopExpr& loop) {
    if (const auto* while_expr = loop.isa<ast::WhileExpr>()) return while_expr->ptrn.get();
    const auto* for_expr = loop.isa<ast::ForExpr>();
    if (!for_expr || !for_expr->call || !for_expr->call->callee) return nullptr;
    const auto* generator = for_expr->call->callee->isa<ast::CallExpr>();
    if (!generator || !generator->arg) return nullptr;
    const auto* lambda = generator->arg->isa<ast::FnExpr>();
    return lambda ? lambda->param.get() : nullptr;
}

lsp::CompletionItem completion_item(const ast::FnDecl* fn) {
    lsp::CompletionItem item;
    item.insertTextFormat = lsp::InsertTextFormat::Snippet;
    std::stringbuf lb; 
    std::ostream str0(&lb);
    log::Output label(str0, false);
    Printer l(label);
    item.filterText = fn->id.name;

    label << fn->id.name;

    if (fn->type_params) fn->type_params->print(l);
    if (auto* param = fn->fn->param.get()) print_param_list(l, *param);
    
    item.label = lb.str();
    lb.str("");

    if(const auto* type = fn->type) {
        if(const auto* forall = type->isa<ForallType>()) type = forall->body;
        if(type) if(const auto* f = type->isa<FnType>()) {
            f->codom->print(l);
            item.detail = lb.str();
        }
    }
    if (!item.detail && fn->fn->ret_type) {
        fn->fn->ret_type->print(l);
        item.detail = lb.str();
    }

    item.insertText = fn->id.name;
    return item;
}

std::optional<lsp::CompletionItem> completion_item(const ast::NamedDecl& decl) {
    if(decl.id.name.empty()) return std::nullopt;
    if(decl.id.name.starts_with('_')) return std::nullopt;

    if (auto fn = decl.isa<ast::FnDecl>()) return completion_item(fn);

    lsp::CompletionItem item;

    item.kind = get_completion_kind(&decl);

    if(decl.type) {
        if (auto fn = decl.type->isa<FnType>()) {
            item.kind = lsp::CompletionItemKind::Function;

            std::stringbuf lb; 
            std::ostream str0(&lb);
            log::Output label(str0, false);
            Printer l(label);
            label << decl.id.name;
            if (fn->dom) print_param_list(l, *fn->dom);
            item.label = lb.str();
            if(fn->codom) {
                lb.str("");
                fn->codom->print(l);
                item.detail = lb.str();
            }

            std::stringbuf pt; 
            std::ostream str1(&pt);
            log::Output ptrn(str1, false);
            Printer p(ptrn);
            int arg = 1;
            ptrn << decl.id.name << "(";
            
            if(fn->dom) {
                if(const auto* tuple = fn->dom->isa<TupleType>()) {
                    for(size_t i = 0; i < tuple->args.size(); i++) {
                        if(i > 0) ptrn << ", ";
                        ptrn << "${" << arg++ << ":" ;
                        tuple->args[i]->print(p);
                        ptrn << "}";
                    }
                } else {
                    ptrn << "${" << arg++ << ":" ;
                    fn->dom->print(p);
                    ptrn << "}";
                }
            } 
            ptrn << ")";
            ptrn << "$0";
            item.insertText = pt.str();
        }
    }
    
    if(item.label.empty()){
        item.label = decl.id.name;
    }

    if (!item.detail && decl.type) {
        item.detail = print_to_string(*decl.type);
    }
    return item;
}

} // anonymous namespace

void Server::setup_events_completion() {
    message_handler_.add<reqst::TextDocument_Completion>([this](lsp::CompletionParams&& params) -> reqst::TextDocument_Completion::Result {
        log_request("TextDocument Completion", params.textDocument, params.position);
        if(get_file_type(params.textDocument.uri.path()) != FileType::SourceFile) return nullptr;
        ensure_compile(params.textDocument.uri.path());
        Loc cursor = to_loc(params.textDocument, params.position);
        const ast::ModDecl* current_module = compile->program.get();
        std::vector<const ast::Node*> local_scopes;
        const ast::Node* outer_node = nullptr;
        const ast::Node* inner_node = nullptr;
        bool only_show_types = false;
        bool inside_block_expr = false;
        bool top_level = false;

        auto is_type_decl = [](const ast::NamedDecl& decl) -> bool {
            return decl.isa<ast::CtorDecl>() || decl.isa<ast::ModDecl>() || decl.isa<ast::TypeParam>() || decl.isa<ast::TypeDecl>() || decl.isa<ast::UseDecl>();
        };

        lsp::CompletionList result{
            .isIncomplete = false,
            .items = {},
            .itemDefaults = lsp::CompletionListItemDefaults{ .insertTextFormat = lsp::InsertTextFormat::Snippet },
        };

        ast::Node::TraverseFn traverse([&](const ast::Node& node) -> bool {
            if(!node.loc.file) return true; // super module
            if(!same_file(cursor, node.loc)) return false;
            if(!overlaps(cursor, node.loc)) {
                return false;
            } else if(!outer_node) {
                outer_node = &node;
            }
            if(!only_show_types && (node.isa<ast::TypedExpr>() || node.isa<ast::TypedPtrn>() || node.isa<ast::TypeApp>())){
                only_show_types = true;
            } else if(const auto* mod = node.isa<ast::ModDecl>()){
                current_module = mod;
            } else if(const auto* fn = node.isa<ast::FnDecl>()){
                if(fn->type_params) local_scopes.push_back(fn->type_params.get());
            } else if(const auto* fn_expr = node.isa<ast::FnExpr>()){
                // Covers a function's own parameters as well as the closure a `for x in ...`
                // loop desugars into, whose parameter is the loop variable.
                if(fn_expr->param) local_scopes.push_back(fn_expr->param.get());
            } else if(const auto* case_expr = node.isa<ast::CaseExpr>()){
                if(case_expr->ptrn) local_scopes.push_back(case_expr->ptrn.get());
            } else if(const auto* loop = node.isa<ast::LoopExpr>()){
                if(const auto* ptrn = loop_binding(*loop)) local_scopes.push_back(ptrn);
            } else if(const auto* block = node.isa<ast::BlockExpr>()){
                local_scopes.push_back(block);
                inside_block_expr = true;
                top_level = false;
            } else if(const auto* error = node.isa<ast::ErrorDecl>(); error && error->is_top_level) {
                top_level = true;
            }
            inner_node = &node;

            return true;
        });
        
        traverse(compile->program);
        if(!current_module) {
            log::info("Error with completion: current_module is null");
            return result;
        }

        // ---
        // One possible modifier `only_show_types` if inside typed expression `a : type`
        // 
        // Different completion contexts:
        // 1. Projection expression `a.b`
        // 2. Path expression `a::b` (do not count if its just a single identifier `a`) | uses `only_show_types`
        // 3. Top level declarations `struct a`
        // 
        // 4. Default: (includes case where inner_node cannot be identified) | uses `only_show_types`
        // Show top_level decls in current module
        // If inside block expr, also show local declarations

        if(inner_node) {
            // 1. Projection expression: a.b
            if(const auto* proj_expr = inner_node->isa<ast::ProjExpr>()) {
                log::info("Showing completion for ProjExpr");
                const Type* type = nullptr;
                if (auto t = proj_expr->type; t && !t->isa<TypeError>()) type = t;
                else if (auto t = proj_expr->expr->type; t && !t->isa<TypeError>()) type = t;
                if (type) {
                    if(auto addr = type->isa<AddrType>(); addr && addr->pointee) type = addr->pointee; // remove reference
                    if(auto app = type->isa<TypeApp>(); app && app-> applied) type = app->applied; // collapse polymorphic type
                    if(auto struct_type = type->isa<StructType>()) {
                        for (auto& field : struct_type->decl.fields) {
                            if(auto item = completion_item(*field)) result.items.push_back(std::move(*item));
                        }
                    } else if(auto enum_type = type->isa<EnumType>()) {
                        for (auto& option : enum_type->decl.options) {
                            if(auto item = completion_item(*option)) result.items.push_back(std::move(*item));
                        }
                    }
                } else {
                    log::info("type could not be identified");
                }
                log::info("{} projection items", result.items.size());
                return result;
            } 
            // 2. Path expression: a::b
            if(const auto* path = inner_node->isa<ast::Path>(); path && path->elems.size() > 1) {
                log::info("Showing completion for Path");
                const ast::Path::Elem* path_elem = &path->elems.front();
                // find element of path
                for (const auto& elem: path->elems) {
                    if(cursor.end > elem.loc.end) path_elem = &elem;
                }

                // Element type cannot be resolved -> no completion
                if(!path_elem->type) return result;
                
                auto path_module = current_module;
                if(const auto* mod = path_elem->type->isa<ModType>()) path_module = &mod->decl;

                // Collect elements in current module
                for (const auto& decl : path_module->decls) {
                    if (const auto* named_decl = decl->isa<ast::NamedDecl>(); 
                        named_decl && (!only_show_types || is_type_decl(*named_decl))
                    ) {
                        if(auto item = completion_item(*named_decl)) result.items.push_back(std::move(*item));
                    }
                }
                std::reverse(result.items.begin(), result.items.end());
                return result;
            }
        }
        
        log::info("Showing completion for top level declaration");
        // 3. Top level declaration: struct a
        if(top_level) {
            // Top level snippets
            result.items.push_back(lsp::CompletionItem {
                .label = "fn",
                .kind = lsp::CompletionItemKind::Keyword,
                .detail = "Function Declaration",
                .insertText = "fn @${1:function}($2) -> ${3:ret_type} {\n\t$0\n}",
            });

            result.items.push_back(lsp::CompletionItem {
                .label = "struct",
                .kind = lsp::CompletionItemKind::Keyword,
                .detail = "Struct Declaration",
                .insertText = "struct ${1:StructName} {\n\t${0}\n}",
            });

            result.items.push_back(lsp::CompletionItem {
                .label = "record",
                .kind = lsp::CompletionItemKind::Keyword,
                .detail = "Record Declaration",
                .insertText = "struct ${1:RecordName}($2);$0",
            });

            result.items.push_back(lsp::CompletionItem {
                .label = "mod",
                .kind = lsp::CompletionItemKind::Keyword,
                .detail = "Module Declaration",
                .insertText = "mod ${1:module_name} {\n\t${0}\n}",
            });

            result.items.push_back(lsp::CompletionItem {
                .label = "enum",
                .kind = lsp::CompletionItemKind::Keyword,
                .detail = "Enum Declaration",
                .insertText = "enum ${1:EnumName} {\n\t${0}\n}",
            });

            result.items.push_back(lsp::CompletionItem {
                .label = "static",
                .kind = lsp::CompletionItemKind::Keyword,
                .detail = "Static Declaration",
                .insertText = "static ${1:variable} = ${2:value};$0",
            });

            result.items.push_back(lsp::CompletionItem {
                .label = "type",
                .kind = lsp::CompletionItemKind::Keyword,
                .detail = "Type Alias Declaration",
                .insertText = "type ${1:TypeName} = ${2:UnderlyingType};$0",
            });

            result.items.push_back(lsp::CompletionItem {
                .label = "use",
                .kind = lsp::CompletionItemKind::Keyword,
                .detail = "Use Declaration",
                .insertText = "use ${1:module_name} as ${2:alias_name};$0",
            });

            return result;
        }

        // 4. Default case
        log::info("Showing default completion");
        log::info("Only types: {}", only_show_types);

        // Top level declarations in current module
        for (const auto& decl : current_module->decls) {
            if (const auto* named_decl = decl->isa<ast::NamedDecl>(); 
                named_decl && (!only_show_types || is_type_decl(*named_decl))
            ) {
                if(auto item = completion_item(*named_decl)) result.items.push_back(std::move(*item));
            }
        }

        if (inside_block_expr){
            // Declarations in local scope
            ast::Node::TraverseFn collect_local_decls([&](const ast::Node& node) -> bool {
                // Every scope that encloses the cursor is already a `local_scopes` entry of
                // its own, so descending into a nested one either duplicates its bindings or
                // offers bindings that are not visible from the cursor at all. `for a in ...`
                // desugars into a call taking an `FnExpr`, which is why the loop variable used
                // to be offered after the loop had ended.
                if(collect_local_decls.depth > 0 &&
                   (node.isa<ast::BlockExpr>() || node.isa<ast::FnExpr>() ||
                    node.isa<ast::CaseExpr>() || node.isa<ast::LoopExpr>())) {
                    return false;
                }
                if (const auto* named_decl = node.isa<ast::NamedDecl>(); 
                    named_decl && (!only_show_types || is_type_decl(*named_decl))
                ) {
                    if(auto item = completion_item(*named_decl)) result.items.push_back(std::move(*item));
                }
                return true;
            });
            for (const auto* scope : local_scopes) {
                collect_local_decls(*scope);
            }

            // Local snippets
            if(!only_show_types){
                result.items.push_back(lsp::CompletionItem {
                    .label = "for",
                    .kind = lsp::CompletionItemKind::Keyword,
                    .detail = "For Loop",
                    .insertText = "for ${1:i} in ${2:range} {\n\t$0\n}",
                });

                result.items.push_back(lsp::CompletionItem {
                    .label = "forrange",
                    .kind = lsp::CompletionItemKind::Keyword,
                    .detail = "Range For Loop",
                    .insertText = "for ${1:i} in range(${2:0}, ${3:count}) {\n\t$0\n}",
                });

                result.items.push_back(lsp::CompletionItem {
                    .label = "if",
                    .kind = lsp::CompletionItemKind::Keyword,
                    .detail = "If Statement",
                    .insertText = "if ${1:condition} {\n\t$0\n}",
                });

                result.items.push_back(lsp::CompletionItem {
                    .label = "else",
                    .kind = lsp::CompletionItemKind::Keyword,
                    .detail = "Else Statement",
                    .insertText = "else {\n\t$0\n}",
                });

                result.items.push_back(lsp::CompletionItem {
                    .label = "match",
                    .kind = lsp::CompletionItemKind::Keyword,
                    .detail = "Match Expression",
                    .insertText = "match ${1:expression} {\n\t${2:pattern} => ${3:result},\n\t${0}\n}",
                });

                result.items.push_back(lsp::CompletionItem {
                    .label = "let",
                    .kind = lsp::CompletionItemKind::Keyword,
                    .detail = "Let Binding",
                    .insertText = "let ${1:variable} = ${2:value};$0",
                });

                result.items.push_back(lsp::CompletionItem {
                    .label = "return",
                    .kind = lsp::CompletionItemKind::Keyword,
                    .detail = "Return Statement",
                    .insertText = "return($1)$0",
                });

                result.items.push_back(lsp::CompletionItem {
                    .label = "continue",
                    .kind = lsp::CompletionItemKind::Keyword,
                    .detail = "Continue Statement",
                    .insertText = "continue()",
                });

                result.items.push_back(lsp::CompletionItem {
                    .label = "break",
                    .kind = lsp::CompletionItemKind::Keyword,
                    .detail = "Break Statement",
                    .insertText = "break()",
                });

                result.items.push_back(lsp::CompletionItem {
                    .label = "asm",
                    .kind = lsp::CompletionItemKind::Keyword,
                    .detail = "Assembly Block",
                    .insertText = "asm(\"$1\"$2);$0",
                });
            }
            
            static auto prim_types = std::vector<std::string_view> {
                "bool", 
                "i8", "i16", "i32", "i64", 
                "u8", "u16", "u32", "u64", 
                "f16", "f32", "f64", 
                "simd", "mut", "super"
            };
            for (auto& prim : prim_types) {
                lsp::CompletionItem item;
                item.kind = lsp::CompletionItemKind::Keyword;
                item.label = prim;
                result.items.push_back(std::move(item));
            }
            
            result.items.push_back(lsp::CompletionItem {
                .label = "simd[...]",
                .kind = lsp::CompletionItemKind::Keyword,
                .insertText = "simd[${1:expr}]$0",
            });

            result.items.push_back(lsp::CompletionItem {
                .label = "addrspace(...)",
                .kind = lsp::CompletionItemKind::Keyword,
                .insertText = "addrspace(${1:1})$0",
            });

            result.items.push_back(lsp::CompletionItem {
                .label = "void",
                .kind = lsp::CompletionItemKind::Keyword,
                .detail = "()",
                .insertText = "()",
            });
        }

        std::reverse(result.items.begin(), result.items.end());
        return result;
    });
}


// -----------------------------------------------------------------------------
//
//
// Signature Help
//
//
// -----------------------------------------------------------------------------


// Signature Help Helper Functions
namespace {

/// An unclosed bracket in front of the cursor, and how many commas of that bracket's own
/// nesting level separate it from the cursor.
struct BracketFrame {
    size_t open;
    unsigned commas;
};

/// Signature help fires on half-written calls, where the parser leaves an error node
/// instead of a `CallExpr`, so the call site is located in the source text.
std::optional<BracketFrame> enclosing_bracket(std::string_view src, size_t cursor) {
    std::vector<BracketFrame> stack;
    for (size_t i = 0; i < cursor; ++i) {
        switch (src[i]) {
            case '/':
                if (i + 1 < cursor && src[i + 1] == '/') {
                    while (i < cursor && src[i] != '\n') ++i;
                } else if (i + 1 < cursor && src[i + 1] == '*') {
                    // Artic's block comments do not nest, so the first `*/` closes this one.
                    for (i += 2; i + 1 < cursor && (src[i] != '*' || src[i + 1] != '/'); ++i) {}
                    ++i;
                }
                break;
            case '"':
            case '\'': {
                const char quote = src[i++];
                while (i < cursor && src[i] != quote) i += src[i] == '\\' ? 2 : 1;
                break;
            }
            case '(': case '[': case '{': stack.push_back({ i, 0 }); break;
            case ')': case ']': case '}': if (!stack.empty()) stack.pop_back(); break;
            case ',': if (!stack.empty()) ++stack.back().commas; break;
            default: break;
        }
    }
    if (stack.empty()) return std::nullopt;
    return stack.back();
}

/// The byte range of the callee identifier in front of an opening parenthesis, skipping a
/// `[...]` type-argument list. Empty for a plain parenthesised expression.
std::optional<std::pair<size_t, size_t>> callee_before(std::string_view src, size_t open) {
    auto skip_spaces = [&](size_t i) {
        while (i > 0 && std::isspace(static_cast<unsigned char>(src[i - 1]))) --i;
        return i;
    };

    size_t end = skip_spaces(open);
    if (end > 0 && src[end - 1] == ']') {
        for (int depth = 0; end > 0;) {
            --end;
            if (src[end] == ']') ++depth;
            else if (src[end] == '[' && --depth == 0) break;
        }
        end = skip_spaces(end);
    }
    size_t begin = end;
    while (begin > 0 && (std::isalnum(static_cast<unsigned char>(src[begin - 1])) || src[begin - 1] == '_'))
        --begin;
    if (begin == end) return std::nullopt;
    return std::pair{ begin, end };
}

std::optional<size_t> offset_at(const LocatorInfo& file, const Loc::Pos& pos) {
    if (pos.row < 1 || size_t(pos.row) >= file.lines.size()) return std::nullopt;
    return size_t(file.at(pos.row, pos.col) - file.data.data());
}

Loc::Pos position_at(const LocatorInfo& file, size_t offset) {
    auto line = std::upper_bound(file.lines.begin(), file.lines.end(), offset);
    int row = std::max(1, static_cast<int>(line - file.lines.begin()));
    int col = 1;
    // Columns count code points, so UTF-8 continuation bytes must not advance one.
    for (size_t i = file.lines[row - 1]; i < offset; ++i)
        if ((static_cast<unsigned char>(file.data[i]) & 0xC0) != 0x80) ++col;
    return Loc::Pos{ .row = row, .col = col };
}

struct RenderedSignature {
    std::string label;
    lsp::Array<lsp::ParameterInformation> params;
};

void add_param(RenderedSignature& sig, const std::string& text) {
    auto begin = static_cast<lsp::uint>(sig.label.size());
    sig.label += text;
    sig.params.push_back(lsp::ParameterInformation{
        .label = std::tuple{ begin, static_cast<lsp::uint>(sig.label.size()) }
    });
}

/// Renders the callee as `fn name[T](a: i32, b: i32) -> i32`, recording where each
/// parameter sits inside the label so the client can highlight the active one.
std::optional<RenderedSignature> render_signature(const ast::NamedDecl& decl) {
    const Type* type = decl.type;
    if (const auto* forall = type ? type->isa<ForallType>() : nullptr) type = forall->body;

    const auto* fn_decl = decl.isa<ast::FnDecl>();
    // An enum option's type is its payload rather than a function type, so a constructor
    // call can only be rendered from the declaration.
    const auto* option_decl = decl.isa<ast::OptionDecl>();
    const auto* fn_type = type ? type->isa<FnType>() : nullptr;
    if (!fn_decl && !fn_type && !(option_decl && option_decl->param)) return std::nullopt;

    RenderedSignature sig;
    if (fn_decl) sig.label += "fn ";
    if (option_decl && option_decl->parent) sig.label += option_decl->parent->id.name + "::";
    sig.label += decl.id.name;
    if (fn_decl && fn_decl->type_params) sig.label += print_to_string(*fn_decl->type_params);

    auto add_params = [&](const auto& nodes) {
        for (size_t i = 0; i < nodes.size(); ++i) {
            if (i > 0) sig.label += ", ";
            add_param(sig, print_to_string(*nodes[i]));
        }
    };

    sig.label += '(';
    // Only a declaration carries parameter names; anything else callable has just a type.
    if (option_decl) {
        if (const auto* tuple = option_decl->param->isa<ast::TupleType>()) add_params(tuple->args);
        else add_param(sig, print_to_string(*option_decl->param));
    } else if (fn_decl && fn_decl->fn->param) {
        const auto& param = *fn_decl->fn->param;
        if (const auto* tuple = param.isa<ast::TuplePtrn>()) add_params(tuple->args);
        else add_param(sig, print_to_string(param));
    } else if (fn_type && fn_type->dom) {
        if (const auto* tuple = fn_type->dom->isa<TupleType>()) add_params(tuple->args);
        else add_param(sig, print_to_string(*fn_type->dom));
    }
    sig.label += ')';

    if (fn_decl && fn_decl->fn->ret_type) sig.label += " -> " + print_to_string(*fn_decl->fn->ret_type);
    else if (!option_decl && fn_type && fn_type->codom) sig.label += " -> " + print_to_string(*fn_type->codom);

    return sig;
}

} // anonymous namespace

void Server::setup_events_signature_help() {
    message_handler_.add<reqst::TextDocument_SignatureHelp>([this](reqst::TextDocument_SignatureHelp::Params&& params) -> reqst::TextDocument_SignatureHelp::Result {
        log_request("TextDocument SignatureHelp", params.textDocument, params.position);

        if (get_file_type(params.textDocument.uri.path()) != FileType::SourceFile) return nullptr;
        ensure_compile(params.textDocument.uri.path());

        auto cursor = to_loc(params.textDocument, params.position);
        const LocatorInfo* file = compile->locator.data(*cursor.file);
        if (!file) return nullptr;

        auto cursor_offset = offset_at(*file, cursor.begin);
        if (!cursor_offset) return nullptr;

        auto frame = enclosing_bracket(file->data, *cursor_offset);
        if (!frame || file->data[frame->open] != '(') {
            log::info("[LSP] >>> SignatureHelp: cursor is not inside an argument list");
            return nullptr;
        }

        auto callee = callee_before(file->data, frame->open);
        if (!callee) return nullptr;

        // A caret on the identifier's first character, so the lookup does not depend on
        // how the lexer spells the end of a token.
        Loc name_loc(cursor.file, position_at(*file, callee->first));
        auto ref = compile->name_map.find_ref_at(name_loc);
        const ast::NamedDecl* decl = ref ? compile->name_map.find_decl(*ref) : nullptr;
        if (!decl) {
            log::info("[LSP] >>> SignatureHelp found no declaration for the callee");
            return nullptr;
        }

        auto sig = render_signature(*decl);
        if (!sig) {
            log::info("[LSP] >>> SignatureHelp: '{}' is not callable", decl->id.name);
            return nullptr;
        }

        lsp::SignatureInformation signature{ .label = std::move(sig->label) };
        if (!sig->params.empty()) {
            // An index past the last parameter would make the client highlight the first
            // one instead, which is worse than sticking to the last.
            signature.activeParameter = std::min<lsp::uint>(frame->commas, lsp::uint(sig->params.size() - 1));
            signature.parameters = std::move(sig->params);
        }
        log::info("[LSP] >>> SignatureHelp '{}' at parameter {}", signature.label, frame->commas);

        return lsp::SignatureHelp{
            .signatures = { std::move(signature) },
            .activeSignature = 0u,
        };
    });
}


// -----------------------------------------------------------------------------
//
//
// Server Compilation / Diagnostics
//
//
// -----------------------------------------------------------------------------

void Server::compile_this_and_related_files(std::filesystem::path file, std::string* new_content) {
    file = paths::canonical_path(file);

    if(new_content) workspace_->set_file_content(file, std::move(*new_content));

    workspace::config::ConfigLog cfg_log;
    auto files = workspace_->collect_project_files(file, cfg_log);
    publish_config_diagnostics(cfg_log);
    
    if (files.empty()) {
        log::info("No input files to compile");
        return;
    }
    log::info("Compiling {} file(s)", files.size());
    for (const auto* f : files) {
        log::info(" - {}", f->path.generic_string());
    }

    // Initialize
    compile.emplace();
    if(safe_mode_) {
        compile->exclude_non_parsed_files = true;
        log::info("Using safe mode");
    }
    try {
        // Compile
        compile->compile_files(files, file);
    } catch(const std::exception& e) {
        log::info("Compilation failed with error: {}", e.what());
        compile.reset();
        return;
    }

    if(safe_mode_ && compile->parsed_all) {
        safe_mode_ = false;
        log::info("Successfully parsed all files, turning off safe mode");
    }

    if(compile->log.errors == 0){
        log::info("Compile success");
    } else {
        log::info("Compile failed");
    }

    // Send Diagnostics for the provided files only
    std::unordered_map<std::string, std::vector<lsp::Diagnostic>> diagnostics_by_file;
    for (const auto& diag : compile->diagnostics) {
        diagnostics_by_file[*diag.loc.file].push_back(to_diagnostic(diag));
    }
    for (const auto* file : files) {
        auto path = file->path.generic_string();

        message_handler_.sendNotification<notif::TextDocument_PublishDiagnostics>(
            notif::TextDocument_PublishDiagnostics::Params {
                .uri = to_file_uri(file->path),
                .diagnostics = diagnostics_by_file.contains(path) ? diagnostics_by_file.at(path) : std::vector<lsp::Diagnostic>{}
            }
        );
    }
}

void Server::ensure_compile(std::string_view file_view) {
    fs::path file = absolute_path(file_view);
    if(get_file_type(file) != FileType::SourceFile) {
        throw lsp::RequestError(lsp::Error::InvalidParams, "File is not an Artic source file");
    }
    if (!has_compiled(file)) compile_this_and_related_files(file);
    if (!compile) throw lsp::RequestError(lsp::Error::ServerCancelled, "Did not get a compilation result");
}


// -----------------------------------------------------------------------------
//
//
// Server Reload Workspace
//
//
// -----------------------------------------------------------------------------


void Server::publish_config_diagnostics(const workspace::config::ConfigLog& log) {
    std::unordered_map<std::filesystem::path, std::vector<lsp::Diagnostic>> fileDiags;

    // Fallback for a message that carries no range: build-file parsers have no position
    // information at all. Searching the text finds every textual occurrence, so one logical
    // error can produce several diagnostics -- which is exactly why anything parsed out of
    // a JSON config carries the range nlohmann recorded for the value instead.
    auto find_in_file = [](std::filesystem::path const& file, std::string_view literal) -> std::vector<lsp::Range> {
        std::vector<lsp::Range> ranges;
        if(literal.empty()) return ranges;
        std::ifstream ifs(file);
        if (!ifs) return ranges;

        std::string line;
        lsp::uint line_number = 0;
        while (std::getline(ifs, line)) {
            size_t pos = line.find(literal);
            while (pos != std::string::npos) {
                ranges.push_back(lsp::Range{
                    lsp::Position{line_number, static_cast<lsp::uint>(pos)},
                    lsp::Position{line_number, static_cast<lsp::uint>(pos + literal.size())}
                });
                pos = line.find(literal, pos + 1);
            }
            line_number++;
        }
        return ranges;
    };

    // create diagnostics
    for (const auto& msg : log.messages) {
        if(msg.file.empty() || !fs::exists(msg.file)) {
            log::info("Dropping config message with no reportable file ({}): {}", msg.file.generic_string(), msg.message);
            continue;
        }

        lsp::Diagnostic diag;
        diag.message = msg.message;
        diag.severity = msg.severity;
        diag.range = lsp::Range{ lsp::Position{0,0}, lsp::Position{0,0} };

        if (msg.range) {
            diag.range = *msg.range;
            fileDiags[msg.file].push_back(std::move(diag));
            continue;
        }

        auto occurrences = msg.context.has_value()
            ? find_in_file(msg.file, msg.context.value().literal)
            : std::vector<lsp::Range>{};

        for(auto& occ : occurrences) {
            lsp::Diagnostic pos_diag(diag);
            pos_diag.range = occ;
            fileDiags[msg.file].push_back(pos_diag);
        }
        if(occurrences.empty()) fileDiags[msg.file].push_back(diag);
    }

    auto publish = [this](const fs::path& file, std::vector<lsp::Diagnostic> diags) {
        message_handler_.sendNotification<notif::TextDocument_PublishDiagnostics>(
            notif::TextDocument_PublishDiagnostics::Params {
                .uri = to_file_uri(file),
                .diagnostics = std::move(diags)
            }
        );
    };

    // Clear files that were reported last time but are clean now, otherwise the editor
    // keeps displaying diagnostics that no longer exist. Only files this pass actually
    // re-evaluated may be cleared: configs are cached, so a pass that merely compiles a
    // source file says nothing about them and must leave their diagnostics standing.
    std::set<fs::path> still_published;
    for (const auto& file : published_config_diagnostics_) {
        if (fileDiags.contains(file)) continue;
        if (log.evaluated_files.contains(file)) publish(file, {});
        else still_published.insert(file);
    }

    published_config_diagnostics_ = std::move(still_published);
    for(auto& [file, diags] : fileDiags) {
        published_config_diagnostics_.insert(file);
        publish(file, diags);
    }
}

void Server::reload_workspace() {
    log::info("Reloading workspace configuration");
    workspace::config::ConfigLog log;
    workspace_->reload();
    symbol_index_ = SymbolIndex{};
    publish_config_diagnostics(log);
    
    // Recompile last compile
    if (compile) {
        compile_this_and_related_files(compile->active_file);
    }
}

void Server::reload_config(const fs::path& file) {
    workspace::config::ConfigLog log{};
    bool known = workspace_->on_config_changed(file, log);
    // `on_config_changed` re-instantiates every project, so anything the index cached
    // under a project name now describes a project that no longer exists.
    symbol_index_ = SymbolIndex{};

    // Captured before the reset: what the editor is showing was derived from the config
    // that just moved, so it has to be built again from the new one.
    std::optional<fs::path> active;
    if(known && compile) active = compile->active_file;
    if(known) compile.reset();

    publish_config_diagnostics(log);
    if(active) compile_this_and_related_files(*active);
}


// -----------------------------------------------------------------------------
//
//
// Selection Range
//
//
// -----------------------------------------------------------------------------
// Selection Range Helper Functions
namespace {

/// Whether `loc` covers `pos`. The end is inclusive so that a cursor placed just after a
/// node still selects it, which is what Shift+Alt+Right does at the end of a word.
bool covers(const Loc& loc, const Loc::Pos& pos) {
    if (pos.row < loc.begin.row || pos.row > loc.end.row) return false;
    if (pos.row == loc.begin.row && pos.col < loc.begin.col) return false;
    if (pos.row == loc.end.row && pos.col > loc.end.col) return false;
    return true;
}

bool same_extent(const Loc& a, const Loc& b) {
    return a.begin.row == b.begin.row && a.begin.col == b.begin.col
        && a.end.row == b.end.row && a.end.col == b.end.col;
}

/// The AST spine at the cursor, outermost first. Nodes that do not cover the cursor are not
/// descended into, so this visits one path rather than the whole program.
std::vector<Loc> spine_at(const ast::ModDecl& program, const std::string& file, const Loc::Pos& pos) {
    std::vector<Loc> spine;
    ast::Node::TraverseFn collect([&](const ast::Node& node) {
        const auto& loc = node.loc;
        if (!loc.file || *loc.file != file || !covers(loc, pos)) return false;
        // Many nodes wrap a child of exactly the same extent; a duplicate range would make
        // the user press Shift+Alt+Right twice for one visible step.
        if (spine.empty() || !same_extent(spine.back(), loc)) spine.push_back(loc);
        return true;
    });
    for (auto& decl : program.decls) if (decl) decl->traverse(collect);
    return spine;
}

} // anonymous namespace

void Server::setup_events_selection_range() {
    message_handler_.add<reqst::TextDocument_SelectionRange>([this](reqst::TextDocument_SelectionRange::Params&& params) -> reqst::TextDocument_SelectionRange::Result {
        log_request("TextDocument SelectionRange", params.textDocument);

        if (get_file_type(params.textDocument.uri.path()) != FileType::SourceFile) return nullptr;
        ensure_compile(params.textDocument.uri.path());
        if (!compile->program) return nullptr;
        auto file = absolute_path(params.textDocument.uri.path()).generic_string();

        lsp::Array<lsp::SelectionRange> result;
        for (const auto& position : params.positions) {
            auto spine = spine_at(*compile->program, file, to_loc(params.textDocument, position).begin);
            if (spine.empty()) {
                // Every position must get an answer, or the client cannot tell which one
                // failed. An empty range at the cursor is the neutral reply.
                result.push_back(lsp::SelectionRange{ .range = lsp::Range{ .start = position, .end = position } });
                continue;
            }

            // Built outermost first, so each new node becomes the child of what came before.
            lsp::SelectionRange range{ .range = to_range(spine.front()) };
            for (size_t i = 1; i < spine.size(); ++i) {
                range = lsp::SelectionRange{
                    .range = to_range(spine[i]),
                    .parent = std::make_unique<lsp::SelectionRange>(std::move(range))
                };
            }
            result.push_back(std::move(range));
        }

        log::info("[LSP] >>> Returning {} selection ranges", result.size());
        return result;
    });
}


// -----------------------------------------------------------------------------
//
//
// Other
//
//
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// Inlay Hint Helper Functions
namespace {

/// The name a parameter pattern binds, or empty when it binds none the caller can address.
std::string parameter_name(const ast::Ptrn& ptrn) {
    // `a: Vec2` parses as a TypedPtrn wrapping the IdPtrn that carries the name.
    if (auto typed = ptrn.isa<ast::TypedPtrn>())
        return typed->ptrn ? parameter_name(*typed->ptrn) : std::string();
    // An implicit parameter is summoned, never written at the call site.
    if (ptrn.isa<ast::ImplicitParamPtrn>()) return {};
    auto id_ptrn = ptrn.isa<ast::IdPtrn>();
    return id_ptrn && id_ptrn->decl ? id_ptrn->decl->id.name : std::string();
}

/// Parameter names of a callee, in declaration order. A parameter that is not a plain
/// identifier yields an empty name, so its position can be skipped.
std::vector<std::string> parameter_names(const ast::NamedDecl& decl) {
    auto fn_decl = decl.isa<ast::FnDecl>();
    if (!fn_decl || !fn_decl->fn || !fn_decl->fn->param) return {};

    const auto& param = *fn_decl->fn->param;
    if (auto tuple = param.isa<ast::TuplePtrn>()) {
        std::vector<std::string> names;
        names.reserve(tuple->args.size());
        for (const auto& arg : tuple->args) names.push_back(arg ? parameter_name(*arg) : std::string());
        return names;
    }
    return { parameter_name(param) };
}

/// Whether labelling this argument would only repeat what the source already says.
bool argument_repeats_name(const ast::Expr& arg, const std::string& name) {
    auto path_expr = arg.isa<ast::PathExpr>();
    if (path_expr && path_expr->path.elems.size() == 1)
        return path_expr->path.elems.front().id.name == name;
    if (auto proj = arg.isa<ast::ProjExpr>(); proj && std::holds_alternative<ast::Identifier>(proj->field))
        return std::get<ast::Identifier>(proj->field).name == name;
    return false;
}

/// The declaration a call's callee names, or null when the callee is not a plain path.
const ast::NamedDecl* callee_decl(const ast::CallExpr& call, const ls::NameMap& name_map) {
    if (!call.callee) return nullptr;
    auto path_expr = call.callee->isa<ast::PathExpr>();
    if (!path_expr || path_expr->path.elems.empty()) return nullptr;
    // The last element is the callee itself; the earlier ones are the modules it sits in.
    auto ref = name_map.find_ref_at(path_expr->path.elems.back().id.loc);
    return ref ? name_map.find_decl(*ref) : nullptr;
}

void collect_parameter_hints(
    const ast::ModDecl& program,
    const ls::NameMap& name_map,
    const std::string& file,
    const lsp::Range& range,
    lsp::Array<lsp::InlayHint>& hints)
{
    auto add = [&](const ast::Expr& arg, const std::string& name) {
        if (name.empty() || name == "_" || argument_repeats_name(arg, name)) return;
        auto position = to_position(arg.loc.begin);
        if (!contains(range, position)) return;
        lsp::InlayHint hint;
        hint.position = position;
        hint.label = name + ":";
        hint.kind = lsp::InlayHintKindEnum(lsp::InlayHintKind::Parameter);
        hint.paddingLeft = false;
        hint.paddingRight = true;
        hints.push_back(std::move(hint));
    };

    ast::Node::TraverseFn visit([&](const ast::Node& node) {
        // `program` is every file of the project concatenated, so anything belonging to
        // another document is pruned rather than walked.
        if (!node.loc.file || *node.loc.file != file) return false;

        auto call = node.isa<ast::CallExpr>();
        if (!call || !call->arg) return true;

        auto decl = callee_decl(*call, name_map);
        if (!decl) return true;
        auto names = parameter_names(*decl);
        if (names.empty()) return true;

        if (auto tuple = call->arg->isa<ast::TupleExpr>()) {
            // A function whose single parameter is a tuple takes the whole tuple as one
            // argument; only a positional match can be labelled.
            if (tuple->args.size() != names.size()) return true;
            for (size_t i = 0; i < tuple->args.size(); ++i)
                if (tuple->args[i]) add(*tuple->args[i], names[i]);
        } else if (names.size() == 1) {
            add(*call->arg, names.front());
        }
        return true;
    });

    for (const auto& decl : program.decls) if (decl) decl->traverse(visit);
}

/// A config document, scanned for the string literals a project is described by.
///
/// A hint is placed at the range the parser recorded for the value, which is exact. Text
/// search is the fallback for a project that came from a build file, where there is no
/// range, and for a range that no longer describes the value -- the config is reparsed on
/// change, but the hints are computed against whatever is on disk now. Two projects may
/// well share a `files` pattern, so an occurrence is annotated at most once.
class ConfigDocument {
public:
    explicit ConfigDocument(const fs::path& file) {
        if (auto text = paths::read_file(file)) {
            std::istringstream is(*text);
            for (std::string line; std::getline(is, line); ) lines_.push_back(std::move(line));
        }
    }

    bool empty() const { return lines_.empty(); }

    /// End position of `value` at `range`, or of the next unannotated occurrence of
    /// `"value"` when there is no usable range.
    std::optional<lsp::Position> take(const std::optional<lsp::Range>& range, const std::string& value) {
        auto quoted = text::quote(value);
        if (range && holds(*range, quoted)) return range->end;
        for (lsp::uint row = 0; row < lines_.size(); ++row) {
            const auto& line = lines_[row];
            for (size_t col = line.find(quoted); col != std::string::npos; col = line.find(quoted, col + 1)) {
                if (!used_.insert({ row, col }).second) continue;
                return lsp::Position{ row, static_cast<lsp::uint>(col + quoted.size()) };
            }
        }
        return std::nullopt;
    }

private:
    bool holds(const lsp::Range& range, const std::string& quoted) const {
        if (range.start.line != range.end.line || range.start.line >= lines_.size()) return false;
        const auto& line = lines_[range.start.line];
        if (range.end.character > line.size() || range.end.character - range.start.character != quoted.size()) return false;
        return line.compare(range.start.character, quoted.size(), quoted) == 0;
    }

    std::vector<std::string> lines_;
    std::set<std::pair<lsp::uint, size_t>> used_;
};

std::string file_count(size_t n) {
    return std::to_string(n) + (n == 1 ? " file" : " files");
}

void collect_config_hints(
    workspace::Workspace& workspace,
    const fs::path& file,
    const lsp::Range& range,
    lsp::Array<lsp::InlayHint>& hints)
{
    ConfigDocument doc(file);
    if (doc.empty()) return;

    auto add = [&](std::optional<lsp::Position> position, std::string label) {
        if (!position || !contains(range, *position)) return;
        lsp::InlayHint hint;
        hint.position = *position;
        hint.label = std::move(label);
        hint.paddingLeft = true;
        hint.paddingRight = false;
        hints.push_back(std::move(hint));
    };

    for (const auto* project : workspace.projects_of_config(file)) {
        auto own = project->files.size();
        auto total = workspace.total_file_count(*project);
        std::string label = file_count(own);
        if (total != own) label += ", " + std::to_string(total) + " with dependencies";
        add(doc.take(project->name_range, project->name), std::move(label));

        for (const auto& match : project->pattern_matches) {
            std::string pattern_label = match.excludes
                ? file_count(match.changed) + " excluded"
                : file_count(match.changed);
            if (match.matched != match.changed)
                pattern_label += " of " + std::to_string(match.matched) + " matched";
            add(doc.take(match.range, match.pattern), std::move(pattern_label));
        }
    }
}

} // anonymous namespace


// -----------------------------------------------------------------------------
//
//
// Workspace Symbols and Code Lens
//
//
// -----------------------------------------------------------------------------
// Symbol Helper Functions
namespace {

/// A client that opens the picker with an empty query asks for everything there is, and a
/// large workspace has more of it than anyone scrolls through.
constexpr size_t max_workspace_symbols = 1000;

/// Declarations worth a reference count above them. Fields and enum options are left out:
/// a lens per struct field turns a record into a ladder of grey text.
bool deserves_code_lens(const ast::Decl& decl) {
    return decl.isa<ast::FnDecl>()
        || decl.isa<ast::StructDecl>()
        || decl.isa<ast::EnumDecl>()
        || decl.isa<ast::TypeDecl>()
        || decl.isa<ast::StaticDecl>()
        || decl.isa<ast::ModDecl>();
}

void collect_code_lenses(
    const ast::ModDecl& program,
    const ls::NameMap& name_map,
    const std::string& file,
    const lsp::FileUri& uri,
    lsp::Array<lsp::CodeLens>& out)
{
    ast::Node::TraverseFn visit([&](const ast::Node& node) {
        // The program is every file of the project concatenated, so anything outside the
        // requested document is pruned rather than walked.
        if (!node.loc.file || *node.loc.file != file) return false;
        auto decl = node.isa<ast::Decl>();
        if (!decl || !deserves_code_lens(*decl)) return true;
        auto named = decl->isa<ast::NamedDecl>();
        if (!named) return true;

        auto count = name_map.find_refs(named).size();
        lsp::CodeLens lens;
        lens.range = to_range(named->id.loc);
        // Registered by the extension, which turns the position into the peek view.
        // A lens without a command would render as unclickable grey text.
        lsp::LSPArray arguments;
        arguments.push_back(lsp::json::String(uri.toString()));
        arguments.push_back(lsp::json::Integer(lens.range.start.line));
        arguments.push_back(lsp::json::Integer(lens.range.start.character));
        lens.command = lsp::Command{
            .title = count == 1 ? "1 reference" : std::to_string(count) + " references",
            .command = "artic.showReferences",
            .arguments = std::move(arguments),
        };
        out.push_back(std::move(lens));
        return true;
    });
    for (const auto& decl : program.decls) if (decl) decl->traverse(visit);
}

} // anonymous namespace

void Server::setup_events_symbols() {

    message_handler_.add<reqst::Workspace_Symbol>([this](reqst::Workspace_Symbol::Params&& params) -> reqst::Workspace_Symbol::Result {
        log::info("\n[LSP] <<< Workspace Symbol '{}'", params.query);

        lsp::Array<lsp::SymbolInformation> symbols;
        for (const auto* symbol : symbol_index_.find(*workspace_, params.query, max_workspace_symbols)) {
            lsp::SymbolInformation info;
            info.name = symbol->name;
            info.kind = lsp::SymbolKindEnum(symbol->kind);
            if (!symbol->container.empty()) info.containerName = symbol->container;
            info.location = symbol->location;
            symbols.push_back(std::move(info));
        }
        log::info("[LSP] >>> Returning {} workspace symbols", symbols.size());
        return symbols;
    });

    message_handler_.add<reqst::TextDocument_CodeLens>([this](reqst::TextDocument_CodeLens::Params&& params) -> reqst::TextDocument_CodeLens::Result {
        log::info("\n[LSP] <<< TextDocument CodeLens {}", params.textDocument.uri.path());
        fs::path file = absolute_path(params.textDocument.uri.path());
        if(get_file_type(file) != FileType::SourceFile) return nullptr;
        ensure_compile(params.textDocument.uri.path());
        if(!compile || !compile->program) return nullptr;

        lsp::Array<lsp::CodeLens> lenses;
        collect_code_lenses(*compile->program, compile->name_map, file.generic_string(),
                            to_file_uri(file), lenses);
        log::info("[LSP] >>> Returning {} code lenses", lenses.size());
        return lenses;
    });
}


void Server::setup_events_other() {

    message_handler_.add<reqst::TextDocument_InlayHint>([this](reqst::TextDocument_InlayHint::Params&& params) -> reqst::TextDocument_InlayHint::Result {
        fs::path file = absolute_path(params.textDocument.uri.path());
        log_request("TextDocument InlayHint", params.textDocument, params.range);

        // A config document is annotated with what its patterns actually matched. This used
        // to be an Information diagnostic, which put a working configuration in the Problems
        // panel; the panel is reserved for actual problems.
        if(get_file_type(file) == FileType::ConfigFile) {
            lsp::Array<lsp::InlayHint> config_hints;
            collect_config_hints(*workspace_, file, params.range, config_hints);
            log::info("[LSP] >>> Returning {} config inlay hints", config_hints.size());
            return config_hints;
        }

        // inlay hints are not allowed to trigger recompile as this is called right after document changed
        if(!has_compiled(file)) return nullptr;

        lsp::Array<lsp::InlayHint> hints;
        if(!compile->name_map.files.contains(file.generic_string()))
            return hints;

        // Convert TypeHint structs to LSP InlayHint objects
        for (const auto* hint : compile->name_map.files.at(file.generic_string()).with_type_hint) {
            auto& loc = hint->loc;
            auto* type = hint->type;
            // Check if the hint location is within the requested range
            if (!loc.file || *loc.file != file || !type || type->isa<TypeError>()) {
                continue;
            }

            lsp::Position hint_pos{
                static_cast<lsp::uint>(loc.end.row - 1),
                static_cast<lsp::uint>(hint->loc.end.col - 1)
            };

            if (!contains(params.range, hint_pos)) continue;

            // Format the type name for display
            lsp::InlayHint lsp_hint;
            lsp_hint.position = hint_pos;
            lsp_hint.label = ": " + print_to_string(*type);
            lsp_hint.kind = lsp::InlayHintKindEnum(lsp::InlayHintKind::Type);
            lsp_hint.paddingLeft = false;
            lsp_hint.paddingRight = true;
            
            hints.push_back(lsp_hint);
        }

        if (compile->program)
            collect_parameter_hints(*compile->program, compile->name_map, file.generic_string(), params.range, hints);

        log::info("[LSP] >>> Returning {} inlay hints", hints.size());
        return hints;
    });

    message_handler_.add<reqst::Workspace_ExecuteCommand>([this](reqst::Workspace_ExecuteCommand::Params&& params) -> reqst::Workspace_ExecuteCommand::Result {
        log::info("\n[LSP] <<< Workspace ExecuteCommand {}", params.command);
        if (params.command != project_for_file_command) return nullptr;
        if (!params.arguments || params.arguments->empty() || !params.arguments->front().isString()) {
            log::error("{} expects a single document URI argument", project_for_file_command);
            return nullptr;
        }

        // Must be a FileUri, not the Uri that parsing yields: only FileUri::path() strips the
        // leading slash Windows drive paths carry, and `path()` borrows from the object.
        auto uri = lsp::FileUri(lsp::Uri::parse(params.arguments->front().string()));
        auto file = absolute_path(uri.path());
        workspace::config::ConfigLog log;
        auto info = workspace_->project_of_file(file, log);
        publish_config_diagnostics(log);

        using Provenance = workspace::FileProject::Provenance;
        lsp::json::Object result;
        result["file"] = lsp::json::String(file.generic_string());
        result["provenance"] = lsp::json::String(
            info.provenance == Provenance::Config ? "config"
            : info.provenance == Provenance::DefaultProject ? "default-project"
            : info.provenance == Provenance::DetectedBuildFile ? "detected"
            : "single-file");
        result["name"] = lsp::json::String(info.name);
        result["origin"] = lsp::json::String(info.origin.empty() ? std::string() : info.origin.generic_string());
        result["fileCount"] = static_cast<lsp::json::Integer>(info.file_count);
        return lsp::json::Value(std::move(result));
    });
}

} // namespace artic::ls

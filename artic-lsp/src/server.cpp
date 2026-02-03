#include "server.h"

#include "config.h"
#include "crash.h"
#include "workspace.h"
#include "artic/log.h"
#include "artic/ast.h"
#include "artic/bind.h"
#include "artic/print.h"
#include "artic/types.h"
#include "lsp/error.h"
#include "lsp/nullable.h"

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

namespace artic::ls {

// Server ---------------------------------------------------------------------

Server::Server() 
    : connection_(lsp::Connection(lsp::io::standardIO()))
    , message_handler_(this->connection_)
{
    crash::setup_crash_handler();
    setup_events();
}

Server::~Server() = default;

int Server::run() {
    log::info("LSP Server starting...");
    running_ = true;
    while (running_) {
        try {
            message_handler_.processIncomingMessages();
            // std::this_thread::sleep_for(std::chrono::milliseconds(10));
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

Server::FileType Server::get_file_type(const std::filesystem::path& file) {
    return file.extension() == ".json" || file.extension() == ".artic-lsp" ? FileType::ConfigFile : FileType::SourceFile;
}

lsp::Location convert_loc(const Loc& loc){
    if (!loc.file) throw lsp::RequestError(lsp::Error::InternalError, "Cannot convert location with undefined file");
    return lsp::Location {
        .uri = lsp::FileUri::fromPath(*loc.file),
        .range = lsp::Range {
            .start = lsp::Position { static_cast<lsp::uint>(loc.begin.row - 1), static_cast<lsp::uint>(loc.begin.col - 1) },
            .end   = lsp::Position { static_cast<lsp::uint>(loc.end.row   - 1), static_cast<lsp::uint>(loc.end.col   - 1) }
        }
    };
}

Loc convert_loc(const lsp::TextDocumentIdentifier& file, const lsp::Position& pos) {
    return Loc(
        std::make_shared<std::string>(file.uri.path()),
        Loc::Pos { .row = static_cast<int>(pos.line + 1), .col = static_cast<int>(pos.character + 1) }
    );
}

Loc convert_loc(const lsp::TextDocumentIdentifier& file, const lsp::Range& pos) {
    return Loc(
        std::make_shared<std::string>(file.uri.path()),
        Loc::Pos { .row = static_cast<int>(pos.start.line + 1), .col = static_cast<int>(pos.start.character + 1) },
        Loc::Pos { .row = static_cast<int>(pos.end.line + 1),   .col = static_cast<int>(pos.end.character + 1) }
    );
}

// -----------------------------------------------------------------------------
//
//
// Initialization
//
//
// -----------------------------------------------------------------------------


struct InitOptions {
    bool restart_from_crash = false;
};

InitOptions parse_initialize_options(const reqst::Initialize::Params& params, Server& server) {
    InitOptions data;
    
    if (!params.rootUri.isNull()) {
        data.workspace_root = std::string(params.rootUri.value().path());
    } else {
        data.workspace_root = std::filesystem::path("/");
    }

    if (auto init = params.initializationOptions; init.has_value() && init->isObject()) {
        const auto& obj = init->object();
        
        if (auto it = obj.find("restartFromCrash"); it != obj.end() && it->second.isBoolean())
            data.restart_from_crash = it->second.boolean();
    }
    // server.send_message("No initialization options provided in initialize request", lsp::MessageType::Error);
    // workspace_root = std::string(params.rootUri.value().path());
    return data;
}

void Server::setup_events_initialization() {
    message_handler_.add<reqst::Initialize>([this](reqst::Initialize::Params&& params) -> reqst::Initialize::Result {
        Timer _("Initialize");
        log::info( "\n[LSP] <<< Initialize");
        
        InitOptions init_data = parse_initialize_options(params, *this);

        safe_mode_ = init_data.restart_from_crash;
        workspace_ = std::make_unique<workspace::Workspace>();
        
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
                .definitionProvider = true,
                .referencesProvider = true,
                .renameProvider = lsp::RenameOptions {
                    .prepareProvider = true
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

    message_handler_.add<notif::TextDocument_DidClose>([](notif::TextDocument_DidClose::Params&& params) {
        log::info("\n[LSP] <<< TextDocument DidClose");
    });
    message_handler_.add<notif::TextDocument_DidOpen>([this](notif::TextDocument_DidOpen::Params&& params) {
        log::info("\n[LSP] <<< TextDocument DidOpen");
        auto path = std::string(params.textDocument.uri.path());

        if(get_file_type(params.textDocument.uri.path()) == FileType::SourceFile) {
            
            // skip compilation on open when it was already compiled
            // we need to do this as go to definition shortly opens the text document in vscode 
            // and we don't want to invalidate the definition while looking it up
            bool already_compiled = compile && compile->locator.data(path);
            if(!already_compiled)
                compile_this_and_related_files(path);
        } else {
            workspace::config::ConfigLog log{};
            bool known = workspace_->on_config_changed(path, log);
            if(known) compile.reset();
            publish_config_diagnostics(log);
        }
    });
    message_handler_.add<notif::TextDocument_DidChange>([this](notif::TextDocument_DidChange::Params&& params) {
        log::info("");
        log::info("--------------------------------");
        log::info("[LSP] <<< TextDocument DidChange");
        std::filesystem::path file = params.textDocument.uri.path();
        if(get_file_type(file) == FileType::ConfigFile) {
            // handled in didsave
            return;
        }
        // Clear the last compilation result to invalidate stale inlay hints
        // compile.reset();
        // workspace_->mark_file_dirty(file);

        auto& content = std::get<lsp::TextDocumentContentChangeEvent_Text>(params.contentChanges[0]).text;
        compile_this_and_related_files(file, &content);
    });

    message_handler_.add<notif::TextDocument_DidSave>([this](notif::TextDocument_DidSave::Params&& params) {
        log::info("\n[LSP] <<< TextDocument DidSave");
        std::filesystem::path file = params.textDocument.uri.path();
        if(get_file_type(file) == FileType::ConfigFile) {
            workspace::config::ConfigLog log{};
            bool known = workspace_->on_config_changed(file, log);
            if(known) compile.reset();
            publish_config_diagnostics(log);
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
        for(auto& change : params.changes) {
            auto path = change.uri.path();

            switch(change.type.index()) {
                case lsp::FileChangeType::Created: 
                case lsp::FileChangeType::Deleted: {
                    reload_workspace();
                    return;
                }
                case lsp::FileChangeType::Changed: break; // Handle elsewhere
                case lsp::FileChangeType::MAX_VALUE: break;
            }
        }
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
lsp::SemanticTokens collect(
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
    data.reserve(tokens.size() * sizeof(SemanticToken) / sizeof(uint32_t));
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

void Server::setup_events_tokens() {
    // Semantic Tokens ----------------------------------------------------------------------
    message_handler_.add<reqst::TextDocument_SemanticTokens_Full>([this](lsp::SemanticTokensParams&& params) -> reqst::TextDocument_SemanticTokens_Full::Result {
        Timer _("TextDocument_SemanticTokens_Full");
        std::string file(params.textDocument.uri.path());
        log::info("\n[LSP] <<< TextDocument SemanticTokens_Full {}", file);
        
        // semantic tokens are not allowed to trigger recompile as this is called right after document changed
        bool already_compiled = compile && compile->locator.data(file);
        if(!already_compiled) return nullptr;
        auto tokens = collect(compile->name_map, std::string(params.textDocument.uri.path()));
        
        log::info("[LSP] >>> Returning {} semantic tokens", tokens.data.size());
        return tokens;
    });

    message_handler_.add<reqst::TextDocument_SemanticTokens_Range>([this](lsp::SemanticTokensRangeParams&& params) -> reqst::TextDocument_SemanticTokens_Range::Result {
        Timer _("TextDocument_SemanticTokens_Range");
        std::string file(params.textDocument.uri.path());
        log::info("\n[LSP] <<< TextDocument SemanticTokens_Range {}:{}:{} to {}:{}", 
                 file,
                 params.range.start.line + 1, params.range.start.character + 1,
                 params.range.end.line + 1, params.range.end.character + 1);
        // semantic tokens are not allowed to trigger recompile as this is called right after document changed
        bool already_compiled = compile && compile->locator.data(file);
        if(!already_compiled) return nullptr;
        auto tokens = collect(
            compile->name_map, std::string(params.textDocument.uri.path()), 
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

struct IndentifierOccurences{
    std::string name;
    std::vector<lsp::Location> all_occurences;

    // Additional info
    lsp::Location cursor_range;
    lsp::Location declaration_range;
};

std::optional<IndentifierOccurences> find_occurrences_of_identifier(Server& server, const Loc& cursor, bool include_declaration) {
    if(Server::get_file_type(*cursor.file) != Server::FileType::SourceFile) return std::nullopt;
    server.ensure_compile(*cursor.file);
    auto& name_map = server.compile->name_map;

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
            log::info("found reference at cursor '{}'", target_decl->id.name);
        }
    }
    // No symbol at cursor position
    if(!target_decl) return std::nullopt;

    std::vector<lsp::Location> locations;

    // Include the declaration itself if requested
    if (include_declaration) {
        locations.push_back(convert_loc(target_decl->id.loc));
    }

    // Find all references to this declaration
    for (auto ref : name_map.find_refs(target_decl)) {
        locations.push_back(convert_loc(name_map.get_identifier(ref).loc));
    }

    return IndentifierOccurences {
        .name = target_decl->id.name,
        .all_occurences = std::move(locations),
        .cursor_range = convert_loc(cursor_range),
        .declaration_range = convert_loc(target_decl->id.loc),
    };
}

void Server::setup_events_definitions() {
    message_handler_.add<reqst::TextDocument_Definition>([this](lsp::TextDocumentPositionParams&& pos) -> reqst::TextDocument_Definition::Result {
        Timer _("TextDocument_Definition");
        log::info("\n[LSP] <<< TextDocument Definition {}:{}:{}", pos.textDocument.uri.path(), pos.position.line + 1, pos.position.character + 1);

        auto cursor = convert_loc(pos.textDocument, pos.position);

        if(get_file_type(pos.textDocument.uri.path()) != FileType::SourceFile) return nullptr;
        ensure_compile(pos.textDocument.uri.path());
        auto& name_map = compile->name_map;
        
        // When on a reference try find declaration
        if(auto ref = name_map.find_ref_at(cursor)) {
            if(auto def = name_map.find_decl(*ref)) {
                auto loc = convert_loc(def->id.loc);
                log::info("[LSP] >>> return TextDocument Definition {}:{}:{}", loc.uri.path(), loc.range.start.line + 1, loc.range.start.character + 1);
                return { loc };
            }
            return nullptr;
        }
        // When on a declaration try find references
        if(auto occurences = find_occurrences_of_identifier(*this, cursor, false)){
            log::info("[LSP] >>> Found {} occurrences of identifier", occurences->all_occurences.size());
            if(occurences->all_occurences.empty()) return { occurences->declaration_range };
            return occurences->all_occurences;
        }

        return nullptr;
    });

    message_handler_.add<reqst::TextDocument_References>([this](lsp::ReferenceParams&& params) -> reqst::TextDocument_References::Result {
        Timer _("TextDocument_References");
        log::info("\n[LSP] <<< TextDocument References {}:{}:{}", params.textDocument.uri.path(), params.position.line + 1, params.position.character + 1);

        auto cursor = convert_loc(params.textDocument, params.position);
        auto occurences = find_occurrences_of_identifier(*this, cursor, true);
        if(!occurences) return {};
        log::info("[LSP] >>> Found {} occurrences of identifier", occurences->all_occurences.size());
        return occurences->all_occurences;
    });

    message_handler_.add<reqst::TextDocument_PrepareRename>([this](lsp::TextDocumentPositionParams&& params) -> reqst::TextDocument_PrepareRename::Result {
        Timer _("TextDocument_PrepareRename");
        log::info("\n[LSP] <<< TextDocument PrepareRename {}:{}:{}", 
                params.textDocument.uri.path(), params.position.line + 1, params.position.character + 1);

        auto cursor = convert_loc(params.textDocument, params.position);
        auto occurences = find_occurrences_of_identifier(*this, cursor, true);
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
        Timer _("TextDocument_Rename");
        log::info("\n[LSP] <<< TextDocument Rename {}:{}:{} -> '{}'", 
                 params.textDocument.uri.path(), params.position.line + 1, params.position.character + 1, params.newName);

        auto cursor = convert_loc(params.textDocument, params.position);
        auto occurences = find_occurrences_of_identifier(*this, cursor, true);
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
std::string get_completion_detail(const ast::NamedDecl* decl) {
    if (auto fn_decl = decl->isa<ast::FnDecl>()) {
        return "function";
    } else if (auto static_decl = decl->isa<ast::StaticDecl>()) {
        return static_decl->is_mut ? "let mut" : "let";
    } else if (auto ptrn_decl = decl->isa<ast::PtrnDecl>()) {
        return ptrn_decl->is_mut ? "parameter mut" : "parameter";
    } else if (auto struct_decl = decl->isa<ast::StructDecl>()) {
        return "struct";
    } else if (auto enum_decl = decl->isa<ast::EnumDecl>()) {
        return "enum";
    } else if (auto type_decl = decl->isa<ast::TypeDecl>()) {
        return "type";
    } else if (auto field_decl = decl->isa<ast::FieldDecl>()) {
        return "field";
    } else if (auto mod_decl = decl->isa<ast::ModDecl>()) {
        return "module";
    }
    return "declaration";
}

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
    if (auto* param = fn->fn->param.get()) {
        if (param->is_tuple()) {
            param->print(l);
        } else {
            l << '(';
            param->print(l);
            l << ')';
        }
    }
    
    item.label = lb.str();
    lb.str("");

    if(const auto* type = fn->type) {
        if(const auto* forall = fn->type->isa<ForallType>()) type = forall->body;
        if(type) if(const auto* f = fn->type->isa<FnType>()) {
            f->codom->print(l);
            item.detail = lb.str();
        }
    }
    if (!item.detail && fn->fn->ret_type) {
        fn->fn->ret_type->print(l);
        item.detail = lb.str();
    }

    std::stringbuf pt; 
    std::ostream str1(&pt);
    log::Output ptrn(str1, false);
    Printer p(ptrn);
    int arg = 1;
    ptrn << fn->id.name;
    if(fn->type_params && !fn->type_params->params.empty()) {
        ptrn << "[";
        for(int i = 0; i < fn->type_params->params.size(); i++) {
            if(i > 0) ptrn << ", ";
            ptrn << "${" << arg++ << ":" ;
            fn->type_params->params[i]->print(p);
            ptrn << "}";
        }
        ptrn << "]";
    }
    if(fn->fn->param){
        ptrn << "(";
        if(fn->fn->param->is_tuple()){
            auto tuple = fn->fn->param->isa<ast::TuplePtrn>();
            for(int i = 0; i < tuple->args.size(); i++) {
                if(i > 0) ptrn << ", ";
                ptrn << "${" << arg++ << ":" ;
                tuple->args[i]->print(p);
                ptrn << "}";
            }
        } else {
            ptrn << "${" << arg++ << ":" ;
            fn->fn->param->print(p);
            ptrn << "}";
        }
        ptrn << ")";
    }
    ptrn << "$0";
    item.insertText = pt.str();
    return item;
}

std::optional<lsp::CompletionItem> completion_item(const ast::NamedDecl& decl) {
    if(decl.id.name.empty()) return std::nullopt;
    if(decl.id.name.starts_with('_')) return std::nullopt;

    if (auto fn = decl.isa<ast::FnDecl>()) return completion_item(fn);

    lsp::CompletionItem item;

    // item.detail = get_completion_detail(&decl);
    item.kind = get_completion_kind(&decl);

    if(decl.type) {
        if (auto fn = decl.type->isa<FnType>()) {
            item.kind = lsp::CompletionItemKind::Function;

            std::stringbuf lb; 
            std::ostream str0(&lb);
            log::Output label(str0, false);
            Printer l(label);
            label << decl.id.name;
            if (fn->dom && fn->dom->isa<TupleType>()) {
                fn->dom->print(l);
            } else {
                l << '(';
                fn->dom->print(l);
                l << ')';
            }
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
                    for(int i = 0; i < tuple->args.size(); i++) {
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
        std::stringbuf lb; 
        std::ostream str0(&lb);
        log::Output label(str0, false);
        Printer l(label);
        decl.type->print(l);
        item.detail = lb.str();
    }
    return item;
}

void Server::setup_events_completion() {
    message_handler_.add<reqst::TextDocument_Completion>([this](lsp::CompletionParams&& params) -> reqst::TextDocument_Completion::Result {
        log::info("[LSP] <<< TextDocument Completion {}:{}:{}", params.textDocument.uri.path(), params.position.line + 1, params.position.character + 1);
        if(get_file_type(params.textDocument.uri.path()) != FileType::SourceFile) return nullptr;
        ensure_compile(params.textDocument.uri.path());
        // params.position.character--;
        Loc cursor = convert_loc(params.textDocument, params.position);
        // const ast::ProjExpr* proj_expr = nullptr;
        // const ast::PathExpr* path_expr = nullptr;
        const ast::ModDecl* current_module = compile->program.get();
        std::vector<const ast::Node*> local_scopes;
        const ast::Node* outer_node = nullptr;
        const ast::Node* inner_node = nullptr;
        bool only_show_types = false;
        bool inside_block_expr = false;
        bool top_level = false;
        static constexpr bool debug_print = false;

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
            if constexpr (debug_print) log::info("test node at {} vs {}", node.loc, cursor);
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
                if(fn->fn->param) local_scopes.push_back(fn->fn->param.get());
                if(fn->type_params) local_scopes.push_back(fn->type_params.get());
            } else if(const auto* block = node.isa<ast::BlockExpr>()){
                local_scopes.push_back(block);
                inside_block_expr = true;
                top_level = false;
            } else if(const auto* error = node.isa<ast::ErrorDecl>(); error && error->is_top_level) {
                top_level = true;
            }
            inner_node = &node;

            if constexpr (debug_print) log::info("Node at {}", node.loc);
            return true;
        });
        
        traverse(compile->program);
        if(!current_module) {
            log::info("Error with completion: current_module is null");
            return result;
        }

        // log::Output out(std::clog, false);
        // Printer p(out);
        // p.print_additional_node_info = true;
        // if(outer_node) {
        //     log::info("\n-- Current Module");
        //     current_module->print(p);
        // }

        // if(inner_node) {
        //     log::info("\n-- Inner Node");
        //     inner_node->print(p);
        // }

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
                proj_expr->dump();
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
                        for (auto& field : struct_type->decl.fields) {
                            if(auto item = completion_item(*field)) result.items.push_back(std::move(*item));
                        }
                    }
                    type->dump();
                } else {
                    log::info("type could not be identified");
                }
                log::info("{} projection items", result.items.size());
                return result;
            } 
            // 2. Path expression: a::b
            if(const auto* path = inner_node->isa<ast::Path>(); path && path->elems.size() > 1) {
                log::info("Showing completion for Path");
                path->dump();
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
                // TODO this shows for definitions outside the loop `for a in ...` -> shows `a`
                if(collect_local_decls.depth > 0 && node.isa<ast::BlockExpr>()) {
                    return false; // do not go into nested blocks
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
            
            auto show_prim_type = [&](std::string_view prim){
                lsp::CompletionItem item;
                item.kind = lsp::CompletionItemKind::Keyword;
                item.label = prim;
                result.items.push_back(std::move(item));
            };
            auto& types = compile->type_table;
            show_prim_type("bool");
            show_prim_type("i8");
            show_prim_type("i16");
            show_prim_type("i32");
            show_prim_type("i64");
            show_prim_type("u8");
            show_prim_type("u16");
            show_prim_type("u32");
            show_prim_type("u64");
            show_prim_type("f16");
            show_prim_type("f32");
            show_prim_type("f64");
            show_prim_type("simd");
            show_prim_type("mut");
            show_prim_type("super");
            
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
// Server Compilation / Diagnostics
//
//
// -----------------------------------------------------------------------------

void Server::compile_this_and_related_files(const std::filesystem::path& file, std::string* new_content) {
    Timer _("Compile Files");

    if(new_content) workspace_->set_file_content(file, std::move(*new_content));

    workspace::config::ConfigLog cfg_log;
    auto files = workspace_->collect_project_files(file, cfg_log);
    publish_config_diagnostics(cfg_log);
    
    if (files.empty()) {
        log::info("No input files to compile");
        return;
    }
    log::info("Compiling {} file(s)", files.size());

    // Initialize
    compile.emplace();
    if(safe_mode_) {
        compile->exclude_non_parsed_files = true;
        log::info("Using safe mode");
    }

    // Compile
    compile->compile_files(files, file);

    if(safe_mode_ && compile->parsed_all) {
        safe_mode_ = false;
        log::info("Successfully parsed all files, turning off safe mode");
    }

    const bool print_compile_log = false;
    if(print_compile_log) compile->log.print_summary();

    if(compile->log.errors == 0){
        log::info("Compile success");
    } else {
        log::info("Compile failed");
    }

    auto convert_diagnostic = [](const Diagnostic& diag) -> lsp::Diagnostic {
        lsp::Diagnostic lsp_diag;
        lsp_diag.message = diag.message;
        lsp_diag.range = lsp::Range {
            .start = lsp::Position { static_cast<uint>(diag.loc.begin.row - 1), static_cast<uint>(diag.loc.begin.col - 1) },
            .end   = lsp::Position { static_cast<uint>(diag.loc.end.row   - 1), static_cast<uint>(diag.loc.end.col   - 1) }
        };
        switch (diag.severity) {
            case Diagnostic::Error:   lsp_diag.severity = lsp::DiagnosticSeverity::Error;       break;
            case Diagnostic::Warning: lsp_diag.severity = lsp::DiagnosticSeverity::Warning;     break;
            case Diagnostic::Info:    lsp_diag.severity = lsp::DiagnosticSeverity::Information; break;
            case Diagnostic::Hint:    lsp_diag.severity = lsp::DiagnosticSeverity::Hint;        break;
        }
        return lsp_diag;
    };

    // Send Diagnostics for the provided files only
    std::unordered_map<std::string, std::vector<lsp::Diagnostic>> diagnostics_by_file;
    for (const auto& diag : compile->diagnostics) {
        diagnostics_by_file[*diag.loc.file].push_back(convert_diagnostic(diag));
    }
    for (const auto* file : files) {
        auto path = file->path.string();

        message_handler_.sendNotification<notif::TextDocument_PublishDiagnostics>(
            notif::TextDocument_PublishDiagnostics::Params {
                .uri = lsp::FileUri::fromPath(path),
                .diagnostics = diagnostics_by_file.contains(path) ? diagnostics_by_file.at(path) : std::vector<lsp::Diagnostic>{}
            }
        );
    }
}

void Server::ensure_compile(std::string_view file_view) {
    std::string file(file_view);
    if(get_file_type(file_view) != FileType::SourceFile) {
        throw lsp::RequestError(lsp::Error::InvalidParams, "File is not an Artic source file");
    }
    bool already_compiled = compile && compile->locator.data(file);
    if (!already_compiled) compile_this_and_related_files(file);
    if (!compile) throw lsp::RequestError(lsp::Error::InternalError, "Did not get a compilation result");
}


// -----------------------------------------------------------------------------
//
//
// Server Reload Workspace
//
//
// -----------------------------------------------------------------------------


void Server::publish_config_diagnostics(const workspace::config::ConfigLog& log) {
    const bool print_to_console = true;
    if(print_to_console) {
        log::info("--- Config Log ---");
        for (auto& e : log.messages) {
            // if(e.severity > lsp::DiagnosticSeverity::Warning) continue;
            auto s = 
                (e.severity == lsp::DiagnosticSeverity::Error)        ? "Error" :
                (e.severity == lsp::DiagnosticSeverity::Warning)      ? "Warning" : 
                (e.severity == lsp::DiagnosticSeverity::Information)  ? "Info" : 
                (e.severity == lsp::DiagnosticSeverity::Hint)         ? "Hint" : 
                                                                        "Unknown";

            log::info("[{}] {}: {}", s, e.file, e.message);
        }
        // log::info("--- Config Log ---");
        // workspace_->projects_.print();
    }

    std::unordered_map<std::filesystem::path, std::vector<lsp::Diagnostic>> fileDiags;
    std::unordered_map<std::filesystem::path, std::vector<lsp::InlayHint>> fileHints;

    // create diagnostics
    for (const auto& msg : log.messages) {
        std::optional<std::string> propagate_to_file;
        // Diagnosics for the file itself
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
        std::vector<lsp::Range> occurrences;
        std::string file = msg.file.string();
        std::string literal = msg.context.value().literal;

        if(propagate_to_file) {
            file = *propagate_to_file;
            literal = "include";
        }

        if(msg.context.has_value()) {
            occurrences = find_in_file(file, literal);
        }

        // bool sendDiagnostic = occurrences.empty() || msg.severity != lsp::DiagnosticSeverity::Hint;
        bool sendDiagnostic = true;
        if(sendDiagnostic) {
            lsp::Diagnostic diag;
            diag.message = msg.message;
            diag.severity = msg.severity;
            diag.range = lsp::Range{ lsp::Position{0,0}, lsp::Position{0,0} };
            
            for(auto& occ : occurrences) {
                lsp::Diagnostic pos_diag(diag);
                pos_diag.range = occ;
                fileDiags[msg.file].push_back(pos_diag);
            }
            if(occurrences.empty()) fileDiags[msg.file].push_back(diag);
        } else {
            // lsp::InlayHint hint;
            
            // for(auto& occ : occurrences) {
            //     lsp::InlayHint hint;
            //     hint.label = msg.message;
            //     hint.position = occ.start;
            //     fileHints[msg.file].push_back(hint);
            // }
        }
    }

    // Send diagnostics
    for(auto& [file, diags] : fileDiags) {
        message_handler_.sendNotification<notif::TextDocument_PublishDiagnostics>(
            notif::TextDocument_PublishDiagnostics::Params {
                .uri = lsp::FileUri::fromPath(file.string()),
                .diagnostics = diags
            }
        );
    }
}

void Server::reload_workspace(const std::string& active_file) {
    Timer _("Reload Workspace");
    log::info("Reloading workspace configuration");
    workspace::config::ConfigLog log;
    workspace_->reload(log);
    publish_config_diagnostics(log);
    
    // Recompile last compile
    if (compile) {
        compile_this_and_related_files(compile->active_file);
    }
}


// -----------------------------------------------------------------------------
//
//
// Other
//
//
// -----------------------------------------------------------------------------
namespace artic::reqst {
    struct DebugAst {
        static constexpr auto Method = std::string_view("artic/debugAst");
        static constexpr auto Direction = lsp::MessageDirection::ClientToServer;
        static constexpr auto Type = lsp::Message::Request;
        using Params = lsp::TextDocumentPositionParams;
        using Result = lsp::Nullable<std::string>;
    };
}

void Server::setup_events_other() {

    // Custom debug command to print AST at cursor position
    message_handler_.add<artic::reqst::DebugAst>([this](lsp::TextDocumentPositionParams&& params) -> artic::reqst::DebugAst::Result {
        Timer _("artic/debugAst");
        
        log::info("\n[LSP] <<< artic/debugAst {}:{}:{}", 
        params.textDocument.uri.path(), 
                 params.position.line + 1, 
                 params.position.character + 1);

        auto file = params.textDocument.uri.path();
        if(get_file_type(file) != FileType::SourceFile) return nullptr;
        ensure_compile(file);
        if (!compile || !compile->program) {
            throw lsp::RequestError(lsp::Error::InternalError, "No compilation result available");
        }

        Loc cursor = convert_loc(params.textDocument, params.position);
        const ast::Node* inner_node = nullptr;
        const ast::Node* outer_node = nullptr;

        // Find the AST node at the cursor position
        ast::Node::TraverseFn traverse([&](const ast::Node& node) -> bool {
            if (node.loc.file && same_file(node.loc, cursor) && overlaps(node.loc, cursor)) {
                if(!outer_node) {
                    outer_node = &node;
                }
                inner_node = &node;
                return true; // Continue to find the most specific node
            }
            return true;
        });
        
        traverse(compile->program);

        if (!outer_node || !inner_node) {
            return nullptr;
            log::info("[LSP] >>> No AST node found at cursor");
        }
        log::info("[LSP] >>> Found AST node at cursor");
        // Print the AST node to a string
        std::stringbuf buffer;
        std::ostream stream(&buffer);
        log::Output output(stream, false);
        Printer printer(output);
        printer.print_additional_node_info = true;
        
        output << "Inner Node: \n";
        inner_node->print(printer);
        output << "Outer Node: \n";
        outer_node->print(printer);
        return buffer.str();
    });

    message_handler_.add<reqst::TextDocument_InlayHint>([this](reqst::TextDocument_InlayHint::Params&& params) -> reqst::TextDocument_InlayHint::Result {
        Timer _("TextDocument_InlayHint");
        std::string file(params.textDocument.uri.path());
        log::info("\n[LSP] <<< TextDocument InlayHint {}:{}:{} to {}:{}", 
            file, 
            params.range.start.line + 1, params.range.start.character + 1,
            params.range.end.line + 1, params.range.end.character + 1);

        // inlay hints are not allowed to trigger recompile as this is called right after document changed
        bool already_compiled = compile && compile->locator.data(file);
        if(!already_compiled) return nullptr;

        lsp::Array<lsp::InlayHint> hints;
        if(!compile->name_map.files.contains(file))
            return hints;

        // Convert TypeHint structs to LSP InlayHint objects
        for (const auto* hint : compile->name_map.files.at(file).with_type_hint) {
            auto& loc = hint->loc;
            auto* type = hint->type;
            // Check if the hint location is within the requested range
            if (!loc.file || *loc.file != file) {
                continue;
            }

            lsp::Position hint_pos{
                static_cast<lsp::uint>(loc.end.row - 1),
                static_cast<lsp::uint>(hint->loc.end.col - 1)
            };

            // Check if the hint position is within the requested range
            if (hint_pos.line < params.range.start.line || 
                hint_pos.line > params.range.end.line ||
                (hint_pos.line == params.range.start.line && hint_pos.character < params.range.start.character) ||
                (hint_pos.line == params.range.end.line && hint_pos.character > params.range.end.character)) {
                continue;
            }

            // Format the type name for display
            std::string type_name = "<unknown>";
            if (type) {
                std::ostringstream oss;
                log::Output output(oss, false);
                Printer printer(output);
                type->print(printer);
                type_name = oss.str();
            }
            
            lsp::InlayHint lsp_hint;
            lsp_hint.position = hint_pos;
            lsp_hint.label = ": " + type_name;
            lsp_hint.kind = lsp::InlayHintKindEnum(lsp::InlayHintKind::Type);
            lsp_hint.paddingLeft = false;
            lsp_hint.paddingRight = true;
            
            hints.push_back(lsp_hint);
        }

        log::info("[LSP] >>> Returning {} inlay hints", hints.size());
        return hints;
    });

    // notif::Workspace_DidChangeWorkspaceFolders
    // notif::Workspace_DidCreateFiles
    // notif::Workspace_DidDeleteFiles
    // notif::Workspace_DidRenameFiles

    // req::CallHierarchy_IncomingCalls
    // req::CallHierarchy_OutgoingCalls
    // req::Client_RegisterCapability
    // req::Client_UnregisterCapability
    // req::CodeAction_Resolve
    // req::CodeLens_Resolve
    // req::CompletionItem_Resolve
    // req::DocumentLink_Resolve
    // req::InlayHint_Resolve
    // req::TextDocument_CodeAction
    // req::TextDocument_CodeLens
    // req::TextDocument_ColorPresentation
    // req::TextDocument_Completion
    // req::TextDocument_Declaration
    

    // req::TextDocument_Diagnostic
    // req::TextDocument_DocumentColor
    // req::TextDocument_DocumentHighlight
    // req::TextDocument_DocumentLink
    // req::TextDocument_DocumentSymbol
    // req::TextDocument_FoldingRange
    // req::TextDocument_Formatting
    // req::TextDocument_Hover
    // req::TextDocument_Implementation
    // req::TextDocument_InlayHint
    // req::TextDocument_InlineCompletion
    // req::TextDocument_InlineValue
    // req::TextDocument_LinkedEditingRange
    // req::TextDocument_Moniker
    // req::TextDocument_OnTypeFormatting
    // req::TextDocument_PrepareCallHierarchy
    // req::TextDocument_PrepareRename
    // req::TextDocument_PrepareTypeHierarchy
    // req::TextDocument_RangeFormatting
    // req::TextDocument_RangesFormatting
    // req::TextDocument_References
    // req::TextDocument_Rename
    // req::TextDocument_SelectionRange
    // req::TextDocument_SemanticTokens_Full
    // req::TextDocument_SemanticTokens_Full_Delta
    // req::TextDocument_SemanticTokens_Range
    // req::TextDocument_SignatureHelp
    // req::TextDocument_TypeDefinition
    // req::TextDocument_WillSaveWaitUntil
    // req::TypeHierarchy_Subtypes
    // req::TypeHierarchy_Supertypes
    // req::Window_ShowDocument
    // req::Window_ShowMessageRequest
    // req::Window_WorkDoneProgress_Create
    // req::Workspace_ApplyEdit
    // req::Workspace_CodeLens_Refresh
    // req::Workspace_Configuration
    // req::Workspace_Diagnostic
    // req::Workspace_Diagnostic_Refresh
    // req::Workspace_ExecuteCommand
    // req::Workspace_FoldingRange_Refresh
    // req::Workspace_InlayHint_Refresh
    // req::Workspace_InlineValue_Refresh
    // req::Workspace_SemanticTokens_Refresh
    // req::Workspace_Symbol
    // req::Workspace_WillCreateFiles
    // req::Workspace_WillDeleteFiles
    // req::Workspace_WillRenameFiles
    // req::Workspace_WorkspaceFolders
    // req::WorkspaceSymbol_Resolve
    // notif::CancelRequest
    // notif::LogTrace
    // notif::Progress
    // notif::SetTrace
    // notif::Exit
    // notif::Telemetry_Event
    // notif::TextDocument_PublishDiagnostics
    // notif::TextDocument_WillSave
    // notif::Window_LogMessage
    // notif::Window_ShowMessage
    // notif::Window_WorkDoneProgress_Cancel
}

} // namespace artic::ls
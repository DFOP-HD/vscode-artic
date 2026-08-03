#ifndef ARTIC_LS_SERVER_H
#define ARTIC_LS_SERVER_H

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "workspace.h"
#include "lsp/types.h"
#include <lsp/connection.h>
#include <lsp/messagehandler.h>
#include <lsp/messagebase.h>
#include "compile.h"
#include "symbol_index.h"

namespace artic::ls {

/**
 * Minimal LSP server implementation for Artic language support.
 * Uses basic JSON-RPC over stdio communication.
 */
class Server {
public:
    Server();
    ~Server();

    /// Start the LSP server main loop
    int run();

private:
    void setup_events() {
        setup_events_initialization();
        setup_events_modifications();
        setup_events_tokens();
        setup_events_definitions();
        setup_events_other();
        setup_events_completion();
        setup_events_signature_help();
        setup_events_selection_range();
        setup_events_symbols();
    }
    void setup_events_initialization();
    void setup_events_modifications();
    void setup_events_tokens();
    void setup_events_definitions();
    void setup_events_completion();
    void setup_events_signature_help();
    void setup_events_selection_range();
    void setup_events_symbols();
    void setup_events_other();

    void send_message(const std::string& message, lsp::MessageType type);
    void compile_this_and_related_files(std::filesystem::path file, std::string* new_content = nullptr);
    void ensure_compile(std::string_view file_view);

    // Whether the last compilation already covers this file. Requests that arrive right
    // after a change (semantic tokens, inlay hints) must not trigger a recompile.
    bool has_compiled(const std::filesystem::path& file) {
        return compile && compile->locator.data(file.generic_string());
    }

    enum class FileType { SourceFile, ConfigFile };
    static FileType get_file_type(const std::filesystem::path& file);

    void reload_workspace();
    void publish_config_diagnostics(const workspace::config::ConfigLog& log);

    lsp::Connection connection_;
    lsp::MessageHandler message_handler_;
    bool running_ = false;
    bool safe_mode_ = false;

    // Folders the editor has open, from `workspaceFolders` or the deprecated `rootUri`.
    // Empty when the editor opened a single file rather than a folder.
    std::vector<std::filesystem::path> workspace_roots_;

    // Project management
    std::unique_ptr<workspace::Workspace> workspace_;
    std::optional<Compiler> compile;

    // Declarations of every project, harvested by parsing alone. Answers `workspace/symbol`
    // without keeping a compiler alive per project.
    SymbolIndex symbol_index_;

    // Files that currently show config diagnostics, so they can be cleared once
    // the config is fixed. The editor keeps the last published set otherwise.
    std::set<std::filesystem::path> published_config_diagnostics_;
};

} // namespace artic::ls

#endif // ARTIC_LS_SERVER_H
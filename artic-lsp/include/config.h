#ifndef ARTIC_LS_CONFIG_H
#define ARTIC_LS_CONFIG_H

#include "artic/ls/workspace.h"
#include "lsp/types.h"

#include <vector>
#include <string>
#include <optional>
#include <filesystem>

namespace artic::ls::workspace::config {

struct ProjectDefinition {
    // Unique project name.
    // May be referenced by other projects.
    Project::Identifier name;

    // Path to the project root directory.
    // FilePatterns are relative to this path.
    std::string root_dir;
    
    // A pattern which can be used to include or exclude one or more files.
    // Exclude patterns start with '!' character.
    std::vector<std::string> file_patterns;

    // Names of other projects that this project depends on.
    // Projects will include all files from dependencies.
    std::vector<Project::Identifier> dependencies;

    // config where project was first defined
    std::filesystem::path origin;

    // -- internal parse info --
    int depth = 100;
    bool was_defined_in_global_config = false;
};

struct IncludeConfig {
    // path to another artic.json
    std::filesystem::path path;

    // -- internal parse info --
    std::string raw_path_string;
    bool is_optional = false;
    bool is_global = false;
};

struct ConfigDocument {
    std::string version;
    std::vector<ProjectDefinition>   projects;
    std::optional<ProjectDefinition> default_project;
    std::vector<IncludeConfig>       includes;
    std::filesystem::path            path;
    
    static std::optional<ConfigDocument> parse(const IncludeConfig& config, ConfigLog& log);
};

struct ConfigLog {
    using Severity = lsp::DiagnosticSeverity;
    struct Context {
        std::string literal;
    };
    struct Message {
        std::string message;
        Severity severity;

        std::filesystem::path file;
        std::optional<Context> context;
    };
    std::filesystem::path file_context;
    std::vector<Message> messages;

    void error(std::string msg, std::optional<std::string> context=std::nullopt) { messages.push_back(make_message(Severity::Error,       std::move(msg), context)); }
    void warn (std::string msg, std::optional<std::string> context=std::nullopt) { messages.push_back(make_message(Severity::Warning,     std::move(msg), context)); }
    void info (std::string msg, std::optional<std::string> context=std::nullopt) { messages.push_back(make_message(Severity::Information, std::move(msg), context)); }

private:
    static std::string quote(std::string_view in) {
        return '\"' + std::string(in) + '\"';
    }
    Message make_message(Severity s, std::string msg, std::optional<std::string> context) {
        return Message{
            .message=std::move(msg),
            .severity=s,
            .file=file_context, 
            .context= context 
                ? std::make_optional(Context{quote(context.value())}) 
                : std::nullopt
        };
    }
};

} // namespace artic::ls::workspace::config

#endif // ARTIC_LS_CONFIG_H
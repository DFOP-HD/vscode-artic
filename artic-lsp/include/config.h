#ifndef ARTIC_LS_CONFIG_H
#define ARTIC_LS_CONFIG_H

#include "workspace.h"
#include <nlohmann/json.hpp>
#include <vector>
#include <string>
#include <optional>
#include <unordered_set>
#include <filesystem>

namespace artic::ls::workspace::config {

    
struct ConfigLog {
    using Severity = lsp::DiagnosticSeverity;
    struct Context {
        std::string literal;
    };
    struct Message {
        std::string message;
        Severity severity;

        fs::path file;
        std::optional<Context> context;
    };
    fs::path file_context;
    std::vector<Message> messages;

    void error(std::string msg, std::string context="") { messages.push_back(make_message(Severity::Error,       std::move(msg), context)); }
    void warn (std::string msg, std::string context="") { messages.push_back(make_message(Severity::Warning,     std::move(msg), context)); }
    void info (std::string msg, std::string context="") { messages.push_back(make_message(Severity::Information, std::move(msg), context)); }

private:
    static std::string quote(std::string_view in) {
        return '\"' + std::string(in) + '\"';
    }
    Message make_message(Severity s, std::string msg, std::string context) {
        return Message{
            .message = std::move(msg),
            .severity = s,
            .file = file_context, 
            .context = context.empty() ? std::make_optional(Context{.literal=quote(context)}) : std::nullopt
        };
    }
};

struct ConfigParser {
    ConfigParser(const IncludeConfig& origin, config::ConfigLog& log) 
        : origin(origin), log(log)
    {}
    IncludeConfig origin;
    config::ConfigLog& log;

    // out
    ConfigFile config{};
    std::vector<Project> projects{};

    bool parse();
private:
    std::optional<Project> parse_project(const nlohmann::json& pj);
    std::unordered_set<fs::path> evaluate_patterns(Project& project);
};


// Find all files under root matching the given glob pattern.
// The pattern is interpreted with '/' as the separator and can include
// *, **, ? as described.
struct FilePatternParser {
    FilePatternParser(fs::path root, std::string pattern, config::ConfigLog& log)
        : root(std::move(root)), pattern(std::move(pattern)), log(log)
    {
        expand();
    }

    static std::vector<fs::path> expand(const fs::path& root, const std::string& pattern, config::ConfigLog& log) {
        return FilePatternParser(root, pattern, log).results;
    }

    std::vector<fs::path> results;
private:
    void expand() {
        expand_home();
        if (!fs::exists(root) || !fs::is_directory(root)) {
            log.error("Folder does not exist: {}", root.string());
            return;
        }
        split();
        dfs(0, root);
    }

    fs::path root;
    std::string pattern;
    config::ConfigLog& log;

    // State
    std::vector<std::string> parts;
    std::unordered_set<std::string> dedup;

    bool is_wildcard(const std::string& s){ return s.find('*') != std::string::npos || s.find('?') != std::string::npos; };

    void expand_home() {
        if (pattern.starts_with("~/")) {
            const char* home = std::getenv("HOME");
            root = home ? home : fs::path("/");
            pattern.erase(0, 2);
        }
        if(pattern[0] == '/') {
            root = fs::path("/");
            pattern.erase(0, 1);
        }
    }

    void split() {
        parts.reserve(8);
        std::string cur; cur.reserve(pattern.size());
        for(char c : pattern) {
            if(c == '/') { parts.push_back(cur); cur.clear(); }
            else cur.push_back(c);
        }
        parts.push_back(cur);
    }

    void dfs(size_t idx,const fs::path& base);
};


} // namespace artic::ls::workspace::config

#endif // ARTIC_LS_CONFIG_H
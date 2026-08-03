#ifndef ARTIC_LS_CONFIG_H
#define ARTIC_LS_CONFIG_H

#include "workspace.h"
#include "json_source.h"
#include "paths.h"
#include "text.h"
#include <nlohmann/json.hpp>
#include <vector>
#include <string>
#include <optional>
#include <set>
#include <unordered_set>
#include <filesystem>

namespace artic::ls::workspace::config {

std::optional<Project> parse_vcxproj(const ConfigPath& origin, ConfigLog& log);

// Absolute paths of the .vcxproj files referenced by a Visual Studio solution.
std::vector<ConfigPath> parse_sln(const ConfigPath& origin, ConfigLog& log);

// One project per ninja target whose command line invokes the artic compiler.
std::vector<Project> parse_ninja(const ConfigPath& origin, ConfigLog& log);

// Build files under `root` that look like they build artic sources, strongest match first.
// This is what makes a CMake- or MSBuild-driven checkout work without an `artic.json`: the
// build system already knows which files are compiled together, and it is the only thing
// that does. Mirrors `selectWorkspaceConfigFiles` in vscode/src/detect.ts, which does the
// same job for the explicit "Detect workspace configuration" command.
//
// The scan is bounded (depth, directory count, file count) and skips hidden and output
// directories, because it runs on the miss path of every file with no config above it.
std::vector<fs::path> detect_build_files(const fs::path& root, ConfigLog& log);
    
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
        // Where in `file` the message belongs, when that could be established while parsing.
        // A message without one is placed by searching the file for `context`.
        std::optional<lsp::Range> range;
    };
    fs::path file_context;
    // Position index of the config currently being parsed, if it is a JSON one.
    const JsonSource* json_context = nullptr;
    std::vector<Message> messages;

    // Config files this pass actually looked at. Configs are cached, so most passes
    // evaluate nothing; without this the publisher would clear the diagnostics of every
    // config simply because the current pass had nothing to say about them.
    std::set<fs::path> evaluated_files;

    // Restores the previous context on destruction. Config parsing recurses through
    // includes, so a plain assignment would leave later messages attributed to
    // whichever config happened to be visited last.
    struct FileContextScope {
        FileContextScope(ConfigLog& log, fs::path file, const JsonSource* source)
            : log_(log), previous_(std::move(log.file_context)), previous_json_(log.json_context)
        {
            log_.evaluated_files.insert(file);
            log_.file_context = std::move(file);
            log_.json_context = source;
        }
        FileContextScope(const FileContextScope&) = delete;
        FileContextScope& operator=(const FileContextScope&) = delete;
        ~FileContextScope() {
            log_.file_context = std::move(previous_);
            log_.json_context = previous_json_;
        }
    private:
        ConfigLog& log_;
        fs::path previous_;
        const JsonSource* previous_json_;
    };
    [[nodiscard]] FileContextScope scoped_file(fs::path file, const JsonSource* source = nullptr) {
        return FileContextScope(*this, std::move(file), source);
    }

    void error(std::string msg, std::string context="") { messages.push_back(make_message(Severity::Error,       std::move(msg), context)); }
    void warn (std::string msg, std::string context="") { messages.push_back(make_message(Severity::Warning,     std::move(msg), context)); }
    void info (std::string msg, std::string context="") { messages.push_back(make_message(Severity::Information, std::move(msg), context)); }

    // The same, for a message that knows which part of the JSON document it is about. The
    // literal is still recorded: the pointer is resolved against the text as parsed, and a
    // config parsed before an edit would otherwise place the message nowhere.
    void error_at(const std::string& ptr, std::string msg, std::string context="") { messages.push_back(make_message(Severity::Error,   std::move(msg), context, ptr)); }
    void warn_at (const std::string& ptr, std::string msg, std::string context="") { messages.push_back(make_message(Severity::Warning, std::move(msg), context, ptr)); }

    // A message whose position is already known, such as a JSON syntax error.
    void error_at_range(lsp::Range range, std::string msg) {
        auto m = make_message(Severity::Error, std::move(msg), "");
        m.range = range;
        messages.push_back(std::move(m));
    }

private:
    Message make_message(Severity s, std::string msg, const std::string& context, const std::string& pointer = {}) {
        Message m{
            .message = std::move(msg),
            .severity = s,
            .file = file_context,
            .context = context.empty() ? std::nullopt : std::make_optional(Context{.literal=text::quote(context)}),
            .range = std::nullopt,
        };
        if (!pointer.empty() && json_context) m.range = json_context->range(pointer);
        return m;
    }
};

struct ConfigParser {
    ConfigParser(const ConfigPath& origin, config::ConfigLog& log) 
        : origin(origin), log(log)
    {}
    ConfigPath origin;
    config::ConfigLog& log;

    // out
    ConfigFile config{};
    std::vector<Project> projects{};

    bool parse();
private:
    std::optional<Project> parse_project(const nlohmann::json& pj, const std::string& pointer);
    std::unordered_set<fs::path> evaluate_patterns(Project& project, const std::string& pointer_prefix);
};


// Find all files under root matching the given glob pattern.
// The pattern is interpreted with '/' as the separator and can include
// *, **, ? as described.
struct FilePatternParser {
    FilePatternParser(fs::path root, std::string pattern, config::ConfigLog& log, std::string pointer = {})
        : root(std::move(root)), pattern(std::move(pattern)), log(log), pointer(std::move(pointer))
    {
        expand();
    }

    static std::vector<fs::path> expand(const fs::path& root, const std::string& pattern, config::ConfigLog& log, const std::string& pointer = {}) {
        return FilePatternParser(root, pattern, log, pointer).results;
    }

    std::vector<fs::path> results;
private:
    void expand();
    void expand_home();
    void split();
    void dfs(size_t idx, const fs::path& base);
    void make_canonical();

    fs::path root;
    std::string pattern;
    config::ConfigLog& log;
    std::string pointer;

    // State
    std::vector<std::string> parts;
    std::unordered_set<std::string> dedup;
};


} // namespace artic::ls::workspace::config

#endif // ARTIC_LS_CONFIG_H
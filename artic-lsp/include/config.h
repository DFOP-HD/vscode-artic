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
struct ConfigLog; struct ConfigDocument;
using ProjectId = std::string;

struct ConfigParser {
    ConfigParser(const IncludeConfig& origin, ConfigLog& log) 
        : origin(origin), log(log), config()
    {}
    IncludeConfig origin;
    ConfigLog& log;

    // out
    ConfigFile config;
    std::vector<Project> projects;

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
        if (pattern[0] == '~') {
            const char* home = std::getenv("HOME");
            root = home ? home : fs::path("/");
            pattern.erase(0, 1);
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
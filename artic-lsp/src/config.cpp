#include "config.h"

#include "artic/log.h"
#include <fstream>
#include <filesystem>
#include <iostream>
#include <optional>
#include <vector>
#if !defined(_WIN32)
// fnmatch is a POSIX function not available on Windows/MSVC/MinGW by default.
// Provide a minimal replacement that supports '*', '?' and simple character
// classes like [abc] and ranges [a-z]. This is sufficient for the glob usage
// in this file (matching filenames in a single directory component).
#include <fnmatch.h>
#else
#include <regex>
#include <cstring>
// Minimal fnmatch replacement for Windows. Returns 0 on match, FNM_NOMATCH otherwise.
#ifndef FNM_NOMATCH
#define FNM_NOMATCH 1
#endif
static inline int fnmatch(const char* pattern, const char* str, int /*flags*/) {
    std::string rx;
    rx.reserve(strlen(pattern) * 2);

    auto escape_char = [&](char c){
        static const char* specials = ".^$+(){}|\\";
        if(std::strchr(specials, c)) { rx.push_back('\\'); rx.push_back(c); }
        else rx.push_back(c);
    };

    for(const char* p = pattern; *p; ++p) {
        char c = *p;
        if (c == '*') {
            rx += ".*";
        } else if (c == '?') {
            rx += '.';
        } else if (c == '[') {
            // copy character class until closing ']' (very basic)
            rx.push_back('[');
            ++p;
            if(*p == '!') { rx.push_back('^'); ++p; }
            for(; *p && *p != ']'; ++p) {
                // push as-is; escape backslash
                if(*p == '\\') { rx += "\\\\"; }
                else rx.push_back(*p);
            }
            rx.push_back(']');
            if(*p == '\0') break;
        } else {
            escape_char(c);
        }
    }

    try {
        std::regex re(rx, std::regex::ECMAScript | std::regex::icase);
        if (std::regex_match(str, re)) return 0;
        return FNM_NOMATCH;
    } catch (...) {
        return FNM_NOMATCH;
    }
}
#endif

namespace artic::ls::workspace {

// Config --------------------------------------------------------------------

namespace config {
static const char* HOME = std::getenv("HOME");

static fs::path to_absolute_path(fs::path base_dir, std::string path) {
    if(path.starts_with("/")) return fs::weakly_canonical(path);
    if(path.starts_with("~/")) {
        base_dir = fs::path(HOME);
        path = path.substr(2);
    }
    return fs::weakly_canonical(base_dir / path);
}

bool ConfigParser::parse() {
    try {
        if (origin.path.empty()) {
            log.error("Config file path is empty", "include");
            return false;
        }
        if (!fs::exists(origin.path)) {
            if(!origin.is_optional) log.error("Config file does not exist: \"" + origin.path.generic_string() + "\"", origin.raw_path_string);
            return false;
        }
        log.file_context = origin.path;

        nlohmann::json j;
        std::ifstream is(origin.path);
        is >> j;

        config.path = origin.path;
        if (!j.contains("artic-config")) {
            log.error(
                "Missing artic-config header\n"
                "Example: \"artic-config\": \"2.0\""
            );
            return false;
        }
        for(auto& [key, value]: j.items()){
            if(key == "artic-config" ||
                key == "default-project" ||
                key == "include" ||
                key == "projects"
            ) continue;
            log.error("unknown json property \"" + key + "\"", key);
        }

        config.version = j["artic-config"].get<std::string>();
        int version = 2;
        if (config.version != "2.0") {
            log.warn("Unsupported artic-config version (Newest is 2.0)", "artic-config");
        } else if (config.version == "1.0") {
            log.warn("Deprecated artic-config version (Newest is 2.0)", "artic-config");
            version = 1;
        }

        if (auto pj = j.find("projects"); pj != j.end()) {
            for (auto& pj : *pj) {
                if(auto proj = parse_project(pj)) {
                    log::info("Parsed project: {}", proj->name);
                    projects.push_back(*proj);
                    config.projects.push_back(proj->name);
                }
            }
        }
        if (j.contains("default-project")) {
            auto dpj = j["default-project"];
            if(dpj.is_string()) {
                // reference to named project
                config.default_project = dpj.get<std::string>();
            } else if(dpj.is_object()) {
                // inline project definition
                if(auto proj = parse_project(dpj)){
                    projects.push_back(*proj);
                    config.projects.push_back(proj->name);
                    config.default_project = proj->name;
                }
            }
        }
        if (j.contains("include")) {
            for (auto& incj : j["include"]) {
                auto path = incj.get<std::string>();
                if(path == "<global>"){
                    log.warn("Deprecated: including a global configuration file with '<global>' is no longer supported", "<global>");
                    continue;
                }
                ConfigPath include;
                include.raw_path_string = path;
                if(path.ends_with('?')){
                    path = path.substr(0, path.size()-1);
                    include.is_optional = true;
                }
                include.path = to_absolute_path(origin.path.parent_path(), path);

                config.includes.push_back(std::move(include));
            }
        }
        return true;
    } catch (const std::exception& e) {
        log.error(std::string("Failed to parse json ") + origin.path.generic_string() + ": " + e.what());
        return false;
    }
}

std::optional<Project> ConfigParser::parse_project(const nlohmann::json& pj) {
    Project p;
    if (!pj.contains("name")) {
        log.error(
            "Every project must have a name"
            "\nExample: " + nlohmann::json{{"name", "my_project"}}.dump(),
            "projects"
        );
        return std::nullopt;
    }
    p.name = pj["name"].get<std::string>();

    std::string folder_ptrn = pj.value<std::string>("folder", "");
    fs::path root = config.path.parent_path();
    if (folder_ptrn.empty()) {
        p.root_dir = root;
    } else {
        auto res = to_absolute_path(root, folder_ptrn);
        if(fs::exists(res) && fs::is_directory(res)) {
            p.root_dir = res;
        } else {
            log.error("Project folder does not exist: " + res.generic_string(), pj.value<std::string>("folder", ""));
            p.root_dir = root;
        }
    }

    p.dependencies =  pj.value<std::vector<std::string>>("dependencies", {});
    p.origin = config.path;
    p.file_patterns = pj.value<std::vector<std::string>>("files", {});
    auto files = evaluate_patterns(p);
    for (auto& file : files) {
        p.files.push_back(fs::weakly_canonical(file));
    }
    return p;
}

std::unordered_set<fs::path> ConfigParser::evaluate_patterns(Project& project) {
    // evaluate file patterns
    std::vector<std::string> include_patterns;
    std::vector<std::string> exclude_patterns;
    for (const auto& pattern : project.file_patterns) {
        if (!pattern.empty() && pattern[0] == '!') {
            exclude_patterns.push_back(pattern);
        } else {
            include_patterns.push_back(pattern);
        }
    }

    fs::path root_dir = project.root_dir;

    // Collect all files matching include patterns and not matching exclude patterns
    std::unordered_set<fs::path> matched_files;

    auto file_arr_to_string = [](const fs::path& root_dir, const auto& files){
        std::ostringstream s;
        s << files.size() << " files:" << std::endl;
        for(const auto& file : files) {
            s << "- " << fs::relative(file, root_dir).generic_string() << " " << std::endl;
        }
        return s.str();
    };

    // Evaluate include patterns
    for (const auto& pattern : include_patterns) {
        auto matches = FilePatternParser::expand(root_dir, pattern, log);
        if (matches.empty()) {
            log.warn("0 files", pattern);
            continue;
        }

        auto before = matched_files.size();
        matched_files.insert(matches.begin(), matches.end());
        auto after = matched_files.size();

        log.info(
            "+ " + std::to_string(after - before) + " files"
            + " | total matches: " + file_arr_to_string(root_dir, matches),
            pattern
        );
    }

    for (const auto& pattern : exclude_patterns) {
        auto matches = FilePatternParser::expand(root_dir, pattern.substr(1), log);
        if (matches.empty()) {
            log.warn("0 files excluded", pattern);
            continue;
        }
        auto before = matched_files.size();
        for (const auto& m : matches) {
            matched_files.erase(m);
        }
        auto after = matched_files.size();

        log.info(
            "- " + std::to_string(before - after) + " files"
            + " | total matches: " + file_arr_to_string(root_dir, matches),
            pattern
        );
    }
    return matched_files;
}

void FilePatternParser::dfs(size_t idx,const fs::path& base){
    if(idx == parts.size()) {
        // End: if base is a regular file, record it.
        if(fs::is_regular_file(base)) {
            auto norm = fs::weakly_canonical(base);
            if(dedup.insert(norm.generic_string()).second) results.emplace_back(norm);
        }
        return;
    }

    const std::string& part = parts[idx];

    // Special case: '**' as its own segment matches zero or more directory levels.
    if(part == "**") {
        // 1) Match zero directories
        dfs(idx+1, base);
        // 2) Recurse into subdirectories (unbounded)
        // Guard against huge traversals
        size_t dir_count = 0;
        for(auto it = fs::directory_iterator(base); it != fs::directory_iterator(); ++it) {
            if(!it->is_directory()) continue;
            if(++dir_count > 20'000) { // arbitrary safety cap
                log.warn("Stopped expanding '**' due to excessive directories", part);
                break;
            }
            dfs(idx, it->path()); // stay on same ** index
        }
        return;
    }

    // If last component and refers to a file name directly without wildcards
    if(!is_wildcard(part)) {
        fs::path next = base / part;
        if(idx + 1 == parts.size()) {
            if(fs::is_regular_file(next)) {
                auto norm = fs::weakly_canonical(next);
                if(dedup.insert(norm.generic_string()).second) results.emplace_back(norm);
            }
            return; // even if it is directory but pattern ended, we only collect files
        } else {
            if(fs::is_directory(next)) {
                dfs(idx+1, next);
            }
        }
        return;
    }

    // Wildcard segment (but not **) -> enumerate entries in this directory only
    size_t checked = 0;
    for(auto it = fs::directory_iterator(base); it != fs::directory_iterator(); ++it) {
        if(++checked > 1'000) { log.warn("Stopped expanding wildcard: too many entries", part); break; }
        const auto& path = it->path();
        std::string filename = path.filename().generic_string();
        if(fnmatch(part.c_str(), filename.c_str(), 0) == 0) {
            if(idx + 1 == parts.size()) {
                if(it->is_regular_file()) {
                    auto norm = fs::weakly_canonical(path);
                    if(dedup.insert(norm.generic_string()).second) results.emplace_back(norm);
                }
            } else if(it->is_directory()) {
                dfs(idx+1, path);
            }
        }
    }
};


std::optional<Project> parse_vcxproj(const ConfigPath& origin, ConfigLog& log) {
    log::info("Parsing vcxproj config file: {}", origin.path.generic_string());
    /* 
    Need to parse "artic.exe path/to/file_1 path/to/file_2 ... path/to/file_n --your_artic_args ..."
    1. find string "artic.exe " in the vcxproj file (there may be multiple, but we will just take the first one for now)
    2. collect all file paths that appear after "artic.exe " and before " --" (if present) or end of line
     - these file paths may be relative to the vcxproj file location, so we need to resolve them to absolute paths
     - we can ignore any arguments after " --"
    */
    std::ifstream is(origin.path);
    if (!is) {
        log.error("Could not read config file" + origin.path.generic_string());
        return std::nullopt;
    }
    std::string line;
    while (std::getline(is, line)) {
        auto find_str = "artic.exe ";
        auto pos = line.find(find_str);
        if (pos != std::string::npos) {
            pos += strlen(find_str);
            auto end_pos = line.find(" --", pos);
            if (end_pos == std::string::npos) end_pos = line.size();
            std::string files_str = line.substr(pos, end_pos - pos);
            std::istringstream ss(files_str);
            std::vector<fs::path> files;
            std::string file;
            while (ss >> file) {
                auto abs_path = to_absolute_path(origin.path.parent_path(), file);
                files.push_back(abs_path);
                // log.info("Found file in vcxproj: " + abs_path.generic_string());
            }
            if (!files.empty()) {
                Project p;
                p.name = origin.path.stem().generic_string();
                p.root_dir = origin.path.parent_path();
                p.files = files;
                p.origin = origin.path;
                log::info("Found project '{}' ({} files) in vcxproj config file: {}", p.name, p.files.size(), origin.path.generic_string());
                return p;
            }
        }
    }
    return std::nullopt;
}

} // config

} // namespace artic::ls
#include "config.h"

#include "artic/log.h"
#include "paths.h"
#include "text.h"
#include <algorithm>
#include <fstream>
#include <filesystem>
#include <optional>
#include <sstream>
#include <unordered_set>
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

bool ConfigParser::parse() {
    JsonSource source;
    try {
        if (origin.path.empty()) {
            log.error("Config file path is empty", "include");
            return false;
        }
        if (!fs::exists(origin.path)) {
            if(!origin.is_optional) log.error("Config file does not exist: \"" + origin.path.generic_string() + "\"", origin.raw_path_string);
            return false;
        }
        auto text = paths::read_file(origin.path);
        if (!text) {
            log.error("Could not read config file: \"" + origin.path.generic_string() + "\"", origin.raw_path_string);
            return false;
        }
        // The positions nlohmann records are offsets into this text, so it has to be kept
        // for as long as messages are made. Scoped: on failure the caller keeps reporting
        // against its own config file.
        source = JsonSource(*text);
        auto ctx = log.scoped_file(origin.path, &source);

        nlohmann::json j = nlohmann::json::parse(source.text());

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
            log.error_at(log.member_range(value), "unknown json property \"" + key + "\"", key);
        }

        config.version = j["artic-config"].get<std::string>();
        if (config.version == "1.0") {
            log.warn_at(log.member_range(j["artic-config"]), "Deprecated artic-config version (Newest is 2.0)", "artic-config");
        } else if (config.version != "2.0") {
            log.warn_at(log.member_range(j["artic-config"]), "Unsupported artic-config version (Newest is 2.0)", "artic-config");
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
            // A reference, not a copy: nlohmann's positions belong to the parsed node.
            const auto& dpj = j["default-project"];
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
            for (const auto& incj : j["include"]) {
                auto path = incj.get<std::string>();
                if(path == "<global>"){
                    log.warn_at(log.value_range(incj), "Deprecated: including a global configuration file with '<global>' is no longer supported", "<global>");
                    continue;
                }
                ConfigPath include;
                include.raw_path_string = path;
                if(path.ends_with('?')){
                    path = path.substr(0, path.size()-1);
                    include.is_optional = true;
                }
                include.path = paths::to_absolute_path(origin.path.parent_path(), path);

                config.includes.push_back(std::move(include));
            }
        }
        return true;
    } catch (const nlohmann::json::parse_error& e) {
        // The scoped file context has already unwound by the time we get here, so it
        // has to be re-established or the message is dropped for having no file.
        auto ctx = log.scoped_file(origin.path, &source);
        auto position = source.position_of_byte(e.byte);
        log.error_at(lsp::Range{ position, position }, std::string("Failed to parse json: ") + e.what());
        return false;
    } catch (const std::exception& e) {
        auto ctx = log.scoped_file(origin.path, &source);
        log.error(std::string("Failed to parse json ") + origin.path.generic_string() + ": " + e.what());
        return false;
    }
}

std::optional<Project> ConfigParser::parse_project(const nlohmann::json& pj) {
    Project p;
    if (!pj.contains("name")) {
        log.error_at(log.member_range(pj),
            "Every project must have a name"
            "\nExample: " + nlohmann::json{{"name", "my_project"}}.dump(),
            "projects"
        );
        return std::nullopt;
    }
    p.name = pj["name"].get<std::string>();
    // The value only: this one places an inlay hint, so it has to end after the name.
    p.name_range = log.value_range(pj["name"]);

    std::string folder_ptrn = pj.value<std::string>("folder", "");
    fs::path root = config.path.parent_path();
    if (folder_ptrn.empty()) {
        p.root_dir = root;
    } else {
        auto res = paths::to_absolute_path(root, folder_ptrn);
        if(fs::exists(res) && fs::is_directory(res)) {
            p.root_dir = res;
        } else {
            log.error_at(log.member_range(pj["folder"]), "Project folder does not exist: " + res.generic_string(), folder_ptrn);
            p.root_dir = root;
        }
    }

    p.dependencies =  pj.value<std::vector<std::string>>("dependencies", {});
    p.origin = config.path;
    p.file_patterns = pj.value<std::vector<std::string>>("files", {});
    auto files = pj.find("files");
    auto matched = evaluate_patterns(p, files == pj.end() ? nullptr : &*files);
    for (auto& file : matched) {
        p.files.push_back(paths::canonical_path(file));
    }
    return p;
}

std::unordered_set<fs::path> ConfigParser::evaluate_patterns(Project& project, const nlohmann::json* files) {
    // evaluate file patterns, keeping each one's index so a message can point back at it
    std::vector<size_t> include_patterns;
    std::vector<size_t> exclude_patterns;
    for (size_t i = 0; i < project.file_patterns.size(); ++i) {
        if (!project.file_patterns[i].empty() && project.file_patterns[i][0] == '!') exclude_patterns.push_back(i);
        else include_patterns.push_back(i);
    }

    fs::path root_dir = project.root_dir;
    auto range_of = [&](size_t i) -> std::optional<lsp::Range> {
        if (!files || !files->is_array() || i >= files->size()) return std::nullopt;
        return log.value_range((*files)[i]);
    };

    // Collect all files matching include patterns and not matching exclude patterns
    std::unordered_set<fs::path> matched_files;

    // Evaluate include patterns
    for (auto i : include_patterns) {
        const auto& pattern = project.file_patterns[i];
        auto range = range_of(i);
        auto matches = FilePatternParser::expand(root_dir, pattern, log, range);
        if (matches.empty()) {
            log.warn_at(range, "0 files", pattern);
            project.pattern_matches.push_back({ .pattern = pattern, .range = range });
            continue;
        }

        auto before = matched_files.size();
        matched_files.insert(matches.begin(), matches.end());
        auto after = matched_files.size();

        // Reported as an inlay hint on the config document rather than as a diagnostic:
        // a pattern that works is not a problem and does not belong in the Problems panel.
        project.pattern_matches.push_back({
            .pattern = pattern,
            .matched = matches.size(),
            .changed = after - before,
            .range = range,
        });
    }

    for (auto i : exclude_patterns) {
        const auto& pattern = project.file_patterns[i];
        auto range = range_of(i);
        auto matches = FilePatternParser::expand(root_dir, pattern.substr(1), log, range);
        if (matches.empty()) {
            log.warn_at(range, "0 files excluded", pattern);
            project.pattern_matches.push_back({ .pattern = pattern, .excludes = true, .range = range });
            continue;
        }
        auto before = matched_files.size();
        for (const auto& m : matches) {
            matched_files.erase(m);
        }
        auto after = matched_files.size();

        project.pattern_matches.push_back({
            .pattern = pattern,
            .matched = matches.size(),
            .changed = before - after,
            .excludes = true,
            .range = range,
        });
    }
    return matched_files;
}

namespace {

bool is_wildcard(const std::string& s) {
    return s.find('*') != std::string::npos || s.find('?') != std::string::npos;
}

} // anonymous namespace

void FilePatternParser::expand() {
    auto original_pattern = pattern;
    expand_home();
    if (!fs::exists(root) || !fs::is_directory(root)) {
        log.error_at(range, "Folder does not exist: " + root.generic_string(), original_pattern);
        return;
    }
    split();
    dfs(0, root);
    make_canonical();
}

void FilePatternParser::expand_home() {
    if (pattern.starts_with("~/")) {
        if (const char* home = std::getenv("HOME")) root = home;
        else root = fs::current_path().root_path();
        pattern.erase(0, 2);
    }
    if (pattern.starts_with('/')) {
        root = fs::current_path().root_path();
        pattern.erase(0, 1);
    }
}

void FilePatternParser::split() {
    parts.reserve(8);
    std::string cur; cur.reserve(pattern.size());
    for (char c : pattern) {
        if (c == '/') { parts.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    parts.push_back(cur);
}

void FilePatternParser::make_canonical() {
    for (auto& p : results) p = paths::canonical_path(p);
}

void FilePatternParser::dfs(size_t idx,const fs::path& base){
    if(idx == parts.size()) {
        // End: if base is a regular file, record it.
        if(fs::is_regular_file(base)) {
            auto norm = paths::canonical_path(base);
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
                log.warn_at(range, "Stopped expanding '**' due to excessive directories", part);
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
        if(++checked > 1'000) { log.warn_at(range, "Stopped expanding wildcard: too many entries", part); break; }
        const auto& path = it->path();
        std::string filename = path.filename().generic_string();
        if(fnmatch(part.c_str(), filename.c_str(), 0) == 0) {
            if(idx + 1 == parts.size()) {
                if(it->is_regular_file()) {
                    auto norm = paths::canonical_path(path);
                    if(dedup.insert(norm.generic_string()).second) results.emplace_back(norm);
                }
            } else if(it->is_directory()) {
                dfs(idx+1, path);
            }
        }
    }
};


namespace {

using text::to_lower;
using text::split_whitespace;
using text::split_command_line;
using text::trim_left;

// Ninja escapes `$`, `:`, space and newline with a leading `$` inside build statements.
std::string ninja_unescape(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '$' && i + 1 < in.size()) ++i;
        out.push_back(in[i]);
    }
    return out;
}

// A token is a path spelled by a build system, so it may use either separator regardless
// of the platform we are parsing it on. fs::path only recognises the native one.
std::string_view base_name(std::string_view path) {
    auto pos = path.find_last_of("/\\");
    return pos == std::string_view::npos ? path : path.substr(pos + 1);
}

std::string extension_of(std::string_view path) {
    auto name = base_name(path);
    auto dot = name.rfind('.');
    return dot == std::string_view::npos || dot == 0 ? std::string{} : to_lower(std::string(name.substr(dot)));
}

bool is_artic_executable(std::string_view token) {
    auto name = base_name(token);
    auto ext = extension_of(token);
    auto stem = name.substr(0, name.size() - ext.size());
    return to_lower(std::string(stem)) == "artic" && (ext.empty() || ext == ".exe");
}

bool is_artic_source(std::string_view token) {
    auto ext = extension_of(token);
    return ext == ".art" || ext == ".impala";
}

// Generated commands are usually `<shell> /C "<real command>"`, which wraps everything in
// one pair of quotes. Left in place, a quote-aware split returns the whole command as a
// single token, so the wrapper has to come off before the command can be tokenised.
std::string_view unwrap_shell_command(std::string_view command) {
    for (size_t pos = 0; pos < command.size();) {
        auto end = command.find(' ', pos);
        auto token = command.substr(pos, end == std::string_view::npos ? end : end - pos);
        if (token == "/C" || token == "/c" || token == "-c") {
            auto body = trim_left(command.substr(end == std::string_view::npos ? command.size() : end));
            while (!body.empty() && (body.back() == ' ' || body.back() == '\t')) body.remove_suffix(1);
            // Only a fully quoted remainder is a wrapper; anything else is a real argument.
            if (body.size() >= 2 && body.front() == '"' && body.back() == '"')
                return body.substr(1, body.size() - 2);
            return command;
        }
        if (end == std::string_view::npos) break;
        pos = command.find_first_not_of(' ', end);
        if (pos == std::string_view::npos) break;
    }
    return command;
}

// A <Command> element holds a shell command with the XML markup still around it. Tags are
// dropped first so `&lt;` in the command itself is not mistaken for one.
std::string xml_text(std::string_view line) {
    std::string out;
    out.reserve(line.size());
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '<') {
            auto close = line.find('>', i);
            if (close == std::string_view::npos) break;
            i = close;
            out.push_back(' ');
            continue;
        }
        if (line[i] == '&') {
            auto semi = line.find(';', i);
            if (semi != std::string_view::npos && semi - i <= 5) {
                auto entity = line.substr(i + 1, semi - i - 1);
                if (entity == "quot")      { out.push_back('"');  i = semi; continue; }
                else if (entity == "apos") { out.push_back('\''); i = semi; continue; }
                else if (entity == "lt")   { out.push_back('<');  i = semi; continue; }
                else if (entity == "gt")   { out.push_back('>');  i = semi; continue; }
                else if (entity == "amp")  { out.push_back('&');  i = semi; continue; }
            }
        }
        out.push_back(line[i]);
    }
    return out;
}

} // anonymous namespace

std::optional<Project> parse_vcxproj(const ConfigPath& origin, ConfigLog& log) {
    log::info("Parsing vcxproj config file: {}", origin.path.generic_string());
    /*
    A custom build step holds the compiler invocation inside a <Command> element:

        <Command>setlocal
        "C:\My Tools\artic.exe" src\a.art "src\b c.art" -emit-llvm -o out.ll

    The first artic invocation wins. Everything between the executable and the first
    option is an input, resolved relative to the .vcxproj. The command is a shell command
    line, so a path containing spaces is quoted and must survive tokenisation intact.
    */
    std::ifstream is(origin.path);
    if (!is) {
        log.error("Could not read config file" + origin.path.generic_string());
        return std::nullopt;
    }
    std::string line;
    while (std::getline(is, line)) {
        auto command = xml_text(line);
        auto tokens = split_command_line(unwrap_shell_command(command));
        auto exe = std::find_if(tokens.begin(), tokens.end(), is_artic_executable);
        if (exe == tokens.end()) continue;

        std::vector<fs::path> files;
        for (auto it = exe + 1; it != tokens.end() && !it->starts_with("-"); ++it) {
            if (!is_artic_source(*it)) continue;
            files.push_back(paths::to_absolute_path(origin.path.parent_path(), paths::from_msbuild_path(*it)));
        }
        if (files.empty()) continue;

        Project p;
        p.name = origin.path.stem().generic_string();
        p.root_dir = origin.path.parent_path();
        p.files = std::move(files);
        p.origin = origin.path;
        log::info("Found project '{}' ({} files) in vcxproj config file: {}", p.name, p.files.size(), origin.path.generic_string());
        return p;
    }
    return std::nullopt;
}

std::vector<ConfigPath> parse_sln(const ConfigPath& origin, ConfigLog& log) {
    /*
    A solution lists one entry per project:

        Project("{<type guid>}") = "<name>", "<relative\path.vcxproj>", "{<guid>}"

    Solution folders reuse the same syntax but put the folder name in the path slot,
    so only entries that actually name a .vcxproj are of interest.
    */
    std::ifstream is(origin.path);
    if (!is) {
        log.error("Could not read solution file " + origin.path.generic_string(), origin.raw_path_string);
        return {};
    }

    std::vector<ConfigPath> projects;
    std::unordered_set<std::string> seen;
    std::string line;
    while (std::getline(is, line)) {
        if (!line.starts_with("Project(")) continue;

        auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        // The three quoted fields after '=' are name, path and guid.
        std::vector<std::string> fields;
        for (auto pos = eq; fields.size() < 3;) {
            auto open = line.find('"', pos);
            if (open == std::string::npos) break;
            auto close = line.find('"', open + 1);
            if (close == std::string::npos) break;
            fields.push_back(line.substr(open + 1, close - open - 1));
            pos = close + 1;
        }
        if (fields.size() < 2) continue;

        auto& rel = fields[1];
        auto ext = text::to_lower(fs::path(rel).extension().string());
        if (ext != ".vcxproj") continue;

        auto abs_path = paths::to_absolute_path(origin.path.parent_path(), paths::from_msbuild_path(rel));
        if (!seen.insert(abs_path.generic_string()).second) continue;
        if (!fs::exists(abs_path)) {
            log.warn("Solution references a project that does not exist: " + abs_path.generic_string(), rel);
            continue;
        }
        projects.push_back(ConfigPath{ .path = abs_path, .raw_path_string = rel });
    }

    log::info("Found {} project(s) in solution {}", projects.size(), origin.path.generic_string());
    return projects;
}

std::vector<Project> parse_ninja(const ConfigPath& origin, ConfigLog& log) {
    /*
    CMake writes one block per generated file:

        build src/add.ll | ...: CUSTOM_COMMAND <deps>
          COMMAND = cmd.exe /C "cd /D <build dir> && <path>\artic.exe a.impala b.art -emit-llvm -o out"

    The build statement names the target, the COMMAND line names the sources. Everything
    up to the first option is an input; the `cd` (if any) is what relative paths are
    relative to. The command is quoted where it has to be, so it is tokenised as a shell
    would: a quoted path containing spaces stays one token.
    */
    std::ifstream is(origin.path);
    if (!is) {
        log.error("Could not read ninja file " + origin.path.generic_string(), origin.raw_path_string);
        return {};
    }

    std::string line;
    std::string logical;
    // Ninja continues a line when it ends in `$`; the continuation's indent is dropped.
    auto read_logical_line = [&]() -> bool {
        logical.clear();
        bool read_any = false;
        while (std::getline(is, line)) {
            read_any = true;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            bool continued = !line.empty() && line.back() == '$';
            if (continued) line.pop_back();
            if (logical.empty()) logical = line;
            else logical.append(trim_left(line));
            if (!continued) break;
        }
        return read_any;
    };

    std::vector<Project> projects;
    std::string target;
    while (read_logical_line()) {
        std::string_view sv = logical;
        if (sv.starts_with("build ")) {
            auto tokens = split_whitespace(sv.substr(strlen("build ")));
            target = tokens.empty() ? "" : ninja_unescape(tokens.front());
            if (target.ends_with(':')) target.pop_back();
            continue;
        }

        auto trimmed = trim_left(sv);
        if (!trimmed.starts_with("COMMAND = ") || target.empty()) continue;
        auto command = unwrap_shell_command(trimmed.substr(strlen("COMMAND = ")));

        fs::path cwd = origin.path.parent_path();
        std::vector<fs::path> files;
        for (size_t pos = 0; pos <= command.size();) {
            auto next = command.find(" && ", pos);
            auto segment = command.substr(pos, next == std::string_view::npos ? std::string_view::npos : next - pos);
            pos = next == std::string_view::npos ? command.size() + 1 : next + 4;

            auto tokens = split_command_line(segment);

            if (auto it = std::find(tokens.begin(), tokens.end(), "cd"); it != tokens.end()) {
                if (++it != tokens.end() && to_lower(*it) == "/d") ++it;
                if (it != tokens.end()) cwd = fs::path(*it);
            }

            auto exe = std::find_if(tokens.begin(), tokens.end(), is_artic_executable);
            if (exe == tokens.end()) continue;
            for (auto it = exe + 1; it != tokens.end() && !it->starts_with("-"); ++it) {
                if (is_artic_source(*it)) files.push_back(paths::to_absolute_path(cwd, *it));
            }
            break; // as with .vcxproj, the first artic invocation wins
        }

        if (files.empty()) continue;
        Project p;
        p.name = target;
        p.root_dir = origin.path.parent_path();
        p.origin = origin.path;
        p.files = std::move(files);
        projects.push_back(std::move(p));
    }

    log::info("Found {} artic target(s) in ninja file {}", projects.size(), origin.path.generic_string());
    return projects;
}

namespace {

// Bounds on the workspace scan. It runs on the miss path of a file with no configuration
// above it, so it must not be able to walk an arbitrarily large tree: an AnyDSL checkout
// with LLVM in it is hundreds of thousands of files.
constexpr size_t max_scan_depth = 12;
constexpr size_t max_scanned_directories = 20000;
constexpr size_t max_build_files = 2000;

// Directories that never contain a build file worth reading, plus everything hidden.
// `build` is deliberately *not* here: it is exactly where the build files live.
bool is_skipped_directory(const std::string& name) {
    static const std::set<std::string_view> skipped = {
        "node_modules", "out", "dist", "bin", "obj", "target", "__pycache__",
    };
    return name.starts_with(".") || skipped.contains(to_lower(name));
}

bool mentions_artic(const fs::path& file) {
    auto content = paths::read_file(file);
    return content && to_lower(*content).find("artic") != std::string::npos;
}

// Whether `dir` is `covered` or below it.
bool is_within(const fs::path& covered, const fs::path& dir) {
    auto a = paths::lookup_key(covered).generic_string();
    auto b = paths::lookup_key(dir).generic_string();
    return b == a || (b.starts_with(a) && b[a.size()] == '/');
}

} // anonymous namespace

std::vector<fs::path> detect_build_files(const fs::path& root, ConfigLog& log) {
    std::vector<fs::path> solutions, ninja_files, vcxprojs;

    size_t directories = 0;
    std::error_code ec;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        log::info("Could not scan workspace root {}: {}", root.generic_string(), ec.message());
        return {};
    }
    for (fs::recursive_directory_iterator end; it != end; it.increment(ec)) {
        if (ec) break;
        if (it->is_directory(ec)) {
            if (++directories > max_scanned_directories) break;
            if (static_cast<size_t>(it.depth()) + 1 >= max_scan_depth ||
                is_skipped_directory(it->path().filename().string()))
                it.disable_recursion_pending();
            continue;
        }
        if (solutions.size() + ninja_files.size() + vcxprojs.size() >= max_build_files) break;

        auto name = to_lower(it->path().filename().string());
        auto ext = to_lower(it->path().extension().string());
        if (ext == ".sln") solutions.push_back(it->path());
        else if (name == "build.ninja") ninja_files.push_back(it->path());
        else if (ext == ".vcxproj") vcxprojs.push_back(it->path());
    }

    // Deterministic order, so which of two overlapping build files wins does not depend on
    // the order the filesystem happened to hand them out in.
    for (auto* files : { &solutions, &ninja_files, &vcxprojs })
        std::sort(files->begin(), files->end());

    // A .sln contains nothing but project names and GUIDs, so it never mentions artic
    // itself; it qualifies when one of the projects it lists does.
    std::set<fs::path> artic_projects;
    for (const auto& vcxproj : vcxprojs)
        if (mentions_artic(vcxproj)) artic_projects.insert(paths::lookup_key(vcxproj));

    std::vector<fs::path> kept;
    std::vector<fs::path> covered_dirs;
    auto is_covered = [&](const fs::path& dir) {
        return std::any_of(covered_dirs.begin(), covered_dirs.end(),
                           [&](const fs::path& covered) { return is_within(covered, dir); });
    };

    // Strongest match first: a solution supersedes the projects it lists, and a ninja file
    // supersedes the projects next to it. Including both would define every project twice,
    // and the duplicate is silently dropped rather than merged.
    for (const auto& solution : solutions) {
        auto listed = parse_sln(ConfigPath{ .path = solution, .is_implicit = true }, log);
        if (!std::any_of(listed.begin(), listed.end(), [&](const ConfigPath& project) {
                return artic_projects.contains(paths::lookup_key(project.path));
            }))
            continue;
        kept.push_back(solution);
        covered_dirs.push_back(solution.parent_path());
    }
    for (const auto& ninja : ninja_files) {
        if (is_covered(ninja.parent_path()) || !mentions_artic(ninja)) continue;
        kept.push_back(ninja);
        covered_dirs.push_back(ninja.parent_path());
    }
    for (const auto& vcxproj : vcxprojs) {
        if (!artic_projects.contains(paths::lookup_key(vcxproj))) continue;
        if (is_covered(vcxproj.parent_path())) continue;
        kept.push_back(vcxproj);
    }

    log::info("Scanned {} directories under {} and detected {} build file(s)",
              directories, root.generic_string(), kept.size());
    return kept;
}

} // config

} // namespace artic::ls
#include "config.h"

#include "artic/log.h"
#include <fstream>
#include <filesystem>
#include <fnmatch.h>
#include <iostream>
#include <memory>
#include <optional>
#include <vector>
#include <unordered_map>

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
            if(!origin.is_optional) log.error("Config file does not exist: \"" + origin.path.string() + "\"", origin.raw_path_string);
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
                "Example: \"artic-config\": \"1.0\""
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
                IncludeConfig include;
                include.raw_path_string = path;
                if(path.ends_with('?')){
                    path = path.substr(0, path.size()-1);
                    include.is_optional = true;
                }
                include.path = to_absolute_path(origin.path.parent_path(), path);
                include.path = fs::weakly_canonical(include.path);

                config.includes.push_back(std::move(include));
            }
        }
        return true;
    } catch (const std::exception& e) {
        log.error(std::string("Failed to parse json ") + origin.path.string() + ": " + e.what());
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
            log.error("Project folder does not exist: " + res.string(), pj.value<std::string>("folder", ""));
            p.root_dir = root;
        }
    }

    p.dependencies =  pj.value<std::vector<std::string>>("dependencies", {});
    p.origin = config.path;
    p.file_patterns = pj.value<std::vector<std::string>>("files", {});
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
            s << "- " << fs::relative(file, root_dir).string() << " " << std::endl;
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
            auto norm = fs::weakly_canonical(base).string();
            if(dedup.insert(norm).second) results.emplace_back(norm);
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
                if(dedup.insert(norm).second) results.emplace_back(norm);
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
        std::string filename = path.filename().string();
        if(fnmatch(part.c_str(), filename.c_str(), 0) == 0) {
            if(idx + 1 == parts.size()) {
                if(it->is_regular_file()) {
                    auto norm = fs::weakly_canonical(path);
                    if(dedup.insert(norm).second) results.emplace_back(norm);
                }
            } else if(it->is_directory()) {
                dfs(idx+1, path);
            }
        }
    }
};

} // config

ConfigParse ConfigParse::parse(const fs::path& path, config::ConfigLog& log){
    IncludeConfig origin{ .path = path, .raw_path_string = path.string(), .is_optional = false };
    config::ConfigParser parser(origin, log);
    bool success = parser.parse();
    if (success) {
        return ConfigParse{ .success = true, .config = parser.config, .projects = parser.projects };
    } else {
        return ConfigParse{ .success = false, };
    }
}

// Workspace --------------------------------------------------------------------------

ConfigFile* Workspace::instantiate_config(const IncludeConfig& origin, config::ConfigLog& log) {
    if(tracked_configs_.contains(origin.path)) {
        return tracked_configs_.at(origin.path).get();
    }

    // std::map<Project::Identifier, Project> project_defs;
    // CollectProjectsData data {.log = log, .projects=project_defs };

    // IncludeConfig origin  { .path = path, .raw_path_string = path, .is_optional = false };
    config::ConfigParser parser(origin, log);
    bool success = parser.parse();
    if (!success) return nullptr;

    // track config
    tracked_configs_[origin.path] = arena_.make_ptr<ConfigFile>(parser.config);

    // track projects
    for (const auto& project : parser.projects){
        if(tracked_projects_.contains(project.name)) {
            log.file_context = origin.path;
            log.warn("ignoring duplicate definition of " + project.name + " in " + project.origin.string(), project.name);
            continue;
        }
        tracked_projects_[project.name] = arena_.make_ptr<Project>(project); // copy
    }
    
    // recurse included configs
    for (const auto& include : parser.config.includes) {
        if(fs::exists(include.path)){
            auto included_config = instantiate_config(include, log);
            if(!included_config) {
                log.file_context = origin.path;
                log.error("Failed to include config" + include.path.string(), include.raw_path_string);
            }
        } else {
            log.file_context = origin.path;
            if(include.is_optional) {
                // log.info("Config file does not exist: \"" + include.path.string() + "\"", include.raw_path_string);
            } else {
                log.error("Config file does not exist: \"" + include.path.string() + "\"", include.raw_path_string);
            }
        }
    }

    // auto log_project_info = [&](const Project& dep, const fs::path& current_config){
    //     log.file_context = current_config;
    //     if(dep.origin != current_config)
    //         log.info("Declared in config \"" + dep.origin.string() + "\"", dep.name);

    //     auto files = dep.collect_files();
    //     std::ostringstream s;
    //     auto num_own_files = dep.files.size();
    //     auto dep_files = files.size() - num_own_files;
    //     s << num_own_files;
    //     if(dep_files > 0) s << " + " << dep_files;
    //     s << " files: " << std::endl;
    //     for(const auto& file : files) {
    //         s << "- " << "\"" << fs::weakly_canonical(file->path).string() << "\" " << std::endl;
    //     }
    //     log.info(s.str(), dep.name);

    //     if(dep.origin == current_config)
    //         log.info("Declared in this config", dep.name);
    // };

    // // log dependency resolution
    // for (auto& [id, p] : projects) {
    //     log.file_context = p.project->origin;
    //     log_project_info(*p.project, p.project->origin);

    //     for (auto& dep_id : p.dependencies) {
    //         if(projects.contains(dep_id)) {
    //             auto& dep = projects.at(dep_id).project;
    //             log_project_info(*dep, p.project->origin);
    //         } else {
    //             // log.error("Failed to resolve dependency " + dep_id + " for project " + p.project->name, p.project->name);
    //             log.error("Failed to resolve dependency " + dep_id, dep_id);
    //         }
    //     }
    // }

    log.file_context = "";
}



} // namespace artic::ls
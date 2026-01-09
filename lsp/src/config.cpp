#include "artic/ls/config.h"
#include "artic/log.h"
#include <fstream>
#include <filesystem>
#include <fnmatch.h>
#include <iostream>
#include <memory>
#include <optional>
#include <vector>
#include <unordered_map>
#include <unordered_set>

#include <nlohmann/json.hpp>

namespace artic::ls::workspace {

// Util --------------------------------------------------------------------------

namespace {

struct CollectProjectsData {
    config::ConfigLog& log;
    std::map<Project::Identifier, config::ProjectDefinition>& projects;
    std::unordered_set<std::filesystem::path> visited_configs;
};

static inline void collect_projects_recursive(const config::ConfigDocument& config, CollectProjectsData& data, int depth = 0) {
    log::info("Collecting projects from config: {}", config.path.string());
    if(data.visited_configs.contains(config.path)) return;
    data.visited_configs.insert(config.path);

    auto& log = data.log;
    
    log.file_context = config.path;

    // Register Projects
    for (const auto& proj : config.projects) {
        if(data.projects.contains(proj.name)) {
            // already registered
            auto& val = data.projects[proj.name];
            if(val.origin == proj.origin) {
                if(depth < val.depth) {
                    val.depth = depth;
                    continue;
                }
            }
            log.warn("ignoring duplicate definition of " + proj.name + " in " + proj.origin.string(), proj.name);
            continue;
        }

        data.projects[proj.name] = proj;
        data.projects[proj.name].depth = depth;
    }
    // Recurse included configs
    for (const auto& include : config.includes) {
        if (!std::filesystem::exists(include.path)) {
            if(include.is_optional) continue;
        }
        
        if(auto include_config = config::ConfigDocument::parse(include, log)) {
            collect_projects_recursive(include_config.value(), data, depth+1);
            
            log.file_context = config.path;
            auto log_project_info = [&](const config::ConfigDocument& cfg){
                std::ostringstream s;

                s << cfg.projects.size() << " + ?" << " projects: ";
                s << std::endl; 
                for(const auto& file : cfg.projects) {
                    s << "- " << file.name << std::endl;
                }
                log.info(s.str(), include.raw_path_string);
            };
            log_project_info(include_config.value());
            log.info("Path: \"" + include.path.string() + "\"", include.raw_path_string);
        }
    }
}

// Find all files under root matching the given glob pattern.
// The pattern is interpreted with '/' as the separator and can include
// *, **, ? as described.
static inline std::vector<std::filesystem::path> find_matching_glob(std::filesystem::path root, std::string glob_pattern, config::ConfigLog& log) {
    std::vector<std::filesystem::path> results;
    if(glob_pattern.empty()) return results;

    if (glob_pattern[0] == '~') {
        const char* home = std::getenv("HOME");
        if (home) {
            root = home;
            glob_pattern.erase(0, 1);
        } else {
            log.warn("Cannot expand ~ in pattern: " + glob_pattern + " $HOME is undefined", glob_pattern);
            return results;
        }
    }
    if(glob_pattern[0] == '/') {
        root = std::filesystem::path("/");
        glob_pattern.erase(0, 1);
    }

    if (!std::filesystem::exists(root) || !std::filesystem::is_directory(root)) {
        log.error("Folder does not exist: {}", root.string());
        return results;
    }

    // Split pattern into components by '/'
    std::vector<std::string> parts; parts.reserve(8);
    {
        std::string cur; cur.reserve(glob_pattern.size());
        for(char c : glob_pattern) {
            if(c == '/') { parts.push_back(cur); cur.clear(); }
            else cur.push_back(c);
        }
        parts.push_back(cur);
    }

    auto is_wild = [](const std::string& s){ return s.find('*') != std::string::npos || s.find('?') != std::string::npos; };

    // Use DFS to expand pattern component by component.
    std::unordered_set<std::string> dedup;

    std::function<void(size_t,const std::filesystem::path&)> dfs = [&](size_t idx, const std::filesystem::path& base){
        if(idx == parts.size()) {
            // End: if base is a regular file, record it.
            if(std::filesystem::is_regular_file(base)) {
                auto norm = std::filesystem::weakly_canonical(base).string();
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
            for(auto it = std::filesystem::directory_iterator(base); it != std::filesystem::directory_iterator(); ++it) {
                if(!it->is_directory()) continue;
                if(++dir_count > 50'000) { // arbitrary safety cap
                    log.warn("Stopped expanding '**' due to excessive directories", part);
                    break;
                }
                dfs(idx, it->path()); // stay on same ** index
            }
            return;
        }

        // If last component and refers to a file name directly without wildcards
        if(!is_wild(part)) {
            std::filesystem::path next = base / part;
            if(idx + 1 == parts.size()) {
                if(std::filesystem::is_regular_file(next)) {
                    auto norm = std::filesystem::weakly_canonical(next).string();
                    if(dedup.insert(norm).second) results.emplace_back(norm);
                }
                return; // even if it is directory but pattern ended, we only collect files
            } else {
                if(std::filesystem::is_directory(next)) {
                    dfs(idx+1, next);
                }
            }
            return;
        }

        // Wildcard segment (but not **) -> enumerate entries in this directory only
        size_t checked = 0;
        for(auto it = std::filesystem::directory_iterator(base); it != std::filesystem::directory_iterator(); ++it) {
            if(++checked > 100'000) { log.warn("Stopped expanding wildcard: too many entries", part); break; }
            const auto& path = it->path();
            std::string filename = path.filename().string();
            if(fnmatch(part.c_str(), filename.c_str(), 0) == 0) {
                if(idx + 1 == parts.size()) {
                    if(it->is_regular_file()) {
                        auto norm = std::filesystem::weakly_canonical(path).string();
                        if(dedup.insert(norm).second) results.emplace_back(norm);
                    }
                } else if(it->is_directory()) {
                    dfs(idx+1, path);
                }
            }
        }
    };

    dfs(0, root);
    return results;
}

static std::shared_ptr<Project> instantiate_project(
    const config::ProjectDefinition& proj_def, 
    std::unordered_map<std::filesystem::path, std::shared_ptr<File>>& tracked_files,
    config::ConfigLog& log
) {
    log.file_context = proj_def.origin;

    // evaluate file patterns
    std::vector<std::string> include_patterns;
    std::vector<std::string> exclude_patterns;
    for (const auto& pattern : proj_def.file_patterns) {
        if (!pattern.empty() && pattern[0] == '!') {
            exclude_patterns.push_back(pattern);
        } else {
            include_patterns.push_back(pattern);
        }
    }

    std::filesystem::path root_dir = proj_def.root_dir;

    // Collect all files matching include patterns and not matching exclude patterns
    std::unordered_set<std::filesystem::path> matched_files;

    auto file_arr_to_string = [](const std::filesystem::path& root_dir, const auto& files){
        std::ostringstream s;
        s << files.size() << " files:" << std::endl;
        for(const auto& file : files) {
            s << "- " << std::filesystem::relative(file, root_dir).string() << " " << std::endl;
        }
        return s.str();
    };

    // Evaluate include patterns
    for (const auto& pattern : include_patterns) {
        if(proj_def.was_defined_in_global_config && pattern.find("**") != std::string::npos){
            log.error("Recursive include patterns are not allowed in global config", pattern);
            continue;
        }
        auto matches = find_matching_glob(root_dir, pattern, log);
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
        auto matches = find_matching_glob(root_dir, pattern.substr(1), log);
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

    auto project = std::make_shared<Project>();
    project->name = proj_def.name;
    project->origin = proj_def.origin;

    // Assign files to the project
    for (const auto& file : matched_files) {
        if(tracked_files.contains(file)) {
            project->files.push_back(tracked_files[file]);
        } else {
            auto file_ptr = std::make_shared<File>(file);
            project->files.push_back(file_ptr);
            tracked_files[file] = file_ptr;
        }
    }

    return project;
}

} // namespace

// Workspace --------------------------------------------------------------------------

void Workspace::reload(config::ConfigLog& log) {
    projects_ = ProjectRegistry();

    std::map<Project::Identifier, config::ProjectDefinition> project_defs;
    std::optional<config::ProjectDefinition> default_project;
    CollectProjectsData data {.log = log, .projects=project_defs };
    config::IncludeConfig include_global { .path = global_config_path,    .raw_path_string = "<global>",            .is_optional = false, .is_global = true };
    config::IncludeConfig include_local  { .path = workspace_config_path, .raw_path_string = workspace_config_path, .is_optional = false, .is_global = false };
    
    log.file_context = workspace_config_path;

    // Discover projects from global config recursively
    if(global_config_path.empty()) {
        log.warn("Missing global config: no path specified in settings", "<global>");
    }
    else {
        if(auto global_config = config::ConfigDocument::parse(include_global, log)) {
            for(auto& proj: global_config->projects){
                proj.was_defined_in_global_config = true;
            }
            if(auto& dp = global_config->default_project){
                dp->was_defined_in_global_config = true;
                default_project = dp.value();
            }
            collect_projects_recursive(global_config.value(), data);

            log.file_context = workspace_config_path;
            log.info("Global config: " + global_config_path.string(), "<global>");
        } else {
            log.file_context = workspace_config_path;
            log.error("Invalid global config", "<global>");
        }
    }

    log.file_context = workspace_config_path;
    
    // Discover projects from local config recursively
    if(workspace_config_path.empty()) {
        log.warn("Missing config: did not find artic.json in workspace");
    }
    else if(auto local_config = config::ConfigDocument::parse(include_local, log)){
        collect_projects_recursive(local_config.value(), data);
        if(auto& dp = local_config->default_project){
            default_project = dp.value();
        }
        active_project = local_config->projects.empty() ? std::nullopt : std::make_optional(local_config->projects.front().name);
    }

    struct Project_WithUnresolvedDependencies {
        std::shared_ptr<Project> project;
        std::vector<Project::Identifier> dependencies;
    };
    std::map<Project::Identifier, Project_WithUnresolvedDependencies> projects;

    // convert project definitions to project instances
    for (const auto& [id, def] : project_defs) {
        projects[id] = {
            .project = instantiate_project(def, projects_.tracked_files, log), 
            .dependencies = std::move(def.dependencies)
        };
    }
    
    // resolve dependencies
    for (auto& [id, p] : projects) {
        for (auto& dep_id : p.dependencies) {
            if(projects.contains(dep_id)) {
                auto& dep = projects.at(dep_id).project;
                p.project->dependencies.push_back(dep);
            } 
        }

        projects_.all_projects.push_back(p.project);
    }

    auto log_project_info = [&](const Project& dep, const std::filesystem::path& current_config){
        log.file_context = current_config;
        if(dep.origin != current_config)
            log.info("Declared in config \"" + dep.origin.string() + "\"", dep.name);

        auto files = dep.collect_files();
        std::ostringstream s;
        auto num_own_files = dep.files.size();
        auto dep_files = files.size() - num_own_files;
        s << num_own_files; 
        if(dep_files > 0) s << " + " << dep_files;
        s << " files: " << std::endl;
        for(const auto& file : files) {
            s << "- " << "\"" << std::filesystem::weakly_canonical(file->path).string() << "\" " << std::endl;
        }
        log.info(s.str(), dep.name);

        if(dep.origin == current_config)
            log.info("Declared in this config", dep.name);
    };

    // log dependency resolution
    for (auto& [id, p] : projects) {     
        log.file_context = p.project->origin;
        log_project_info(*p.project, p.project->origin);

        for (auto& dep_id : p.dependencies) {
            if(projects.contains(dep_id)) {
                auto& dep = projects.at(dep_id).project;
                log_project_info(*dep, p.project->origin);
            } else {
                // log.error("Failed to resolve dependency " + dep_id + " for project " + p.project->name, p.project->name);
                log.error("Failed to resolve dependency " + dep_id, dep_id);
            }
        }
    }

    // register default project
    if(default_project){
        auto project = instantiate_project(*default_project, projects_.tracked_files, log);
        log.file_context = project->origin;
        log_project_info(*project, project->origin);

        for (auto& dep_id : default_project->dependencies) {
            if(projects.contains(dep_id)) {
                auto& dep = projects.at(dep_id).project;
                project->dependencies.push_back(dep);            

                log_project_info(*dep, project->origin);
            } else {
                log.file_context = default_project->origin;
                log.error("Failed to resolve dependency " + dep_id + " for default project " + project->name, dep_id);
            }
        }

        projects_.default_project = project;
    } else {
        auto project = std::make_shared<Project>();
        project->name = "<no project>";
        project->files = {};
        project->dependencies = {};
        projects_.default_project = project;
    }

    log.file_context = "";
}

// Config --------------------------------------------------------------------

namespace config {

static std::filesystem::path to_absolute_path(const std::filesystem::path& base_dir, std::string_view path) {
    if(path.starts_with("~")) {
        static const char* home = std::getenv("HOME");
        if(home) 
            return std::filesystem::path(home) / path.substr(1);
        else 
            return path.substr(1); // cannot expand ~
    }
    if(path.starts_with("/"))
        return path;

    return base_dir/path;
}

std::optional<ConfigDocument> ConfigDocument::parse(const IncludeConfig& config, ConfigLog& log) {
    if(config.path.empty()){
        log.error("Config file path is empty", "include");
        return std::nullopt;
    }
    if (!std::filesystem::exists(config.path)) {
        if(!config.is_optional) log.error("Config file does not exist: \"" + config.path.string() + "\"", config.raw_path_string);
        return std::nullopt;
    }
    log.file_context = config.path;
    try {
        nlohmann::json j; 
        std::ifstream is(config.path);
        is >> j;

        ConfigDocument doc;
        doc.path = config.path;
        if (!j.contains("artic-config")) {
            log.error(
                "Missing artic-config header\n"
                "Example: \"artic-config\": \"1.0\""
            );
            return std::nullopt;
        }
        for(auto& [key, value]: j.items()){
            if(key == "artic-config" || 
               key == "default-project" || 
               key == "include" || 
               key == "projects" 
            ) continue;
            log.error("unknown json property \"" + key + "\"", key);
        }

        doc.version = j["artic-config"].get<std::string>();
        if (doc.version != "1.0") {
            log.warn("Unsupported artic-config version (Should be 1.0)", "artic-config");
        }

        auto parse_project = [&](const nlohmann::json& pj) -> std::optional<ProjectDefinition> {
            ProjectDefinition p;
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
            std::filesystem::path root = config.path.parent_path();
            if (folder_ptrn.empty()) {
                p.root_dir = root;
            } else {
                if (folder_ptrn.starts_with("~/")) {
                    const char* home = std::getenv("HOME");
                    if (home) {
                        root = home;
                        folder_ptrn.erase(0, 2);
                    } 
                } else if(folder_ptrn.starts_with("/")) {
                    root = std::filesystem::path("/");
                    folder_ptrn.erase(0, 1);
                }
                auto res = root / folder_ptrn;
                if(std::filesystem::exists(res) && std::filesystem::is_directory(res)) {
                    p.root_dir = std::filesystem::weakly_canonical(res);
                } else {
                    log.error("Project folder does not exist: " + res.string(), pj.value<std::string>("folder", ""));
                    p.root_dir = root;
                }
            }
           
            p.dependencies =  pj.value<std::vector<std::string>>("dependencies", {});
            p.file_patterns = pj.value<std::vector<std::string>>("files", {});
            p.origin = config.path;
            return p;
        };

        if (auto pj = j.find("projects"); pj != j.end()) {
            for (auto& pj : *pj) {
                if(auto pd = parse_project(pj)){
                    doc.projects.push_back(pd.value());
                }
            }
        }
        if (auto dpj = j.find("default-project"); dpj != j.end()) {
            doc.default_project = parse_project(*dpj);
        }
        if (j.contains("include")) {
            bool include_global = false;
            for (auto& incj : j["include"]) {
                auto path = incj.get<std::string>();
                if(!include_global && path == "<global>"){
                    include_global = true;
                    continue;
                }
                IncludeConfig include;
                include.raw_path_string = path;
                if(path.ends_with('?')){
                    path = path.substr(0, path.size()-1);
                    include.is_optional = true;
                } 
                include.path = to_absolute_path(config.path.parent_path(), path);
                include.path = std::filesystem::weakly_canonical(include.path);

                doc.includes.push_back(std::move(include));
            }
            if (config.is_global && include_global) {
                log.warn("including <global> in the global configuration file has no effect", "<global>");
            }
            if(!config.is_global && !include_global) {
                log.warn("add '<global>' to improve readability as global projects are implicitly included", "include");
            }
            
        }
        return doc;
    } catch (const std::exception& e) {
        log.error(std::string("Failed to parse json ") + config.path.string() + ": " + e.what());
        return std::nullopt;
    }
}

} // config

} // namespace artic::ls
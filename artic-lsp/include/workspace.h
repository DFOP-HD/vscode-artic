#ifndef ARTIC_LS_PROJECT_H
#define ARTIC_LS_PROJECT_H

#include "artic/arena.h"
#include "artic/log.h"
#include "lsp/types.h"
#include <vector>
#include <string>
#include <optional>
#include <filesystem>
#include <unordered_map>

namespace artic::ls::workspace {
namespace config { struct ConfigLog; }
namespace fs = std::filesystem;
template <typename T> using Ptr = arena_ptr<T>;
template <typename T> using PtrVector = std::vector<Ptr<T>>;

struct File {
    fs::path path;
    std::optional<std::string> text;
    void read();

    explicit File(fs::path path) 
        : path(std::move(path)), text(std::nullopt) 
    {}
};

struct Project {
    using Identifier = std::string;
    
    // Unique project name
    // May be referenced by other projects
    Identifier name;

    // Path to the project root directory
    // FilePatterns are relative to this path
    fs::path root_dir;

    // Config that defines this project
    fs::path origin; 

    // A pattern which can be used to include or exclude one or more files.
    // Exclude patterns start with '!' character.
    std::vector<std::string> file_patterns;

    // Expansion of file patterns
    std::vector<fs::path> files;

    // Names of other projects that this project depends on
    // Projects will include all files from dependencies
    std::vector<Project::Identifier> dependencies; 

    // -- internal parse info --
    int depth = 100;
};

struct IncludeConfig {
    // path to another artic.json
    fs::path path;

    // -- internal parse info --
    std::string raw_path_string;
    bool is_optional = false;
};

struct ConfigFile {
    std::string version;
    fs::path path;
    std::optional<Project::Identifier> default_project = std::nullopt;
    std::vector<Project::Identifier> projects;
    std::vector<IncludeConfig>       includes;
};


struct ConfigParse {
    bool success;
    ConfigFile config;
    std::vector<Project> projects;
    static ConfigParse parse(const fs::path& path, config::ConfigLog& log); // defined in config.cpp
};

class Workspace {
public:
    Workspace(const fs::path& workspace_root)
        : workspace_root(workspace_root)
    {}

    void reload(config::ConfigLog& log);

    Project* discover_project_for_file(fs::path file, config::ConfigLog& log) {
        if (project_for_file_cache_.contains(file)) return project_for_file_cache_.at(file).get();
        if (auto project = find_config_recursive(file.parent_path(), file, log)) {
            project_for_file_cache_[file] = project;
            return project;
        }
        return nullptr;
    }
    
    void mark_file_dirty(const fs::path& file);
    void set_file_content(const fs::path& file, std::string&& content);
    
    fs::path workspace_root;

private:
    ConfigFile* instantiate_config(const IncludeConfig& origin, config::ConfigLog& log);

    Project* find_config_recursive(fs::path dir, const fs::path& file, config::ConfigLog& log) {
        while(dir != fs::path("")) {
            if(auto config = find_config_in_dir(dir, file, log)) {
                if(auto project = find_project_in_config_using_file(*config, file, log)) {
                    return project;
                }
            }
            dir = dir.parent_path();
        }
        return nullptr;
    }
    
    ConfigFile* find_config_in_dir(fs::path dir, const fs::path& file, config::ConfigLog& log) {
        static constexpr std::string_view file_names[] = {
            ".artic-lsp",
            "artic.json"
        };
        fs::path path;
        for (const auto& name : file_names) {
            auto p = dir / name; 
            if(std::filesystem::exists(p)) {
                path = p;
                break;
            }
        }
        if (tracked_configs_.contains(path)) return tracked_configs_.at(path).get();
        if (auto config = instantiate_config(IncludeConfig{.path = path}, log)) {
            tracked_configs_[path] = std::move(config);
            return config;
        }
    
        return nullptr;
    }
    
    Project* find_project_in_config_using_file(const ConfigFile& config, const fs::path& file, config::ConfigLog& log){
        for (const auto& project_id : config.projects) {
            if(!tracked_projects_.contains(project_id)) continue; 
            auto& project = tracked_projects_.at(project_id);
            auto files = files_for_project(*project);
            for (const auto& f : files) {
                // Project uses file
                if(f->path == file) {
                    return project.get();
                }
            }
        }
        // No project found, use default project if one is defined in this config
        if(config.default_project && tracked_projects_.contains(config.default_project.value())) {
            return tracked_projects_.at(config.default_project.value()).get();
        }
        return nullptr;
    }

    std::vector<File*> files_for_project(const Project& project) {
        std::vector<File*> res;
        for (const auto& path : project.files) {
            if (!tracked_files_.contains(path)) {
                tracked_files_[path] = arena_.make_ptr<File>(path);
            }
            res.push_back(tracked_files_.at(path).get());
        }
        return res;
    }

    std::vector<File*> files_for_project(const Project::Identifier& project_id) {
        if(tracked_projects_.contains(project_id)) {
            auto& project = tracked_projects_.at(project_id);
            return files_for_project(*project);
        }
        log::info("Querying files for unknown project '{}'", project_id);
        return {};
    }
    
    std::unordered_map<fs::path, Ptr<Project>> project_for_file_cache_;

    std::unordered_map<Project::Identifier, Ptr<Project>> tracked_projects_;
    std::unordered_map<fs::path, Ptr<File>> tracked_files_;
    std::unordered_map<fs::path, Ptr<ConfigFile>> tracked_configs_;
    Arena arena_;
};

} // namespace artic::ls

#endif // ARTIC_LS_PROJECT_H
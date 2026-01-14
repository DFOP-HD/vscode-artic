#ifndef ARTIC_LS_PROJECT_H
#define ARTIC_LS_PROJECT_H

#include "artic/arena.h"
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

    std::vector<File*> collect_files();
    bool uses_file(const fs::path& file) const;

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
        if(auto project = find_config_recursive(file.parent_path(), fs::path(file))) {
            return project;
        }
        return nullptr;

    }

    ConfigFile* instantiate_config(const IncludeConfig& origin, config::ConfigLog& log);
    // struct ProjectQuery {
    //     Project* project = nullptr;
    //     std::optional<config::ConfigLog> newly_loaded_configs_log = std::nullopt;
    // };
    Project* project_for_file(const fs::path& file) {
        auto project = find_config_recursive(file.parent_path(), fs::path(file));
        if(auto project = find_config_recursive(file.parent_path(), fs::path(file))) {
            project_for_file_cache_[file] = project;
            return project;
        }
        return nullptr;
    }

    Project* find_config_recursive(fs::path dir, const fs::path& file) {
        if(dir == fs::path("")) return nullptr;
        if(auto config = find_config_in_dir(dir, file)) {
            if(auto project = find_project_in_config(*config, file)) {
                return project;
            }
        }
        find_config_recursive(dir.parent_path(), file);
        return nullptr;
    }

    ConfigFile* find_config_in_dir(fs::path dir, const fs::path& file) {
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
        if(tracked_configs_.contains(path)) return tracked_configs_.at(path).get();
        if(auto config = instantiate_config(path)) {
            tracked_configs_[path] = std::move(config);
            return config.get();
        }

        return nullptr;
    }

    Project* find_project_in_config(const ConfigFile& config, const fs::path& file){
        for (const auto& project : config.projects) {
            for (const auto& f : project->files) {
                if (f->path == file) {
                    return project.get();
                }
            }
        }
        return nullptr;
    }

    
    void mark_file_dirty(const fs::path& file);
    void set_file_content(const fs::path& file, std::string&& content);
    
    fs::path workspace_root;
    // fs::path workspace_config_path;
    // fs::path global_config_path;
    File* track_file(const fs::path& file) {
        if(!tracked_files_.contains(file)) {
            auto f = arena_.make_ptr<File>(file);
            tracked_files_[file] = std::move(f);    
        }
        return tracked_files_.at(file).get();
    }

    Project* track_project(const Project::Identifier& id) {
        if(tracked_projects_.contains(id)) {
            return nullptr;
        }
        return tracked_projects_.at(id).get();
    }
private:
    
    std::unordered_map<fs::path, Ptr<Project>> project_for_file_cache_;

    std::unordered_map<Project::Identifier, Ptr<Project>> tracked_projects_;
    std::unordered_map<fs::path, Ptr<File>> tracked_files_;
    std::unordered_map<fs::path, Ptr<ConfigFile>> tracked_configs_;
    Arena arena_;
};

} // namespace artic::ls

#endif // ARTIC_LS_PROJECT_H
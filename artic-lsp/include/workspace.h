#ifndef ARTIC_LS_PROJECT_H
#define ARTIC_LS_PROJECT_H

#include "artic/arena.h"
#include <vector>
#include <string>
#include <optional>
#include <filesystem>
#include <unordered_map>

namespace artic::ls::workspace {

namespace fs = std::filesystem;
template <typename T> using Ptr = arena_ptr<T>;
template <typename T> using PtrVector = std::vector<Ptr<T>>;

namespace config { struct ConfigLog; struct ConfigDocument;}
struct Project;
struct ConfigFile;


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
    Identifier name;
    fs::path origin; // config that declared this project
    fs::path root_dir;

    std::vector<File*> files;
    std::vector<Project::Identifier> dependencies; 

    std::vector<File*> collect_files();
    bool uses_file(const fs::path& file) const;
};

struct ConfigFile {
    fs::path path;
    Project* default_project = nullptr;
    std::vector<Project*> projects;
};

class Workspace {
public:
    Workspace(const fs::path& workspace_root)
        : workspace_root(workspace_root)
    {}

    void reload(config::ConfigLog& log);

    ConfigFile* instantiate_config(const fs::path& path);
    
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
        if(!tracked_projects_.contains(id)) {
            auto p = arena_.make_ptr<Project>(std::move(project));
            tracked_projects_[id] = std::move(p);    
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
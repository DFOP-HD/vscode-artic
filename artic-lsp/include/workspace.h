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


class Workspace {
public:
    Workspace()
        : arena_(std::make_unique<Arena>())
    {}

    void reload(config::ConfigLog& log);

    void mark_file_dirty(const fs::path& file) {
        if(auto f = tracked_file(file)) f->text = std::nullopt;
    }
    
    void set_file_content(const std::filesystem::path& file, std::string&& content){
        if(auto f = tracked_file(file)) f->text = std::move(content);
    }

    // Collect all files that belong to the project containing the given file
    // If no project is found, return just the given file
    // The project config might not be known yet, therefore we may need to look for it and initialize it, hence the log output
    std::vector<File*> collect_project_files(fs::path file, config::ConfigLog& log) {
        if (auto project = discover_project_for_file(file, log)) {
            auto files = files_for_project(*project);
            bool is_default_project = !uses_file(*project, file);
            if (is_default_project) {
                files.push_back(tracked_file(file));
            }
            log::info("Found file '{}' in project '{}' with {} total files {}", file, project->name, files.size(), is_default_project ? " (default project)" : "");
            return files;
        }
        return {tracked_file(file)};
    }
private:
    ConfigFile* instantiate_config(const IncludeConfig& origin, config::ConfigLog& log);

    Project* discover_project_for_file(fs::path file, config::ConfigLog& log) {
        file = fs::weakly_canonical(file);
        if (project_for_file_cache_.contains(file)) {
            return project_for_file_cache_.at(file).get();
        }
        if (auto project = find_config_recursive(file.parent_path(), file, log)) {
            project_for_file_cache_[file] = project;
            return project;
        }
        return nullptr;
    }

    Project* find_config_recursive(fs::path dir, const fs::path& file, config::ConfigLog& log) {
        while(dir != fs::path("/")) {
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
        for (auto file_name : file_names) {
            auto path = dir / file_name;
            if(!fs::exists(path)) continue;
            if (configs_.contains(path)) 
                return configs_.at(path).get();

            IncludeConfig origin{ .path = path };
            if (auto config = instantiate_config(origin, log)) {
                configs_[path] = std::move(config);
                return config;
            }
        }
        return nullptr;
    }
    
    Project* find_project_in_config_using_file(const ConfigFile& config, const fs::path& file, config::ConfigLog& log){
        for (const auto& project_id : config.projects) {
            if(auto project = try_get_project(project_id); project && uses_file(*project, file)) {
                return project;
            }
        }
        if(config.default_project) {
            return try_get_project(*config.default_project);
        }
        return nullptr;
    }

    bool uses_file(const Project& project, const fs::path& file) const {
        for (const auto& f : project.files) {
            if (f == file) return true;
        }
        for (const auto& dep_id : project.dependencies) {
            if(auto dep = try_get_project(dep_id)) {
                if(uses_file(*dep, file)) return true;
            }
        }
        return false;
    }

    std::vector<File*> files_for_project(const Project& project) {
        std::vector<File*> res;
        for (const auto& f : project.files) {
            res.push_back(tracked_file(f));
        }
        for (const auto& dep_id : project.dependencies) {
            if(auto dep = try_get_project(dep_id)) {
                auto dep_files = files_for_project(*dep);
                res.insert(res.end(), dep_files.begin(), dep_files.end());
            }
        }
        return res;
    }

    File* tracked_file(fs::path file) {
        file = fs::weakly_canonical(file);
        if (!files_.contains(file)) {
            files_.insert({file, arena_->make_ptr<File>(file)});
        }
        return files_.at(file).get();
    }

    Project* try_get_project(const Project::Identifier& project_id) const {
        return projects_.contains(project_id) ? projects_.at(project_id).get() : nullptr;
    }
    
    std::unordered_map<fs::path, Ptr<Project>> project_for_file_cache_;

    std::unordered_map<Project::Identifier, Ptr<Project>> projects_;
    std::unordered_map<fs::path, Ptr<File>> files_;
    std::unordered_map<fs::path, Ptr<ConfigFile>> configs_;
    std::unique_ptr<Arena> arena_;
};

} // namespace artic::ls

#endif // ARTIC_LS_PROJECT_H
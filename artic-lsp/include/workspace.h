#ifndef ARTIC_LS_PROJECT_H
#define ARTIC_LS_PROJECT_H

#include "artic/arena.h"
#include "artic/log.h"
#include "paths.h"
#include "lsp/types.h"
#include <unordered_set>
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

struct ConfigPath {
    // path to another artic.json
    fs::path path;

    // -- internal parse info --
    std::string raw_path_string;
    bool is_optional = false;

    // Derived from another config (e.g. a project listed in a .sln) rather than written
    // by the user. Problems with it are not the user's doing and are not reported.
    bool is_implicit = false;
};

struct ConfigFile {
    std::string version;
    fs::path path;
    std::optional<Project::Identifier> default_project = std::nullopt;
    std::vector<Project::Identifier> projects;
    std::vector<ConfigPath>       includes;
};


class Workspace {
public:
    Workspace()
        : arena_(std::make_unique<Arena>())
    {}

    void reload();

    void mark_file_dirty(const fs::path& file) {
        if(auto f = tracked_file(file)) f->text = std::nullopt;
    }
    
    void set_file_content(const fs::path& file, std::string&& content){
        if(auto f = tracked_file(file)) f->text = std::move(content);
    }

    // Collect all files that belong to the project containing the given file
    // If no project is found, return just the given file
    // The project config might not be known yet, therefore we may need to look for it and initialize it, hence the log output
    std::vector<File*> collect_project_files(const fs::path& file, config::ConfigLog& log);

    // return true if file was known before
    bool on_config_changed(fs::path config_path, config::ConfigLog& log);

private:
    ConfigFile* instantiate_config(const ConfigPath& origin, config::ConfigLog& log);
    ConfigFile* instantiate_config_json(const ConfigPath& origin, config::ConfigLog& log);
    ConfigFile* instantiate_config_vcxproj(const ConfigPath& origin, config::ConfigLog& log);
    ConfigFile* instantiate_config_sln(const ConfigPath& origin, config::ConfigLog& log);
    ConfigFile* instantiate_config_ninja(const ConfigPath& origin, config::ConfigLog& log);

    Project* discover_project_for_file(const fs::path& file, config::ConfigLog& log);
    Project* find_config_recursive(fs::path dir, const fs::path& file, config::ConfigLog& log);
    ConfigFile* find_config_in_dir(const fs::path& dir, config::ConfigLog& log);
    Project* find_project_in_config_using_file(const ConfigFile& config, const fs::path& file, config::ConfigLog& log);
    bool uses_file(const Project& project, const fs::path& file) const;
    std::unordered_set<File*> files_for_project(const Project& project);
    File* tracked_file(const fs::path& file);
    Project* try_get_project(const Project::Identifier& project_id) const;

    // Non-owning: the projects themselves live in `projects_`.
    std::unordered_map<fs::path, Project*> project_for_file_cache_;

    std::unordered_map<Project::Identifier, Ptr<Project>> projects_;
    std::unordered_map<fs::path, Ptr<File>> files_;
    std::unordered_map<fs::path, Ptr<ConfigFile>> configs_;
    std::unique_ptr<Arena> arena_;
};

} // namespace artic::ls

#endif // ARTIC_LS_PROJECT_H
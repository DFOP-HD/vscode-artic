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
    // Whether `text` is the editor's buffer rather than what `read()` found on disk.
    // Only an editor buffer may hold unsaved edits, so only it has to be discarded when
    // the document is closed.
    bool text_from_editor = false;
    void read();

    explicit File(fs::path path) 
        : path(std::move(path)), text(std::nullopt) 
    {}
};

struct Project {
    using Identifier = std::string;

    // What one `files` pattern contributed. Kept so the config document can be annotated
    // with it; patterns are evaluated once and the result is cached, so it cannot be
    // recomputed when the editor asks.
    struct PatternMatch {
        std::string pattern;
        // Files the pattern itself matched, and how many of them the project did not
        // already have. An exclude pattern reports how many it removed.
        size_t matched = 0;
        size_t changed = 0;
        bool excludes = false;
        // RFC 6901 pointer to the pattern in `origin`, so the hint lands on the right one
        // when two projects share a pattern. Empty for a project taken from a build file.
        std::string pointer;
    };

    // Unique project name
    // May be referenced by other projects
    Identifier name;

    // RFC 6901 pointer to the name in `origin`. Empty for a project taken from a build file.
    std::string name_pointer;

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

    // What each entry of `file_patterns` contributed, in the order they were evaluated.
    std::vector<PatternMatch> pattern_matches;

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

// Which project a file is compiled as part of, and where that answer came from.
// Without this the fallback to a single-file compile is completely silent: every
// cross-file reference becomes "unknown identifier" and nothing says why.
struct FileProject {
    enum class Provenance {
        // No configuration was found above the file, so it is compiled on its own.
        SingleFile,
        // A project that lists this file.
        Config,
        // A configuration's `default-project`: the file is listed nowhere, but it is
        // compiled alongside whatever that project depends on.
        DefaultProject,
        // A build file found by scanning the workspace, with no configuration involved.
        DetectedBuildFile,
    };

    Provenance provenance = Provenance::SingleFile;
    Project::Identifier name;
    // Configuration file that declared the project.
    fs::path origin;
    // How many files are compiled together, this one included.
    size_t file_count = 1;
};


class Workspace {
public:
    Workspace()
        : arena_(std::make_unique<Arena>())
    {}

    void reload();

    // Folders the editor has open. They bound the search for build files: without one,
    // there is nothing to scan and a file with no configuration stays a single-file
    // compile. Replacing them invalidates whatever the previous roots detected.
    void set_workspace_roots(std::vector<fs::path> roots);

    // Discards the editor's in-memory buffer, so the file is read from disk again.
    // Returns whether there actually was an editor buffer to discard: content that came
    // from disk is not stale and dropping it would force a pointless recompile.
    bool discard_editor_buffer(const fs::path& file) {
        auto f = tracked_file(file);
        if(!f || !f->text_from_editor) return false;
        f->text = std::nullopt;
        f->text_from_editor = false;
        return true;
    }
    
    void set_file_content(const fs::path& file, std::string&& content){
        if(auto f = tracked_file(file)) {
            f->text = std::move(content);
            f->text_from_editor = true;
        }
    }

    // Collect all files that belong to the project containing the given file
    // If no project is found, return just the given file
    // The project config might not be known yet, therefore we may need to look for it and initialize it, hence the log output
    std::vector<File*> collect_project_files(const fs::path& file, config::ConfigLog& log);

    // The same answer `collect_project_files` acts on, in a form that can be shown to the
    // user. Goes through the same discovery, so it reports what a compile would really do.
    FileProject project_of_file(const fs::path& file, config::ConfigLog& log);

    // return true if file was known before
    bool on_config_changed(fs::path config_path, config::ConfigLog& log);

    // Projects a config file declares, in declaration order. Used to annotate the config
    // document itself; it never triggers parsing, so an unopened config yields nothing.
    std::vector<const Project*> projects_of_config(const fs::path& config) const;

    // Number of files a project compiles, including those it inherits from dependencies.
    size_t total_file_count(const Project& project) const;

    // Every project instantiated so far, in no particular order. Nothing is parsed here:
    // a config that has not been reached yet simply has no projects.
    std::vector<const Project*> all_projects() const;

    // Content of a file: the editor's buffer when there is one, what is on disk otherwise.
    // Null when the file cannot be read.
    const std::string* file_text(const fs::path& file);

private:
    ConfigFile* instantiate_config(const ConfigPath& origin, config::ConfigLog& log);
    ConfigFile* instantiate_config_json(const ConfigPath& origin, config::ConfigLog& log);
    ConfigFile* instantiate_config_vcxproj(const ConfigPath& origin, config::ConfigLog& log);
    ConfigFile* instantiate_config_sln(const ConfigPath& origin, config::ConfigLog& log);
    ConfigFile* instantiate_config_ninja(const ConfigPath& origin, config::ConfigLog& log);

    Project* discover_project_for_file(const fs::path& file, config::ConfigLog& log);
    Project* find_config_recursive(fs::path dir, const fs::path& file, config::ConfigLog& log);
    Project* find_detected_project(const fs::path& file, config::ConfigLog& log);
    ConfigFile* detected_config_for_root(const fs::path& root, config::ConfigLog& log);
    ConfigFile* find_config_in_dir(const fs::path& dir, config::ConfigLog& log);
    Project* find_project_in_config_using_file(const ConfigFile& config, const fs::path& file, config::ConfigLog& log);
    bool uses_file(const Project& project, const fs::path& file) const;
    std::unordered_set<File*> files_for_project(const Project& project);
    File* tracked_file(const fs::path& file);
    Project* try_get_project(const Project::Identifier& project_id) const;

    // Non-owning: the projects themselves live in `projects_`.
    std::unordered_map<fs::path, Project*> project_for_file_cache_;

    std::vector<fs::path> workspace_roots_;
    // Synthetic config per workspace root, holding the build files the scan found. Null
    // when the scan found nothing: the miss has to be cached too, or every file without a
    // configuration rescans the whole tree. Owned by `configs_`.
    std::unordered_map<fs::path, ConfigFile*> detected_config_for_root_;
    // Projects that were reached through such a config, so their provenance can be
    // reported as detected rather than configured.
    std::unordered_set<const Project*> detected_projects_;

    std::unordered_map<Project::Identifier, Ptr<Project>> projects_;
    std::unordered_map<fs::path, Ptr<File>> files_;
    std::unordered_map<fs::path, Ptr<ConfigFile>> configs_;
    std::unique_ptr<Arena> arena_;
};

} // namespace artic::ls

#endif // ARTIC_LS_PROJECT_H
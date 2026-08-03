#include "workspace.h"

#include "artic/arena.h"
#include "config.h"
#include "paths.h"

#include "artic/log.h"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <vector>
#include <unordered_set>


namespace artic::ls::workspace {

// File ----------------------------------------------------------------------

void File::read() {
    if (!text)
        text = paths::read_file(path);
    if (!text) log::error("Could not read file {}", path);
}

// Workspace --------------------------------------------------------------------

void Workspace::reload() {
    projects_.clear();
    files_.clear();
    configs_.clear();
    arena_ = std::make_unique<Arena>();
    project_for_file_cache_.clear();
    detected_config_for_root_.clear();
    detected_projects_.clear();
}

void Workspace::set_workspace_roots(std::vector<fs::path> roots) {
    for (auto& root : roots) root = paths::canonical_path(root);
    if (roots == workspace_roots_) return;
    workspace_roots_ = std::move(roots);
    reload();
}

std::vector<File*> Workspace::collect_project_files(const fs::path& file, config::ConfigLog& log) {
    if (auto project = discover_project_for_file(file, log)) {
        auto files = files_for_project(*project);
        bool is_default_project = !uses_file(*project, file);
        if (is_default_project) {
            files.insert(tracked_file(file));
        }
        log::info("Found file '{}' in project '{}' with {} total files {}", file.generic_string(), project->name, files.size(), is_default_project ? " (default project)" : "");
        return std::vector<File*>(files.begin(), files.end());
    }
    return {tracked_file(file)};
}

FileProject Workspace::project_of_file(const fs::path& file, config::ConfigLog& log) {
    auto project = discover_project_for_file(file, log);
    if (!project) return {};

    FileProject info;
    info.name = project->name;
    info.origin = project->origin;
    if (uses_file(*project, file)) {
        info.provenance = detected_projects_.contains(project)
            ? FileProject::Provenance::DetectedBuildFile
            : FileProject::Provenance::Config;
        info.file_count = total_file_count(*project);
    } else {
        // The default project does not list the file, so a compile adds it to the project's
        // own files, exactly as `collect_project_files` does.
        info.provenance = FileProject::Provenance::DefaultProject;
        info.file_count = total_file_count(*project) + 1;
    }
    return info;
}

bool Workspace::on_config_changed(fs::path config_path, config::ConfigLog& log) {
    config_path = paths::canonical_path(config_path);
    log::info("Configuration file changed: {}", config_path.generic_string());
    ConfigPath p {
        .path = config_path,
        .raw_path_string = config_path.generic_string(),
        .is_optional = false
    };
    bool known = configs_.contains(config_path);
    if(known) reload();
    instantiate_config(p, log);
    return known;
}

Project* Workspace::discover_project_for_file(const fs::path& file, config::ConfigLog& log) {
    auto key = paths::canonical_path(file);
    if (auto it = project_for_file_cache_.find(key); it != project_for_file_cache_.end())
        return it->second;
    auto project = find_config_recursive(key.parent_path(), key, log);
    // Only once no configuration claims the file: a config the user wrote always wins over
    // whatever a build file in the same tree happens to say.
    if (!project) project = find_detected_project(key, log);
    if (project) {
        project_for_file_cache_[key] = project;
        return project;
    }
    return nullptr;
}

Project* Workspace::find_config_recursive(fs::path dir, const fs::path& file, config::ConfigLog& log) {
    log::info("- Searching for config for file {}", file.generic_string());
    do {
        log::info("- Looking at dir {}", dir.generic_string());
        if(auto config = find_config_in_dir(dir, log)) {
            if(auto project = find_project_in_config_using_file(*config, file, log)) {
                log::info("- Found matching project '{}' in config {}", project->name, project->origin.generic_string());
                return project;
            }
            log::info("- Found config '{}', but does not contain a matching project, continuing...", config->path.generic_string());
        }
        dir = dir.parent_path();
    } while(dir.root_path() != dir);
    log::info("- Did not find matching config for file {}", file.generic_string());
    return nullptr;
}

namespace {
// Whether `file` is `root` or below it.
bool is_within(const fs::path& root, const fs::path& file) {
    auto a = paths::lookup_key(root).generic_string();
    auto b = paths::lookup_key(file).generic_string();
    return b == a || (b.starts_with(a) && b.size() > a.size() && b[a.size()] == '/');
}
} // anonymous namespace

Project* Workspace::find_detected_project(const fs::path& file, config::ConfigLog& log) {
    for (const auto& root : workspace_roots_) {
        if (!is_within(root, file)) continue;
        auto config = detected_config_for_root(root, log);
        if (!config) continue;
        if (auto project = find_project_in_config_using_file(*config, file, log)) {
            log::info("- Found matching project '{}' in detected build file {}",
                      project->name, project->origin.generic_string());
            detected_projects_.insert(project);
            return project;
        }
    }
    return nullptr;
}

ConfigFile* Workspace::detected_config_for_root(const fs::path& root, config::ConfigLog& log) {
    if (auto it = detected_config_for_root_.find(root); it != detected_config_for_root_.end())
        return it->second;
    // Cache the miss before scanning: this runs for every file that has no configuration
    // above it, and a workspace with no build files must not be walked more than once.
    detected_config_for_root_[root] = nullptr;

    auto build_files = config::detect_build_files(root, log);
    if (build_files.empty()) return nullptr;

    // Named after a file that does not exist, so nothing can look it up as a real config
    // and no diagnostic can ever be attributed to it.
    ConfigFile cfg{ .path = root / "<detected build files>" };
    for (auto& build_file : build_files) {
        cfg.includes.push_back(ConfigPath{
            .path = build_file,
            .raw_path_string = build_file.generic_string(),
            // Nothing here was asked for by name, so a build file that turns out not to
            // build artic after all is not a problem the user can act on.
            .is_optional = true,
            .is_implicit = true,
        });
    }

    auto path = cfg.path;
    configs_[path] = arena_->make_ptr<ConfigFile>(std::move(cfg));
    auto* config = configs_.at(path).get();
    detected_config_for_root_[root] = config;
    return config;
}

ConfigFile* Workspace::find_config_in_dir(const fs::path& dir, config::ConfigLog& log) {
    static constexpr std::string_view file_names[] = {
        ".artic-lsp",
        "artic.json"
    };
    for (auto file_name : file_names) {
        // Must match how instantiate_config() keys the cache, or every lookup misses.
        auto path = paths::canonical_path(dir / file_name);
        if(!fs::exists(path)) continue;
        if (auto it = configs_.find(path); it != configs_.end())
            return it->second.get();

        ConfigPath origin{ .path = path };
        if (auto config = instantiate_config(origin, log)) {
            return config;
        }
    }
    return nullptr;
}

Project* Workspace::find_project_in_config_using_file(const ConfigFile& config, const fs::path& file, config::ConfigLog& log) {
    for (const auto& project_id : config.projects) {
        if(auto project = try_get_project(project_id); project && uses_file(*project, file)) {
            return project;
        }
    }
    for (const auto& include : config.includes) {
        if(auto cfg = instantiate_config(include, log)) {
            if(auto project = find_project_in_config_using_file(*cfg, file, log)) {
                return project;
            }
        }
    }
    if(config.default_project) {
        return try_get_project(*config.default_project);
    }
    return nullptr;
}

bool Workspace::uses_file(const Project& project, const fs::path& file) const {
    auto key = paths::lookup_key(file);
    for (const auto& f : project.files) {
        if (paths::lookup_key(f) == key) return true;
    }
    for (const auto& dep_id : project.dependencies) {
        if(auto dep = try_get_project(dep_id)) {
            if(uses_file(*dep, file)) return true;
        }
    }
    return false;
}

std::vector<const Project*> Workspace::projects_of_config(const fs::path& config) const {
    // `configs_` is keyed by the canonical path, the same one `instantiate_config` inserts.
    auto it = configs_.find(paths::canonical_path(config));
    if (it == configs_.end()) return {};

    std::vector<const Project*> result;
    for (const auto& name : it->second->projects) {
        if (auto project = try_get_project(name)) result.push_back(project);
    }
    return result;
}

size_t Workspace::total_file_count(const Project& project) const {
    // Dependencies form a DAG at best, and a cycle at worst until the registry rejects it,
    // so the walk has to remember where it has been.
    std::unordered_set<fs::path> files;
    std::unordered_set<const Project*> seen;
    std::function<void(const Project&)> walk = [&](const Project& p) {
        if (!seen.insert(&p).second) return;
        files.insert(p.files.begin(), p.files.end());
        for (const auto& dep_id : p.dependencies)
            if (auto dep = try_get_project(dep_id)) walk(*dep);
    };
    walk(project);
    return files.size();
}

std::vector<const Project*> Workspace::all_projects() const {
    std::vector<const Project*> result;
    result.reserve(projects_.size());
    for (const auto& [name, project] : projects_) {
        (void)name;
        result.push_back(project.get());
    }
    return result;
}

const std::string* Workspace::file_text(const fs::path& file) {
    auto f = tracked_file(file);
    f->read();
    return f->text ? &*f->text : nullptr;
}

std::unordered_set<File*> Workspace::files_for_project(const Project& project) {
    std::unordered_set<File*> res;
    for (const auto& f : project.files) {
        res.insert(tracked_file(f));
    }
    for (const auto& dep_id : project.dependencies) {
        if(auto dep = try_get_project(dep_id)) {
            auto dep_files = files_for_project(*dep);
            res.insert(dep_files.begin(), dep_files.end());
        }
    }
    return res;
}

File* Workspace::tracked_file(const fs::path& file) {
    auto key = paths::lookup_key(file);
    if (auto it = files_.find(key); it != files_.end())
        return it->second.get();
    return files_.insert({key, arena_->make_ptr<File>(paths::canonical_path(file))}).first->second.get();
}

Project* Workspace::try_get_project(const Project::Identifier& project_id) const {
    return projects_.contains(project_id) ? projects_.at(project_id).get() : nullptr;
}

ConfigFile* Workspace::instantiate_config(const ConfigPath& origin, config::ConfigLog& log) {
    auto o = origin;
    o.path = paths::canonical_path(o.path);
    if(configs_.contains(o.path)) {
        return configs_.at(o.path).get();
    }
    // A missing file is reported where the include was written, by whoever knows whether
    // it was optional. Parsing it here would blame the file that does not exist.
    if(!fs::exists(o.path)) return nullptr;

    // `.artic-lsp` is a dotfile and therefore has no extension; match the filename first.
    if (o.path.filename() == ".artic-lsp") return instantiate_config_json(o, log);
    if (o.path.has_extension()) {
        if(o.path.extension() == ".json") return instantiate_config_json(o, log);
        if(o.path.extension() == ".vcxproj") return instantiate_config_vcxproj(o, log);
        if(o.path.extension() == ".sln") return instantiate_config_sln(o, log);
        if(o.path.extension() == ".ninja") return instantiate_config_ninja(o, log);
    }

    // Attribute to the including config if there is one, otherwise to the file itself,
    // so the message can never end up without a home and get dropped.
    auto ctx = log.scoped_file(log.file_context.empty() ? o.path : log.file_context);
    log.error("Unsupported config file type: " + o.path.filename().generic_string(),
              origin.raw_path_string);
    return nullptr;
}

ConfigFile* Workspace::instantiate_config_vcxproj(const ConfigPath& origin, config::ConfigLog& log) {
    auto ctx = log.scoped_file(origin.path);
    auto project = config::parse_vcxproj(origin, log);

    ConfigFile cfg{ .path = origin.path };
    if(!project) {
        // A solution pulls in every project it lists, most of which have nothing to do
        // with artic, so this is only a problem when the user asked for this file by name.
        if(!origin.is_implicit) {
            log.error("Failed to parse vcxproj file");
            return nullptr;
        }
    } else if(projects_.contains(project->name)) {
        if(!origin.is_implicit)
            log.warn("ignoring duplicate definition of " + project->name + " in " + project->origin.generic_string(), project->name);
    } else {
        projects_[project->name] = arena_->make_ptr<Project>(*project); // copy
        cfg.projects.push_back(project->name);
    }

    // Cached even when nothing was found, so a large solution parses each project once.
    configs_[origin.path] = arena_->make_ptr<ConfigFile>(std::move(cfg));
    return configs_.at(origin.path).get();
}

ConfigFile* Workspace::instantiate_config_sln(const ConfigPath& origin, config::ConfigLog& log) {
    auto ctx = log.scoped_file(origin.path);

    // The projects are only turned into configs when something actually looks for a
    // file in them: a CMake-generated solution can list hundreds of them.
    ConfigFile cfg{ .path = origin.path };
    for (auto& project : config::parse_sln(origin, log)) {
        project.is_implicit = true;
        cfg.includes.push_back(std::move(project));
    }
    configs_[origin.path] = arena_->make_ptr<ConfigFile>(std::move(cfg));
    return configs_.at(origin.path).get();
}

ConfigFile* Workspace::instantiate_config_ninja(const ConfigPath& origin, config::ConfigLog& log) {
    auto ctx = log.scoped_file(origin.path);

    ConfigFile cfg{ .path = origin.path };
    for (auto& project : config::parse_ninja(origin, log)) {
        auto name = project.name;
        if(projects_.contains(name)) {
            if(!origin.is_implicit)
                log.warn("ignoring duplicate definition of " + name + " in " + project.origin.generic_string(), name);
            continue;
        }
        cfg.projects.push_back(name);
        projects_[name] = arena_->make_ptr<Project>(std::move(project));
    }
    if(cfg.projects.empty() && !origin.is_implicit)
        log.warn("No artic build commands found in " + origin.path.filename().generic_string());

    configs_[origin.path] = arena_->make_ptr<ConfigFile>(std::move(cfg));
    return configs_.at(origin.path).get();
}

ConfigFile* Workspace::instantiate_config_json(const ConfigPath& origin, config::ConfigLog& log) {
    log::info("Instantiating config: {}", origin.path.generic_string());
    config::ConfigParser parser(origin, log);
    bool success = parser.parse();
    if (!success) return nullptr;

    auto ctx = log.scoped_file(origin.path);
    // track config
    configs_[origin.path] = arena_->make_ptr<ConfigFile>(parser.config);
    // track projects
    for (const auto& project : parser.projects){
        if(projects_.contains(project.name)) {
            log.warn("ignoring duplicate definition of " + project.name + " in " + project.origin.generic_string(), project.name);
            continue;
        }
        projects_[project.name] = arena_->make_ptr<Project>(project); // copy
    }
    
    // recurse included configs
    for (const auto& include : parser.config.includes) {
        if(!fs::exists(include.path)) {
            if(!include.is_optional)
                log.error("Config file does not exist: \"" + include.path.generic_string() + "\"", include.raw_path_string);
            continue;
        }
        // Whatever goes wrong below reports itself; a generic "failed to include" on top
        // of that would just duplicate the message.
        instantiate_config(include, log);
    }

    // fix circular project dependencies
    std::unordered_set<std::string> visited;
    std::unordered_set<std::string> rec_stack;

    // A single DFS over all roots. The previous version restarted from every
    // project-dependency edge with the two arguments swapped, and erased the offending
    // entry with std::remove while a range-for over that same vector was still live.
    std::function<void(const std::string&)> detect_cycle = [&](const std::string& name) {
        auto it = projects_.find(name);
        if (it == projects_.end()) return; // dependency doesn't exist, handled elsewhere
        if (!visited.insert(name).second) return;

        rec_stack.insert(name);
        auto& deps = it->second->dependencies;
        for (auto dep = deps.begin(); dep != deps.end();) {
            if (rec_stack.contains(*dep)) {
                auto cycle_ctx = log.scoped_file(it->second->origin);
                log.error("Circular dependency detected: " + name + " -> " + *dep +
                          " creates a cycle. Removing this dependency.", *dep);
                log::info("Circular dependency detected in config '{}': {} -> {}",
                          it->second->origin.generic_string(), name, *dep);
                dep = deps.erase(dep);
                continue;
            }
            detect_cycle(*dep);
            ++dep;
        }
        rec_stack.erase(name);
    };

    for (const auto& project : parser.projects) detect_cycle(project.name);

    return configs_.at(origin.path).get();
}

} // namespace artic::ls::workspace
#include "workspace.h"

#include "artic/arena.h"
#include "config.h"

#include "artic/log.h"

#include <algorithm>
#include <fstream>
#include <filesystem>
#include <fnmatch.h>
#include <iostream>
#include <memory>
#include <optional>
#include <vector>
#include <unordered_set>


namespace artic::ls::workspace {

// File ----------------------------------------------------------------------

static std::optional<std::string> read_file(const std::string& file) {
    std::ifstream is(file);
    if (!is)
        return std::nullopt;
    // Try/catch needed in case file is a directory (throws exception upon read)
    try {
        return std::make_optional(std::string(
            std::istreambuf_iterator<char>(is),
            std::istreambuf_iterator<char>()
        ));
    } catch (...) {
        return std::nullopt;
    }
}

void File::read() {
    if (!text) // could do that to avoid re-reading, but force re-reading does not take too long so do it just to be sure
        text = read_file(path);
    if (!text) log::error("Could not read file {}", path);
}

// Project --------------------------------------------------------------------

// std::vector<const File*> Project::collect_files() const {
//     std::unordered_set<const File*> result;
//     for (const auto& file : files) {
//         result.insert(file.get());
//     }
//     for (const auto& dependency : dependencies){
//         auto dep_files = dependency->collect_files();
//         result.insert(dep_files.begin(), dep_files.end());
//     }
//     std::vector res(result.begin(), result.end());
//     return res;
// }

// Workspace --------------------------------------------------------------------



void Workspace::mark_file_dirty(const std::filesystem::path& file){
    tracked_files_.erase(file);
}
void Workspace::set_file_content(const std::filesystem::path& file, std::string&& content){
    if (tracked_files_.contains(file)){
        tracked_files_.at(file)->text = std::move(content);
    } else  {
        // TODO?
    }
}

void Workspace::reload(config::ConfigLog& log) {
    tracked_projects_.clear();
    tracked_files_.clear();
    tracked_configs_.clear();
    arena_ = Arena();
    project_for_file_cache_.clear();
}

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

    return tracked_configs_.at(origin.path).get();
}






    


// Project Registry --------------------------------------------------------------------

// static inline void print_project(const Project& proj, int ind = 0){
//     auto indent = [](int i){ 
//         // log::Output out(std::clog, false);
//         for (int j=0; j<i; j++)
//             std::clog << "  ";
//     };

//     indent(ind);
//     log::info("project: '{}' (", proj.name);
    
//     indent(ind+1);
//     log::info("files: (");
//     for (const auto& file : proj.files) {
//         indent(ind+2);
//         log::info("{}", file->path);
//     }

//     indent(ind+1);
//     log::info(")");
    
//     indent(ind+1);
//     log::info("dependencies: (");
//     for (const auto& dep : proj.dependencies) {
//         const bool print_recursive = false;
//         if(print_recursive)
//             print_project(*dep, ind + 2);
//         else {
//             indent(ind+2);
//             log::info("project: '{}'", dep->name);
//         }
//     }
//     indent(ind+1);
//     log::info(")");
//     indent(ind);
//     log::info(")");
// }

// void ProjectRegistry::print() const {
//     log::info("--- Project Registry ---");
//     print_project(*default_project);
//     for (const auto& p : all_projects){
//         print_project(*p);
//     }
//     log::info("--- Project Registry ---");
//     std::clog << std::endl;
// }

} // namespace artic::ls
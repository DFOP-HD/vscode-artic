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
    if (!text)
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

void Workspace::reload(config::ConfigLog& log) {
    projects_.clear();
    files_.clear();
    configs_.clear();
    arena_ = std::make_unique<Arena>();
    project_for_file_cache_.clear();
}

ConfigFile* Workspace::instantiate_config(const IncludeConfig& origin, config::ConfigLog& log) {
    if(configs_.contains(origin.path)) {
        return configs_.at(origin.path).get();
    }
    log::info("Instantiating config: {}", origin.path.string());
    config::ConfigParser parser(origin, log);
    bool success = parser.parse();
    if (!success) return nullptr;

    // track config
    configs_[origin.path] = arena_->make_ptr<ConfigFile>(parser.config);
    // track projects
    for (const auto& project : parser.projects){
        if(projects_.contains(project.name)) {
            log.file_context = origin.path;
            log.warn("ignoring duplicate definition of " + project.name + " in " + project.origin.string(), project.name);
            continue;
        }
        projects_[project.name] = arena_->make_ptr<Project>(project); // copy
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

    // fix circular project dependencies
    std::unordered_set<std::string> visited;
    std::unordered_set<std::string> rec_stack;
    
    std::function<bool(const std::string&, const std::string&)> detect_cycle = 
        [&](const std::string& project_name, const std::string& parent) -> bool {
        
        if (!projects_.contains(project_name)) {
            return false; // dependency doesn't exist, will be handled elsewhere
        }
        
        if (rec_stack.count(project_name)) {
            // Cycle detected!
            auto& parent_project = projects_.at(parent);

            log.file_context = parent_project->origin;
            log.error("Circular dependency detected: " + parent + " -> " + project_name + 
                     " creates a cycle. Removing this dependency.", project_name);
            log::info("Circular dependency detected in config '{}': {} -> {}", parent_project->origin.string(), parent, project_name);
            
            // Remove the dependency that creates the cycle
            auto& deps = parent_project->dependencies;
            deps.erase(std::remove(deps.begin(), deps.end(), project_name), deps.end());
            
            return true;
        }
        
        if (visited.count(project_name)) {
            return false; // already processed
        }
        
        visited.insert(project_name);
        rec_stack.insert(project_name);
        
        // Check all dependencies
        auto& proj = projects_.at(project_name);
        for (const auto& dep : proj->dependencies) {
            if (detect_cycle(dep, project_name)) {
                // Cycle was detected and fixed in recursive call
            }
        }
        
        rec_stack.erase(project_name);
        return false;
    };
    
    // Check each project for cycles
    for (auto& project : parser.projects) {
        for (auto& dep : project.dependencies) {
            log::info("Checking dependency {} -> {}", project.name, dep);
            visited.clear();
            rec_stack.clear();
            detect_cycle(project.name, dep);
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

    return configs_.at(origin.path).get();
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
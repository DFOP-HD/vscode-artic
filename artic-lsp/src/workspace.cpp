#include "workspace.h"
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

#include <nlohmann/json.hpp>

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

std::vector<const File*> Project::collect_files() const {
    std::unordered_set<const File*> result;
    for (const auto& file : files) {
        result.insert(file.get());
    }
    for (const auto& dependency : dependencies){
        auto dep_files = dependency->collect_files();
        result.insert(dep_files.begin(), dep_files.end());
    }
    std::vector res(result.begin(), result.end());
    return res;
}

bool Project::uses_file(const std::filesystem::path& file) const {
    for (const auto& f : files)
        if(f->path == file) return true;
    for (const auto& dep : dependencies)
        if(dep->uses_file(file)) return true;
    return false;
}

// Workspace --------------------------------------------------------------------

std::optional<std::shared_ptr<Project>> Workspace::project_for_file(const std::filesystem::path& file) const {
    // try active project
    if(active_project.has_value()){
        auto active = std::ranges::find_if(projects_.all_projects, [&](auto& p){ return p->name == *active_project; });
        if(active != projects_.all_projects.end() && (*active)->uses_file(file)){
            return *active;
        }
    }

    // if not in active project, try next best project
    for (const auto& project : projects_.all_projects) {
        for (const auto& f : project->files) {
            // do not use recursive check, as dependencies are also contained in all_projects
            if (f->path == file) { 
                return project;
            }
        }
    }

    // no project can be found -> may need to use default project
    return std::nullopt;
}

void Workspace::mark_file_dirty(const std::filesystem::path& file){
    if(auto it = projects_.tracked_files.find(file); it != projects_.tracked_files.end()){
        it->second->text = std::nullopt;
    }
}
void Workspace::set_file_content(const std::filesystem::path& file, std::string&& content){
    if(auto it = projects_.tracked_files.find(file); it != projects_.tracked_files.end()){
        it->second->text = std::move(content);
    }
}

Ptr<ConfigFile> Workspace::load_config(std::filesystem::path config) {
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
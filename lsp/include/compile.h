#ifndef ARTIC_LS_COMPILE_H
#define ARTIC_LS_COMPILE_H

#include "artic/arena.h"
#include "artic/bind.h"
#include "artic/ls/workspace.h"
#include "artic/types.h"
#include "artic/check.h"
#include "artic/locator.h"
#include "artic/log.h"
#include <memory>
#include <span>

namespace artic::ls{

struct Compiler {
    Compiler()
        : arena(), type_table(), locator()
        , log(log::err, &locator, 0, 0, &diagnostics)
        , name_binder(log, &name_map)
        , type_checker(log, type_table, arena, &name_map)
    {
        log.max_errors = 100;
        type_checker.warns_as_errors = warns_as_errors;
        name_binder.warns_as_errors = warns_as_errors;
        if (enable_all_warns)
            name_binder.warn_on_shadowing = true;
    }

    void compile_files(std::span<const workspace::File*> files);

    // Output -----
    NameMap name_map;
    std::vector<Diagnostic> diagnostics;
    Ptr<ast::ModDecl> program;
    bool parsed_all;
    
    // Input -----
    bool exclude_non_parsed_files = false;
    std::vector<std::unique_ptr<workspace::File>> temporary_files; // used to keep temporary file alive after compilation
    std::filesystem::path active_file; // used for recompilation when the configuration changes. Could be done in a cleaner way

    // Compiler Internals
    Arena arena;
    TypeTable type_table;
    Locator locator;
    Log log;

    NameBinder name_binder;
    TypeChecker type_checker;

    bool warns_as_errors = false;
    bool enable_all_warns = true;
};

class Timer {
public:
    explicit Timer(std::string_view label)
        : label_(label), start_(std::chrono::steady_clock::now()) {}

    ~Timer() {
        auto end = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start_).count();
        // log::info("{} took {} ms", label_, ms);
    }
private:
    std::string label_;
    std::chrono::steady_clock::time_point start_;
};

} // namespace artic::ls

#endif // ARTIC_LS_COMPILE_H

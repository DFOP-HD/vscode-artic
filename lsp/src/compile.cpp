#include "artic/ls/compile.h"

#include "artic/parser.h"
#include "artic/locator.h"
#include "artic/bind.h"
#include "artic/check.h"
#include "artic/summoner.h"
#include <iostream>

namespace {

struct MemBuf : public std::streambuf {
    MemBuf(const std::string& str) {
        setg(
            const_cast<char*>(str.data()),
            const_cast<char*>(str.data()),
            const_cast<char*>(str.data() + str.size()));
    }

    std::streampos seekoff(std::streamoff off, std::ios_base::seekdir way, std::ios_base::openmode) override {
        if (way == std::ios_base::beg)
            setg(eback(), eback() + off, egptr());
        else if (way == std::ios_base::cur)
            setg(eback(), gptr() + off, egptr());
        else if (way == std::ios_base::end)
            setg(eback(), egptr() + off, egptr());
        else
            return std::streampos(-1);
        return gptr() - eback();
    }

    std::streampos seekpos(std::streampos pos, std::ios_base::openmode mode) override {
        return seekoff(std::streamoff(pos), std::ios_base::beg, mode);
    }

    std::streamsize showmanyc() override {
        return egptr() - gptr();
    }
};

} // anonymous namespace

namespace artic::ls {

void Compiler::compile_files(std::span<const workspace::File*> files) {
    program = arena.make_ptr<ast::ModDecl>();
    constexpr bool include_non_parsed_files = true;

    for (auto& file : files){
        file->read();
        auto prev_errors = log.errors;
        if (!file->text) {
            log::error("cannot open file '{}'", file->path);
            continue;
        }
        if (log.locator)
            log.locator->register_file(file->path, file->text.value());

        MemBuf mem_buf(file->text.value());
        std::istream is(&mem_buf);

        Lexer lexer(log, file->path, is);
        Parser parser(log, lexer, arena);
        parser.warns_as_errors = warns_as_errors;
        auto module = parser.parse();

        if(log.errors > prev_errors) {
            log::error("Parsing failed for file {}", file->path);
            if(exclude_non_parsed_files) continue;
        } else {
            // log::info("Parsing success for file {}", file->path);
        }
        program->decls.insert(
            program->decls.end(),
            std::make_move_iterator(module->decls.begin()),
            std::make_move_iterator(module->decls.end())
        );
    }

    program->set_super();

    parsed_all = log.errors == 0;
    if(!parsed_all) {
        log::error("Parsing failed");
    }

    (void)name_binder.run(*program);
    if(!type_checker.run(*program))
        return;
    
    Summoner summoner(log, arena);
    if(!summoner.run(*program))
        return;
}


} // namespace artic::ls

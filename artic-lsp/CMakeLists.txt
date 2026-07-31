cmake_minimum_required(VERSION 3.20)
project(artic-lsp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(BUILD_SHARED_LIBS OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Registers artic's own CTest suite, which guards the artic fork against regressions.
# Requires the `artic` target: cmake --build <dir> --target artic
option(ARTIC_LSP_ENABLE_ARTIC_TESTS "Register the artic compiler test suite" ON)
if(ARTIC_LSP_ENABLE_ARTIC_TESTS)
    include(CTest)
endif()

if(UNIX)
    if (CMAKE_BUILD_TYPE STREQUAL "Debug")
        # Apply sanitizer flags globally before including dependencies
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address")
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address")
        set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fsanitize=address")    
    endif()
endif()

# thorin, lsp-framework, nlohmann_json
include(cmake/Dependencies.cmake)

add_executable(artic-lsp
    src/main.cpp
    include/compile.h
    include/config.h
    include/crash.h
    include/server.h
    include/workspace.h
    src/server.cpp
    src/workspace.cpp
    src/crash.cpp
    src/compile.cpp
    src/config.cpp
)

# Not EXCLUDE_FROM_ALL: the standalone `artic` binary is needed to validate fixtures and
# to run the compiler's own ctest suite, and asking for it by name is unreliable with the
# Visual Studio generator, which cannot locate a target defined in a subdirectory.
add_subdirectory(../artic artic)
target_include_directories(artic-lsp PUBLIC ../artic/include)

target_include_directories(artic-lsp PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# libartic's own options/definitions are set by artic/src/CMakeLists.txt,
# which enables ENABLE_LSP when the artic-lsp target exists.
if(MSVC)
    target_compile_options(artic-lsp PRIVATE /wd4996)
else()
    target_compile_options(artic-lsp PRIVATE -Wno-deprecated-declarations)
endif()

# Statically link the GNU runtimes so the binary is portable. Probed rather than
# hardcoded, since MSVC-ABI toolchains (MSVC, clang-cl, clang --target=*-msvc)
# link the CRT instead and reject/ignore these flags.
include(CheckLinkerFlag)
check_linker_flag(CXX "-static-libgcc;-static-libstdc++" HAVE_STATIC_GNU_RUNTIME)
if(HAVE_STATIC_GNU_RUNTIME)
    target_link_options(artic-lsp PRIVATE -static-libgcc -static-libstdc++)
endif()

target_link_libraries(artic-lsp PUBLIC
    libartic
    lsp 
    nlohmann_json::nlohmann_json
)
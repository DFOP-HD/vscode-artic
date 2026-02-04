cmake_minimum_required(VERSION 3.20)
project(artic-lsp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(BUILD_SHARED_LIBS OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# set(CMAKE_CXX_COMPILER_ID "Clang")

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    # Apply sanitizer flags globally before including dependencies
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fsanitize=address")    
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

add_subdirectory(../artic artic EXCLUDE_FROM_ALL)
target_include_directories(artic-lsp PUBLIC ../artic/include)

target_include_directories(artic-lsp PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_options(artic-lsp PRIVATE -Wno-deprecated-declarations)
target_compile_options(libartic PRIVATE -Wno-deprecated-declarations)
target_compile_definitions(libartic PUBLIC -DENABLE_LSP)

set_target_properties(artic-lsp PROPERTIES LINK_FLAGS "-static-libgcc -static-libstdc++")

target_link_libraries(artic-lsp PUBLIC
    libartic
    lsp 
    nlohmann_json::nlohmann_json
)
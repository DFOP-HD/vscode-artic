cmake_minimum_required(VERSION 3.20)
project(artic-lsp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# set(CMAKE_CXX_COMPILER_ID "Clang")

add_library(artic-lsp
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

# target_include_directories(artic-lsp PUBLIC include)
target_include_directories(artic-lsp PUBLIC ../artic/include)
target_compile_options(artic-lsp PRIVATE -Wno-deprecated-declarations)
target_include_directories(artic-lsp PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# find_package(lsp 1.3.0 EXACT REQUIRED)
# target_link_libraries(artic-lsp PUBLIC lsp::lsp)
include(FetchContent)
message(STATUS "Fetching lsp-framework")
FetchContent_Declare(
    lsp 
    GIT_REPOSITORY https://github.com/leon-bckl/lsp-framework.git
    GIT_TAG cf82b9ad4c89e3dc75fefb58352749dad45c5c0d # 1.3.0
)
FetchContent_MakeAvailable(lsp)
message(STATUS "Fetching lsp-framework - done")
target_link_libraries(artic-lsp PUBLIC lsp)


message(STATUS "Fetching nlohmann_json")
FetchContent_Declare(
    nlohmann_json 
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG 55f93686c01528224f448c19128836e7df245f72 # 3.12.0
)
FetchContent_MakeAvailable(nlohmann_json)
message(STATUS "Fetching nlohmann_json - done")
target_link_libraries(artic-lsp PUBLIC nlohmann_json::nlohmann_json)
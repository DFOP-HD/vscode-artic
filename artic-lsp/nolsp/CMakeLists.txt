# Proves the standalone artic compiler still builds without ENABLE_LSP: a tiny project
# that pulls in artic/ without ever defining an artic-lsp target, so the define that
# artic/src/CMakeLists.txt attaches to libartic is never applied.
#
# Thorin and Half are not fetched here. Point Thorin_DIR and Half_DIR at a configured
# artic-lsp build tree, for example:
#
#   cmake -S artic-lsp/nolsp -B artic-lsp/build-nolsp -G Ninja \
#         -D CMAKE_BUILD_TYPE=Release \
#         -D Thorin_DIR=<repo>/artic-lsp/build/share/anydsl/cmake \
#         -D Half_DIR=<repo>/artic-lsp/build/_deps/half-src/include

cmake_minimum_required(VERSION 3.20)
project(artic-nolsp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(BUILD_SHARED_LIBS OFF)

include(CTest)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../artic artic)

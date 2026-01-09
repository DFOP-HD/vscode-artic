
include(FetchContent)
# set(FETCHCONTENT_QUIET OFF)

function(fetch_half)
    FetchContent_Declare(
        half
        URL https://sourceforge.net/projects/half/files/latest/download
        DOWNLOAD_NAME half.zip
    )
    FetchContent_MakeAvailable(half)
    set(Half_INCLUDE_DIR ${half_SOURCE_DIR}/include CACHE PATH "Path to half headers" FORCE)
endfunction()

function(fetch_thorin)
    # Disable warnings temporarily
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -w")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -w")
    
    # These packages are not relevant for lsp as we don't use the backend anyways
    set(CMAKE_DISABLE_FIND_PACKAGE_LLVM TRUE)
    set(CMAKE_DISABLE_FIND_PACKAGE_SPIRV-Headers TRUE)
    set(CMAKE_DISABLE_FIND_PACKAGE_shady TRUE)
    set(CMAKE_DISABLE_FIND_PACKAGE_nlohmann_json TRUE)

    # NOTE: Using fork of thorin util clang compile fix is merged upstream

    # FetchContent_Declare(
    #     thorin 
    #     GIT_REPOSITORY https://github.com/AnyDSL/thorin.git
    #     GIT_TAG d60f0e31e70270bb1242d12c050563d0befa8ad2 # master as of 09.01.2026
    # )
    FetchContent_Declare(
        thorin 
        GIT_REPOSITORY git@github.com:TimGrun/thorin.git
        GIT_TAG ae4b94570e6fa5f564d02eb7ebecd6be6fd64ee8 # 3.12.0
    )
    FetchContent_MakeAvailable(thorin)

    set(Thorin_DIR "${CMAKE_CURRENT_BINARY_DIR}/share/anydsl/cmake" CACHE PATH "Path to Thorin CMake files" FORCE)

    # Restore warnings
    string(REPLACE " -w" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    string(REPLACE " -w" "" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
endfunction()

fetch_half()
fetch_thorin()

add_subdirectory(../artic artic EXCLUDE_FROM_ALL) # libartic

FetchContent_Declare(
    lsp
    GIT_REPOSITORY https://github.com/leon-bckl/lsp-framework.git
    GIT_TAG cf82b9ad4c89e3dc75fefb58352749dad45c5c0d # 1.3.0
)
FetchContent_MakeAvailable(lsp)

FetchContent_Declare(
    nlohmann_json 
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG 55f93686c01528224f448c19128836e7df245f72 # 3.12.0
)
FetchContent_MakeAvailable(nlohmann_json)

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(MINGW_TRIPLET "x86_64-w64-mingw32")

set(CMAKE_C_COMPILER   clang)
set(CMAKE_CXX_COMPILER clang++)

set(CLANG_MINGW_COMMON_FLAGS
    "--target=${MINGW_TRIPLET}"
    "-fuse-ld=lld"
)

# Tell CMake to prepend these to all compile and link lines
set(CMAKE_C_FLAGS_INIT   "${CLANG_MINGW_COMMON_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${CLANG_MINGW_COMMON_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${CLANG_MINGW_COMMON_FLAGS}")

# Ensure CMake finds the MinGW sysroot for headers/libs
set(CMAKE_FIND_ROOT_PATH
    /usr/${MINGW_TRIPLET}
)

set(CMAKE_SYSROOT "/usr/${MINGW_TRIPLET}")

# How to search for libraries, headers, programs:
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# set(CMAKE_C_COMPILER_TARGET x86_64-w64-mingw32)
# set(CMAKE_CXX_COMPILER_TARGET x86_64-w64-mingw32)

# # Tell Clang where to find MinGW sysroot
# set(CMAKE_SYSROOT /usr/x86_64-w64-mingw32)

# set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
# list(APPEND CMAKE_FIND_ROOT_PATH ${CMAKE_CURRENT_BINARY_DIR})

# # Add MinGW C++ standard library include paths
# set(MINGW_INCLUDE_DIRS
#     "-isystem /usr/x86_64-w64-mingw32/include"
#     "-isystem /usr/x86_64-w64-mingw32/include/c++/10.3-posix"
#     "-isystem /usr/x86_64-w64-mingw32/include/c++/10.3-posix/x86_64-w64-mingw32"
#     "-isystem /usr/lib/gcc/x86_64-w64-mingw32/10-posix/include"
# )
# string(REPLACE ";" " " MINGW_INCLUDE_FLAGS "${MINGW_INCLUDE_DIRS}")
# set(CMAKE_CXX_FLAGS_INIT "${MINGW_INCLUDE_FLAGS}")
# set(CMAKE_C_FLAGS_INIT "${MINGW_INCLUDE_FLAGS}")

# Add MinGW library paths for linking
# set(CMAKE_EXE_LINKER_FLAGS_INIT "-L/usr/lib/gcc/x86_64-w64-mingw32/10-posix -L/usr/x86_64-w64-mingw32/lib")
# set(CMAKE_SHARED_LINKER_FLAGS_INIT "-L/usr/lib/gcc/x86_64-w64-mingw32/10-posix -L/usr/x86_64-w64-mingw32/lib")

# # Use MinGW tools for linking
# set(CMAKE_RC_COMPILER /usr/bin/x86_64-w64-mingw32-windres)
# set(CMAKE_AR /usr/bin/x86_64-w64-mingw32-ar)
# set(CMAKE_RANLIB /usr/bin/x86_64-w64-mingw32-ranlib)

# Search for programs in the build host directories
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
# Search for libraries and headers in the target directories
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
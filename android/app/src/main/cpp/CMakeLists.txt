cmake_minimum_required(VERSION 3.25)

# Debug variants of Gradle would build the emulator at -O0 (unplayably slow):
# always build the native code optimized. A normal variable shadows the cache
# value AGP passes on the command line.
set(CMAKE_BUILD_TYPE Release)
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG -g0")

# Keep memory use low on small build machines (Ninja only).
set(CMAKE_JOB_POOL_COMPILE gbah_compile)
set_property(GLOBAL PROPERTY JOB_POOLS gbah_compile=3)

project(gbahandoff_android LANGUAGES C)

# Reuse the repo-root CMake (same mGBA flags as desktop) instead of duplicating it.
set(GBAH_BUILD_DESKTOP OFF CACHE BOOL "" FORCE)
set(GBAH_BUILD_TESTS   OFF CACHE BOOL "" FORCE)
get_filename_component(GBAH_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../.." ABSOLUTE)
add_subdirectory(${GBAH_ROOT} gbah_root)

add_library(gbahandoff SHARED jni_bridge.c link_jni.c)
target_link_libraries(gbahandoff PRIVATE gbah_core gbah_net aaudio android log)
# lets __builtin_available(android N) guard APIs newer than minSdk (AAudio setUsage = 28)
target_compile_definitions(gbahandoff PRIVATE __ANDROID_UNAVAILABLE_SYMBOLS_ARE_WEAK__)
target_compile_options(gbahandoff PRIVATE -Wall -Wextra -fvisibility=hidden)

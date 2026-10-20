cmake_minimum_required(VERSION 3.25)
project(gbahandoff LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(GBAH_BUILD_DESKTOP "Build the SDL3 desktop app" ON)
option(GBAH_BUILD_TESTS   "Build tests" ON)

# ---- app version: VERSION (code=N / name=X.Y) is the single source, also read by the Android Gradle build ----
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/VERSION)
file(STRINGS ${PROJECT_SOURCE_DIR}/VERSION GBAH_VERSION_LINES)
foreach(line IN LISTS GBAH_VERSION_LINES)
  if(line MATCHES "^code=([0-9]+)$")
    set(GBAH_APP_VERSION_CODE ${CMAKE_MATCH_1})
  elseif(line MATCHES "^name=(.+)$")
    set(GBAH_APP_VERSION_NAME ${CMAKE_MATCH_1})
  endif()
endforeach()
if(NOT DEFINED GBAH_APP_VERSION_CODE OR NOT DEFINED GBAH_APP_VERSION_NAME)
  message(FATAL_ERROR "VERSION must contain 'code=N' and 'name=X.Y'")
endif()
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/generated)
file(CONFIGURE OUTPUT ${CMAKE_BINARY_DIR}/generated/version.h CONTENT
"// generated from VERSION by CMake; do not edit\n#pragma once\n#define GBAH_APP_VERSION_CODE @GBAH_APP_VERSION_CODE@\n#define GBAH_APP_VERSION_NAME \"@GBAH_APP_VERSION_NAME@\"\n"
  @ONLY)

# ---- mGBA: only the static libmgba, GBA core, no frontends/optional deps ----
# EXCLUDE_FROM_ALL makes mGBA's CMake enter its LIBMGBA_ONLY mode (static lib,
# no frontends, DISABLE_DEPS). It must stay on add_subdirectory itself.
set(M_CORE_GBA ON  CACHE BOOL "" FORCE)
set(M_CORE_GB  OFF CACHE BOOL "" FORCE)
set(USE_DEBUGGERS OFF CACHE BOOL "" FORCE)
set(USE_GDB_STUB  OFF CACHE BOOL "" FORCE)
set(USE_EDITLINE  OFF CACHE BOOL "" FORCE)
set(ENABLE_SCRIPTING OFF CACHE BOOL "" FORCE)
set(USE_LUA OFF CACHE BOOL "" FORCE)
set(USE_FFMPEG OFF CACHE BOOL "" FORCE)
set(USE_ZLIB OFF CACHE BOOL "" FORCE)
set(USE_MINIZIP OFF CACHE BOOL "" FORCE)
set(USE_PNG OFF CACHE BOOL "" FORCE)
set(USE_LIBZIP OFF CACHE BOOL "" FORCE)
set(USE_SQLITE3 OFF CACHE BOOL "" FORCE)
set(USE_ELF OFF CACHE BOOL "" FORCE)
set(USE_LZMA OFF CACHE BOOL "" FORCE)
set(USE_DISCORD_RPC OFF CACHE BOOL "" FORCE)
set(BUILD_QT OFF CACHE BOOL "" FORCE)
set(BUILD_SDL OFF CACHE BOOL "" FORCE)
set(BUILD_GL OFF CACHE BOOL "" FORCE)
set(BUILD_GLES2 OFF CACHE BOOL "" FORCE)
set(BUILD_GLES3 OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC ON CACHE BOOL "" FORCE)
set(BUILD_LTO OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/mgba EXCLUDE_FROM_ALL)

# ---- wire protocol (pure C, no mGBA dependency) ----
add_subdirectory(proto)

# ---- version rules + game store (pure C, shared by desktop and Android) ----
add_subdirectory(sync)

# ---- core wrapper ----
add_library(gbah_core STATIC core/gbah_core.c)
target_include_directories(gbah_core PUBLIC core)
target_link_libraries(gbah_core PUBLIC mgba)

if(GBAH_BUILD_DESKTOP)
  add_subdirectory(desktop)
endif()

if(GBAH_BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)
  enable_testing()
  add_subdirectory(tests)
endif()

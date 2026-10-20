# Version rules + game store shared by desktop and Android. Pure C99, libc only.
add_library(gbah_sync STATIC gbah_sync.c)
target_include_directories(gbah_sync PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(gbah_sync PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)

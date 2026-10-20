# Wire protocol: pure C99, no dependencies (also built by the Android NDK).
add_library(gbah_proto STATIC gbahp.c)
target_include_directories(gbah_proto PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(gbah_proto PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)

# Blocking loopback TCP helpers (POSIX / Winsock).
add_library(gbah_net STATIC gbahp_net.c)
target_link_libraries(gbah_net PUBLIC gbah_proto)
if(WIN32)
  target_link_libraries(gbah_net PUBLIC ws2_32)
endif()

set(GBAH_TEST_ROM ${CMAKE_CURRENT_SOURCE_DIR}/roms/gbah_test.gba)
file(SHA1 ${GBAH_TEST_ROM} GBAH_TEST_ROM_SHA1)  # independent reference for the core's own SHA-1
set(GBAH_TEST_ROM_LATE ${CMAKE_CURRENT_SOURCE_DIR}/roms/gbah_test_late.gba)
file(SHA1 ${GBAH_TEST_ROM_LATE} GBAH_TEST_ROM_LATE_SHA1)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${GBAH_TEST_ROM} ${GBAH_TEST_ROM_LATE})

add_executable(core_test core_test.c)
target_link_libraries(core_test PRIVATE gbah_core)
add_test(NAME core_test COMMAND core_test ${GBAH_TEST_ROM} ${GBAH_TEST_ROM_SHA1}
         ${GBAH_TEST_ROM_LATE} ${GBAH_TEST_ROM_LATE_SHA1})

# ---- wire protocol ----
set(GBAH_PROTO_SRC ${PROJECT_SOURCE_DIR}/proto/gbahp.c)
add_executable(proto_test proto_test.c)
target_link_libraries(proto_test PRIVATE gbah_proto)
add_test(NAME proto_test COMMAND proto_test)

# ASan on WSL2/new kernels sometimes dies at startup (endless DEADLYSIGNAL loop, it hung ctest):
# run_san.sh retries exactly that signature. setarch -R is not permitted in the sandbox.
set(GBAH_NOASLR ${CMAKE_CURRENT_SOURCE_DIR}/run_san.sh)

# Same test + fuzz rebuilt from source with ASan/UBSan (the parser must never crash).
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  add_executable(proto_test_san proto_test.c ${GBAH_PROTO_SRC})
  target_include_directories(proto_test_san PRIVATE ${PROJECT_SOURCE_DIR}/proto)
  target_compile_options(proto_test_san PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -g -O1)
  target_link_options(proto_test_san PRIVATE -fsanitize=address,undefined)
  add_test(NAME proto_test_san COMMAND ${GBAH_NOASLR} $<TARGET_FILE:proto_test_san>)
  set_tests_properties(proto_test_san PROPERTIES TIMEOUT 120)
endif()

find_package(Threads REQUIRED)
add_executable(proto_loopback_test proto_loopback_test.c)
target_link_libraries(proto_loopback_test PRIVATE gbah_net Threads::Threads)
add_test(NAME proto_loopback_test COMMAND proto_loopback_test)

# ---- desktop server <-> simulated phone (real binary, real TCP, fake adb) ----
if(GBAH_BUILD_DESKTOP AND UNIX)
  file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/fake_adb.sh DESTINATION ${CMAKE_CURRENT_BINARY_DIR}
       FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
  add_executable(link_integration_test link_integration_test.c)
  target_link_libraries(link_integration_test PRIVATE gbah_net gbah_core)
  add_dependencies(link_integration_test gbahandoff)
  add_test(NAME link_integration_test
           COMMAND link_integration_test $<TARGET_FILE:gbahandoff> ${GBAH_TEST_ROM} ${GBAH_TEST_ROM_SHA1}
                   ${CMAKE_CURRENT_BINARY_DIR}/fake_adb.sh)
  set_tests_properties(link_integration_test PROPERTIES TIMEOUT 60)
  add_executable(handoff_e2e_test handoff_e2e_test.c)
  target_link_libraries(handoff_e2e_test PRIVATE gbah_net gbah_core gbah_sync)
  add_dependencies(handoff_e2e_test gbahandoff)
  add_test(NAME handoff_e2e_test
           COMMAND handoff_e2e_test $<TARGET_FILE:gbahandoff> ${GBAH_TEST_ROM} ${GBAH_TEST_ROM_SHA1}
                   ${CMAKE_CURRENT_BINARY_DIR}/fake_adb.sh)
  set_tests_properties(handoff_e2e_test PROPERTIES TIMEOUT 180)
endif()

# ---- sync rules + store (unit) ----
add_executable(sync_test sync_test.c)
target_link_libraries(sync_test PRIVATE gbah_sync)
add_test(NAME sync_test COMMAND sync_test)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  add_executable(sync_test_san sync_test.c ${PROJECT_SOURCE_DIR}/sync/gbah_sync.c)
  target_include_directories(sync_test_san PRIVATE ${PROJECT_SOURCE_DIR}/sync)
  target_compile_options(sync_test_san PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=undefined -g -O1)
  target_link_options(sync_test_san PRIVATE -fsanitize=address,undefined)
  add_test(NAME sync_test_san COMMAND ${GBAH_NOASLR} $<TARGET_FILE:sync_test_san>)
  set_tests_properties(sync_test_san PROPERTIES TIMEOUT 120)
endif()

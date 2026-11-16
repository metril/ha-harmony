cmake_minimum_required(VERSION 3.28)
project(ha_harmony C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(CMAKE_CROSSCOMPILING)
  set(_hp_san OFF)
  set(_hp_tests OFF)
else()
  set(_hp_san ON)
  set(_hp_tests ON)
endif()
option(HP_SANITIZE "Build with ASan+UBSan" ${_hp_san})

add_compile_options(-Wall -Wextra -Werror)
if(HP_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# Vendored cJSON: relaxed warnings (third-party code, unmodified).
add_library(cjson STATIC vendor/cjson/cJSON.c)
target_include_directories(cjson PUBLIC ${CMAKE_SOURCE_DIR}/vendor/cjson)
target_compile_options(cjson PRIVATE -Wno-error -w)
target_compile_definitions(cjson PRIVATE CJSON_NESTING_LIMIT=64)
target_link_libraries(cjson PRIVATE m)

# Protocol + RF core: shared by hub and remote.
add_library(hp_core STATIC
  proto/gen/harmony_proto.c
  hub/rf/framing.c
  hub/rf/rf_payload.c
  remote/page/page.c
)
target_include_directories(hp_core PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(hp_core PUBLIC m)

if(CMAKE_CROSSCOMPILING AND HP_TARGET STREQUAL "remote")
  # Remote firmware: shared core + entry stub; no host-only tools/tests.
  add_executable(remote remote/main.c)
  target_link_libraries(remote PRIVATE hp_core)
  return()
endif()

# Hub code library; later tasks append sources here.
add_library(hp_hub STATIC
  hub/net/ws.c
  hub/net/jsonrpc.c
  hub/engine/mapping.c
  hub/engine/sched.c
  hub/io/sim_sinks.c
  hub/io/rf_pair.c
  hub/ir/encode.c
  hub/hub_handlers.c
)
target_link_libraries(hp_hub PUBLIC hp_core cjson m)

add_executable(hub hub/main.c)
target_link_libraries(hub PRIVATE hp_hub)

if(NOT CMAKE_CROSSCOMPILING)
  add_executable(remote_sim remote/sim/remote_sim.c)
  target_link_libraries(remote_sim PRIVATE hp_hub)
endif()

# Host default ON; cross builds opt in (run under qemu-user via
# CMAKE_CROSSCOMPILING_EMULATOR) to catch endianness bugs.
option(HP_BUILD_TESTS "Build C tests" ${_hp_tests})
if(HP_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests/c)
endif()

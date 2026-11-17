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
  hub/rf/link.c
  hub/rf/rf_payload.c
  hub/io/rf_spi.c
  remote/page/page.c
)
target_include_directories(hp_core PUBLIC ${CMAKE_SOURCE_DIR})
find_package(Threads REQUIRED)
target_link_libraries(hp_core PUBLIC m Threads::Threads)

# Remote app: vendored LVGL (render only, remote/lv_conf.h) + our app (display, ui, input, power).
# Built for the remote ARM target and for host tests; the hub MIPS build never needs it.
if(NOT CMAKE_CROSSCOMPILING OR HP_TARGET STREQUAL "remote")
  file(GLOB_RECURSE _lvgl_src CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/vendor/lvgl/src/*.c)
  add_library(lvgl STATIC ${_lvgl_src})
  # SYSTEM: LVGL's headers must not trip our -Werror. Its own sources are built with relaxed flags.
  target_include_directories(lvgl SYSTEM PUBLIC ${CMAKE_SOURCE_DIR}/vendor/lvgl ${CMAKE_SOURCE_DIR}/remote)
  target_compile_definitions(lvgl PUBLIC LV_CONF_INCLUDE_SIMPLE)
  target_compile_options(lvgl PRIVATE -w -Wno-error -ffunction-sections -fdata-sections)
  target_link_libraries(lvgl PRIVATE m)
  if(NOT CMAKE_CROSSCOMPILING)
    target_compile_definitions(lvgl PUBLIC HP_HOST_BUILD)  # asserts on (remote/lv_conf.h)
  endif()

  add_library(hp_remote_app STATIC
    remote/app/sys.c
    remote/app/real_ops.c
    remote/app/display.c
    remote/app/ui.c
    remote/app/input.c
    remote/app/power.c
    remote/app/app.c
  )
  target_link_libraries(hp_remote_app PUBLIC hp_core lvgl)
  if(NOT CMAKE_CROSSCOMPILING)
    # Host build only to keep main.c under -Werror/ASan; it needs a real 240x320 /dev/fb0 to run.
    add_executable(harmony_remote remote/app/main.c)
    target_link_libraries(harmony_remote PRIVATE hp_remote_app)
  endif()
endif()

if(CMAKE_CROSSCOMPILING AND HP_TARGET STREQUAL "remote")
  # Remote firmware: the LVGL app (shared core + page decoder); no host-only tools/tests.
  add_executable(harmony_remote remote/app/main.c)
  target_link_libraries(harmony_remote PRIVATE hp_remote_app)
  target_link_options(harmony_remote PRIVATE -Wl,--gc-sections)
  # Bring-up probes run on the remote itself (static ARM): the radio probe with its 32-byte write
  # cap, and the read-only framebuffer/input probe.
  add_executable(rf_probe tools/rf_probe.c)
  target_include_directories(rf_probe PRIVATE ${CMAKE_SOURCE_DIR})
  add_executable(remote_probe tools/remote_probe.c)
  return()
endif()

# Hub code library; later tasks append sources here.
add_library(hp_hub STATIC
  hub/net/ws.c
  hub/net/jsonrpc.c
  hub/net/mdns.c
  hub/net/mdns_srv.c
  hub/engine/mapping.c
  hub/engine/sched.c
  hub/engine/irout.c
  hub/engine/activity.c
  hub/engine/atomic_file.c
  hub/io/sim_sinks.c
  hub/io/hw_sinks.c
  hub/io/rf_pair.c
  hub/ir/encode.c
  hub/hub_handlers.c
)
find_package(Threads REQUIRED)
target_link_libraries(hp_hub PUBLIC hp_core cjson m Threads::Threads)

add_executable(hub hub/main.c)
target_link_libraries(hub PRIVATE hp_hub)

# Bench-only IR emitter experiment (not shipped in the hub binary).
add_executable(ir_bench tools/ir_bench.c)

# Read-only CC2544 radio probe (/dev/rfspi); built for the hub target alongside hub/remote_sim.
if(NOT CMAKE_CROSSCOMPILING OR HP_TARGET STREQUAL "hub")
  add_executable(rf_probe tools/rf_probe.c)
  target_include_directories(rf_probe PRIVATE ${CMAKE_SOURCE_DIR})
endif()

# Host build of the remote probe (for its tests); it ships only in the remote (ARM) target above.
if(NOT CMAKE_CROSSCOMPILING)
  add_executable(remote_probe tools/remote_probe.c)
endif()

# remote_sim drives the hub's RF link, which is a Unix socket *on the hub*, so the hub target
# needs it too: with the physical remote deliberately unrooted, a simulated press has to
# originate on the device. It links only hp_hub and plain sockets, so there is nothing
# host-specific about it. The remote (ARM) target still skips it.
if(NOT CMAKE_CROSSCOMPILING OR HP_TARGET STREQUAL "hub")
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

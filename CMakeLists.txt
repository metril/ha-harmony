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

# The hub's /data is ~2.4 MB and an OTA needs twice the image plus HP_OTA_SLACK free: hub cross builds are size-first
# (-Os, per-function sections, linker gc, stripped output; tools/check_hub_size.py enforces the budget).
if(CMAKE_CROSSCOMPILING AND HP_TARGET STREQUAL "hub")
  if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE MinSizeRel CACHE STRING "Build type" FORCE)
  endif()
  add_compile_options(-ffunction-sections -fdata-sections)
endif()

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

# Signed OTA images (Ed25519 via vendored TweetNaCl): shared by hub and remote.
# The compiled-in public key comes from HP_OTA_PUBKEY_HEADER; the default is the DEV/TEST key,
# which release builds (CMAKE_BUILD_TYPE=Release or -DHP_RELEASE=ON) refuse.
set(HP_OTA_PUBKEY_HEADER "${CMAKE_SOURCE_DIR}/common/ota/ota_pubkey_dev.h" CACHE FILEPATH
    "Generated public-key header (ota_sign pubheader)")
file(READ "${HP_OTA_PUBKEY_HEADER}" _ota_key_h)
string(FIND "${_ota_key_h}" "#define OTA_PUBKEY_IS_DEV 1" _ota_dev_pos)
set(_ota_is_dev OFF)
if(NOT _ota_dev_pos EQUAL -1)
  set(_ota_is_dev ON)
endif()
option(HP_ALLOW_DEV_OTA_KEY "Permit the DEV/TEST OTA public key in cross/non-Debug builds (never for shipping)" OFF)
set(_ota_guard OFF)  # builds that must not carry the dev key
if(CMAKE_CROSSCOMPILING OR HP_RELEASE OR (CMAKE_BUILD_TYPE AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug"))
  set(_ota_guard ON)
endif()
if(_ota_is_dev)
  if(_ota_guard AND NOT HP_ALLOW_DEV_OTA_KEY)
    message(FATAL_ERROR "OTA: the DEV/TEST public key is selected in a cross or non-Debug build. "
                        "Generate one with 'ota_sign pubheader' and pass -DHP_OTA_PUBKEY_HEADER=<file>, "
                        "or for development only pass -DHP_ALLOW_DEV_OTA_KEY=ON.")
  endif()
  message(WARNING "OTA: ************ DEV/TEST OTA PUBLIC KEY BAKED IN: images signed with the public dev seed are "
                  "accepted. Never ship this build. ************")
endif()
add_library(tweetnacl STATIC vendor/tweetnacl/tweetnacl.c)
target_include_directories(tweetnacl PUBLIC ${CMAKE_SOURCE_DIR}/vendor/tweetnacl)
target_compile_options(tweetnacl PRIVATE -w -Wno-error -fno-sanitize=shift)  # third-party, unmodified (left-shifts negatives by design)
add_library(hp_ota STATIC common/ota/ota_image.c common/ota/ota_apply.c)
target_include_directories(hp_ota PUBLIC ${CMAKE_SOURCE_DIR}/common/ota)
target_link_libraries(hp_ota PUBLIC tweetnacl)
get_filename_component(_ota_key_dir "${HP_OTA_PUBKEY_HEADER}" DIRECTORY)
get_filename_component(_ota_key_name "${HP_OTA_PUBKEY_HEADER}" NAME)
target_include_directories(hp_ota PRIVATE ${_ota_key_dir})
target_compile_definitions(hp_ota PRIVATE OTA_PUBKEY_HEADER="${_ota_key_name}")
if(_ota_guard)
  target_compile_definitions(hp_ota PRIVATE HP_RELEASE=1)
endif()
if(HP_ALLOW_DEV_OTA_KEY)
  target_compile_definitions(hp_ota PRIVATE HP_ALLOW_DEV_OTA_KEY=1)
endif()
if(NOT CMAKE_CROSSCOMPILING)
  add_executable(ota_sign tools/ota_sign.c)
  target_link_libraries(ota_sign PRIVATE hp_ota)
endif()

# Protocol + RF core: shared by hub and remote.
add_library(hp_core STATIC
  proto/gen/harmony_proto.c
  hub/rf/framing.c
  hub/rf/link.c
  hub/rf/rf_payload.c
  hub/io/rf_spi.c
  remote/page/page.c
  remote/page/store.c
)
target_include_directories(hp_core PUBLIC ${CMAKE_SOURCE_DIR})
find_package(Threads REQUIRED)
target_link_libraries(hp_core PUBLIC m Threads::Threads hp_ota)

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
    remote/app/theme.c
    remote/app/theme_presets.c
    remote/app/theme_derive.c
    remote/app/fonts/ra_font_b20.c
    remote/app/fonts/ra_font_b18.c
    remote/app/fonts/ra_font_m16.c
    remote/app/fonts/ra_font_b13.c
    remote/app/fonts/ra_font_r12.c
    remote/app/fonts/ra_icons_20.c
    remote/app/fonts/ra_icons_24.c
    remote/app/fonts/ra_icons_32.c
    remote/app/input.c
    remote/app/power.c
    remote/app/app.c
    remote/app/cli.c
    remote/app/rf_link.c
  )
  target_link_libraries(hp_remote_app PUBLIC hp_core lvgl)
  if(CMAKE_CROSSCOMPILING)
    target_compile_definitions(hp_remote_app PRIVATE HP_REMOTE_DEVICE=1)  # --rf defaults to spi
  endif()
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

# Bluetooth HID helper (`hub bt-helper`): BTstack Classic HID device and, with HH_BT_LE, LE HID peripheral + LE remotes.
option(HH_BT "Build the Bluetooth HID helper into the hub binary" ON)
option(HH_BT_LE "Bluetooth helper: LE peripheral (HID over GATT) and LE remotes (HOGP host), besides Classic" ON)
# The supervisor (bt_link.c) when the helper is built in, else the "no Bluetooth" stub.
if(HH_BT)
  set(_hp_bt_link hub/bt_link.c)
else()
  set(_hp_bt_link hub/bt_link_stub.c)
endif()

# Hub code library; later tasks append sources here.
add_library(hp_hub STATIC
  hub/net/ws.c
  hub/net/jsonrpc.c
  hub/net/mdns.c
  hub/net/mdns_srv.c
  hub/net/ident.c
  hub/net/hublog.c
  hub/net/peers.c
  hub/engine/mapping.c
  hub/engine/sched.c
  hub/engine/irout.c
  hub/engine/activity.c
  hub/engine/atomic_file.c
  hub/io/sim_sinks.c
  hub/io/hw_sinks.c
  hub/io/rf_pair.c
  hub/ir/encode.c
  ${_hp_bt_link}
  hub/hub_handlers.c
  hub/ota/hub_ota.c
)
find_package(Threads REQUIRED)
target_link_libraries(hp_hub PUBLIC hp_core cjson m Threads::Threads)
# Build id of this binary (the OTA downgrade floor on a hub that never had an update); release builds set it.
# Derived at every configure (a normal variable, never cached): max(floor, git commit count), or -DHP_BUILD_ID_OVERRIDE=N.
# The floor is the highest id ever released: a history rewrite changes commit counts, an id must never go back.
file(STRINGS "${CMAKE_SOURCE_DIR}/cmake/build_id_floor.txt" _hp_floor LIMIT_COUNT 1)
if(HP_BUILD_ID_OVERRIDE)
  set(_hp_count "${HP_BUILD_ID_OVERRIDE}")
else()
  execute_process(COMMAND git -C "${CMAKE_SOURCE_DIR}" rev-list --count HEAD
                  OUTPUT_VARIABLE _hp_count OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE _hp_rc)
  if(NOT _hp_rc EQUAL 0 OR NOT _hp_count MATCHES "^[0-9]+$")
    message(WARNING "build id: not a git checkout (or git missing); using the floor ${_hp_floor}. "
                    "Pass -DHP_BUILD_ID_OVERRIDE=N for a real release build.")
    set(_hp_count 0)
  endif()
endif()
if(_hp_count LESS _hp_floor)
  set(HP_BUILD_ID ${_hp_floor})
else()
  set(HP_BUILD_ID ${_hp_count})
endif()
message(STATUS "firmware build id ${HP_BUILD_ID}")
set_source_files_properties(hub/ota/hub_ota.c PROPERTIES COMPILE_DEFINITIONS HP_BUILD_ID=${HP_BUILD_ID})

add_executable(hub hub/main.c)
target_link_libraries(hub PRIVATE hp_hub)
if(HH_BT AND HH_BT_LE)
  target_compile_definitions(hp_hub PRIVATE HH_BT_LE=1)  # bt_link.c reports LE in its default limits
endif()
if(HH_BT)
  include(hub/bt/bt.cmake)  # include(), not add_subdirectory: a hub/ build dir would collide with the hub executable
  target_link_libraries(hub PRIVATE hp_bt)
  target_compile_definitions(hub PRIVATE HH_BT=1)
endif()
if(NOT HP_SANITIZE)
  target_link_options(hub PRIVATE -Wl,--gc-sections)  # drop unreferenced functions/data (all hub objects are built with -ffunction-sections)
endif()
if(CMAKE_CROSSCOMPILING AND HP_TARGET STREQUAL "hub" AND CMAKE_STRIP)
  # What deploy_hub.py pushes and ota_sign signs is the stripped build/hub; hub.debug keeps the symbols next to it.
  add_custom_command(TARGET hub POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:hub> $<TARGET_FILE:hub>.debug
    COMMAND ${CMAKE_STRIP} --strip-all $<TARGET_FILE:hub>
    COMMENT "Stripping hub (unstripped copy: hub.debug)")
endif()

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

# One executable per module: add_hp_test(name) builds test_<name>.c.
function(add_hp_test name)
  add_executable(test_${name} test_${name}.c)
  target_link_libraries(test_${name} PRIVATE hp_hub)
  # 32-bit MIPS has no native 64-bit atomics (_Atomic uint64_t in fakes): libgcc's libatomic.
  if(CMAKE_SYSTEM_PROCESSOR STREQUAL "mips")
    target_link_libraries(test_${name} PRIVATE atomic)
  endif()
  add_test(NAME ${name} COMMAND test_${name})
endfunction()

add_hp_test(ws)
add_hp_test(jsonrpc)
add_hp_test(mapping)
add_hp_test(sched)
add_hp_test(irout)
add_hp_test(activity)
add_hp_test(config2)
add_hp_test(budget)
add_hp_test(rf_payload)
add_hp_test(rf_pair)
add_hp_test(link)
add_hp_test(link_radio)
add_hp_test(link_v2)
add_hp_test(hub_rf_order)
add_hp_test(store)
add_hp_test(rf_spi)
target_compile_definitions(test_rf_spi PRIVATE HP_LINK_VECTORS="${CMAKE_SOURCE_DIR}/tests/vectors/rf/link_status.json")
add_hp_test(mdns)
add_hp_test(ident)
add_hp_test(hwkeys)

# RF object codec: golden vectors shared with the remote build (argv[1] = frames.json).
add_executable(test_framing test_framing.c)
target_link_libraries(test_framing PRIVATE hp_hub)
add_test(NAME framing COMMAND test_framing ${CMAKE_SOURCE_DIR}/tests/vectors/rf/frames.json)

add_hp_test(page)
target_compile_definitions(test_page PRIVATE HP_VECTORS="${CMAKE_SOURCE_DIR}/tests/vectors/pages.json")

# Decoder fuzz: every byte mutation / truncation of the valid vectors, split in 4 shards for ctest -j.
add_executable(test_page_fuzz test_page_fuzz.c)
target_link_libraries(test_page_fuzz PRIVATE hp_hub)
target_compile_definitions(test_page_fuzz PRIVATE HP_VECTORS="${CMAKE_SOURCE_DIR}/tests/vectors/pages.json")
foreach(i RANGE 0 3)
  add_test(NAME page_fuzz_${i} COMMAND test_page_fuzz ${i} 4)
endforeach()

# IR encoder: vectors dir passed as argv[1] (shared with tests/py/test_ir_vectors.py).
add_executable(test_ir_encode test_ir_encode.c)
target_link_libraries(test_ir_encode PRIVATE hp_hub)
add_test(NAME ir_encode COMMAND test_ir_encode ${CMAKE_SOURCE_DIR}/tests/vectors/ir)

# Real-hardware IR sink: host-only via injected syscalls. argv[1] = ir_bench (byte-for-byte reference).
add_executable(test_hw_sinks test_hw_sinks.c)
target_link_libraries(test_hw_sinks PRIVATE hp_hub)
add_dependencies(test_hw_sinks ir_bench)
add_test(NAME hw_sinks COMMAND test_hw_sinks $<TARGET_FILE:ir_bench>)

# rf_probe (read-only radio probe): host-only via injected syscalls; the tool source is #included.
add_executable(test_rf_probe test_rf_probe.c)
target_include_directories(test_rf_probe PRIVATE ${CMAKE_SOURCE_DIR})
add_test(NAME rf_probe COMMAND test_rf_probe)

# remote_probe (read-only fb/input probe): host-only via injected syscalls; the source is #included.
add_executable(test_remote_probe test_remote_probe.c)
target_include_directories(test_remote_probe PRIVATE ${CMAKE_SOURCE_DIR})
add_test(NAME remote_probe COMMAND test_remote_probe)

# Remote app (LVGL render, evdev input, power): host-only through the injected syscall seam.
if(TARGET hp_remote_app AND NOT CMAKE_CROSSCOMPILING)
  foreach(t remote_input remote_power remote_suspend remote_render remote_mem remote_oom remote_app remote_rf remote_latency remote_v2 remote_fuzz remote_themes remote_colors)
    add_executable(test_${t} test_${t}.c)
    target_link_libraries(test_${t} PRIVATE hp_remote_app hp_hub)
    if(NOT t STREQUAL "remote_mem" AND NOT t STREQUAL "remote_oom" AND NOT t STREQUAL "remote_fuzz")
      add_test(NAME ${t} COMMAND test_${t})
    endif()
  endforeach()
  # the pool tests are the slowest: one ctest entry per half so they run in parallel
  add_test(NAME remote_mem_v1 COMMAND test_remote_mem v1)
  add_test(NAME remote_mem_v2 COMMAND test_remote_mem v2)
  foreach(i RANGE 0 3) # fuzz of the v2 event path, 4 shards
    add_test(NAME remote_fuzz_${i} COMMAND test_remote_fuzz ${i})
  endforeach()
  add_test(NAME remote_oom_precheck COMMAND test_remote_oom 1)
  add_test(NAME remote_oom_precheck_v2 COMMAND test_remote_oom 4)
  add_test(NAME remote_oom_creation COMMAND test_remote_oom 2)
  foreach(t remote_render remote_rf remote_themes remote_colors)
    target_compile_definitions(test_${t} PRIVATE
      HP_PAGES_JSON="${CMAKE_SOURCE_DIR}/tests/vectors/pages.json"
      HP_RENDER_JSON="${CMAKE_SOURCE_DIR}/tests/vectors/remote_render.json"
      HP_THEME_RENDER_JSON="${CMAKE_SOURCE_DIR}/tests/vectors/remote_render_themes.json"
      HP_COLORS_JSON="${CMAKE_SOURCE_DIR}/tests/vectors/remote_render_colors.json")
  endforeach()
endif()

# No colour literal may live in remote/app outside theme.c / theme_presets.c.
add_test(NAME theme_literals COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/test_theme_literals.sh ${CMAKE_SOURCE_DIR}/remote/app)

# Theme tables vs theme.h and the shared integer-derivation vectors.
if(TARGET hp_remote_app AND NOT CMAKE_CROSSCOMPILING)
  add_executable(test_theme test_theme.c)
  target_link_libraries(test_theme PRIVATE hp_remote_app hp_hub cjson)
  add_test(NAME theme COMMAND test_theme ${CMAKE_SOURCE_DIR}/tests/vectors/themes.json)
endif()

# Host tool: dump the remote UI's reference screens as PNGs (build/tests/c/dump_remote_png <outdir>).
if(TARGET hp_remote_app AND NOT CMAKE_CROSSCOMPILING)
  add_executable(dump_remote_png dump_remote_png.c)
  target_link_libraries(dump_remote_png PRIVATE hp_remote_app hp_hub)
endif()

# Signed OTA images: SHA-512/Ed25519 vectors, verify rejections, hub staged apply (host fs only).
add_hp_test(ota)
add_hp_test(hub_ota)

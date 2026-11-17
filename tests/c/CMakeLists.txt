# One executable per module: add_hp_test(name) builds test_<name>.c.
function(add_hp_test name)
  add_executable(test_${name} test_${name}.c)
  target_link_libraries(test_${name} PRIVATE hp_hub)
  add_test(NAME ${name} COMMAND test_${name})
endfunction()

add_hp_test(ws)
add_hp_test(jsonrpc)
add_hp_test(mapping)
add_hp_test(sched)
add_hp_test(irout)
add_hp_test(activity)
add_hp_test(config2)
add_hp_test(rf_payload)
add_hp_test(rf_pair)
add_hp_test(link)
add_hp_test(rf_spi)
add_hp_test(mdns)
add_hp_test(hwkeys)

# RF object codec: golden vectors shared with the remote build (argv[1] = frames.json).
add_executable(test_framing test_framing.c)
target_link_libraries(test_framing PRIVATE hp_hub)
add_test(NAME framing COMMAND test_framing ${CMAKE_SOURCE_DIR}/tests/vectors/rf/frames.json)

add_hp_test(page)
target_compile_definitions(test_page PRIVATE HP_VECTORS="${CMAKE_SOURCE_DIR}/tests/vectors/pages.json")

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
  foreach(t remote_input remote_power remote_render remote_app)
    add_executable(test_${t} test_${t}.c)
    target_link_libraries(test_${t} PRIVATE hp_remote_app hp_hub)
    add_test(NAME ${t} COMMAND test_${t})
  endforeach()
  target_compile_definitions(test_remote_render PRIVATE
    HP_PAGES_JSON="${CMAKE_SOURCE_DIR}/tests/vectors/pages.json"
    HP_RENDER_JSON="${CMAKE_SOURCE_DIR}/tests/vectors/remote_render.json")
endif()

# One executable per module: add_hp_test(name) builds test_<name>.c.
function(add_hp_test name)
  add_executable(test_${name} test_${name}.c)
  target_link_libraries(test_${name} PRIVATE hp_hub)
  add_test(NAME ${name} COMMAND test_${name})
endfunction()

add_hp_test(framing)
add_hp_test(ws)
add_hp_test(jsonrpc)
add_hp_test(mapping)
add_hp_test(sched)
add_hp_test(irout)
add_hp_test(activity)
add_hp_test(config2)
add_hp_test(rf_payload)
add_hp_test(rf_pair)

add_executable(gen_rf_vectors gen_rf_vectors.c)
target_link_libraries(gen_rf_vectors PRIVATE hp_hub)

add_hp_test(page)
target_compile_definitions(test_page PRIVATE HP_VECTORS="${CMAKE_SOURCE_DIR}/tests/vectors/pages.json")

# IR encoder: vectors dir passed as argv[1] (shared with tests/py/test_ir_vectors.py).
add_executable(test_ir_encode test_ir_encode.c)
target_link_libraries(test_ir_encode PRIVATE hp_hub)
add_test(NAME ir_encode COMMAND test_ir_encode ${CMAKE_SOURCE_DIR}/tests/vectors/ir)

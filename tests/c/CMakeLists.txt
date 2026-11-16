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
add_hp_test(rf_payload)
add_hp_test(rf_pair)

add_executable(gen_rf_vectors gen_rf_vectors.c)
target_link_libraries(gen_rf_vectors PRIVATE hp_hub)

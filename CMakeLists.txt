cmake_minimum_required(VERSION 3.28)
project(ha_harmony C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(HP_SANITIZE "Build with ASan+UBSan" ON)

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

# Hub code library; later tasks append sources here.
add_library(hp_hub STATIC
  proto/gen/harmony_proto.c
  hub/rf/framing.c
  hub/rf/rf_payload.c
  hub/net/ws.c
  hub/net/jsonrpc.c
  hub/engine/mapping.c
  hub/io/sim_sinks.c
  hub/io/rf_pair.c
  hub/hub_handlers.c
)
target_include_directories(hp_hub PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(hp_hub PUBLIC cjson m)

add_executable(hub hub/main.c)
target_link_libraries(hub PRIVATE hp_hub)

add_executable(remote_sim remote/sim/remote_sim.c)
target_link_libraries(remote_sim PRIVATE hp_hub)

enable_testing()
add_subdirectory(tests/c)

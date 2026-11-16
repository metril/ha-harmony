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

# Hub code library; later tasks append sources here.
add_library(hp_hub STATIC
  proto/gen/harmony_proto.c
  hub/rf/framing.c
)
target_include_directories(hp_hub PUBLIC ${CMAKE_SOURCE_DIR})

enable_testing()
add_subdirectory(tests/c)

# Hub: Atheros AR9344 (MIPS 74Kc, mips32r2, big-endian), uClibc, static.
# Toolchain root: -DHP_TOOLCHAIN_ROOT=... or env HP_TOOLCHAIN_ROOT
# (Bootlin mips32--uclibc--stable-2025.08-1; hard-float libc, code never uses FP).
if(NOT HP_TOOLCHAIN_ROOT)
  set(HP_TOOLCHAIN_ROOT "$ENV{HP_TOOLCHAIN_ROOT}")
endif()
if(NOT HP_TOOLCHAIN_ROOT)
  message(FATAL_ERROR "set HP_TOOLCHAIN_ROOT (env or -D) to the extracted toolchain")
endif()
set(HP_TOOLCHAIN_ROOT "${HP_TOOLCHAIN_ROOT}" CACHE PATH "Cross toolchain root")

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR mips)
set(HP_TARGET hub CACHE STRING "Cross target (hub|remote)")
set(CMAKE_C_COMPILER "${HP_TOOLCHAIN_ROOT}/bin/mips-linux-gcc")
set(CMAKE_SYSROOT "${HP_TOOLCHAIN_ROOT}/mips-buildroot-linux-uclibc/sysroot")
set(CMAKE_C_FLAGS_INIT "-march=mips32r2 -mtune=74kc")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")

set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

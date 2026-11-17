# Remote: TI DM365 (ARM926EJ-S, armv5te, soft-float), musl, static.
# MUST be musl: the Elite runs Linux 2.6.37, and uClibc-ng 1.0.54 calls statx and
# clock_gettime64 with no fallback, so stat/opendir/clock_gettime fail with ENOSYS.
# musl retries the old syscalls on ENOSYS. Verified on the device 2026-11-17.
# Toolchain root: -DHP_TOOLCHAIN_ROOT=... or env HP_TOOLCHAIN_ROOT
# (Bootlin armv5-eabi--musl--stable-2025.08-1).
if(NOT HP_TOOLCHAIN_ROOT)
  set(HP_TOOLCHAIN_ROOT "$ENV{HP_TOOLCHAIN_ROOT}")
endif()
if(NOT HP_TOOLCHAIN_ROOT)
  message(FATAL_ERROR "set HP_TOOLCHAIN_ROOT (env or -D) to the extracted toolchain")
endif()
set(HP_TOOLCHAIN_ROOT "${HP_TOOLCHAIN_ROOT}" CACHE PATH "Cross toolchain root")

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(HP_TARGET remote CACHE STRING "Cross target (hub|remote)")
set(CMAKE_C_COMPILER "${HP_TOOLCHAIN_ROOT}/bin/arm-linux-gcc")
set(CMAKE_SYSROOT "${HP_TOOLCHAIN_ROOT}/arm-buildroot-linux-musleabi/sysroot")
if(NOT EXISTS "${CMAKE_SYSROOT}")
  message(FATAL_ERROR "remote toolchain must be musl (no ${CMAKE_SYSROOT}); uClibc-ng "
                      "binaries fail with ENOSYS on the Elite's 2.6.37 kernel. "
                      "Run tools/fetch_toolchains.sh remote.")
endif()
set(CMAKE_C_FLAGS_INIT "-march=armv5te -mtune=arm926ej-s")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")

set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

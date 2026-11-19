# Hub: Atheros AR9330 (MIPS 24Kc, mips32r2, big-endian), NO FPU and no kernel FP
# emulation, so everything (libc included) must be soft-float: any FP opcode is
# SIGILL. Static musl.
# Toolchain root: -DHP_TOOLCHAIN_ROOT=... or env HP_TOOLCHAIN_ROOT
# (OpenWrt 24.10.0 ath79 SDK toolchain, gcc 13.3 musl mips_24kc; see
# tools/fetch_toolchains.sh). Export STAGING_DIR=$HP_TOOLCHAIN_ROOT to silence its
# "STAGING_DIR not defined" warning.
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
set(CMAKE_C_COMPILER "${HP_TOOLCHAIN_ROOT}/bin/mips-openwrt-linux-musl-gcc.bin")
set(CMAKE_STRIP "${HP_TOOLCHAIN_ROOT}/bin/mips-openwrt-linux-musl-strip")
set(CMAKE_SYSROOT "${HP_TOOLCHAIN_ROOT}")
set(CMAKE_C_FLAGS_INIT "-march=mips32r2 -mtune=24kc -msoft-float -mno-branch-likely")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")

set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

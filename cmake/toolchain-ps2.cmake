# ---------------------------------------------------------------------------
# CMake toolchain file for the PlayStation 2 EE (MIPS R5900).
#
# Based on ps2dev's share/ps2dev.cmake, modernized for CMake 3.20+ / 4.x:
#   - CMakeForceCompiler was removed in CMake 4.0; replaced with
#     CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY.
#   - Also usable with ninja (no binutils fixups needed).
#
# v1.0 by the upstream ps2dev authors; this is a derived version.
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.20)

if(DEFINED ENV{PS2SDK})
    set(PS2SDK $ENV{PS2SDK})
else()
    message(FATAL_ERROR "The environment variable PS2SDK needs to be defined.")
endif()

if(DEFINED ENV{PS2DEV})
    set(PS2DEV $ENV{PS2DEV})
else()
    message(FATAL_ERROR "The environment variable PS2DEV needs to be defined.")
endif()

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_VERSION 1)
set(CMAKE_SYSTEM_PROCESSOR mips)

# Avoid the compiler-detection link step (replaces CMakeForceCompiler).
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_C_COMPILER mips64r5900el-ps2-elf-gcc)
set(CMAKE_CXX_COMPILER mips64r5900el-ps2-elf-g++)

set(EE_CFLAGS "-I${PS2SDK}/ee/include -I${PS2SDK}/common/include -I${PS2DEV}/gsKit/include -I${PS2SDK}/ports/include -D_EE -DPS2 -D__PS2__ -G0" CACHE STRING "EE C compiler flags" FORCE)
set(EE_LDFLAGS "-L${PS2SDK}/ee/lib -L${PS2DEV}/gsKit/lib -L${PS2SDK}/ports/lib -Wl,-zmax-page-size=128 -T${PS2SDK}/ee/startup/linkfile" CACHE STRING "EE linker flags" FORCE)

set(CMAKE_C_FLAGS_INIT ${EE_CFLAGS})
set(CMAKE_CXX_FLAGS_INIT ${EE_CFLAGS})
set(CMAKE_EXE_LINKER_FLAGS_INIT ${EE_LDFLAGS})

set(CMAKE_FIND_ROOT_PATH ${PS2DEV} ${PS2SDK} ${PS2SDK}/ports)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

find_program(PKG_CONFIG_EXECUTABLE NAMES mips64r5900el-ps2-elf-pkg-config HINTS "$ENV{PS2SDK}/bin" REQUIRED)

set(PS2 TRUE)
set(PLATFORM_PS2 TRUE)
set(EE TRUE)

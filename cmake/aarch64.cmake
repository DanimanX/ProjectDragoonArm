# cmake/aarch64.cmake — cross-compilation for the RG34XX (Allwinner H700).
#
# Uses Zig as a hermetic C/C++ compiler targeting aarch64-linux-gnu.2.28, the
# same toolchain pin ProjectDural uses. Zig's sysroot has no SDL3/EGL/GLES
# libraries, which is expected: the port links against generated stub libraries
# and loads the firmware's real ones at runtime (SDL3 directly, EGL/GLESv2 by
# dlopen from platform/arm64/bgfx/bgfx_gles.cpp).

set(PD_AARCH64 ON CACHE BOOL "" FORCE)

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(PD_ZIG_ROOT "$ENV{PD_ZIG_ROOT}" CACHE PATH "Directory containing the zig binary")
if(NOT PD_ZIG_ROOT)
  set(PD_ZIG_ROOT "${CMAKE_CURRENT_LIST_DIR}/../deps/zig")
endif()
set(PD_ZIG "${PD_ZIG_ROOT}/zig")
if(NOT EXISTS "${PD_ZIG}")
  message(FATAL_ERROR "zig not found at ${PD_ZIG} (run tools/build-arm64.sh, or set PD_ZIG_ROOT)")
endif()

set(PD_AARCH64_TARGET "aarch64-linux-gnu.2.28" CACHE STRING "Zig target triple")
set(PD_STUB_INCLUDE "${CMAKE_CURRENT_LIST_DIR}/../build-arm64/stubs/include" CACHE PATH "Stub headers")
set(PD_STUB_LIB "${CMAKE_CURRENT_LIST_DIR}/../build-arm64/stubs/lib" CACHE PATH "Stub libraries")

set(CMAKE_C_COMPILER "${PD_ZIG}" CACHE FILEPATH "" FORCE)
set(CMAKE_CXX_COMPILER "${PD_ZIG}" CACHE FILEPATH "" FORCE)
set(CMAKE_C_COMPILER_ARG1 "cc" CACHE STRING "" FORCE)
set(CMAKE_CXX_COMPILER_ARG1 "c++" CACHE STRING "" FORCE)

set(CMAKE_C_FLAGS_INIT "-target ${PD_AARCH64_TARGET} -mcpu=cortex_a53")
set(CMAKE_CXX_FLAGS_INIT "-target ${PD_AARCH64_TARGET} -mcpu=cortex_a53")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_SKIP_RPATH TRUE)

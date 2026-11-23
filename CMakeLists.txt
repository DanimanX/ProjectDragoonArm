# ProjectDragoonArm — Panzer Dragoon Saga, ARM64 / OpenGL ES 3.x
#
# Two build shapes from one tree:
#   cmake -S . -B build-host                    # host x86_64, SDL3 + GLES (dev)
#   cmake -S . -B build-arm64 \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64.cmake   # RG34XX / PortMaster
#
# The engine (engine/AzelLib) is vendored unmodified except for two documented
# edits: the shader table fix in shaders/embeddedShadersMacro.h and the two
# host-only renderer backends excluded below, which platform/arm64 replaces.

cmake_minimum_required(VERSION 3.20)
project(PanzerDragoonArm LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(PD_SHIPPING "Compile out engine debug windows (SHIPPING_BUILD)" ON)
option(PD_TESTS "Build the port's own self-checks" ON)

set(PD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
set(ENGINE ${PD_ROOT}/engine)
set(TP ${ENGINE}/third_party)

# ---------------------------------------------------------------------------
# compiler expectations
# ---------------------------------------------------------------------------
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wno-unused-parameter -Wno-sign-compare
                      -Wno-unused-but-set-variable -Wno-unused-variable
                      -Wno-unused-function -Wno-attributes -Wno-missing-field-initializers
                      -fno-strict-aliasing)
  add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>)
endif()

if(PD_SHIPPING)
  add_compile_definitions(SHIPPING_BUILD)
endif()

# ---------------------------------------------------------------------------
# engine sources
# ---------------------------------------------------------------------------
# Excluded, and replaced by platform/arm64:
#   renderer/renderer_gl.cpp  (bgfx + its example ImGui renderer)
#   renderer/renderer_vk.cpp  (Vulkan backend, desktop only)
#   imguiBGFX.cpp             (bgfx example ImGui bridge; compat/ supplies the header)
file(GLOB_RECURSE ENGINE_SOURCES CONFIGURE_DEPENDS ${ENGINE}/AzelLib/*.cpp)
list(FILTER ENGINE_SOURCES EXCLUDE REGEX
     "renderer/renderer_gl\\.cpp$|renderer/renderer_vk\\.cpp$|imguiBGFX\\.cpp$")

file(GLOB PLATFORM_SOURCES CONFIGURE_DEPENDS ${PD_ROOT}/platform/arm64/*.cpp)
file(GLOB BGFX_SOURCES CONFIGURE_DEPENDS ${PD_ROOT}/platform/arm64/bgfx/*.cpp)

set(IMGUI_SOURCES
  ${TP}/imgui/imgui.cpp
  ${TP}/imgui/imgui_draw.cpp
  ${TP}/imgui/imgui_tables.cpp
  ${TP}/imgui/imgui_widgets.cpp
  ${TP}/imgui/backends/imgui_impl_sdl3.cpp
  ${TP}/imgui/backends/imgui_impl_opengl3.cpp
)

file(GLOB_RECURSE SOLOUD_SOURCES CONFIGURE_DEPENDS
     ${TP}/soloud/soloud/src/audiosource/*.cpp
     ${TP}/soloud/soloud/src/core/*.cpp
     ${TP}/soloud/soloud/src/filter/*.cpp)
# OpenMPT support needs libopenmpt, which the port does not use; the engine only
# asks SoLoud for Ogg music and PCM voices.
list(FILTER SOLOUD_SOURCES EXCLUDE REGEX "audiosource/openmpt/")
list(APPEND SOLOUD_SOURCES ${TP}/soloud/soloud/src/audiosource/wav/stb_vorbis.c)
# SoLoud backend: SDL3 (same audio device the rest of the port uses, and it is
# the only one that works on both KMSDRM handhelds and the host).
list(APPEND SOLOUD_SOURCES
  ${TP}/soloud/soloud/src/backend/sdl3_static/soloud_sdl3_static.cpp)

set(AOSDK_SOURCES
  ${TP}/aosdk/eng_ssf/m68kcpu.c
  ${TP}/aosdk/eng_ssf/m68kopac.c
  ${TP}/aosdk/eng_ssf/m68kopdm.c
  ${TP}/aosdk/eng_ssf/m68kopnz.c
  ${TP}/aosdk/eng_ssf/m68kops.c
  ${TP}/aosdk/eng_ssf/sat_hw.c
  ${TP}/aosdk/eng_ssf/scsp.c
  ${TP}/aosdk/eng_ssf/scspdsp.c
)

add_executable(panzerdragoon
  ${ENGINE_SOURCES}
  ${PLATFORM_SOURCES}
  ${BGFX_SOURCES}
  ${IMGUI_SOURCES}
  ${SOLOUD_SOURCES}
  ${AOSDK_SOURCES}
  ${ENGINE}/PDS/PDS.cpp
)

target_include_directories(panzerdragoon PRIVATE
  ${PD_ROOT}/platform/arm64/compat      # imguiBGFX.h replacement
  ${ENGINE}/AzelLib
  ${TP}/imgui
  ${TP}/imgui/backends
  ${TP}/soloud/soloud/include
  ${TP}/glm
  ${TP}/aosdk
  ${TP}/tracy/public
  ${PD_ROOT}/platform/arm64/bgfx        # bgfx/bgfx.h, bx/*
  ${TP}/sdl3                            # vendored SDL3 headers (<SDL3/SDL.h>)
  ${TP}/sdl3/SDL3                       # SoLoud's SDL3 backend uses <SDL.h>
  ${TP}/gl-headers                      # Khronos GLES2/GLES3/EGL headers
)

# Tracy is header-only in this configuration (TRACY_ENABLE undefined).
target_compile_definitions(panzerdragoon PRIVATE
  BX_CONFIG_DEBUG=0
  IMGUI_IMPL_OPENGL_ES3
  WITH_SDL3_STATIC=1
  WITH_STB_VORBIS=1
  WITH_WAV=1
  WITH_NOISE=1
)

# ---------------------------------------------------------------------------
# platform dependencies
# ---------------------------------------------------------------------------
if(PD_AARCH64)
  message(STATUS "cross-compiling for aarch64 (${PD_AARCH64_TARGET})")
  # Device builds load the firmware's libSDL3/libEGL/libGLESv2 at runtime.
  # See cmake/aarch64.cmake for the stub libraries.
  target_include_directories(panzerdragoon PRIVATE ${PD_STUB_INCLUDE})
  target_link_directories(panzerdragoon PRIVATE ${PD_STUB_LIB})
  # Stub-linked: the real SDL3/GLESv2/EGL come from the firmware at runtime
  # (the shim also dlopens EGL/GLESv2 itself). The stubs only satisfy the
  # linker; their SONAMEs make the loader bind the firmware libraries.
  target_link_libraries(panzerdragoon PRIVATE SDL3 GLESv2 dl m pthread)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
  target_link_libraries(panzerdragoon PRIVATE PkgConfig::SDL3)
  # SoLoud's SDL3 backend includes <SDL.h> (not <SDL3/SDL.h>)

  find_package(OpenGL REQUIRED COMPONENTS OpenGL EGL)
  target_link_libraries(panzerdragoon PRIVATE OpenGL::OpenGL OpenGL::EGL dl m pthread)
endif()

# ---------------------------------------------------------------------------
# self-check: proves the GLES bgfx layer rasterises, without engine code
# ---------------------------------------------------------------------------
if(PD_TESTS)
  add_executable(shim_smoketest
    tests/shim_smoketest.cpp
    ${BGFX_SOURCES})
  target_include_directories(shim_smoketest PRIVATE ${PD_ROOT}/platform/arm64/bgfx)
  if(NOT PD_AARCH64)
    target_link_libraries(shim_smoketest PRIVATE PkgConfig::SDL3 dl m pthread)
    target_include_directories(shim_smoketest PRIVATE ${SDL3_INCLUDE_DIRS}/SDL3)
  else()
    target_include_directories(shim_smoketest PRIVATE ${PD_STUB_INCLUDE})
    target_link_directories(shim_smoketest PRIVATE ${PD_STUB_LIB})
    target_link_libraries(shim_smoketest PRIVATE SDL3 GLESv2 dl m pthread)
  endif()
endif()

# ---------------------------------------------------------------------------
# install layout (PortMaster)
# ---------------------------------------------------------------------------
install(TARGETS panzerdragoon RUNTIME DESTINATION .)
install(FILES ${PD_ROOT}/portmaster/panzerdragoon/port.json
              ${PD_ROOT}/portmaster/panzerdragoon/gameinfo.xml
        DESTINATION . OPTIONAL)

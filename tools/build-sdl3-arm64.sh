#!/usr/bin/env bash
# build-sdl3-arm64.sh — build libSDL3.so.0 for the handheld (AArch64, glibc 2.28).
#
# Why this exists: the port links against SDL3 through link-time stubs and loads
# the real library at runtime (see cmake/aarch64.cmake). On the RG34XX running
# Knulli the firmware provides SDL2 only — it has no libSDL3 at all, and it has
# neither libdrm nor libgbm, so SDL3's KMSDRM backend cannot be used. What the
# firmware does have is libmali plus a framebuffer, which is why its own SDL2
# ships a "Mali EGL" video driver.
#
# So we build SDL3 ourselves with the backends that firmware can actually serve:
#   video:    dummy + offscreen  (the port takes its GLES context straight from
#             libmali's EGL over /dev/fb0; see platform/arm64/renderer_gles.cpp)
#   audio:    alsa, loaded dynamically (the firmware has libasound.so.2)
#   joystick: linux evdev, no libudev/libdbus needed
# No X11, Wayland, KMSDRM, Vulkan, HIDAPI or udev dependencies are compiled in,
# so the result needs only libc/libm/libpthread/libdl — all firmware-provided.
#
# Usage:
#   tools/build-sdl3-arm64.sh          # download (once) + build
#   SDL3_VER=3.4.20 tools/build-sdl3-arm64.sh
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"

SDL3_VER="${SDL3_VER:-3.4.20}"
SDL3_SHA="${SDL3_SHA:-f3bb06f55bed9557227a700af2ea62e49497ebdf58037732a731a5fb3ef8581f}"
SRC_TGZ="$ROOT/deps/dl/SDL3-$SDL3_VER.tar.gz"
SRC="$ROOT/deps/sdl3-src/SDL3-$SDL3_VER"
BDIR="$ROOT/build-sdl3"
ZIG="$ROOT/deps/zig/zig"

if [ ! -x "$ZIG" ]; then
    echo "zig not found at $ZIG — run tools/build-arm64.sh first" >&2
    exit 1
fi

if [ ! -d "$SRC" ]; then
    echo "[1/3] fetching SDL3 $SDL3_VER"
    mkdir -p "$ROOT/deps/dl"
    if [ ! -f "$SRC_TGZ" ]; then
        curl -fsSL --retry 3 -o "$SRC_TGZ" \
            "https://github.com/libsdl-org/SDL/releases/download/release-$SDL3_VER/SDL3-$SDL3_VER.tar.gz"
    fi
    echo "$SDL3_SHA  $SRC_TGZ" | sha256sum -c --quiet
    rm -rf "$ROOT/deps/sdl3-src"
    mkdir -p "$ROOT/deps/sdl3-src"
    tar -C "$ROOT/deps/sdl3-src" -xzf "$SRC_TGZ"
fi

# Device patch: the RG34XX pad reports RX/RY and no ABS_X/ABS_Y, which SDL's
# evdev classifier rejected — the port ran with zero controllers. Idempotent:
# applying twice would fail, so skip when the marker is already present.
JSTICK="$SRC/src/core/linux/SDL_evdev_capabilities.c"
if [ -f "$JSTICK" ] && ! grep -q 'Accept either pair' "$JSTICK"; then
    echo "[1b/3] patching SDL joystick classification"
    patch -p1 -s -d "$SRC" < "$ROOT/patches/sdl3-linux-joystick-rx-ry.patch"
fi

echo "[2/3] configuring"
cmake -S "$SRC" -B "$BDIR" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/aarch64.cmake" \
    -DPD_ZIG_ROOT="$ROOT/deps/zig" \
    -DCMAKE_BUILD_TYPE=Release \
    -DSDL_UNIX_CONSOLE_BUILD=ON \
    -DSDL_SHARED=ON -DSDL_STATIC=OFF \
    -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_TEST_LIBRARY=OFF \
    -DSDL_X11=OFF -DSDL_WAYLAND=OFF -DSDL_KMSDRM=OFF \
    -DSDL_DUMMY=ON -DSDL_OFFSCREEN=ON \
    -DSDL_OPENGL=OFF -DSDL_OPENGLES=OFF -DSDL_VULKAN=OFF \
    -DSDL_ALSA=ON -DSDL_ALSA_SHARED=ON \
    -DSDL_PULSEAUDIO=OFF -DSDL_PIPEWIRE=OFF -DSDL_JACK=OFF \
    -DSDL_SNDIO=OFF -DSDL_DISKAUDIO=ON \
    -DSDL_CAMERA=OFF -DSDL_JOYSTICK=ON -DSDL_HAPTIC=ON -DSDL_HIDAPI=OFF \
    -DSDL_LIBUDEV=OFF -DSDL_DBUS=OFF -DSDL_WERROR=OFF >/dev/null

echo "[3/3] compiling"
ninja -C "$BDIR" 2>&1 | tail -3

SO="$BDIR/libSDL3.so.0.4.20"
[ -f "$SO" ] || { echo "missing $SO" >&2; exit 1; }
echo
echo "built $SO"
echo "  needed:  $(readelf -d "$SO" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' | tr '\n' ' ')"
echo "  max GLIBC: $(readelf -V "$SO" | grep -oE 'GLIBC_[0-9.]+' | sort -V | tail -1)"

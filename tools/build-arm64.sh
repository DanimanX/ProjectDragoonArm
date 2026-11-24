#!/usr/bin/env bash
# build-arm64.sh — cross-compile ProjectDragoonArm for the Anbernic RG34XX
# (Allwinner H700, Cortex-A53, Mali-G31 MP2) and assemble a PortMaster package.
#
# Toolchain: Zig 0.17.0 as a hermetic aarch64-linux-gnu.2.28 C/C++ compiler,
# the same pin ProjectDural uses (sha256 verified below).
#
# The device supplies the Mali EGL/GLES drivers but **not** SDL3: Knulli/Batocera
# on the H700 ships SDL2 (with its own "Mali EGL" fbdev driver) and has no
# libSDL3, libdrm or libgbm. Zig's sysroot has none of it either, so this script
# generates a stub libSDL3 to link against (see tools/make-stubs.py) and the
# package carries a real libSDL3.so.0 built by tools/build-sdl3-arm64.sh; EGL
# and GLESv2 are loaded with dlopen at runtime.
#
# Usage:
#   tools/build-arm64.sh              # build + package
#   PD_JOBS=8 tools/build-arm64.sh
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"

ZIG_VER="0.17.0"
ZIG_SHA="1cbe9df9f27e6b78d14ccbca43b6703a404ef79ef1c463de901d7f088d4e2026"
ZIG="$ROOT/deps/zig/zig"
BDIR="$ROOT/build-arm64"
STUB_LIB="$BDIR/stubs/lib"
STUB_INC="$BDIR/stubs/include"
DIST="$ROOT/dist/PanzerDragoonArm-PortMaster-aarch64"
ZIP="$ROOT/dist/PanzerDragoonArm-PortMaster-aarch64.zip"
JOBS="${PD_JOBS:-$(nproc)}"

# --- 1. toolchain ----------------------------------------------------------
if [ ! -x "$ZIG" ]; then
    echo "[1/5] fetching Zig $ZIG_VER"
    mkdir -p "$ROOT/deps/dl"
    if [ ! -f "$ROOT/deps/dl/zig.tar.xz" ]; then
        curl -fsSL --retry 3 -o "$ROOT/deps/dl/zig.tar.xz" \
            "https://ziglang.org/download/$ZIG_VER/zig-x86_64-linux-$ZIG_VER.tar.xz"
    fi
    echo "$ZIG_SHA  $ROOT/deps/dl/zig.tar.xz" | sha256sum -c --quiet
    tar -C "$ROOT/deps" -xJf "$ROOT/deps/dl/zig.tar.xz"
    rm -rf "$ROOT/deps/zig"
    mv "$ROOT/deps/zig-x86_64-linux-$ZIG_VER" "$ROOT/deps/zig"
fi
"$ZIG" version

# --- 2. configure ----------------------------------------------------------
echo "[2/5] configuring ($JOBS jobs)"
mkdir -p "$STUB_LIB" "$STUB_INC"
cmake -S "$ROOT" -B "$BDIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/aarch64.cmake" \
    -DPD_ZIG_ROOT="$ROOT/deps/zig" \
    -DPD_STUB_INCLUDE="$STUB_INC" \
    -DPD_STUB_LIB="$STUB_LIB" \
    -DPD_SHIPPING=ON >/dev/null

# --- 3. compile ------------------------------------------------------------
echo "[3/5] compiling for aarch64"
# Objects first: the link needs a stub libSDL3, which is generated from the
# symbols the objects actually reference. -k 0 keeps going past the expected
# link failure so every object is available for the scan.
ninja -C "$BDIR" -j "$JOBS" -k 0 panzerdragoon >"$BDIR/compile.log" 2>&1 || true
# Only real compilation errors are fatal here. The first link is expected to fail
# with "undefined symbol: SDL_*" because the stub library does not exist yet —
# that is the whole point of compiling the objects first.
if grep -E '(fatal )?error:' "$BDIR/compile.log" | grep -qvE '^(ld\.lld|/usr/bin/ld|collect2):? error:'; then
    grep -E '(fatal )?error:' "$BDIR/compile.log" | grep -vE '^(ld\.lld|/usr/bin/ld|collect2):? error:' | head -20
    echo "compile failed" >&2
    exit 1
fi

python3 "$ROOT/tools/make-stubs.py" \
    --objects "$BDIR" --lib "$STUB_LIB" --include "$STUB_INC" --zig "$ZIG"

echo "      linking"
ninja -C "$BDIR" -j "$JOBS" panzerdragoon 2>&1 | tail -5

# Strip debug info: 38 MB -> ~4.5 MB without changing the required GLIBC.
STRIP="$(command -v llvm-strip || command -v strip || true)"
if [ -n "$STRIP" ]; then
    cp "$BDIR/panzerdragoon" "$BDIR/panzerdragoon.stripped"
    "$STRIP" --strip-debug --strip-unneeded "$BDIR/panzerdragoon.stripped" 2>/dev/null || true
fi

BIN="$BDIR/panzerdragoon"
[ -f "$BIN" ] || { echo "missing $BIN" >&2; exit 1; }
file "$BIN"

# --- 4. assemble the PortMaster package -----------------------------------
echo "[4/5] assembling $DIST"
rm -rf "$DIST"
mkdir -p "$DIST/panzerdragoon/libs.aarch64"
if [ -f "$BDIR/panzerdragoon.stripped" ]; then
    install -m 0755 "$BDIR/panzerdragoon.stripped" "$DIST/panzerdragoon/panzerdragoon.aarch64"
else
    install -m 0755 "$BIN" "$DIST/panzerdragoon/panzerdragoon.aarch64"
fi
{
  echo "required shared libraries:"
  readelf -d "$BIN" | sed -n 's/.*Shared library: \[\(.*\)\]/  \1/p'
  echo "  (SDL3 ships in libs.aarch64; the rest are firmware-provided)"
  echo "max GLIBC symbol version:"
  readelf -V "$BIN" | grep -o 'GLIBC_[0-9.]*' | sort -V | tail -1 | sed 's/^/  /'
} > "$DIST/panzerdragoon/DEPENDENCIES.txt"

# The firmware on the H700 ships SDL2 only, so the port carries its own SDL3
# (built by tools/build-sdl3-arm64.sh with the backends that firmware can serve).
SDL3_SO="$ROOT/build-sdl3/libSDL3.so.0.4.20"
if [ -f "$SDL3_SO" ]; then
    install -m 0755 "$SDL3_SO" "$DIST/panzerdragoon/libs.aarch64/libSDL3.so.0"
    echo "      bundled $(basename "$SDL3_SO") as libs.aarch64/libSDL3.so.0"
else
    echo "      WARNING: build-sdl3/libSDL3.so.0.4.20 missing; the port will not start" >&2
    echo "      run tools/build-sdl3-arm64.sh and re-run this script" >&2
fi
install -m 0644 "$ROOT/portmaster/panzerdragoon/port.json" "$DIST/panzerdragoon/port.json"
install -m 0644 "$ROOT/portmaster/panzerdragoon/gameinfo.xml" "$DIST/panzerdragoon/gameinfo.xml" 2>/dev/null || true
install -m 0644 "$ROOT/portmaster/panzerdragoon/README.md" "$DIST/panzerdragoon/README.md" 2>/dev/null || true
install -m 0644 "$ROOT/README.md" "$DIST/panzerdragoon/README.md" 2>/dev/null || true
install -m 0755 "$ROOT/portmaster/Panzer Dragoon Saga.sh" "$DIST/Panzer Dragoon Saga.sh"
if [ -f "$ROOT/assets/projectdragoonarm-banner.jpg" ]; then
    install -m 0644 "$ROOT/assets/projectdragoonarm-banner.jpg" "$DIST/panzerdragoon/cover.jpg"
fi
mkdir -p "$DIST/panzerdragoon/data"
cat > "$DIST/panzerdragoon/PUT-YOUR-DISC-HERE.txt" <<'TXT'
Extract your own legally dumped Panzer Dragoon Saga (USA) disc set into
  panzerdragoon/data/disc1/   (and disc2/, disc3/, disc4/ for the others)

From a Linux/macOS machine with this repository:

  python3 tools/extract_disc.py --7z "Panzer Dragoon Saga (USA) (Disc 1).7z" \
                                --out panzerdragoon/data/disc1

The Disc 1 extraction is hash-verified against blythos/atolm's sha256 manifest,
so a bad dump or a truncated copy fails loudly instead of booting to black.
TXT

# --- 5. zip ---------------------------------------------------------------
echo "[5/5] packaging"
mkdir -p "$ROOT/dist"
( cd "$ROOT/dist" && rm -f "$(basename "$ZIP")" && zip -qr "$(basename "$ZIP")" "$(basename "$DIST")" )
ls -l "$ZIP"
echo "done: $ZIP"

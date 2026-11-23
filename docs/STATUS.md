# Status ledger

**Verified** = a command was run and its output inspected. **Pending** = designed
but not proven. Last updated: 2026-11-22.

## Verified

| Item | Evidence |
|---|---|
| Supplied dumps are retail USA | 14/14 PRG sha256 match atolm's Disc 1 manifest (`tools/extract_disc.py` hash gate) |
| Disc to engine data | 1,695 files, 534.2 MiB, 7.7 s from the `.7z` |
| Engine builds for x86_64 | `ninja -C build-host panzerdragoon`, exit 0 |
| Engine builds for AArch64 | every TU, `zig c++ -target aarch64-linux-gnu.2.28 -mcpu=cortex_a53` |
| **The game renders** | captured back buffer: `SEGA ENTERPRISES, LTD.` then the `AZEL` title card (`docs/images/boot-*.png`) |
| Frame rate | 30.3 FPS shown by the port's own overlay |
| In-game menu | opens over gameplay; display/audio/controls/about/exit (`docs/images/ingame-menu.png`) |
| GLES layer correctness | `shim_smoketest`: 76,800 / 76,800 pixels match the shader colour |
| Integer-texture sampling | same test, now with an `R8U` texture read by `texelFetch` |
| ARM64 package | `dist/PanzerDragoonArm-PortMaster-aarch64.zip` (1.9 MB) |
| Binary dependencies | `libSDL3.so.0`, `libGLESv2.so.2`, `libm`, `libc`, `libpthread`, `libdl` only |
| ABI floor | max symbol version `GLIBC_2.27` (target <= 2.28) |
| Binary size | 4.56 MB stripped, from 38 MB |

## Fixed during the port (see patches/README.md)

| # | Defect | Symptom |
|---|---|---|
| 1 | Integer textures created with `LINEAR` filtering | every VDP2 layer rendered black, no GL error |
| 2 | Task work areas allocated with `new T` (not zeroed) | SIGSEGV in `killTask` on a stale pointer |
| 3 | VDP2 VRAM masked with `0x1FFFFF` over a 512 KB array | subtitle text overwrote the task heap ~1.4 MB out of bounds |
| 4 | Stale embedded shader table (ESSL/GLSL commented out) | no shader found for any GL backend |

## Pending

- **Device run.** No RG34XX attached, and no emulation possible here:
  `qemu-user` is absent and cannot be installed (no root, `no-new-privs`);
  `qemu-system-aarch64` needs an ARM64 rootfs with Mesa. All evidence stops at
  the device boundary.
- On-device performance. 30.3 FPS is the *host* rate with llvmpipe; H700 numbers
  are unmeasured. The engine's own pacing targets 30 Hz.
- Audio. SoLoud's SDL3 backend compiles and initialises, but no audio output was
  captured in this environment.
- Discs 2-4. Only Disc 1 data is extracted and exercised; disc swap is untested.
- Input mapping. Keyboard and gamepad paths exist in the engine; no physical
  controller was exercised.
- Save/load. The engine's save screen exists; the port's storage path is untested.

## Inherited upstream limitations

Azel upstream is playable from the opening through the Arachnoth battle; later
discs and several Berserk effect producers are unfinished *in the engine*. Those
calls log `Unimplemented` and continue rather than failing. This port does not
change that.

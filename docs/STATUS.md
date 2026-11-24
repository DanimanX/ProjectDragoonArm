# Status ledger

**Verified** = a command was run and its output inspected. **Pending** = designed
but not proven. Last updated: 2026-11-24.

## Verified

| Item | Evidence |
|---|---|
| **It runs on the RG34XX** | Booted on the device (Knulli "scarab 2026/05/11", Mali-G31): `GL_VERSION: OpenGL ES 3.2 v1.r20p0-01rel0`, SEGA logo and the intro scene captured straight from `/dev/fb0` |
| Clean shutdown | `PD_EXIT_AFTER_MS=6000` leaves through `exit()`: `rc=0`, log ends with `[port] ordered shutdown` / `[port] shutdown complete`, no `Segmentation fault`. Stopping with SIGTERM (what EmulationStation does) also exits without a crash |
| Audio reaches the sink | captured from the PipeWire sink monitor with `parec` while Disc 1 plays: peak 18056, RMS ~3000 (SoLoud tone probe in the same graph: peak 6655). Measured after `pw-record --target <sink>` was found to return silence even for a known WAV, so it was discarded |
| Audio reaches the speaker | the codec boots with `SPK Switch` off (LINEOUT and both Output DAC mixers on), which is why a correct audio stream was inaudible. Enabling it produced audible tones on the device; the launcher now enables it for the session and restores the original value on exit |
| Presentation performance | 31 fps (32.2 ms/frame) measured on the device, up from 14.7 fps. The old path cost 35.5 ms/frame in `glReadPixels` alone; the port now presents by GPU (`eglSwapBuffers` on a Mali `fbdev_window`), 0.09 ms per swap |
| Tearing in cinematics | a captured frame held two different movie frames, top and bottom: the fbdev surface draws into the buffer the panel scans and the driver has no page flip (a 720x960 surface is rejected: `eglCreateWindowSurface` returns NULL). Fixed by rendering into an offscreen FBO and publishing it during vblank (`FBIO_WAITFORVSYNC`); consecutive captures now show single coherent frames. Present costs 6.7 ms, frame rate stays at ~30 fps |
| Input on the handheld | SDL enumerates the pad but delivers no `SDL_EVENT_GAMEPAD_*` and `SDL_GetGamepadButton()` returns 0 forever inside the game process, while `/dev/input/event1` reads correctly. The port now polls evdev itself (`pd_evdev_pad_mask`), verified end to end: A=0x0001, B=0x0002, C=0x0004, X=0x0800, L1=0x8000, R1=0x1000 |
| EmulationStation overlay | ES holds `/dev/fb0` open; the launcher now stops ES for the session and restores the exact frame it was showing before resuming it. Verified: clean game picture while playing (ES in state T), ES menu identical after exit (8,105 colours, same as before), ES back to state S |
| Movie-end crash | `closeMovieStream()` stopped a SoLoud voice from the main thread while the mixer thread was already reaping it (`hasEnded()` had flipped), a double free inside `stopVoice_internal`. The voice is now owned solely by SoLoud's audio thread; the handle is just dropped. Verified: `crash=0` and the movie state machine ran to completion (`state 3→4→5→6`) |
| CPK movies rendered as glyphs | the bitmap layer took its base from `MPOFN`, which addresses VRAM in 128 KB banks and yields 0 here, while the decoder writes frames at 0x25E14040 (computed from the video size). `MPOFN` cannot express that, so the layer read VRAM offset 0 — where `unpackGraphicsToVDP2` puts the font — and drew glyphs as graphics. Proven by dumping VDP2 VRAM mid-movie and decoding it as a 320x176 XRGB bitmap (stride 2048): coherent frame at 0x14040, font data at 0. After the fix, device captures of attract mode show real frames (a dragon, a rider) |
| Menu exit | the overlay wrote `bContinue = false`, but the hook runs inside `azelSdl_EndFrame()` and `loopIteration()` assigns `bContinue = azelSdl_EndFrame()` right after, discarding it. The menu now sets `closeApp`, which `EndFrame` derives its return from. Confirmed on the device |
| Adaptive vblank | `FBIO_WAITFORVSYNC` can block far longer than one refresh once the panel stops paging (measured: ~1 fps with the process idle, load 1.6/4 cores). The wait is now timed and disables itself past 50 ms; frame rate returns to ~30 fps |
| Renderer geometry | the engine sizes itself from `SDL_GetWindowSize()`; asking the `offscreen` driver for a fullscreen window made it adopt 1024x768, so a 720x480 panel showed an off-centre, cropped picture. Backbuffer is now 720x480 and the composite view is 720x540 as upstream computes it |
| Controller enumeration | `/dev/input/event1` is 'Anbernic RG34XX Controller' with ABS_RX/RY/RZ + HAT0 and no ABS_X/ABS_Y; SDL's evdev classifier required X/Y and enumerated zero pads. After the patch in `patches/`: `SDL_GetGamepads()` returns 1 |
| Device EGL behaviour (measured, not guessed) | all 25 configs report `surface_type == 0`; passing `EGL_SURFACE_TYPE` makes `eglChooseConfig` return `EGL_BAD_ATTRIBUTE`; `eglCreateWindowSurface(dpy, cfg, NULL, NULL)` succeeds; `eglSwapBuffers` returns `EGL_TRUE` but never reaches the panel |
| Presentation path | the port renders as usual and publishes by reading the frame back and writing it to the off-screen `/dev/fb0` page, then `FBIOPAN_DISPLAY` |
| SDL3 on this firmware | hint is `SDL_VIDEO_DRIVER` (SDL3), not SDL2's `SDL_VIDEODRIVER`; `offscreen` and `dummy` both initialise and give a 720x480 window (no GL) |
| Why the window matters | `engine/AzelLib/renderer.cpp:267` reads `SDL_GetWindowSize(gWindowBGFX, ...)`; with no window the resolution stays 0 and the composite draws nothing (black frame) |
| Supplied dumps are retail USA | 14/14 PRG sha256 match atolm's Disc 1 manifest (`tools/extract_disc.py` hash gate) |
| Disc to engine data | 1,695 files, 534.2 MiB, 7.7 s from the `.7z` |
| Engine builds for x86_64 | `ninja -C build-host panzerdragoon`, exit 0 |
| Engine builds for AArch64 | every TU, `zig c++ -target aarch64-linux-gnu.2.28 -mcpu=cortex_a53` |
| **The game renders** | captured back buffer: `SEGA ENTERPRISES, LTD.` then the `AZEL` title card (`docs/images/boot-*.png`) |
| Frame rate | 30.3 FPS shown by the port's own overlay |
| In-game menu | opens over gameplay; display/audio/controls/about/exit (`docs/images/ingame-menu.png`) |
| GLES layer correctness | `shim_smoketest`: 76,800 / 76,800 pixels match the shader colour |
| Integer-texture sampling | same test, now with an `R8U` texture read by `texelFetch` |
| ARM64 package | `dist/PanzerDragoonArm-PortMaster-aarch64.zip` (4.6 MB, now including `libSDL3.so.0`) |
| Firmware inventory (RG34XX, Knulli "scarab 2026/05/11") | squashfs `usr/lib` has SDL2 (with a "Mali EGL" fbdev driver), `libmali.so.0.20.0`, `libGLESv2.so.2`, `libEGL.so.1`, `libasound.so.2`; **no** `libSDL3`, `libdrm`, `libgbm`, X11 or Wayland |
| SD card deployment | `/userdata/roms/ports/Panzer Dragoon Saga.sh` + `panzerdragoon/` with discs 1-4 (1,696 / 1,943 / 2,219 / 1,952 files), all four trees byte-count matched and Disc 1 PRGs re-hashed after the copy |
| Binary dependencies | `libSDL3.so.0` (bundled in `libs.aarch64`), `libGLESv2.so.2`, `libm`, `libc`, `libpthread`, `libdl` |
| ABI floor | max symbol version `GLIBC_2.27` (target <= 2.28) |
| Binary size | 4.56 MB stripped, from 38 MB |

## Fixed during the port (see patches/README.md)

| # | Defect | Symptom |
|---|---|---|
| 1 | Integer textures created with `LINEAR` filtering | every VDP2 layer rendered black, no GL error |
| 2 | Task work areas allocated with `new T` (not zeroed) | SIGSEGV in `killTask` on a stale pointer |
| 3 | VDP2 VRAM masked with `0x1FFFFF` over a 512 KB array | subtitle text overwrote the task heap ~1.4 MB out of bounds |
| 4 | Stale embedded shader table (ESSL/GLSL commented out) | no shader found for any GL backend |
| 5 | Firmware has no SDL3 (Knulli ships SDL2 + a Mali fbdev EGL driver, no libdrm/libgbm) | `libSDL3.so.0: cannot open shared object file` at exec; no SDL video backend even if provided |

## Pending

- **The field does not render yet.** This is the one that matters. Note that
  earlier field measurements were partly invalid: `PD_FAKE_MASK` could only
  repeat one button, so it navigated into the *save menu* (visible in the task
  traces as `LoadSavegameScreen`, `saveMenuSubTask0`) rather than into the field.
  `PD_FAKE_SEQ` now scripts a press sequence for reproducible navigation.
  Recorded observations, still to be re-taken on a real playthrough:
  - The engine does advance: it creates the field tasks (`BattleManager`,
    `fieldDebugTaskWorkArea`), VDP1 command count grows from 4 to 23, and the
    movie state machine completes.
  - But a captured frame shows **text glyphs drawn as graphics**, the signature
    of a layer reading VRAM at the wrong address, and the movie's NBG0 bitmap
    layer is the prime suspect: `setNBG0MapOffset(0, 0x25E00000)` leaves the map
    offset at zero while the decoder writes frames at `0x25E14040`.
  - The *first* cinematic renders correctly and the *second* does not, with the
    same window registers (`w=320 h=176`, `WPSX0=32 WPSY0=24 WPEX0=670
    WPEY0=399`), so something differs between the two that has not been isolated
    yet.
- **Frame rate collapses to ~1 fps once the field is entered.** Profiled by
  phase on the device rather than guessed:

  | Phase | Cost |
  |---|---|
  | `EndFrame` (render + present) | 51-71 ms |
  | `present` (eglSwapBuffers) | 0.7 ms |
  | `vdp2` layers (BG0/BG1/BG3) | 26-63 ms |
  | `vdp1` | 0.2 ms |
  | composite | 0.2 ms |
  | `bgfx::frame()` | 2 ms |
  | `StartFrame`, tasks, audio | 0.0 ms |

  **Settled, after two wrong turns worth recording.**

  Wrong turn 1: per-pass CPU timers cannot attribute GPU cost, because GL
  commands queue — the stall appears wherever the CPU first waits. That made one
  profile blame BG0 and the next blame VDP1 for identical work.

  Wrong turn 2: adding a GPU drain *between* passes, and then reading the
  frame-start drain, made the GPU look idle (0.6 ms) while the game ran at
  7.4 fps. The drains had already emptied the queue. `PD_SYNC_PASSES` now gates
  them, so the two kinds of measurement cannot contaminate each other.

  Clean numbers, no inter-pass syncs, field reached with synthetic input:

  | Measurement | Value |
  |---|---|
  | CPU to issue a whole frame (vdp2+vdp1+composite+bgfx::frame) | ~4 ms |
  | GPU work per frame (frame-start `glFinish` drain) | 47-73 ms |
  | Frame interval at the low points | ~101 ms (9.9 fps) |

  **So the field is GPU-bound, and the CPU side is not the problem.**

  With `PD_SYNC_PASSES=1` (per-pass GPU cost, accepting that it serialises):
  BG0 13-15 ms, BG1 8.6 ms, BG3 7.4 ms, VDP1 30-65 ms, composite 0.6 ms.

  And the decisive experiment: rendering BG0 at **half resolution** lowered BG0
  to 4-7 ms *and also* BG1 to 2.5 ms and BG3 to 2.1 ms. One layer using fewer
  pixels sped up the others, which is the signature of shared **memory
  bandwidth**, not shader ALU. That also retires the earlier "integer division
  per pixel" theory: the compiled ESSL has no such chain (shaderc folded it to
  /256 and /255.0).

  Open: VDP1 is the largest single consumer (30-65 ms) while submitting only
  4-6 commands per frame, so its cost is fixed per-frame work, not command
  volume. Finding what that is — most likely a full-panel clear/draw or a
  full-size texture touched every frame — is the next step.

- The half-resolution A/B for BG0 did run cleanly in the end, and its result
  (halving one layer sped up the others) is what points at bandwidth.

- Disc swap. All four discs are deployed; only Disc 1 has been exercised.
- In-game audio balance and whether the mixer restore interferes with anything
  Knulli does after the port exits.
- Save/load. The engine's save screen exists; the port's storage path is
  untested on the device.

## Inherited upstream limitations

Azel upstream is playable from the opening through the Arachnoth battle; later
discs and several Berserk effect producers are unfinished *in the engine*. Those
calls log `Unimplemented` and continue rather than failing. This port does not
change that.

# Porting notes — how this port was reasoned about

Working notes from the port. They record what was measured, not what was
hoped. Numbers here come from commands that were actually run.

## 1. Disc identity comes from atolm, and it is a real gate

`blythos/atolm` publishes `config/targets/*.yaml` with size + sha256 for the
retail PRGs. Running `tools/extract.py` from a checkout of atolm against the
user's Disc 1 data track verified, in order:

```
extract: OK 1ST_READ.PRG (253650 bytes, sha256 2bb6c2d66ec17fe3…)
… 14/14 targets verified into extracted/
```

So the supplied dumps are the retail USA set, not a bad rip. `tools/extract_disc.py`
in this repository keeps that gate (it is the same 14 hashes) and runs it on
every extraction, so a truncated SMB copy or a wrong-region disc fails loudly.

Disc layout as supplied (7z): three tracks per disc — Track 1 is the
`MODE1/2352` data track carrying the ISO9660 payload (645,501,696 bytes for
Disc 1), Tracks 2–3 are CD audio. The engine needs the ISO9660 payload only;
CD-DA is handled separately.

## 2. atolm cannot be the thing that runs

atolm's own README is unambiguous:

> **Is not:** a playable reimplementation, an asset viewer/extractor, or a ROM
> host.

and its status table reports 122 matched bytes of 253,650 in `1ST_READ.PRG`,
with every town overlay ~90 bytes matched out of 50–421 KB. A port built on it
would have no game. Its value is the byte-identity manifests (see §1) and the
documented Saturn facts (load address 0x06006000, SHC codegen notes) that help
when reading the engine.

The engine that actually runs is `yaz0r/Azel` (MIT), which atolm credits and
whose `PDS/` entry point builds and boots. That is what `engine/` vendors.

## 3. The engine builds and boots — verified on x86_64

```
cmake -S engine -B build-host -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build-host          # 1675 targets, exit 0
cd data && ./PDS            # reaches the boot task graph
```

Observed boot log (headless Xvfb, OpenGL backend):

```
[pds] bgfx renderer: OpenGL 2.1
Created task 17s_warningWorkArea
Created task 28s_videoDecoderNotifyWorkArea
Created task 19s_movieMainWorkArea
Created task 22s_subtitleDataWorkArea
```

The task graph runs; the frame is black. Instrumenting the pipeline showed why
(§4), and that is the single most important finding for the port.

## 4. The presentation path needs GLES 3.0, and the failure is silent

Dumping Saturn memory 120 frames in (`PDS_DUMP_VRAM=<dir>`) proved the engine
is producing graphics:

```
vdp2_vram.bin   524288 bytes   ~24 KB non-zero
vdp2_cram.bin     4096 bytes   ~0.5 KB non-zero
vdp1_vram.bin   524288 bytes   zero (expected: intro movie is VDP2 NBG0)
BGON = 0x0009  (NBG0 + NBG3 enabled)
```

Reading back the rendered NBG0 layer texture gave **all-zero pixels**. The
cause is in the layer path: `renderLayerGPU()` uploads each plane's descriptor
as an `R32U` texture and the fragment shader reads it with `texelFetch`
(`usampler2D s_planeConfig`). Integer textures and `texelFetch` do not exist in
GL 2.1 / GLSL 120, which is what bgfx picks by default on Linux
(`BGFX_CONFIG_RENDERER_OPENGL` defaults to the minimum, 21). The texture
creation fails, the shader samples nothing, every fragment is discarded, and
nothing logs an error in a release build.

Consequences for the port, recorded so they are not rediscovered:

- Target **GLES 3.x**. Mali-G31 MP2 exposes GLES 3.2; this is fine on the
  H700, but a "GLES 2.0" assumption in the port design would be wrong.
- Embedded shaders already contain valid GLSL ES 3.0 source. Decoding
  `shaders/generated/essl/VDP2_ps.sc.bin.h` shows the blob is a bgfx header
  followed by plain text:
  `precision highp int; … out mediump vec4 bgfx_FragColor; uniform usampler2D
  s_VDP2_RAM; … texelFetch (s_planeConfig, ivec2(0, 0), 0)`. The `#version`
  line is not in the blob because bgfx prepends it at compile time.
- Therefore a GLES shim can compile those blobs directly by prepending
  `#version 300 es` — no shader compiler, no offline shader build, and the
  shader text stays byte-identical to what the engine was written against.

## 5. A stale shader-table header crashes the Vulkan path

`engine/AzelLib/shaders/embeddedShadersMacro.h` is a hand-copied, outdated
version of bgfx's own `embedded_shader.h`: it leaves the ESSL and GLSL entries
commented out and omits `Direct3D12`/`WebGPU`. With a renderer enum where
`Vulkan` is index 9 but the table only fills indices 2/5/9-shaped slots, the
lookup can return a null shader and `bgfx::createEmbeddedShader` dereferences
it — the SIGSEGV seen at startup on the Vulkan backend.

Fixed by restoring the upstream table shape and adding the ESSL/GLSL includes
(the engine already ships both generated sets, `generated/essl` and
`generated/glsl`). Kept in `patches/` so the change is visible against upstream.

## 6. Toolchain: Zig, not Docker, on this host

The Docker daemon is not running and `sudo` is blocked by the `no-new-privs`
kernel flag, so container-based toolchains (atolm's SHC images, cross builds)
are unavailable here. `qemu-user` (`qemu-aarch64`) is also not installed;
only `qemu-system-aarch64` is.

ProjectDural's approach works instead and is adopted unchanged:
**Zig 0.17.0 as a hermetic `aarch64-linux-gnu.2.28` cross compiler.**

```
zig-x86_64-linux-0.17.0.tar.xz
sha256 1cbe9df9f27e6b78d14ccbca43b6703a404ef79ef1c463de901d7f088d4e2026
```

That SHA matches ProjectDural's pin exactly, so the two ports share a
toolchain. Verified here: `zig cc -target aarch64-linux-gnu.2.28` produces
valid AArch64 ELF, and `-ldl` links for runtime `dlopen` of firmware libraries.
Zig's bundled sysroot has no `libEGL.so`/`libGLESv2.so`/SDL, which is expected
and correct: the handheld builds link against **stubs** and load the firmware's
real libraries at runtime (`libSDL2-2.0.so.0`, `libEGL.so.1`, `libGLESv2.so.2`),
exactly as ProjectDural/SegaRallyArm/DaytonaArm do.

## 7. bgfx API surface to reimplement

Enumerated with `grep -rho 'bgfx::[A-Za-z_0-9]*' engine/AzelLib` — ~50 entry
points, dominated by textures, uniforms, views and state:

```
TextureHandle/uniforms   createTexture2D, updateTexture2D, getTexture, destroy,
                         createUniform, setUniform, setTexture, createFrameBuffer,
                         createVertexBuffer, createIndexBuffer, allocTransient*
Views & submission       setViewRect, setViewClear, setViewScissor, setViewFrameBuffer,
                         setViewMode, setViewName, setViewTransform, touch, submit,
                         frame, reset
Shaders                  createEmbeddedShader, createProgram, EmbeddedShader,
                         getRendererType, getRendererName
State                    setState (WRITE_RGB/A, blend, depth test), setVertexBuffer,
                         setIndexBuffer, copy/makeRef, isValid
```

That is a bounded, implementable subset over GLES 3.0 — which is why the shim
was chosen over editing 5,000 lines of presentation code or shipping bgfx.

## 8. What the handheld needs that the desktop does not

Recorded from the sibling ports (ProjectDural, SegaRallyArm, DaytonaArm), all of
which ship on this exact SoC:

- **CPU governor**: pin `performance` on all four A53s before the first frame;
  the default `schedutil` ramp costs 10–20 ms on the first seconds of audio.
- **Mali devfreq**: set `performance` too, otherwise the first scene renders at
  200 MHz and looks like a rendering bug.
- **720×480 is 3:2**; the game is 4:3 (352×224 / 704×224). The port letterboxes
  to 640×480 centred by default (`Fill` / `Integer` toggles in the menu).
- **No analog stick on RG34XX.** Saturn 3D control (Azel's dragon flight) needs
  the same treatment the racing ports needed for steering: a spring/ramp model
  on the D-pad, not a raw digital axis.
- **Audio underrun** at 30 Hz needs a jitter buffer; Azel's SCSP streams at
  44.1 kHz via SoLoud.

## 9. The four defects that had to be fixed for anything to appear

Recorded in full in `patches/README.md`; summarised here because each one cost
real time and each one fails silently.

1. **Integer textures and filtering.** `bgfx::createTexture2D(..., R8U, 0)`
   leaves the sampler flags at 0, and the backend mapped that to `LINEAR`. In
   GLES an integer texture with a non-`NEAREST` filter is *incomplete*, so every
   `texelFetch` returns zero — no GL error, no warning, black layers forever.
   Force `NEAREST` for integer formats.
2. **`new T` does not zero a task work area.** `s_workArea`'s zeroing
   `operator new` is not on the path used by `new T` in the task helpers, so
   `s_videoDecoderNotifyWorkArea::m0_textSpriteTask` held heap garbage. A
   hardware watchpoint showed the bad value was present *before* the first
   write, and gdb resolved it to a string in `.rodata`. Use `new T()`.
3. **VDP2 VRAM mask.** `getVdp2Vram()` masked with `0x1FFFFF` while the array is
   `0x80000`, so a subtitle that walked its cursor out of the window wrote glyph
   tiles 1.4 MB past the buffer, into the task heap. Mask with `0x7FFFF`.
4. **Stale shader table.** `AzelLib/shaders/embeddedShadersMacro.h` had the
   ESSL and GLSL entries commented out, so `createEmbeddedShader` found nothing
   for a GL backend and every program creation failed.

Two backend-level details worth remembering for anyone writing a similar shim:

- `GL_DEPTH_STENCIL_ATTACHMENT` is an *attachment* enum; `glTexImage2D` wants
  `GL_DEPTH_STENCIL` as the format. Mesa rejects the former with
  `GL_INVALID_ENUM`. This port uses a renderbuffer for the depth attachment,
  which the engine never samples anyway.
- GLSL ES 3.00 forbids overloading built-ins, so the integer `mod()` that bgfx's
  shader compiler emitted for VDP2 tile maths has to be renamed (`pd_mod`) at
  compile time rather than overloaded.

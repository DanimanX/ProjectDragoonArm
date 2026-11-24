# Engine patches applied to the vendored Azel sources

`engine/` is yaz0r/Azel at the pinned revision, with seven documented changes.
Each one is a real defect found while bringing the game up on OpenGL ES; they are
recorded here so they can be sent upstream and so a future re-vendor is diffable.

## 1. `AzelLib/shaders/embeddedShadersMacro.h` — restore the shader table

The file is a hand-copied, outdated copy of bgfx's `embedded_shader.h`. It left
the ESSL and GLSL entries commented out, so `createEmbeddedShader` could not
find a shader for a GL/GLES backend. Enabled both entries (the generated shader
sets are already in `shaders/generated/{essl,glsl}`).

## 2. `AzelLib/shaders/shaders.cpp` — include the ESSL/GLSL generated shaders

Corresponding includes for the entries above.

## 3. `AzelLib/task.h` — value-initialise task work areas

`createSubTask` and friends used `new T`, which for these aggregate work areas
does **not** zero the object (the `operator new` that does the `memset` is on a
base class that the templates do not inherit through). Members such as
`s_videoDecoderNotifyWorkArea::m0_textSpriteTask` therefore started as whatever
was on the heap; in practice they held a stale pointer into `.rodata` and the
first `killTask()` on it segfaulted. Changed all 11 call sites to `new T()`.

## 4. `AzelLib/VDP2.cpp` — mask VDP2 VRAM to its real size

`getVdp2Vram()` masked offsets with `0x1FFFFF` (2 MB) while `vdp2Ram[]` is
512 KB, so any access in `[0x80000, 0x200000)` wrote past the array. A subtitle
line that walked the text cursor out of its window wrote glyph tiles ~1.4 MB
past the buffer and corrupted the task heap. Mask is now `0x7FFFF`, which is
what the Saturn's address decoder does.

## 5. `AzelLib/3dEngine.cpp` — swap Y as well as X in `resetCamera()`

The X axis was swapped correctly but the Y axis only did `y0 = y1`, leaving `y1`
untouched, so the vertical extent `y1 - y0` collapsed to zero and VDP1 received a
zero-area clip. Harmless on the title (which draws no VDP1 polygons) and fatal on
the field, where the scene is VDP1 polygons.

## 6. `AzelLib/movie/movieAudio.cpp`, `movie/movie.cpp` — one owner for movie audio

The movie audio voice was stopped from the main thread in `closeMovieStream()`
while SoLoud's mixer thread was already reaping it as `hasEnded()` flipped — a
double free inside `stopVoice_internal`, which crashed exactly as the intro
finished. The instance no longer sets `DISABLE_AUTOSTOP`, and the close path just
drops the handle; the audio thread remains the only owner.

## 7. `AzelLib/renderer.cpp` — let the port's menu own input

`azelSdl_StartFrame()` polls the gamepad and keyboard directly and always
injects what it finds. The port's overlay menu (`platform/arm64/menu.cpp`) is a
transparent window over the running game, so with the menu open the game also
acted on every press: it never behaved like a pause screen and the first
Start+Select landed in the game as well.

The input section now reads a flag exported by the platform layer,
`pdMenuOwnsInput`, and skips both the gamepad read and the keyboard read while
it is set. The menu sets it whenever it is open. Two lines in the gamepad
branch, one in the keyboard branch.

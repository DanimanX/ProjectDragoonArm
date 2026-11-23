# Engine patches applied to the vendored Azel sources

`engine/` is yaz0r/Azel at the pinned revision, with four documented changes.
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

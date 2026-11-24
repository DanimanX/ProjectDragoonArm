#pragma once

#include <array>
#include <bgfx/bgfx.h>

struct s_NBG_data
{
    bgfx::FrameBufferHandle BGFXFB = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle BGFXTexture = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle bgfx_vdp2_planeDataBuffer = BGFX_INVALID_HANDLE;
    // Second copy of the plane descriptor, alternated every frame. The upload is
    // 64x1 texels, but on Mali writing a texture the GPU is still reading forces
    // a stall: measured at 11-20 ms per frame to upload 256 bytes, while the draw
    // that consumes it costs 0.3 ms. Alternating means the upload always targets
    // the buffer the previous frame did not use.
    bgfx::TextureHandle bgfx_vdp2_planeDataBufferAlt = BGFX_INVALID_HANDLE;
    unsigned planeDataParity = 0;
    bgfx::ViewId viewId = -1;
    int planeId = -1;

    int m_currentWidth = -1;
    int m_currentHeight = -1;
};

extern std::array<s_NBG_data, 6> NBG_data;

extern bgfx::ProgramHandle bgfx_vdp2_program;
extern bgfx::ProgramHandle bgfx_vdp2_rbg0_program;
extern bgfx::ProgramHandle bgfx_vdp2_rbg0_dual_program;

extern bgfx::TextureHandle bgfx_vdp2_ram_texture;
extern bgfx::TextureHandle bgfx_vdp2_cram_texture;

void renderBG0(u32 width, u32 height, bool bGPU);
void renderBG1(u32 width, u32 height, bool bGPU);
void renderBG2();
void renderBG3(u32 width, u32 height, bool bGPU);
void renderRBG0(u32 width, u32 height, bool bGPU);

// imguiBGFX.h — ProjectDragoonArm replacement for bgfx's example ImGui bridge.
// Same entry points the engine calls; implemented over GLES in
// platform/arm64/imgui_gles.cpp instead of bgfx's example renderer.
#ifndef PROJECTDRAGOON_IMGUI_BGFX_COMPAT_H
#define PROJECTDRAGOON_IMGUI_BGFX_COMPAT_H

#include <bgfx/bgfx.h>
#include <bx/bx.h>
#include <imgui.h>
#include <stdint.h>

#define IMGUI_MBUT_LEFT   0x01
#define IMGUI_MBUT_RIGHT  0x02
#define IMGUI_MBUT_MIDDLE 0x04

inline uint32_t imguiRGBA(uint8_t _r, uint8_t _g, uint8_t _b, uint8_t _a = 255)
{
	return (uint32_t(_r) << 0) | (uint32_t(_g) << 8) | (uint32_t(_b) << 16) | (uint32_t(_a) << 24);
}

namespace bx { struct AllocatorI; }

void imguiCreate(float _fontSize = 18.0f, bx::AllocatorI* _allocator = NULL);
void imguiDestroy();
void imguiBeginFrame(int32_t _mx, int32_t _my, uint8_t _button, int32_t _scroll,
                     uint16_t _width, uint16_t _height, int _inputChar = -1, bgfx::ViewId _view = 255);
void imguiEndFrame();

namespace ImGui
{
	// bgfx's example bridge returns a texture id the engine can pass to
	// ImGui::Image. Here it is the GL texture name.
	inline ImTextureID toId(bgfx::TextureHandle _handle, uint8_t _flags, uint8_t _mip)
	{
		(void)_flags; (void)_mip;
		return (ImTextureID)(intptr_t)_handle;
	}
}

#endif

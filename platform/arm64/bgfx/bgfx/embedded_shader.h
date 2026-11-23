// bgfx/embedded_shader.h — same contract as bgfx's header, but the renderer
// table only lists the backends this port implements (OpenGLES / OpenGL).
#ifndef PROJECTDRAGOON_BGFX_EMBEDDED_SHADER_H
#define PROJECTDRAGOON_BGFX_EMBEDDED_SHADER_H

#include <bx/bx.h>
#include "bgfx.h"

#define BGFX_EMBEDDED_SHADER_ESSL(...)
#define BGFX_EMBEDDED_SHADER_GLSL(...)

#undef  BGFX_EMBEDDED_SHADER_ESSL
#define BGFX_EMBEDDED_SHADER_ESSL(_renderer, _name) \
	{ _renderer, BX_CONCATENATE(_name, _essl), BX_COUNTOF(BX_CONCATENATE(_name, _essl)) },

#undef  BGFX_EMBEDDED_SHADER_GLSL
#define BGFX_EMBEDDED_SHADER_GLSL(_renderer, _name) \
	{ _renderer, BX_CONCATENATE(_name, _glsl), BX_COUNTOF(BX_CONCATENATE(_name, _glsl)) },

#define BGFX_EMBEDDED_SHADER(_name)                                                  \
	{                                                                                \
		#_name,                                                                      \
		{                                                                            \
			BGFX_EMBEDDED_SHADER_ESSL(bgfx::RendererType::OpenGLES, _name)           \
			BGFX_EMBEDDED_SHADER_GLSL(bgfx::RendererType::OpenGL,   _name)           \
			{ bgfx::RendererType::Count, NULL, 0 }                                   \
		}                                                                            \
	}

#define BGFX_EMBEDDED_SHADER_END()                                   \
	{                                                                \
		NULL,                                                        \
		{ { bgfx::RendererType::Count, NULL, 0 } }                   \
	}

namespace bgfx
{
	struct EmbeddedShader
	{
		struct Data
		{
			RendererType::Enum type;
			const uint8_t* data;
			uint32_t size;
		};

		const char* name;
		Data data[RendererType::Count];
	};
}

#endif

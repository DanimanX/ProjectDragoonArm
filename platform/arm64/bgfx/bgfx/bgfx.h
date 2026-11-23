// bgfx/bgfx.h — ProjectDragoonArm GLES compatibility layer (declarations).
//
// The Azel engine is written against bgfx. This header provides the subset of
// the bgfx 1.x API that engine/AzelLib actually uses, backed by a small
// OpenGL ES 3.0 renderer (bgfx_gles.c). Engine sources stay unmodified.
//
// Scope is deliberately fixed to what `grep -rho 'bgfx::[A-Za-z_0-9]*'` finds
// in engine/AzelLib — not a general bgfx replacement.
#ifndef PROJECTDRAGOON_BGFX_COMPAT_H
#define PROJECTDRAGOON_BGFX_COMPAT_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace bx { struct AllocatorI; }

namespace bgfx
{
	typedef uint16_t ViewId;
	typedef uint16_t ProgramHandle;
	typedef uint16_t ShaderHandle;
	typedef uint16_t TextureHandle;
	typedef uint16_t UniformHandle;
	typedef uint16_t VertexBufferHandle;
	typedef uint16_t IndexBufferHandle;
	typedef uint16_t DynamicIndexBufferHandle;
	typedef uint16_t DynamicVertexBufferHandle;
	typedef uint16_t FrameBufferHandle;
	typedef uint16_t OcclusionQueryHandle;
	typedef uint32_t TextureFlags;
	typedef uint32_t SamplerFlags;

	static const uint16_t kInvalidHandle = UINT16_MAX;
	#define BGFX_INVALID_HANDLE UINT16_MAX
	#define BGFX_HANDLE_IS_VALID(h) ((h) != UINT16_MAX)

	struct RendererType { enum Enum { Noop, Agc, Direct3D11, Direct3D12, Gnm, Metal, Nvn, OpenGLES, OpenGL, Vulkan, WebGPU, Count }; };
	struct AttribType   { enum Enum { Uint8, Uint10, Int16, Half, Float, Count }; };
	struct Attrib       { enum Enum { Position, Normal, Tangent, Bitangent, Color0, Color1, Color2, Color3, Indices, TexCoord0, TexCoord1, TexCoord2, TexCoord3, TexCoord4, TexCoord5, TexCoord6, TexCoord7, Count }; };
	struct UniformType  { enum Enum { Sampler, Vec4, Mat3, Mat4, Count }; };
	struct TextureFormat{ enum Enum { BC1, BC2, BC3, BC4, BC5, BC6H, BC7, ETC1, ETC2, ETC2A, ETC2A1, PTC12, PTC14, PTC12A, PTC14A, PTC22, PTC24, PVRTC1_2, PVRTC1_4, PVRTC1_2A, PVRTC1_4A, PVRTC2_2, PVRTC2_4, R1, R8, R16, R16F, R16I, R16U, R32F, R32I, R32U, RG8, RG16, RG16F, RG16I, RG16U, RG32F, RG32I, RG32U, RGB8, RGB9E5, RGB16, RGB16F, RGB16I, RGB16U, RGB32F, RGB32I, RGB32U, RGBA8, RGBA16, RGBA16F, RGBA16I, RGBA16U, RGBA32F, RGBA32I, RGBA32U, BGRA8, RGBA16S, R8U, R8I, D16, D24, D24S8, D32F, D32, COUNT }; };
	struct ViewMode     { enum Enum { Default, Sequential, DepthAscending, DepthDescending }; };
	struct NativeWindowHandleType { enum Enum { Default, Wayland, Count }; };

	// --- vertex layout ---------------------------------------------------
	struct VertexLayout
	{
		VertexLayout& begin();
		VertexLayout& add(Attrib::Enum _attrib, uint8_t _num, AttribType::Enum _type, bool _normalized = false, bool _asInt = false);
		VertexLayout& end();
		uint16_t getStride() const { return m_stride; }
		bool has(Attrib::Enum _attrib) const { return m_attr[_attrib].num != 0; }
		uint8_t getOffset(Attrib::Enum _attrib) const { return m_attr[_attrib].offset; }
		uint8_t getNum(Attrib::Enum _attrib) const { return m_attr[_attrib].num; }
		AttribType::Enum getType(Attrib::Enum _attrib) const { return m_attr[_attrib].type; }
		bool isNormalized(Attrib::Enum _attrib) const { return m_attr[_attrib].normalized; }

		struct Attr { uint8_t num = 0; uint8_t offset = 0; AttribType::Enum type = AttribType::Float; bool normalized = false; };
		Attr m_attr[Attrib::Count];
		uint16_t m_stride = 0;
	};

	// --- memory ----------------------------------------------------------
	struct Memory
	{
		static Memory* make(size_t _size);
		uint8_t* data;
		uint32_t size;
		uint32_t offset;
		void (*release)(void* _ptr, void* _userData);
		void* userData;
	};
	inline Memory* copy(const void* _data, uint32_t _size) { Memory* m = Memory::make(_size); if (_size) memcpy(m->data, _data, _size); return m; }
	inline Memory* makeRef(const void* _data, uint32_t _size, void (*_release)(void*, void*) = NULL, void* _userData = NULL)
	{ Memory* m = Memory::make(0); m->data = const_cast<uint8_t*>((const uint8_t*)_data); m->size = _size; m->release = _release; m->userData = _userData; return m; }
	inline uint32_t copy(const char* _src, char* _dst, uint32_t _dsize) { uint32_t l = (uint32_t)strlen(_src); if (l < _dsize) { memcpy(_dst, _src, l + 1); } else { _dst[0] = '\0'; } return l; }

	// --- transient geometry ---------------------------------------------
	struct TransientVertexBuffer { uint8_t* data; uint32_t size; uint32_t startVertex; uint16_t stride; VertexBufferHandle handle; VertexLayout layout; };
	struct TransientIndexBuffer  { uint8_t* data; uint32_t size; uint32_t startIndex; bool isIndex16; IndexBufferHandle handle; };

	// --- state -----------------------------------------------------------
	// Canonical bgfx values: the engine only ORs these together, but keeping
	// the original encodings means its bit tests (if any) stay meaningful.
	typedef uint64_t StateFlags;
#define BGFX_STATE_WRITE_R  UINT64_C(0x0000000000000001)
#define BGFX_STATE_WRITE_G  UINT64_C(0x0000000000000002)
#define BGFX_STATE_WRITE_B  UINT64_C(0x0000000000000004)
#define BGFX_STATE_WRITE_A  UINT64_C(0x0000000000000008)
#define BGFX_STATE_WRITE_Z  UINT64_C(0x0000000000000100)
#define BGFX_STATE_WRITE_RGB (BGFX_STATE_WRITE_R|BGFX_STATE_WRITE_G|BGFX_STATE_WRITE_B)
#define BGFX_STATE_DEPTH_TEST_DEFAULT UINT64_C(0x0000000000000000)
#define BGFX_STATE_DEPTH_TEST_LESS    UINT64_C(0x0000000000000010)
#define BGFX_STATE_DEPTH_TEST_LEQUAL  UINT64_C(0x0000000000000020)
#define BGFX_STATE_DEPTH_TEST_EQUAL   UINT64_C(0x0000000000000030)
#define BGFX_STATE_DEPTH_TEST_GEQUAL  UINT64_C(0x0000000000000040)
#define BGFX_STATE_DEPTH_TEST_GREATER UINT64_C(0x0000000000000050)
#define BGFX_STATE_DEPTH_TEST_NOTEQUAL UINT64_C(0x0000000000000060)
#define BGFX_STATE_DEPTH_TEST_NEVER   UINT64_C(0x0000000000000070)
#define BGFX_STATE_DEPTH_TEST_ALWAYS  UINT64_C(0x0000000000000080)
#define BGFX_STATE_DEPTH_TEST_MASK    UINT64_C(0x00000000000000f0)
#define BGFX_STATE_CULL_CW            UINT64_C(0x0000000000000200)
#define BGFX_STATE_CULL_CCW           UINT64_C(0x0000000000000400)
#define BGFX_STATE_MSAA               UINT64_C(0x0000000000000800)
#define BGFX_STATE_PT_LINES           UINT64_C(0x0000000000002000)
#define BGFX_STATE_PT_POINTS          UINT64_C(0x0000000000004000)
#define BGFX_STATE_PT_MASK            UINT64_C(0x0000000000006000)

#define BGFX_STATE_BLEND_SRC_ALPHA      UINT32_C(0x00000001)
#define BGFX_STATE_BLEND_INV_SRC_ALPHA  UINT32_C(0x00000002)
#define BGFX_STATE_BLEND_ONE            UINT32_C(0x00000004)
#define BGFX_STATE_BLEND_ZERO           UINT32_C(0x00000008)
#define BGFX_STATE_BLEND_SRC_COLOR      UINT32_C(0x00000010)
#define BGFX_STATE_BLEND_INV_SRC_COLOR  UINT32_C(0x00000020)
#define BGFX_STATE_BLEND_DST_COLOR      UINT32_C(0x00000040)
#define BGFX_STATE_BLEND_INV_DST_COLOR  UINT32_C(0x00000080)
#define BGFX_STATE_BLEND_DST_ALPHA      UINT32_C(0x00000100)
#define BGFX_STATE_BLEND_INV_DST_ALPHA  UINT32_C(0x00000200)
#define BGFX_STATE_BLEND_SRC_ALPHA_SAT  UINT32_C(0x00000400)
#define BGFX_STATE_BLEND_FUNC(_src, _dst) (((uint64_t)(_src) << 32) | (uint64_t)(_dst))
#define BGFX_STATE_BLEND_ALPHA BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_INV_SRC_ALPHA)
#define BGFX_STATE_BLEND_ADD   BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_ONE)

#define BGFX_CLEAR_NONE          UINT16_C(0x0000)
#define BGFX_CLEAR_COLOR         UINT16_C(0x0001)
#define BGFX_CLEAR_DEPTH         UINT16_C(0x0002)
#define BGFX_CLEAR_STENCIL       UINT16_C(0x0004)
#define BGFX_CLEAR_COLOR_DEPTH   UINT16_C(0x0003)
#define BGFX_CLEAR_COLOR_STENCIL UINT16_C(0x0005)
#define BGFX_CLEAR_DEPTH_STENCIL UINT16_C(0x0006)

#define BGFX_TEXTURE_NONE       UINT64_C(0)
#define BGFX_TEXTURE_RT         UINT64_C(0x00000010)
#define BGFX_SAMPLER_U_CLAMP    UINT64_C(0x00000040)
#define BGFX_SAMPLER_V_CLAMP    UINT64_C(0x00000080)
#define BGFX_SAMPLER_MIN_POINT  UINT64_C(0x00000100)
#define BGFX_SAMPLER_MAG_POINT  UINT64_C(0x00000200)
#define BGFX_SAMPLER_MIP_POINT  UINT64_C(0x00000400)
#define BGFX_SAMPLER_POINT      (BGFX_SAMPLER_MIN_POINT|BGFX_SAMPLER_MAG_POINT)

#define BGFX_DEBUG_NONE  UINT32_C(0x00000000)
#define BGFX_DEBUG_TEXT  UINT32_C(0x00000002)
#define BGFX_DEBUG_STATS UINT32_C(0x00000004)

	struct Attachment
	{
		Attachment() : handle(kInvalidHandle), access(0) {}
		void init(TextureHandle _handle, uint8_t _access = 0) { handle = _handle; access = _access; }
		TextureHandle handle;
		uint8_t access;
	};

	struct PlatformData
	{
		PlatformData() : ndt(NULL), nwh(NULL), context(NULL), queue(NULL), backBuffer(NULL), backBufferDS(NULL), type(NativeWindowHandleType::Default) {}
		void* ndt;
		void* nwh;
		void* context;
		void* queue;
		void* backBuffer;
		void* backBufferDS;
		NativeWindowHandleType::Enum type;
	};

	struct Init
	{
		Init() : type(RendererType::Count), vendorId(0), deviceId(0), debug(false), callbacks(NULL) {}
		RendererType::Enum type;
		uint16_t vendorId;
		uint16_t deviceId;
		bool debug;
		void* callbacks;
		PlatformData platformData;
	};

	struct Caps
	{
		struct Limits
		{
			uint32_t maxTextureSize;
			uint32_t maxViews;
		} limits;
	};

	// --- lifetime --------------------------------------------------------
	bool init(const Init& _init);
	void shutdown();
	void reset(uint32_t _width, uint32_t _height, uint32_t _flags = 0, RendererType::Enum _type = RendererType::Count);
	RendererType::Enum getRendererType();
	const char* getRendererName(RendererType::Enum _type);
	const Caps* getCaps();
	void setDebug(uint32_t _debug);
	void setMarker(const char* _marker);
	void setName(TextureHandle _handle, const char* _name, int32_t _len = -1);
	void frame();
	uint32_t getAvailTransientVertexBuffer(uint32_t _num, const VertexLayout& _layout);
	uint32_t getAvailTransientIndexBuffer(uint32_t _num);
	void allocTransientVertexBuffer(TransientVertexBuffer* _tvb, uint32_t _num, const VertexLayout& _layout);
	void allocTransientIndexBuffer(TransientIndexBuffer* _tib, uint32_t _num, bool _index32 = false);

	// --- resources -------------------------------------------------------
	ShaderHandle createEmbeddedShader(const struct EmbeddedShader* _es, RendererType::Enum _type, const char* _name);
	ProgramHandle createProgram(ShaderHandle _vsh, ShaderHandle _fsh, bool _destroyShaders = false);
	TextureHandle createTexture2D(uint16_t _width, uint16_t _height, bool _hasMips, uint16_t _numLayers, TextureFormat::Enum _format, uint64_t _flags = BGFX_TEXTURE_NONE, const Memory* _mem = NULL);
	void updateTexture2D(TextureHandle _handle, uint16_t _layer, uint8_t _mip, uint16_t _x, uint16_t _y, uint16_t _width, uint16_t _height, const Memory* _mem, uint16_t _pitch = UINT16_MAX);
	TextureHandle getTexture(FrameBufferHandle _handle, uint8_t _attachment = 0);
	FrameBufferHandle createFrameBuffer(uint16_t _width, uint16_t _height, TextureFormat::Enum _format, uint64_t _textureFlags = BGFX_TEXTURE_NONE);
	FrameBufferHandle createFrameBuffer(uint8_t _num, const Attachment* _attachment, bool _destroyTextures = false);
	VertexBufferHandle createVertexBuffer(const Memory* _mem, const VertexLayout& _layout, uint16_t _flags = 0);
	IndexBufferHandle createIndexBuffer(const Memory* _mem, uint16_t _flags = 0);
	UniformHandle createUniform(const char* _name, UniformType::Enum _type, uint16_t _num = 1);
	// Handles are all uint16_t in this backend, so `destroy`/`isValid` take one
	// value and dispatch on the handle's allocation range (see bgfx_gles.cpp).
	void destroy(uint16_t _handle);
	bool isValid(uint16_t _handle);
	void readTexture(TextureHandle _handle, void* _data, uint8_t _mip = 0);

	// --- views -----------------------------------------------------------
	void setViewName(ViewId _id, const char* _name);
	void setViewRect(ViewId _id, uint16_t _x, uint16_t _y, uint16_t _width, uint16_t _height);
	void setViewScissor(ViewId _id, uint16_t _x, uint16_t _y, uint16_t _width, uint16_t _height);
	void setViewClear(ViewId _id, uint16_t _flags, uint32_t _rgba = 0x000000ff, float _depth = 1.0f, uint8_t _stencil = 0);
	void setViewFrameBuffer(ViewId _id, FrameBufferHandle _handle);
	void setViewTransform(ViewId _id, const void* _view, const void* _proj);
	void setViewMode(ViewId _id, ViewMode::Enum _mode);
	void setViewOrder(ViewId _id, ViewId _before);
	void touch(ViewId _id);

	// --- draw ------------------------------------------------------------
	void setState(uint64_t _state, uint32_t _rgba = 0);
	void setUniform(UniformHandle _handle, const void* _value, uint16_t _num = 1);
	void setTexture(uint8_t _stage, UniformHandle _sampler, TextureHandle _handle, uint32_t _flags = UINT32_MAX);
	void setVertexBuffer(uint8_t _stream, VertexBufferHandle _handle, uint32_t _startVertex = 0, uint32_t _numVertices = UINT32_MAX);
	void setIndexBuffer(IndexBufferHandle _handle, uint32_t _firstIndex = 0, uint32_t _numIndices = UINT32_MAX);
	bool allocTransientBuffers(TransientVertexBuffer* _tvb, const VertexLayout& _layout, uint32_t _numVertices, TransientIndexBuffer* _tib, uint32_t _numIndices);
	void setVertexBuffer(uint8_t _stream, const TransientVertexBuffer* _tvb, uint32_t _startVertex = 0, uint32_t _numVertices = UINT32_MAX);
	void setIndexBuffer(const TransientIndexBuffer* _tib, uint32_t _firstIndex = 0, uint32_t _numIndices = UINT32_MAX);
	void submit(ViewId _id, ProgramHandle _program, uint32_t _depth = 0, uint8_t _flags = 0);

	struct Encoder
	{
		void setState(uint64_t _state, uint32_t _rgba = 0);
		void setUniform(UniformHandle _handle, const void* _value, uint16_t _num = 1);
		void setTexture(uint8_t _stage, UniformHandle _sampler, TextureHandle _handle, uint32_t _flags = UINT32_MAX);
		void setVertexBuffer(uint8_t _stream, VertexBufferHandle _handle, uint32_t _startVertex = 0, uint32_t _numVertices = UINT32_MAX);
		void setIndexBuffer(IndexBufferHandle _handle, uint32_t _firstIndex = 0, uint32_t _numIndices = UINT32_MAX);
		void submit(ViewId _id, ProgramHandle _program, uint32_t _depth = 0, uint8_t _flags = 0);
	};
	Encoder* begin(bool _forThread = false);
	void end(Encoder* _encoder);
}

// The engine calls isValid() unqualified on handle values (bgfx 1.x relies on
// ADL through handle structs; this port keeps handles as u16 and exposes the
// same unqualified spelling here).
inline bool isValid(uint16_t _handle) { return _handle != UINT16_MAX; }

#endif // PROJECTDRAGOON_BGFX_COMPAT_H

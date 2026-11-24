// bgfx_gles.cpp — ProjectDragoonArm backend for the bgfx API subset the Azel
// engine uses, implemented directly on OpenGL ES 3.x (see bgfx/bgfx.h).
//
// Design decisions, recorded because they explain the shape of this file:
//
//  * Immediate mode, not bgfx's deferred command buffer. The engine submits
//    each draw right after setting its state, and drives a handful of views per
//    frame, so a view is prepared (FBO bound, viewport/scissor set, clear
//    honoured once) the first time it is touched in a frame and the draw is
//    issued at submit() time. This keeps state tracking trivial and portable.
//  * Shaders are the engine's embedded bgfx blobs. bgfx stores the GLSL source
//    as plain text after a small header; we parse that header, take the text,
//    prepend "#version 300 es" (which bgfx itself would have done) and compile.
//    No shader compiler and no offline shader build are needed on the device.
//  * GL is loaded through EGL at runtime (dlopen), because the handheld ships
//    its own libEGL/libGLESv2 and Zig's sysroot has neither.
//
// Formats: only what the engine creates is supported — RGBA8 (and its BGRA8
// spelling), R8/R8UI, RGBA32F, R32F, R32UI, D24S8. Every CPU upload in the
// engine is RGBA8, so BGRA8 render targets need no channel swizzle: they are
// rendered into and sampled from consistently.

#include <bgfx/bgfx.h>
#include <bgfx/embedded_shader.h>
#include <unistd.h>
#include <ctime>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <unordered_map>

#include <dlfcn.h>

// ---------------------------------------------------------------------------
// EGL / GLES 3 declarations (no headers: the target sysroot has none)
// ---------------------------------------------------------------------------
typedef void* EGLDisplay;
typedef void* EGLConfig;
typedef void* EGLContext;
typedef void* EGLSurface;
typedef void* EGLNativeWindowType;
typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned int GLbitfield;
typedef unsigned char GLboolean;
typedef unsigned char GLubyte;
typedef float GLfloat;
typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;
typedef unsigned int GLuint64;

#define EGL_NONE                 0x3038
#define EGL_SUCCESS              0x3000
#define EGL_SURFACE_TYPE         0x3033
#define EGL_WINDOW_BIT           0x0004
#define EGL_PBUFFER_BIT          0x0001
#define EGL_RENDERABLE_TYPE      0x3040
#define EGL_OPENGL_ES3_BIT       0x0040
#define EGL_OPENGL_ES2_BIT       0x0004
#define EGL_RED_SIZE             0x3024
#define EGL_GREEN_SIZE           0x3023
#define EGL_BLUE_SIZE            0x3022
#define EGL_ALPHA_SIZE           0x3021
#define EGL_DEPTH_SIZE           0x3025
#define EGL_STENCIL_SIZE         0x3026
#define EGL_SAMPLES              0x3032
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_CONTEXT_MAJOR_VERSION 0x3098
#define EGL_CONTEXT_MINOR_VERSION 0x30FB
#define EGL_OPENGL_ES_API        0x30A0
#define EGL_DEFAULT_DISPLAY      ((void*)0)
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#define EGL_PLATFORM_GBM_KHR     0x31D7
#define EGL_PLATFORM_WAYLAND_KHR 0x31D8
#define EGL_PLATFORM_X11_KHR     0x31D5

typedef EGLDisplay (*PFN_eglGetDisplay)(void*);
typedef EGLDisplay (*PFN_eglGetPlatformDisplay)(unsigned, void*, const intptr_t*);
typedef GLboolean (*PFN_eglInitialize)(EGLDisplay, int*, int*);
typedef GLboolean (*PFN_eglChooseConfig)(EGLDisplay, const int*, EGLConfig*, int, int*);
typedef EGLContext (*PFN_eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const int*);
typedef EGLSurface (*PFN_eglCreateWindowSurface)(EGLDisplay, EGLConfig, EGLNativeWindowType, const int*);
typedef EGLSurface (*PFN_eglCreatePbufferSurface)(EGLDisplay, EGLConfig, const int*);
typedef GLboolean (*PFN_eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
typedef GLboolean (*PFN_eglSwapBuffers)(EGLDisplay, EGLSurface);
typedef GLboolean (*PFN_eglSwapInterval)(EGLDisplay, int);
typedef GLint (*PFN_eglGetError)(void);
typedef void* (*PFN_eglGetProcAddress)(const char*);
typedef GLboolean (*PFN_eglBindAPI)(unsigned);
typedef const char* (*PFN_eglQueryString)(EGLDisplay, int);
#define EGL_EXTENSIONS 0x3055

// GL constants used
enum : GLenum
{
	GL_TEXTURE_2D = 0x0DE1, GL_TEXTURE_2D_MULTISAMPLE = 0x9100,
	GL_TEXTURE0 = 0x84C0,
	GL_RGBA = 0x1908, GL_RGBA8 = 0x8058, GL_RED = 0x1903, GL_R8 = 0x8229, GL_R8UI = 0x8232,
	GL_RGBA32F = 0x8814, GL_R32F = 0x822E, GL_R32UI = 0x8236, GL_RED_INTEGER = 0x8D94,
	GL_DEPTH24_STENCIL8 = 0x88F0, GL_DEPTH_STENCIL_ATTACHMENT = 0x821A, GL_DEPTH_STENCIL = 0x84F9,
	GL_UNSIGNED_BYTE = 0x1401, GL_FLOAT = 0x1406, GL_UNSIGNED_INT = 0x1405,
	GL_UNSIGNED_SHORT = 0x1403, GL_SHORT = 0x1402, GL_INT = 0x1404,
	GL_FRAMEBUFFER = 0x8D40, GL_RENDERBUFFER = 0x8D41,
	GL_COLOR_ATTACHMENT0 = 0x8CE0,
	GL_ARRAY_BUFFER = 0x8892, GL_ELEMENT_ARRAY_BUFFER = 0x8893,
	GL_DYNAMIC_DRAW = 0x88E8, GL_STATIC_DRAW = 0x88E4,
	GL_STREAM_DRAW = 0x88E0,
	GL_VERTEX_SHADER = 0x8B31, GL_FRAGMENT_SHADER = 0x8B30,
	GL_COMPILE_STATUS = 0x8B81, GL_LINK_STATUS = 0x8B82, GL_INFO_LOG_LENGTH = 0x8B84,
	GL_ACTIVE_UNIFORMS = 0x8B86,
	GL_BLEND = 0x0BE2, GL_DEPTH_TEST = 0x0B71, GL_CULL_FACE = 0x0B44,
	GL_SCISSOR_TEST = 0x0C11,
	GL_SRC_ALPHA = 0x0302, GL_ONE_MINUS_SRC_ALPHA = 0x0303, GL_ONE = 1, GL_ZERO = 0,
	GL_SRC_COLOR = 0x0300, GL_ONE_MINUS_SRC_COLOR = 0x0301,
	GL_DST_COLOR = 0x0306, GL_ONE_MINUS_DST_COLOR = 0x0307,
	GL_DST_ALPHA = 0x0304, GL_ONE_MINUS_DST_ALPHA = 0x0305,
	GL_SRC_ALPHA_SATURATE = 0x0308,
	GL_LESS = 0x0201, GL_LEQUAL = 0x0203, GL_EQUAL = 0x0202, GL_GEQUAL = 0x0206,
	GL_GREATER = 0x0204, GL_NOTEQUAL = 0x0205, GL_NEVER = 0x0200, GL_ALWAYS = 0x0207,
	GL_FRONT = 0x0404, GL_BACK = 0x0405, GL_CCW = 0x0901, GL_CW = 0x0900,
	GL_TRIANGLES = 0x0004, GL_LINES = 0x0001, GL_POINTS = 0x0000,
	GL_TRIANGLE_STRIP = 0x0005,
	GL_COLOR_BUFFER_BIT = 0x4000, GL_DEPTH_BUFFER_BIT = 0x0100,
	GL_STENCIL_BUFFER_BIT = 0x0400,
	GL_DEPTH_BUFFER_BIT_STENCIL = 0x0100,
	GL_TEXTURE_MIN_FILTER = 0x2801, GL_TEXTURE_MAG_FILTER = 0x2800,
	GL_TEXTURE_WRAP_S = 0x2802, GL_TEXTURE_WRAP_T = 0x2803,
	GL_LINEAR = 0x2601, GL_NEAREST = 0x2600,
	GL_CLAMP_TO_EDGE = 0x812F, GL_REPEAT = 0x2901,
	GL_UNPACK_ALIGNMENT = 0x0CF5, GL_PACK_ALIGNMENT = 0x0D05,
	GL_NO_ERROR = 0,
	GL_VERSION = 0x1F02, GL_RENDERER = 0x1F01, GL_VENDOR = 0x1F00,
	GL_MAX_TEXTURE_SIZE = 0x0D33,
	GL_FRAMEBUFFER_COMPLETE = 0x8CD5,
	GL_DRAW_FRAMEBUFFER = 0x8CA9, GL_READ_FRAMEBUFFER = 0x8CA8,
	GL_READ_FRAMEBUFFER_BINDING = 0x8CAA,
	GL_FRAMEBUFFER_BINDING = 0x8CA6,
	GL_TRUE = 1, GL_FALSE = 0,
	GL_DEPTH_WRITEMASK = 0x0B72,
	GL_UPPER_LEFT = 0x8CA2,
	GL_FRAMEBUFFER_SRGB = 0x8DB9,
	GL_TEXTURE_BASE_LEVEL = 0x813C,
	GL_TEXTURE_MAX_LEVEL = 0x813D,
	GL_PIXEL_PACK_BUFFER = 0x88EB,
};

// function pointers
#define GL_FUNCS(X) \
	X(void, glEnable, (GLenum)) \
	X(void, glDisable, (GLenum)) \
	X(void, glBlendFunc, (GLenum, GLenum)) \
	X(void, glDepthFunc, (GLenum)) \
	X(void, glDepthMask, (GLboolean)) \
	X(void, glColorMask, (GLboolean, GLboolean, GLboolean, GLboolean)) \
	X(void, glCullFace, (GLenum)) \
	X(void, glFrontFace, (GLenum)) \
	X(void, glViewport, (GLint, GLint, GLsizei, GLsizei)) \
	X(void, glScissor, (GLint, GLint, GLsizei, GLsizei)) \
	X(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat)) \
	X(void, glClearDepthf, (GLfloat)) \
	X(void, glClearStencil, (GLint)) \
	X(void, glClear, (GLbitfield)) \
	X(void, glGenTextures, (GLsizei, GLuint*)) \
	X(void, glDeleteTextures, (GLsizei, const GLuint*)) \
	X(void, glBindTexture, (GLenum, GLuint)) \
	X(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*)) \
	X(void, glTexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*)) \
	X(void, glTexParameteri, (GLenum, GLenum, GLint)) \
	X(void, glActiveTexture, (GLenum)) \
	X(void, glPixelStorei, (GLenum, GLint)) \
	X(void, glGenFramebuffers, (GLsizei, GLuint*)) \
	X(void, glDeleteFramebuffers, (GLsizei, const GLuint*)) \
	X(void, glBindFramebuffer, (GLenum, GLuint)) \
	X(GLenum, glCheckFramebufferStatus, (GLenum)) \
	X(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) \
	X(void, glDrawBuffers, (GLsizei, const GLenum*)) \
	X(void, glReadBuffer, (GLenum)) \
	X(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*)) \
	X(void, glGenBuffers, (GLsizei, GLuint*)) \
	X(void, glDeleteBuffers, (GLsizei, const GLuint*)) \
	X(void, glBindBuffer, (GLenum, GLuint)) \
	X(void, glBufferData, (GLenum, GLsizeiptr, const void*, GLenum)) \
	X(void, glBufferSubData, (GLenum, GLintptr, GLsizeiptr, const void*)) \
	X(GLuint, glCreateShader, (GLenum)) \
	X(void, glDeleteShader, (GLuint)) \
	X(void, glShaderSource, (GLuint, GLsizei, const GLchar* const*, const GLint*)) \
	X(void, glCompileShader, (GLuint)) \
	X(void, glGetShaderiv, (GLuint, GLenum, GLint*)) \
	X(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*)) \
	X(GLuint, glCreateProgram, (void)) \
	X(void, glDeleteProgram, (GLuint)) \
	X(void, glAttachShader, (GLuint, GLuint)) \
	X(void, glLinkProgram, (GLuint)) \
	X(void, glGetProgramiv, (GLuint, GLenum, GLint*)) \
	X(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*)) \
	X(void, glUseProgram, (GLuint)) \
	X(GLint, glGetUniformLocation, (GLuint, const GLchar*)) \
	X(void, glGetActiveUniform, (GLuint, GLuint, GLsizei, GLsizei*, GLint*, GLenum*, GLchar*)) \
	X(void, glUniform1i, (GLint, GLint)) \
	X(void, glUniform1f, (GLint, GLfloat)) \
	X(void, glUniform1fv, (GLint, GLsizei, const GLfloat*)) \
	X(void, glUniform2fv, (GLint, GLsizei, const GLfloat*)) \
	X(void, glUniform3fv, (GLint, GLsizei, const GLfloat*)) \
	X(void, glUniform4fv, (GLint, GLsizei, const GLfloat*)) \
	X(void, glUniformMatrix3fv, (GLint, GLsizei, GLboolean, const GLfloat*)) \
	X(void, glUniformMatrix4fv, (GLint, GLsizei, GLboolean, const GLfloat*)) \
	X(void, glUniform1uiv, (GLint, GLsizei, const GLuint*)) \
	X(GLint, glGetAttribLocation, (GLuint, const GLchar*)) \
	X(void, glEnableVertexAttribArray, (GLuint)) \
	X(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void*)) \
	X(void, glVertexAttribIPointer, (GLuint, GLint, GLenum, GLsizei, const void*)) \
	X(void, glBindAttribLocation, (GLuint, GLuint, const GLchar*)) \
	X(void, glDrawArrays, (GLenum, GLint, GLsizei)) \
	X(void, glDrawElements, (GLenum, GLsizei, GLenum, const void*)) \
	X(void, glGenRenderbuffers, (GLsizei, GLuint*)) \
	X(void, glDeleteRenderbuffers, (GLsizei, const GLuint*)) \
	X(void, glBindRenderbuffer, (GLenum, GLuint)) \
	X(void, glRenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei)) \
	X(void, glFramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint)) \
	X(void, glGenVertexArrays, (GLsizei, GLuint*)) \
	X(void, glBindVertexArray, (GLuint)) \
	X(void, glDeleteVertexArrays, (GLsizei, const GLuint*)) \
	X(const GLubyte*, glGetString, (GLenum)) \
	X(GLenum, glGetError, (void)) \
	X(void, glGetIntegerv, (GLenum, GLint*)) \
	X(void, glFlush, (void)) \
	X(void, glFinish, (void))

#define DECL_GL(ret, name, args) typedef ret (*PFN_##name) args; static PFN_##name p_##name = NULL;
GL_FUNCS(DECL_GL)
#undef DECL_GL

static void* g_eglLib = NULL;
static void* g_glLib = NULL;

static PFN_eglGetDisplay              p_eglGetDisplay;
static PFN_eglGetPlatformDisplay      p_eglGetPlatformDisplay;
static PFN_eglInitialize              p_eglInitialize;
static PFN_eglChooseConfig            p_eglChooseConfig;
static PFN_eglCreateContext           p_eglCreateContext;
static PFN_eglCreateWindowSurface     p_eglCreateWindowSurface;
static PFN_eglCreatePbufferSurface    p_eglCreatePbufferSurface;
static PFN_eglMakeCurrent             p_eglMakeCurrent;
static PFN_eglSwapBuffers             p_eglSwapBuffers;
static PFN_eglSwapInterval            p_eglSwapInterval;
static PFN_eglGetError                p_eglGetError;
static PFN_eglGetProcAddress          p_eglGetProcAddress;
static PFN_eglBindAPI                 p_eglBindAPI;

static EGLDisplay g_display = NULL;
static EGLContext g_context = NULL;
static EGLSurface g_surface = NULL;
static EGLConfig  g_config  = NULL;
static int g_width = 1280, g_height = 720;
static bool g_headless = false;

// ---------------------------------------------------------------------------
// resources
// ---------------------------------------------------------------------------
namespace
{
	struct Texture
	{
		bool used = false;
		GLuint id = 0;
		uint16_t w = 0, h = 0;
		bgfx::TextureFormat::Enum format = bgfx::TextureFormat::RGBA8;
		GLuint fbo = 0;          // lazily created for readback
		bool isRT = false;
		bool isRenderbuffer = false;
		std::string name;
	};

	struct FrameBuffer
	{
		bool used = false;
		GLuint fbo = 0;
		uint16_t w = 0, h = 0;
		bgfx::TextureHandle attachments[4];
		int numAttachments = 0;
	};

	struct Buffer
	{
		bool used = false;
		GLuint id = 0;
		uint32_t count = 0;      // vertices or indices
		bgfx::VertexLayout layout;
		bool isIndex = false;
	};

	struct Program
	{
		bool used = false;
		GLuint id = 0;
		std::unordered_map<std::string, GLint> uniforms;
		std::string vsName, psName;
	};

	struct Uniform
	{
		bool used = false;
		std::string name;
		bgfx::UniformType::Enum type = bgfx::UniformType::Vec4;
		float f[16] = {0};
		uint32_t u[4] = {0};
		bool dirty = true;
	};

	static const int kMaxTextures = 4096;
	static const int kMaxFrameBuffers = 256;
	static const int kMaxBuffers = 2048;
	static const int kMaxPrograms = 512;
	static const int kMaxUniforms = 1024;

	// Handle value ranges. bgfx distinguishes handle types by struct type; this
	// backend keeps them all uint16_t (the engine compares and stores them as
	// integers), so each resource kind owns a disjoint block of values and
	// destroy()/isValid() dispatch on the block.
	//   textures    0      .. 4095
	//   framebuffers 4096  .. 4351   (spans 4608 so programs never overlap in
	//                                 the low block used by readTexture(diag))
	//   buffers     8192   .. 10239
	//   programs    10240  .. 10751
	//   uniforms    16384  .. 17407
	enum
	{
		kTexBase = 0,
		kFboBase = 4096,
		kBufBase = 8192,
		kProgBase = 10240,
		kUniBase = 16384,
	};

	static bool isTexHandle(uint16_t h) { return h >= kTexBase && h < kTexBase + kMaxTextures; }
	static bool isFboHandle(uint16_t h) { return h >= kFboBase && h < kFboBase + kMaxFrameBuffers; }
	static bool isBufHandle(uint16_t h) { return h >= kBufBase && h < kBufBase + kMaxBuffers; }
	static bool isProgHandle(uint16_t h) { return h >= kProgBase && h < kProgBase + kMaxPrograms; }
	static bool isUniHandle(uint16_t h) { return h >= kUniBase && h < kUniBase + kMaxUniforms; }

	static Texture g_textures[kMaxTextures];
	static FrameBuffer g_fbos[kMaxFrameBuffers];
	static Buffer g_buffers[kMaxBuffers];
	static Program g_programs[kMaxPrograms];
	static Uniform g_uniforms[kMaxUniforms];

	const char* s_attribNames[bgfx::Attrib::Count] = {
		"a_position", "a_normal", "a_tangent", "a_bitangent",
		"a_color0", "a_color1", "a_color2", "a_color3",
		"a_indices", "a_texcoord0", "a_texcoord1", "a_texcoord2",
		"a_texcoord3", "a_texcoord4", "a_texcoord5", "a_texcoord6", "a_texcoord7",
	};

	template <typename T, int N>
	static T* allocSlot(T (&arr)[N], uint16_t& handle, int base)
	{
		for (int i = 0; i < N; ++i)
		{
			if (!arr[i].used)
			{
				arr[i].used = true;
				handle = (uint16_t)(base + i);
				return &arr[i];
			}
		}
		handle = bgfx::kInvalidHandle;
		fprintf(stderr, "[gles] out of resource slots (%d)\n", N);
		return NULL;
	}

	// accessors that translate handle -> slot
	static Texture*     texSlot(uint16_t h) { return g_textures + (h - kTexBase); }
	static FrameBuffer* fboSlot(uint16_t h) { return g_fbos + (h - kFboBase); }
	static Buffer*      bufSlot(uint16_t h) { return g_buffers + (h - kBufBase); }
	static Program*     progSlot(uint16_t h) { return g_programs + (h - kProgBase); }
	static Uniform*     uniSlot(uint16_t h) { return g_uniforms + (h - kUniBase); }

	// --- view state ------------------------------------------------------
	struct View
	{
		uint16_t x = 0, y = 0, w = 0, h = 0;
		uint16_t clearFlags = 0;
		uint32_t clearColor = 0;
		float clearDepth = 1.0f;
		uint8_t clearStencil = 0;
		bgfx::FrameBufferHandle fbo = bgfx::kInvalidHandle;
		bool hasScissor = false;
		uint16_t sx = 0, sy = 0, sw = 0, sh = 0;
		bool touchedThisFrame = false;
		char name[64] = {0};
	};
	static View g_views[256];

	// --- current draw state ---------------------------------------------
	struct PendingTexture { uint8_t stage; bgfx::UniformHandle sampler; bgfx::TextureHandle texture; };
	struct PendingUniform { bgfx::UniformHandle handle; };

	static uint64_t g_state = 0;
	static uint32_t g_stateRgba = 0;
	static bgfx::ProgramHandle g_program = bgfx::kInvalidHandle;
	static bgfx::VertexBufferHandle g_vb = bgfx::kInvalidHandle;
	static bgfx::IndexBufferHandle g_ib = bgfx::kInvalidHandle;
	static uint32_t g_vbStart = 0, g_vbCount = UINT32_MAX;
	static uint32_t g_ibFirst = 0, g_ibCount = UINT32_MAX;
	static std::vector<PendingTexture> g_pendingTextures;
	static std::vector<PendingUniform> g_pendingUniforms;
	static const bgfx::TransientVertexBuffer* g_tvb = NULL;
	static const bgfx::TransientIndexBuffer* g_tib = NULL;
	static uint32_t g_tvbStart = 0, g_tvbCount = UINT32_MAX;

	// transient ring
	static GLuint g_transientVbo = 0, g_transientIbo = 0, g_transientVao = 0;
	static uint8_t* g_transientVboMem = NULL;
	static uint8_t* g_transientIboMem = NULL;
	static size_t g_transientVboCap = 0, g_transientIboCap = 0;
	static size_t g_transientVboUsed = 0, g_transientIboUsed = 0;

	static GLuint g_scratchFbo = 0;
	static GLuint g_defaultFbo = 0;

	// glGetError() is a synchronous pipeline flush on Mali. Calling it after
	// every texture upload and every draw serialised the GPU against the CPU and
	// was the dominant cost of the VDP2 layers (measured: 26-63 ms/frame for
	// three quads). Diagnostics are opt-in now.
	static bool glDiagEnabled()
	{
		static const bool on = getenv("PDS_GL_DIAG") != nullptr;
		return on;
	}
	static void (*g_swapHook)(void) = NULL;
	static void (*g_preSwapHook)(void) = NULL;

	// shader blobs registered by createEmbeddedShader, consumed by createProgram
	struct ShaderSlot { bool used = false; const uint8_t* data = NULL; uint32_t size = 0; char name[64] = {0}; };
	static const int kMaxShaders = 256;
	static ShaderSlot g_shaderSlots[kMaxShaders];
	static int g_shaderCount = 0;
}

// ---------------------------------------------------------------------------
// GL loading
// ---------------------------------------------------------------------------
static bool loadEGL()
{
	g_eglLib = dlopen("libEGL.so.1", RTLD_NOW);
	if (!g_eglLib) g_eglLib = dlopen("libEGL.so", RTLD_NOW);
	if (!g_eglLib)
	{
		fprintf(stderr, "[gles] libEGL not found: %s\n", dlerror());
		return false;
	}
	p_eglGetDisplay = (PFN_eglGetDisplay)dlsym(g_eglLib, "eglGetDisplay");
	p_eglGetPlatformDisplay = (PFN_eglGetPlatformDisplay)dlsym(g_eglLib, "eglGetPlatformDisplay");
	p_eglInitialize = (PFN_eglInitialize)dlsym(g_eglLib, "eglInitialize");
	p_eglChooseConfig = (PFN_eglChooseConfig)dlsym(g_eglLib, "eglChooseConfig");
	p_eglCreateContext = (PFN_eglCreateContext)dlsym(g_eglLib, "eglCreateContext");
	p_eglCreateWindowSurface = (PFN_eglCreateWindowSurface)dlsym(g_eglLib, "eglCreateWindowSurface");
	p_eglCreatePbufferSurface = (PFN_eglCreatePbufferSurface)dlsym(g_eglLib, "eglCreatePbufferSurface");
	p_eglMakeCurrent = (PFN_eglMakeCurrent)dlsym(g_eglLib, "eglMakeCurrent");
	p_eglSwapBuffers = (PFN_eglSwapBuffers)dlsym(g_eglLib, "eglSwapBuffers");
	p_eglSwapInterval = (PFN_eglSwapInterval)dlsym(g_eglLib, "eglSwapInterval");
	p_eglGetError = (PFN_eglGetError)dlsym(g_eglLib, "eglGetError");
	p_eglGetProcAddress = (PFN_eglGetProcAddress)dlsym(g_eglLib, "eglGetProcAddress");
	p_eglBindAPI = (PFN_eglBindAPI)dlsym(g_eglLib, "eglBindAPI");
	return p_eglGetDisplay && p_eglInitialize && p_eglCreateContext && p_eglMakeCurrent;
}

static void* glSym(const char* name)
{
	void* p = NULL;
	if (p_eglGetProcAddress) p = (void*)p_eglGetProcAddress(name);
	if (!p && g_glLib) p = dlsym(g_glLib, name);
	return p;
}

static bool loadGL()
{
	g_glLib = dlopen("libGLESv2.so.2", RTLD_NOW);
	if (!g_glLib) g_glLib = dlopen("libGLESv2.so", RTLD_NOW);
	bool ok = true;
#define LOAD_GL(ret, name, args) \
	p_##name = (PFN_##name)glSym(#name); \
	if (!p_##name) { fprintf(stderr, "[gles] missing %s\n", #name); ok = false; }
	GL_FUNCS(LOAD_GL)
#undef LOAD_GL
	return ok;
}

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
static GLenum toGLFormat(bgfx::TextureFormat::Enum f, GLenum& internal, GLenum& type, bool& integer)
{
	integer = false;
	switch (f)
	{
	case bgfx::TextureFormat::RGBA8:
	case bgfx::TextureFormat::BGRA8:
		internal = GL_RGBA8; type = GL_UNSIGNED_BYTE; return GL_RGBA;
	case bgfx::TextureFormat::R8:
		internal = GL_R8; type = GL_UNSIGNED_BYTE; return GL_RED;
	case bgfx::TextureFormat::R8U:
		internal = GL_R8UI; type = GL_UNSIGNED_BYTE; integer = true; return GL_RED_INTEGER;
	case bgfx::TextureFormat::R32F:
		internal = GL_R32F; type = GL_FLOAT; return GL_RED;
	case bgfx::TextureFormat::RGBA32F:
		internal = GL_RGBA32F; type = GL_FLOAT; return GL_RGBA;
	case bgfx::TextureFormat::R32U:
		internal = GL_R32UI; type = GL_UNSIGNED_INT; integer = true; return GL_RED_INTEGER;
	case bgfx::TextureFormat::D24S8:
		internal = GL_DEPTH24_STENCIL8; type = GL_UNSIGNED_INT; return GL_DEPTH_STENCIL;
	default:
		fprintf(stderr, "[gles] unsupported texture format %d, using RGBA8\n", (int)f);
		internal = GL_RGBA8; type = GL_UNSIGNED_BYTE; return GL_RGBA;
	}
}

static void prepareView(bgfx::ViewId id);

// ---------------------------------------------------------------------------
// bgfx API
// ---------------------------------------------------------------------------
namespace bgfx
{
	Memory* Memory::make(size_t _size)
	{
		Memory* m = (Memory*)malloc(sizeof(Memory));
		m->data = (uint8_t*)malloc(_size ? _size : 1);
		m->size = (uint32_t)_size;
		m->offset = 0;
		m->release = NULL;
		m->userData = NULL;
		return m;
	}

	// --- VertexLayout ---------------------------------------------------
	VertexLayout& VertexLayout::begin()
	{
		memset(m_attr, 0, sizeof(m_attr));
		m_stride = 0;
		return *this;
	}

	VertexLayout& VertexLayout::add(Attrib::Enum _attrib, uint8_t _num, AttribType::Enum _type,
	                                bool _normalized, bool _asInt)
	{
		(void)_asInt;
		Attr& a = m_attr[_attrib];
		a.num = _num;
		a.offset = (uint8_t)m_stride;
		a.type = _type;
		a.normalized = _normalized;
		uint8_t size = 0;
		switch (_type)
		{
		case AttribType::Uint8: size = 1; break;
		case AttribType::Int16: size = 2; break;
		case AttribType::Half:  size = 2; break;
		case AttribType::Float: size = 4; break;
		default: size = 4; break;
		}
		m_stride = (uint16_t)(m_stride + size * _num);
		return *this;
	}

	VertexLayout& VertexLayout::end() { return *this; }

	// --- lifetime -------------------------------------------------------
	bool init(const Init& _init)
	{
		if (!loadEGL()) return false;

		if (p_eglBindAPI) p_eglBindAPI(EGL_OPENGL_ES_API);

		void* nwh = _init.platformData.nwh;
		void* ndt = _init.platformData.ndt;

		// Adopt mode: the platform layer (SDL3) already created a GLES context
		// and made it current. This is how the port runs on both the handheld
		// (KMSDRM) and the host, and it keeps one context for game + ImGui.
		if (_init.platformData.context != NULL)
		{
			g_context = (EGLContext)_init.platformData.context;
			g_surface = NULL;
			g_headless = true;
			if (!loadGL()) return false;
			p_glGetIntegerv(GL_FRAMEBUFFER_BINDING, (GLint*)&g_defaultFbo);
			p_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
			p_glPixelStorei(GL_PACK_ALIGNMENT, 1);
			p_glDisable(GL_CULL_FACE);
			p_glDisable(GL_BLEND);
			p_glDepthMask(GL_TRUE);
			p_glGenFramebuffers(1, &g_scratchFbo);
			if (const GLubyte* ver = p_glGetString(GL_VERSION))
				fprintf(stderr, "[gles] GL_VERSION: %s (adopted context)\n", (const char*)ver);
			if (const GLubyte* ren = p_glGetString(GL_RENDERER))
				fprintf(stderr, "[gles] GL_RENDERER: %s\n", (const char*)ren);
			return true;
		}

		if (ndt)
		{
			g_display = p_eglGetDisplay(ndt);
		}
		else if (p_eglGetPlatformDisplay)
		{
			// headless: surfaceless Mesa/GBM display
			g_display = p_eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, NULL, NULL);
			if (!g_display) g_display = p_eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, NULL, NULL);
		}
		if (!g_display && p_eglGetDisplay) g_display = p_eglGetDisplay(nwh ? ndt : EGL_DEFAULT_DISPLAY);
		if (!g_display)
		{
			fprintf(stderr, "[gles] no EGL display\n");
			return false;
		}

		int major = 0, minor = 0;
		if (!p_eglInitialize(g_display, &major, &minor))
		{
			fprintf(stderr, "[gles] eglInitialize failed (0x%x)\n", p_eglGetError());
			return false;
		}

		const int cfgAttrs[] = {
			EGL_SURFACE_TYPE, nwh ? EGL_WINDOW_BIT : EGL_PBUFFER_BIT,
			EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
			EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
			EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
			EGL_NONE
		};
		int numConfigs = 0;
		if (!p_eglChooseConfig(g_display, cfgAttrs, &g_config, 1, &numConfigs) || numConfigs < 1)
		{
			fprintf(stderr, "[gles] eglChooseConfig failed (0x%x)\n", p_eglGetError());
			return false;
		}

		const int ctxAttrs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
		g_context = p_eglCreateContext(g_display, g_config, NULL, ctxAttrs);
		if (!g_context)
		{
			fprintf(stderr, "[gles] eglCreateContext(GLES3) failed (0x%x)\n", p_eglGetError());
			return false;
		}

		if (nwh)
		{
			g_surface = p_eglCreateWindowSurface(g_display, g_config, (EGLNativeWindowType)nwh, NULL);
			g_headless = false;
		}
		else
		{
			const int pbAttrs[] = { EGL_NONE };
			g_surface = p_eglCreatePbufferSurface(g_display, g_config, pbAttrs);
			g_headless = true;
		}

		if (!p_eglMakeCurrent(g_display, g_surface, g_surface, g_context))
		{
			// surfaceless: try without a surface (EGL_KHR_surfaceless_context)
			if (!p_eglMakeCurrent(g_display, NULL, NULL, g_context))
			{
				fprintf(stderr, "[gles] eglMakeCurrent failed (0x%x)\n", p_eglGetError());
				return false;
			}
			g_headless = true;
		}

		if (!loadGL()) return false;

		p_glGetIntegerv(GL_FRAMEBUFFER_BINDING, (GLint*)&g_defaultFbo);
		p_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		p_glPixelStorei(GL_PACK_ALIGNMENT, 1);
		p_glDisable(GL_CULL_FACE);
		p_glDisable(GL_BLEND);
		p_glDepthMask(GL_TRUE);
		p_glGenFramebuffers(1, &g_scratchFbo);

		if (const GLubyte* ver = p_glGetString(GL_VERSION))
			fprintf(stderr, "[gles] GL_VERSION: %s\n", (const char*)ver);
		if (const GLubyte* ren = p_glGetString(GL_RENDERER))
			fprintf(stderr, "[gles] GL_RENDERER: %s\n", (const char*)ren);

		return true;
	}

	void shutdown()
	{
		if (g_scratchFbo) { p_glDeleteFramebuffers(1, &g_scratchFbo); g_scratchFbo = 0; }
		if (g_transientVbo) { p_glDeleteBuffers(1, &g_transientVbo); g_transientVbo = 0; }
		if (g_transientIbo) { p_glDeleteBuffers(1, &g_transientIbo); g_transientIbo = 0; }
		if (g_transientVao) { p_glDeleteVertexArrays(1, &g_transientVao); g_transientVao = 0; }
		free(g_transientVboMem); g_transientVboMem = NULL; g_transientVboCap = 0;
		free(g_transientIboMem); g_transientIboMem = NULL; g_transientIboCap = 0;
		if (g_display && g_context) { p_eglMakeCurrent(g_display, NULL, NULL, NULL); }
	}

	void reset(uint32_t _width, uint32_t _height, uint32_t _flags, RendererType::Enum _type)
	{
		(void)_flags; (void)_type;
		g_width = (int)_width;
		g_height = (int)_height;
		if (getenv("PD_TRACE_GEOM"))
			fprintf(stderr, "[geom] reset backbuffer to %dx%d\n", g_width, g_height);
	}

	RendererType::Enum getRendererType() { return RendererType::OpenGLES; }

	const char* getRendererName(RendererType::Enum _type)
	{
		(void)_type;
		return "OpenGL ES 3";
	}

	const Caps* getCaps()
	{
		static Caps caps;
		static bool init = false;
		if (!init)
		{
			init = true;
			GLint maxTex = 2048;
			if (p_glGetIntegerv) p_glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTex);
			caps.limits.maxTextureSize = (uint32_t)maxTex;
			caps.limits.maxViews = 256;
		}
		return &caps;
	}

	void setDebug(uint32_t) {}
	void setMarker(const char*) {}
	void setName(TextureHandle _handle, const char* _name, int32_t)
	{
		if (isTexHandle(_handle) && (*texSlot(_handle)).used && _name)
			(*texSlot(_handle)).name = _name;
	}

	void frame()
	{
		if (g_preSwapHook) g_preSwapHook();
		{
			// Diagnostics: PDS_TRACE_GL=1 logs draw counts, PDS_DUMP_FRAME=<n>
			// writes frame n's back buffer to /tmp as a PPM (host debugging).
			static const char* traceEnv = getenv("PDS_TRACE_GL");
			static const char* dumpEnv = getenv("PDS_DUMP_FRAME");
			static unsigned frameNo = 0;
			static unsigned submitsThisFrame = 0;
			++submitsThisFrame;
			if (traceEnv && (frameNo % 60) == 0)
				fprintf(stderr, "[gles] frame %u: %u submits\n", frameNo, submitsThisFrame);
			// A snapshot can also be requested at runtime: touching /tmp/pd_snap
			// makes the next frame dump itself once. That removes the guesswork
			// of catching a specific frame while playing on the device.
			const bool snapRequested = (access("/tmp/pd_snap", F_OK) == 0);
			if (snapRequested) unlink("/tmp/pd_snap");
			if (snapRequested ||
			    (dumpEnv && (frameNo == (unsigned)atoi(dumpEnv) || (frameNo > 60 && frameNo % 120 == 0))))
			{
				const int w = g_width, h = g_height;
				std::vector<uint8_t> px((size_t)w * h * 4);
				p_glBindFramebuffer(GL_FRAMEBUFFER, g_defaultFbo);
				p_glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
				char path[256];
				snprintf(path, sizeof(path), "/tmp/pd_frame_%u.ppm", frameNo);
				if (FILE* f = fopen(path, "wb"))
				{
					fprintf(f, "P6\n%d %d\n255\n", w, h);
					for (int y = h - 1; y >= 0; --y)
						for (int x = 0; x < w; ++x)
						{
							const uint8_t* p = &px[((size_t)y * w + x) * 4];
							fputc(p[0], f); fputc(p[1], f); fputc(p[2], f);
						}
					fclose(f);
				}
				fprintf(stderr, "[gles] dumped frame %u (%dx%d) -> %s\n", frameNo, w, h, path);
				// also dump every render target so we can see which stage is empty
				for (int i = 0; i < kMaxFrameBuffers; ++i)
				{
					if (!g_fbos[i].used || !g_fbos[i].fbo || g_fbos[i].w == 0) continue;
					const int fw = g_fbos[i].w, fh = g_fbos[i].h;
					std::vector<uint8_t> fp((size_t)fw * fh * 4);
					p_glBindFramebuffer(GL_FRAMEBUFFER, g_fbos[i].fbo);
					if (p_glReadBuffer) p_glReadBuffer(GL_COLOR_ATTACHMENT0);
					p_glReadPixels(0, 0, fw, fh, GL_RGBA, GL_UNSIGNED_BYTE, fp.data());
					char p2[256];
					snprintf(p2, sizeof(p2), "/tmp/pd_fbo_%d.ppm", i);
					if (FILE* f = fopen(p2, "wb"))
					{
						fprintf(f, "P6\n%d %d\n255\n", fw, fh);
						size_t nz = 0;
						for (int y = fh - 1; y >= 0; --y)
							for (int x = 0; x < fw; ++x)
							{
								const uint8_t* p = &fp[((size_t)y * fw + x) * 4];
								if (p[0] | p[1] | p[2]) ++nz;
								fputc(p[0], f); fputc(p[1], f); fputc(p[2], f);
							}
						fclose(f);
						fprintf(stderr, "[gles]   fbo %d (%dx%d) nonzero px %zu\n", i, fw, fh, nz);
					}
				}
				p_glBindFramebuffer(GL_FRAMEBUFFER, g_defaultFbo);
			}
			submitsThisFrame = 0;
			++frameNo;
		}
		if (g_swapHook) g_swapHook();
		else if (!g_headless && g_surface && p_eglSwapBuffers) p_eglSwapBuffers(g_display, g_surface);
		else if (p_glFlush) p_glFlush();

		for (int i = 0; i < 256; ++i) g_views[i].touchedThisFrame = false;

		// orphan transient buffers so the driver need not wait on in-flight draws
		if (g_transientVbo && g_transientVboCap)
			p_glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)g_transientVboCap, NULL, GL_STREAM_DRAW);
		if (g_transientIbo && g_transientIboCap)
			p_glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)g_transientIboCap, NULL, GL_STREAM_DRAW);
		g_transientVboUsed = 0;
		g_transientIboUsed = 0;
	}

	// --- transient geometry ---------------------------------------------
	uint32_t getAvailTransientVertexBuffer(uint32_t _num, const VertexLayout& _layout)
	{
		if (!_layout.getStride()) return 0;
		const size_t need = (size_t)_num * _layout.getStride();
		if (need > g_transientVboCap) return 0;
		if (g_transientVboUsed + need > g_transientVboCap) return 0;
		return _num;
	}

	uint32_t getAvailTransientIndexBuffer(uint32_t _num)
	{
		const size_t need = (size_t)_num * sizeof(uint16_t);
		if (need > g_transientIboCap) return 0;
		if (g_transientIboUsed + need > g_transientIboCap) return 0;
		return _num;
	}

	static void ensureTransient()
	{
		if (!g_transientVbo)
		{
			g_transientVboCap = 8u << 20;
			g_transientVboMem = (uint8_t*)malloc(g_transientVboCap);
			p_glGenBuffers(1, &g_transientVbo);
			p_glBindBuffer(GL_ARRAY_BUFFER, g_transientVbo);
			p_glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)g_transientVboCap, NULL, GL_STREAM_DRAW);
		}
		if (!g_transientIbo)
		{
			g_transientIboCap = 2u << 20;
			g_transientIboMem = (uint8_t*)malloc(g_transientIboCap);
			p_glGenBuffers(1, &g_transientIbo);
			p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_transientIbo);
			p_glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)g_transientIboCap, NULL, GL_STREAM_DRAW);
		}
		if (!g_transientVao)
		{
			p_glGenVertexArrays(1, &g_transientVao);
		}
	}

	bool allocTransientBuffers(TransientVertexBuffer* _tvb, const VertexLayout& _layout,
	                           uint32_t _numVertices, TransientIndexBuffer* _tib, uint32_t _numIndices)
	{
		ensureTransient();
		const size_t vneed = (size_t)_numVertices * _layout.getStride();
		const size_t ineed = (size_t)_numIndices * sizeof(uint16_t);
		if (g_transientVboUsed + vneed > g_transientVboCap) return false;
		if (g_transientIboUsed + ineed > g_transientIboCap) return false;

		// wrap-around: start the frame's ring again rather than failing
		_tvb->data = g_transientVboMem + g_transientVboUsed;
		_tvb->size = (uint32_t)vneed;
		_tvb->startVertex = 0;
		_tvb->stride = _layout.getStride();
		_tvb->layout = _layout;
		_tvb->handle = bgfx::kInvalidHandle;

		_tib->data = g_transientIboMem + g_transientIboUsed;
		_tib->size = (uint32_t)ineed;
		_tib->startIndex = 0;
		_tib->isIndex16 = true;
		_tib->handle = bgfx::kInvalidHandle;

		g_transientVboUsed += vneed;
		g_transientIboUsed += ineed;
		return true;
	}

	void allocTransientVertexBuffer(TransientVertexBuffer* _tvb, uint32_t _num, const VertexLayout& _layout)
	{
		ensureTransient();
		const size_t need = (size_t)_num * _layout.getStride();
		_tvb->data = g_transientVboMem + g_transientVboUsed;
		_tvb->size = (uint32_t)need;
		_tvb->startVertex = 0;
		_tvb->stride = _layout.getStride();
		_tvb->layout = _layout;
		_tvb->handle = kInvalidHandle;
		g_transientVboUsed += need;
	}

	void allocTransientIndexBuffer(TransientIndexBuffer* _tib, uint32_t _num, bool _index32)
	{
		ensureTransient();
		(void)_index32;
		const size_t need = (size_t)_num * sizeof(uint16_t);
		_tib->data = g_transientIboMem + g_transientIboUsed;
		_tib->size = (uint32_t)need;
		_tib->startIndex = 0;
		_tib->isIndex16 = true;
		_tib->handle = kInvalidHandle;
		g_transientIboUsed += need;
	}

	// --- shaders ---------------------------------------------------------
	// bgfx shader blob: magic('VSH'/'FSH') | version<<24 | hashIn | [hashOut] |
	// uniforms[] | shaderSize | source. Source for GL/GLES is plain text.
	static bool parseShaderBlob(const uint8_t* data, uint32_t size, std::string& outSource,
	                            std::vector<std::string>& outUniforms)
	{
		if (size < 8) return false;
		const uint32_t magic = *(const uint32_t*)data;
		const char* m = (const char*)&magic;
		if (!((m[0] == 'V' || m[0] == 'F' || m[0] == 'C') && m[1] == 'S' && m[2] == 'H'))
		{
			// not a header: treat the whole blob as source (some builds embed raw text)
			outSource.assign((const char*)data, size);
			return true;
		}
		const int version = (magic >> 24) & 0xff;
		uint32_t off = 8;
		if (version >= 6) off += 4;                 // hashOut
		if (off + 2 > size) return false;
		const uint16_t count = *(const uint16_t*)(data + off); off += 2;
		for (uint16_t i = 0; i < count; ++i)
		{
			if (off >= size) return false;
			const uint8_t nameSize = data[off++];
			if (off + nameSize + 8 > size) return false;
			outUniforms.emplace_back((const char*)(data + off), nameSize);
			off += nameSize;
			off += 2;                                // type, num
			off += 2;                                // regIndex
			off += 2;                                // regCount
			if (version >= 8) off += 2;              // texInfo
			if (version >= 10) off += 2;             // texFormat
		}
		if (off + 4 > size) return false;
		const uint32_t srcSize = *(const uint32_t*)(data + off); off += 4;
		if (off + srcSize > size) return false;
		outSource.assign((const char*)(data + off), srcSize);
		return true;
	}

	static GLuint compileStage(GLenum type, const std::string& src, const char* label)
	{
		// The embedded ESSL blobs are bgfx's GLSL-ES output. Two things are still
		// needed to compile them under a strict "#version 300 es" front end:
		//   * ES requires an explicit precision for sampler types;
		//   * bgfx's compiler emitted integer mod() for VDP2 tile maths, which
		//     exists in desktop GLSL but not in GLSL ES 3.0.
		// GLSL ES 3.00 refuses to let a shader overload a built-in, so the
		// integer mod() that bgfx's compiler emitted for VDP2 tile maths has to
		// be renamed rather than overloaded. Every call site is rewritten and
		// the replacements cover both the integer and float forms.
		std::string body;
		body.reserve(src.size() + 64);
		for (size_t i = 0; i < src.size(); )
		{
			const bool identStart = (i == 0) || !(isalnum((unsigned char)src[i - 1]) || src[i - 1] == '_');
			if (identStart && src.compare(i, 3, "mod") == 0)
			{
				size_t j = i + 3;
				while (j < src.size() && (src[j] == ' ' || src[j] == '\t')) ++j;
				if (j < src.size() && src[j] == '(')
				{
					body += "pd_mod";
					i += 3;
					continue;
				}
			}
			body += src[i++];
		}

		// ------------------------------------------------------------------
		// Mali fixes for the VDP2 tile shader.
		//
		// Both were found by comparing our decompiled ESSL against the Lagi port's
		// independent VDP2 implementation (Lagi/shaders/vdp2_nbg_f.cg), which
		// documents the first one as the cause of "cyan/teal title corruption" —
		// exactly the garbage this port showed on tiled layers:
		//
		//  1. characterOffset must be characterNumber * 32. A Saturn character
		//     number is in 32-byte units; without the scale every pattern is read
		//     32x too low in VRAM.
		//  2. In 256-colour mode (CHCN==1) the 8-bit dot is already the palette
		//     index. Applying the 4bpp pattern palette number selects unrelated
		//     CRAM banks.
		//
		// pdPatch is loud on purpose: if the embedded shader text ever changes
		// shape, a silent no-op here would quietly reintroduce the bug.
		auto pdPatch = [&](std::string& body, const std::string& from,
		                   const std::string& to, const char* what) -> void {
			const size_t at = body.find(from);
			if (at == std::string::npos)
			{
				fprintf(stderr, "[gles] WARNING: shader patch '%s' did not apply\n", what);
				return;
			}
			body.replace(at, from.size(), to);
			fprintf(stderr, "[gles] shader patch applied: %s\n", what);
		};
		if (strstr(label, "VDP2_ps") != nullptr)
		{
			pdPatch(body,
			        "characterOffset_54 = characterNumber_56;",
			        "characterOffset_54 = (characterNumber_56 * 32);",
			        "character offset is 32-byte units");
			pdPatch(body,
			        "          paletteOffset_52 = (((\n"
			        "            (paletteNumber_55 << 4)\n"
			        "           + dotColor_53) * 2) + (texelValue_10 * 512));",
			        "          paletteOffset_52 = ((texelValue_4 == 1) ? (dotColor_53 * 2)"
			        " : ((((paletteNumber_55 << 4) + dotColor_53) * 2) + (texelValue_10 * 512)));",
			        "256-colour palette index");
		}

		std::string full = "#version 300 es\n";
		full += "precision highp int;\n"
		        "precision highp float;\n"
		        "precision highp sampler2D;\n"
		        "precision highp usampler2D;\n"
		        "precision highp isampler2D;\n"
		        "precision highp samplerCube;\n"
		        "float pd_mod(float a, float b) { return a - b * floor(a / b); }\n"
		        "vec2 pd_mod(vec2 a, float b) { return a - b * floor(a / b); }\n"
		        "vec3 pd_mod(vec3 a, float b) { return a - b * floor(a / b); }\n"
		        "vec4 pd_mod(vec4 a, float b) { return a - b * floor(a / b); }\n"
		        "int pd_mod(int a, int b) { return a % b; }\n"
		        "ivec2 pd_mod(ivec2 a, ivec2 b) { return a % b; }\n"
		        "ivec3 pd_mod(ivec3 a, ivec3 b) { return a % b; }\n";
		// PD_PROBE_VRAM=1 replaces a fragment shader's entry point with a probe
		// that paints the VDP2 VRAM texel under each fragment. It answers "is the
		// shader's input data actually there?" without touching engine code.
		if (getenv("PD_PROBE_VRAM") && type == GL_FRAGMENT_SHADER
		    && body.find("s_VDP2_RAM") != std::string::npos
		    && body.find("void main ()") != std::string::npos)
		{
			const size_t at = body.find("void main ()");
			body.replace(at, strlen("void main ()"), "void pd_original_main ()");
			body +=
			    "\nvoid main()\n"
			    "{\n"
			    "  ivec2 p = ivec2(gl_FragCoord.x * 256.0 / 352.0, gl_FragCoord.y * 2048.0 / 224.0);\n"
			    "  uint v = texelFetch(s_VDP2_RAM, p, 0).x;\n"
			    "  bgfx_FragColor = vec4(float(v) / 255.0, float(v) / 255.0, float(v) / 255.0, 1.0);\n"
			    "}\n";
		}
		full += body;

		GLuint sh = p_glCreateShader(type);
		const GLchar* p = full.c_str();
		GLint len = (GLint)full.size();
		p_glShaderSource(sh, 1, &p, &len);
		p_glCompileShader(sh);
		GLint ok = 0;
		p_glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
		if (!ok)
		{
			GLint logLen = 0;
			p_glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &logLen);
			std::vector<char> log(logLen > 1 ? logLen : 1, 0);
			p_glGetShaderInfoLog(sh, logLen, NULL, log.data());
			fprintf(stderr, "[gles] %s compile failed:\n%s\n", label, log.data());
			// dump the first lines to make offline diagnosis possible
			fprintf(stderr, "[gles] --- source (first 600 bytes) ---\n%.600s\n", full.c_str());
			p_glDeleteShader(sh);
			return 0;
		}
		return sh;
	}

	ShaderHandle createEmbeddedShader(const EmbeddedShader* _es, RendererType::Enum _type, const char* _name)
	{
		if (!_es || !_name) return kInvalidHandle;

		static std::unordered_map<std::string, uint16_t> cache;
		auto cached = cache.find(_name);
		if (cached != cache.end()) return cached->second;

		for (const EmbeddedShader* es = _es; es->name != NULL; ++es)
		{
			if (0 != strcmp(es->name, _name)) continue;
			for (int i = 0; i < RendererType::Count; ++i)
			{
				if (es->data[i].type != _type) continue;
				if (!es->data[i].data || !es->data[i].size)
				{
					fprintf(stderr, "[gles] shader '%s': no %s payload embedded\n", _name,
					        _type == RendererType::OpenGLES ? "ESSL" : "GLSL");
					return kInvalidHandle;
				}
				if (g_shaderCount >= kMaxShaders)
				{
					fprintf(stderr, "[gles] shader table full\n");
					return kInvalidHandle;
				}
				ShaderSlot& slot = g_shaderSlots[g_shaderCount];
				slot.used = true;
				slot.data = es->data[i].data;
				slot.size = es->data[i].size;
				snprintf(slot.name, sizeof(slot.name), "%s", _name);
				const uint16_t handle = (uint16_t)g_shaderCount++;
				cache[_name] = handle;
				return handle;
			}
		}
		fprintf(stderr, "[gles] shader '%s' not present in embedded table\n", _name);
		return kInvalidHandle;
	}

	ProgramHandle createProgram(ShaderHandle _vsh, ShaderHandle _psh, bool)
	{
		if (_vsh >= kMaxShaders || _psh >= kMaxShaders) return kInvalidHandle;
		if (!g_shaderSlots[_vsh].used || !g_shaderSlots[_psh].used) return kInvalidHandle;

		std::string vsSrc, psSrc;
		std::vector<std::string> vsU, psU;
		if (!parseShaderBlob(g_shaderSlots[_vsh].data, g_shaderSlots[_vsh].size, vsSrc, vsU))
		{
			fprintf(stderr, "[gles] cannot parse %s\n", g_shaderSlots[_vsh].name);
			return kInvalidHandle;
		}
		if (!parseShaderBlob(g_shaderSlots[_psh].data, g_shaderSlots[_psh].size, psSrc, psU))
		{
			fprintf(stderr, "[gles] cannot parse %s\n", g_shaderSlots[_psh].name);
			return kInvalidHandle;
		}

		// Shader override from disk. The engine's blobs are baked in and there is
		// no shaderc in the tree, so this is how a shader is iterated on the
		// device: drop <PD_SHADER_DIR>/<name>.glsl and it replaces the embedded
		// source. Also how the Mali-specific arithmetic can be tried before it is
		// baked into the .sc files.
		{
			const char* dir = getenv("PD_SHADER_DIR");
			if (dir && dir[0])
			{
				auto tryOverride = [&](std::string& src, const char* name) {
					char path[512];
					snprintf(path, sizeof(path), "%s/%s.glsl", dir, name);
					FILE* f = fopen(path, "rb");
					if (!f) return;
					std::string text;
					char buf[4096];
					size_t got;
					while ((got = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, got);
					fclose(f);
					if (!text.empty())
					{
						src = text;
						fprintf(stderr, "[gles] shader override: %s (%zu bytes)\n", path, text.size());
					}
				};
				tryOverride(vsSrc, g_shaderSlots[_vsh].name);
				tryOverride(psSrc, g_shaderSlots[_psh].name);
			}
		}

		// Diagnostic: PD_DUMP_SHADERS=<dir> writes every compiled source to disk,
		// which is how the optimised variants are produced from the real input.
		if (const char* dd = getenv("PD_DUMP_SHADERS"))
		{
			auto dumpOne = [&](const std::string& src, const char* name) {
				char path[512];
				snprintf(path, sizeof(path), "%s/%s.glsl", dd, name);
				if (FILE* f = fopen(path, "wb")) { fwrite(src.data(), 1, src.size(), f); fclose(f); }
			};
			dumpOne(vsSrc, g_shaderSlots[_vsh].name);
			dumpOne(psSrc, g_shaderSlots[_psh].name);
		}

		GLuint vs = compileStage(GL_VERTEX_SHADER, vsSrc, g_shaderSlots[_vsh].name);
		if (!vs) return kInvalidHandle;
		GLuint fs = compileStage(GL_FRAGMENT_SHADER, psSrc, g_shaderSlots[_psh].name);
		if (!fs) { p_glDeleteShader(vs); return kInvalidHandle; }

		GLuint prog = p_glCreateProgram();
		p_glAttachShader(prog, vs);
		p_glAttachShader(prog, fs);
		p_glLinkProgram(prog);
		GLint ok = 0;
		p_glGetProgramiv(prog, GL_LINK_STATUS, &ok);
		if (!ok)
		{
			GLint logLen = 0;
			p_glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &logLen);
			std::vector<char> log(logLen > 1 ? logLen : 1, 0);
			p_glGetProgramInfoLog(prog, logLen, NULL, log.data());
			fprintf(stderr, "[gles] link failed for %s/%s:\n%s\n",
			        g_shaderSlots[_vsh].name, g_shaderSlots[_psh].name, log.data());
			p_glDeleteProgram(prog);
			p_glDeleteShader(vs);
			p_glDeleteShader(fs);
			return kInvalidHandle;
		}
		p_glDeleteShader(vs);
		p_glDeleteShader(fs);

		uint16_t handle = kInvalidHandle;
		Program* p = allocSlot(g_programs, handle, kProgBase);
		if (!p) { p_glDeleteProgram(prog); return kInvalidHandle; }
		p->id = prog;
		p->vsName = g_shaderSlots[_vsh].name;
		p->psName = g_shaderSlots[_psh].name;

		GLint active = 0;
		p_glGetProgramiv(prog, GL_ACTIVE_UNIFORMS, &active);
		char nm[256];
		for (GLint i = 0; i < active; ++i)
		{
			GLsizei len = 0; GLint sz = 0; GLenum ty = 0;
			p_glGetActiveUniform(prog, (GLuint)i, sizeof(nm) - 1, &len, &sz, &ty, nm);
			nm[len] = '\0';
			std::string name(nm);
			if (name.size() > 3 && name.compare(name.size() - 3, 3, "[0]") == 0)
				name = name.substr(0, name.size() - 3);
			p->uniforms[name] = p_glGetUniformLocation(prog, name.c_str());
		}
		fprintf(stderr, "[gles] program %s/%s -> handle %u, %d active uniforms\n",
		        p->vsName.c_str(), p->psName.c_str(), handle, active);
		if (getenv("PDS_TRACE_UNIF"))
			for (auto& kv : p->uniforms)
				fprintf(stderr, "[gles]   uniform '%s' loc %d\n", kv.first.c_str(), kv.second);
		return handle;
	}

	// --- textures --------------------------------------------------------
	TextureHandle createTexture2D(uint16_t _width, uint16_t _height, bool, uint16_t,
	                              TextureFormat::Enum _format, uint64_t _flags, const Memory* _mem)
	{
		uint16_t handle = kInvalidHandle;
		Texture* t = allocSlot(g_textures, handle, kTexBase);
		if (!t) return kInvalidHandle;

		GLenum internal, type, fmt;
		bool integer = false;
		fmt = toGLFormat(_format, internal, type, integer);

		p_glGenTextures(1, &t->id);
		p_glBindTexture(GL_TEXTURE_2D, t->id);
		t->w = _width; t->h = _height;
		t->format = _format;
		t->isRT = (_flags & BGFX_TEXTURE_RT) != 0;

		if (fmt == GL_DEPTH_STENCIL)
		{
			// Renderbuffer instead of a depth texture (see createFrameBuffer).
			t->isRenderbuffer = true;
			p_glGenRenderbuffers(1, &t->id);
			p_glBindRenderbuffer(GL_RENDERBUFFER, t->id);
			p_glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, _width, _height);
			p_glBindRenderbuffer(GL_RENDERBUFFER, 0);
			if (_mem && _mem->release) _mem->release(_mem->data, _mem->userData);
			return handle;
		}
		else
		{
			p_glTexImage2D(GL_TEXTURE_2D, 0, (GLint)internal, _width, _height, 0, fmt, type,
			               (_mem && _mem->data) ? _mem->data : NULL);
			if (glDiagEnabled())
				if (GLenum e = p_glGetError())
					fprintf(stderr, "[gles]   texImage2D err 0x%x (internal 0x%x fmt 0x%x type 0x%x)\n",
					        e, internal, fmt, type);
		}

		// Integer textures cannot be filtered: GL only allows NEAREST for them,
		// and any other filter makes the texture incomplete, so every texelFetch
		// in the VDP2 shaders would read zero. This is why the layers rendered
		// black before the port forced NEAREST here.
		const bool point = integer || (_flags & (BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT)) != 0;
		const GLint filt = point ? GL_NEAREST : GL_LINEAR;
		p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filt);
		p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filt);
		p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
		                  (_flags & BGFX_SAMPLER_U_CLAMP) ? GL_CLAMP_TO_EDGE : GL_REPEAT);
		p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
		                  (_flags & BGFX_SAMPLER_V_CLAMP) ? GL_CLAMP_TO_EDGE : GL_REPEAT);

		if (glDiagEnabled())
			if (GLenum err = p_glGetError())
				fprintf(stderr, "[gles] createTexture2D(%ux%u fmt %d) gl error 0x%x\n",
				        _width, _height, (int)_format, err);
		if (_mem && _mem->release) _mem->release(_mem->data, _mem->userData);
		return handle;
	}

	void updateTexture2D(TextureHandle _handle, uint16_t _layer, uint8_t _mip,
	                     uint16_t _x, uint16_t _y, uint16_t _width, uint16_t _height,
	                     const Memory* _mem, uint16_t _pitch)
	{
		(void)_layer; (void)_mip; (void)_pitch;
		if (!isTexHandle(_handle) || !texSlot(_handle)->used || !_mem) return;
		Texture& t = (*texSlot(_handle));
		GLenum internal, type, fmt;
		bool integer = false;
		fmt = toGLFormat(t.format, internal, type, integer);

		p_glBindTexture(GL_TEXTURE_2D, t.id);
		p_glTexSubImage2D(GL_TEXTURE_2D, 0, _x, _y, _width, _height, fmt, type, _mem->data);
		{
			static const char* t5 = getenv("PDS_TRACE_UPL");
			static unsigned u = 0;
			if (t5 && u < 10) { ++u; size_t nz = 0; const uint8_t* q = (const uint8_t*)_mem->data;
				for (size_t k = 0; k < (size_t)_width * _height; ++k) if (q[k]) ++nz;
				fprintf(stderr, "[gles]   upload handle %u gl id %u %ux%u at %u,%u src nonzero %zu\n",
				        _handle, t.id, _width, _height, _x, _y, nz); }
		}
		if (glDiagEnabled())
			if (GLenum e = p_glGetError())
				fprintf(stderr, "[gles] updateTexture2D(handle %u fmt %d %ux%u at %u,%u) gl error 0x%x\n",
				        _handle, (int)t.format, _width, _height, _x, _y, e);
		{
			// One-shot proof that the upload landed: compare source vs GPU copy.
			static bool verified = false;
			static const char* want = getenv("PDS_VERIFY_UPLOAD");
			if (want && !verified && _width == 256 && _height == 2048)
			{
				verified = true;
				size_t srcNonZero = 0;
				const uint8_t* src = (const uint8_t*)_mem->data;
				for (size_t i = 0; i < (size_t)_width * _height; ++i) if (src[i]) ++srcNonZero;
				std::vector<uint8_t> back((size_t)_width * _height, 0);
				GLuint fbo = 0;
				p_glGenFramebuffers(1, &fbo);
				p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
				p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.id, 0);
				p_glReadPixels(0, 0, _width, _height, 0x1903 /*GL_RED_INTEGER*/, GL_UNSIGNED_BYTE, back.data());
				GLenum rd = p_glGetError();
				p_glBindFramebuffer(GL_FRAMEBUFFER, g_defaultFbo);
				p_glDeleteFramebuffers(1, &fbo);
				size_t dstNonZero = 0;
				for (uint8_t b : back) if (b) ++dstNonZero;
				fprintf(stderr, "[gles] upload check: src nonzero %zu, gpu nonzero %zu (read err 0x%x)\n",
				        srcNonZero, dstNonZero, rd);
			}
		}
		if (_mem->release) _mem->release(_mem->data, _mem->userData);
	}

	TextureHandle getTexture(FrameBufferHandle _handle, uint8_t _attachment)
	{
		if (!isFboHandle(_handle) || !fboSlot(_handle)->used) return kInvalidHandle;
		if (_attachment >= (*fboSlot(_handle)).numAttachments) return kInvalidHandle;
		return (*fboSlot(_handle)).attachments[_attachment];
	}

	FrameBufferHandle createFrameBuffer(uint16_t _width, uint16_t _height, TextureFormat::Enum _format,
	                                    uint64_t _textureFlags)
	{
		uint16_t texHandle = createTexture2D(_width, _height, false, 1, _format,
		                                     _textureFlags | BGFX_TEXTURE_RT, NULL);
		if (texHandle == kInvalidHandle) return kInvalidHandle;
		Attachment at;
		at.init(texHandle);
		return createFrameBuffer(1, &at, false);
	}

	FrameBufferHandle createFrameBuffer(uint8_t _num, const Attachment* _attachment, bool)
	{
		uint16_t handle = kInvalidHandle;
		FrameBuffer* fb = allocSlot(g_fbos, handle, kFboBase);
		if (!fb) return kInvalidHandle;

		while (p_glGetError() != GL_NO_ERROR) {}
		p_glGenFramebuffers(1, &fb->fbo);
		p_glBindFramebuffer(GL_FRAMEBUFFER, fb->fbo);
		fb->numAttachments = _num > 4 ? 4 : _num;
		GLenum drawBufs[4] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT0 };
		int colorAttachments = 0;
		for (int i = 0; i < fb->numAttachments; ++i)
		{
			TextureHandle th = _attachment[i].handle;
			fb->attachments[i] = th;
			if (th == kInvalidHandle || !isTexHandle(th) || !(*texSlot(th)).used) continue;
			Texture& t = (*texSlot(th));
			if (t.format == TextureFormat::D24S8)
			{
				p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
				                            GL_RENDERBUFFER, t.id);
			}
			else
			{
				GLenum att = GL_COLOR_ATTACHMENT0 + colorAttachments;
				p_glFramebufferTexture2D(GL_FRAMEBUFFER, att, GL_TEXTURE_2D, t.id, 0);
				drawBufs[colorAttachments] = att;
				colorAttachments++;
				if (t.w > fb->w) fb->w = t.w;
				if (t.h > fb->h) fb->h = t.h;
			}
		}
		if (colorAttachments > 1)
		{
			p_glDrawBuffers(colorAttachments, drawBufs);
		}
		GLenum status = p_glCheckFramebufferStatus(GL_FRAMEBUFFER);
		if (status != GL_FRAMEBUFFER_COMPLETE)
		{
			fprintf(stderr, "[gles] framebuffer 0x%x incomplete: 0x%x (%ux%u, %d attachments, glerr 0x%x)\n",
			        handle, status, fb->w, fb->h, fb->numAttachments, p_glGetError());
			for (int i = 0; i < fb->numAttachments; ++i)
			{
				TextureHandle th = _attachment[i].handle;
				if (isTexHandle(th) && texSlot(th)->used)
					fprintf(stderr, "[gles]   attachment %d: handle %u fmt %d %ux%u id %u\n", i, th,
					        (int)texSlot(th)->format, texSlot(th)->w, texSlot(th)->h, texSlot(th)->id);
				else
					fprintf(stderr, "[gles]   attachment %d: handle %u INVALID\n", i, th);
			}
		}
		p_glBindFramebuffer(GL_FRAMEBUFFER, g_defaultFbo);
		return handle;
	}

	// --- buffers ---------------------------------------------------------
	VertexBufferHandle createVertexBuffer(const Memory* _mem, const VertexLayout& _layout, uint16_t)
	{
		uint16_t handle = kInvalidHandle;
		Buffer* b = allocSlot(g_buffers, handle, kBufBase);
		if (!b) return kInvalidHandle;
		b->isIndex = false;
		b->layout = _layout;
		b->count = (_mem && _layout.getStride()) ? _mem->size / _layout.getStride() : 0;
		p_glGenBuffers(1, &b->id);
		p_glBindBuffer(GL_ARRAY_BUFFER, b->id);
		p_glBufferData(GL_ARRAY_BUFFER, _mem ? (GLsizeiptr)_mem->size : 0,
		               _mem ? _mem->data : NULL, GL_STATIC_DRAW);
		if (_mem && _mem->release) _mem->release(_mem->data, _mem->userData);
		return handle;
	}

	IndexBufferHandle createIndexBuffer(const Memory* _mem, uint16_t)
	{
		uint16_t handle = kInvalidHandle;
		Buffer* b = allocSlot(g_buffers, handle, kBufBase);
		if (!b) return kInvalidHandle;
		b->isIndex = true;
		b->count = _mem ? _mem->size / sizeof(uint16_t) : 0;
		p_glGenBuffers(1, &b->id);
		p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, b->id);
		p_glBufferData(GL_ELEMENT_ARRAY_BUFFER, _mem ? (GLsizeiptr)_mem->size : 0,
		               _mem ? _mem->data : NULL, GL_STATIC_DRAW);
		if (_mem && _mem->release) _mem->release(_mem->data, _mem->userData);
		return handle;
	}

	void destroy(uint16_t h)
	{
		if (isTexHandle(h))
		{
			Texture& t = *texSlot(h);
			if (!t.used) return;
			if (t.id)
			{
				if (t.isRenderbuffer) p_glDeleteRenderbuffers(1, &t.id);
				else p_glDeleteTextures(1, &t.id);
			}
			if (t.fbo) p_glDeleteFramebuffers(1, &t.fbo);
			t = Texture();
		}
		else if (isFboHandle(h))
		{
			FrameBuffer& f = *fboSlot(h);
			if (!f.used) return;
			if (f.fbo) p_glDeleteFramebuffers(1, &f.fbo);
			f = FrameBuffer();
		}
		else if (isBufHandle(h))
		{
			Buffer& b = *bufSlot(h);
			if (!b.used) return;
			if (b.id) p_glDeleteBuffers(1, &b.id);
			b = Buffer();
		}
		else if (isProgHandle(h))
		{
			Program& p = *progSlot(h);
			if (!p.used) return;
			if (p.id) p_glDeleteProgram(p.id);
			p = Program();
		}
		else if (isUniHandle(h))
		{
			Uniform& u = *uniSlot(h);
			if (!u.used) return;
			u = Uniform();
		}
	}

	bool isValid(uint16_t h)
	{
		if (isTexHandle(h)) return texSlot(h)->used;
		if (isFboHandle(h)) return fboSlot(h)->used;
		if (isBufHandle(h)) return bufSlot(h)->used;
		if (isProgHandle(h)) return progSlot(h)->used;
		if (isUniHandle(h)) return uniSlot(h)->used;
		return false;
	}

	void readTexture(TextureHandle h, void* _data, uint8_t)
	{
		if (!isTexHandle(h) || !(*texSlot(h)).used || !_data) return;
		Texture& t = (*texSlot(h));
		if (!t.fbo)
		{
			p_glGenFramebuffers(1, &t.fbo);
			p_glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
			p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.id, 0);
		}
		else
		{
			p_glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
		}
		if (p_glReadBuffer) p_glReadBuffer(GL_COLOR_ATTACHMENT0);
		p_glReadPixels(0, 0, t.w, t.h, GL_RGBA, GL_UNSIGNED_BYTE, _data);
		p_glBindFramebuffer(GL_FRAMEBUFFER, g_defaultFbo);
	}

	// --- views -----------------------------------------------------------
	void setViewName(ViewId _id, const char* _name)
	{
		if (_id < 256 && _name) snprintf(g_views[_id].name, sizeof(g_views[_id].name), "%s", _name);
	}
	void setViewRect(ViewId _id, uint16_t _x, uint16_t _y, uint16_t _w, uint16_t _h)
	{
		if (_id >= 256) return;
		g_views[_id].x = _x; g_views[_id].y = _y; g_views[_id].w = _w; g_views[_id].h = _h;
		if (getenv("PD_TRACE_GEOM"))
			fprintf(stderr, "[geom] setViewRect view=%u rect=%u,%u %ux%u (backbuffer %dx%d)\n",
			        (unsigned)_id, _x, _y, _w, _h, g_width, g_height);
	}
	void setViewScissor(ViewId _id, uint16_t _x, uint16_t _y, uint16_t _w, uint16_t _h)
	{
		if (_id >= 256) return;
		View& v = g_views[_id];
		v.hasScissor = (_w != 0 && _h != 0);
		v.sx = _x; v.sy = _y; v.sw = _w; v.sh = _h;
	}
	void setViewClear(ViewId _id, uint16_t _flags, uint32_t _rgba, float _depth, uint8_t _stencil)
	{
		if (_id >= 256) return;
		View& v = g_views[_id];
		v.clearFlags = _flags; v.clearColor = _rgba; v.clearDepth = _depth; v.clearStencil = _stencil;
	}
	void setViewFrameBuffer(ViewId _id, FrameBufferHandle _handle)
	{
		if (_id >= 256) return;
		g_views[_id].fbo = _handle;
	}
	void setViewTransform(ViewId, const void*, const void*) {}
	void setViewMode(ViewId, ViewMode::Enum) {}
	void setViewOrder(ViewId, ViewId) {}

	void touch(ViewId _id) { prepareView(_id); }

	// --- draw ------------------------------------------------------------
	void setState(uint64_t _state, uint32_t _rgba)
	{
		g_state = _state;
		g_stateRgba = _rgba;
	}

	void setUniform(UniformHandle _handle, const void* _value, uint16_t _num)
	{
		if (!isUniHandle(_handle) || !(*uniSlot(_handle)).used || !_value) return;
		Uniform& u = (*uniSlot(_handle));
		size_t bytes = 0;
		switch (u.type)
		{
		case UniformType::Vec4: bytes = sizeof(float) * 4 * _num; break;
		case UniformType::Mat3: bytes = sizeof(float) * 9 * _num; break;
		case UniformType::Mat4: bytes = sizeof(float) * 16 * _num; break;
		default: bytes = sizeof(float) * 4; break;
		}
		if (bytes > sizeof(u.f)) bytes = sizeof(u.f);
		memcpy(u.f, _value, bytes);
		memcpy(u.u, _value, bytes > sizeof(u.u) ? sizeof(u.u) : bytes);
		u.dirty = true;

		bool already = false;
		for (auto& pu : g_pendingUniforms) if (pu.handle == _handle) { already = true; break; }
		if (!already) g_pendingUniforms.push_back({ _handle });
	}

	void setTexture(uint8_t _stage, UniformHandle _sampler, TextureHandle _handle, uint32_t)
	{
		g_pendingTextures.push_back({ _stage, _sampler, _handle });
	}

	void setVertexBuffer(uint8_t, VertexBufferHandle _handle, uint32_t _start, uint32_t _num)
	{
		g_vb = _handle; g_vbStart = _start; g_vbCount = _num;
	}

	void setIndexBuffer(IndexBufferHandle _handle, uint32_t _first, uint32_t _num)
	{
		g_ib = _handle; g_ibFirst = _first; g_ibCount = _num;
	}

	void setVertexBuffer(uint8_t, const TransientVertexBuffer* _tvb, uint32_t _start, uint32_t _num)
	{
		g_tvb = _tvb; g_tvbStart = _start; g_tvbCount = _num;
		g_vb = kInvalidHandle;
	}

	void setIndexBuffer(const TransientIndexBuffer* _tib, uint32_t _first, uint32_t _num)
	{
		g_tib = _tib;
		g_ib = kInvalidHandle; g_ibFirst = _first; g_ibCount = _num;
	}

	// A built-in "paint it magenta" program, used with PD_FORCE_SOLID=1 to prove
	// that a view's geometry, viewport and target are wired correctly without
	// involving the engine's shaders.
	static GLuint solidProgram()
	{
		static GLuint prog = 0;
		if (prog) return prog;
		static const char* vs =
		    "precision highp float;\n"
		    "in highp vec3 a_position;\n"
		    "void main() { gl_Position = vec4(a_position, 1.0); }\n";
		static const char* fs =
		    "precision highp float;\n"
		    "out mediump vec4 bgfx_FragColor;\n"
		    "void main() { bgfx_FragColor = vec4(1.0, 0.0, 1.0, 1.0); }\n";
		GLuint v = compileStage(GL_VERTEX_SHADER, vs, "pd_solid_vs");
		GLuint f = compileStage(GL_FRAGMENT_SHADER, fs, "pd_solid_fs");
		if (!v || !f) return 0;
		prog = p_glCreateProgram();
		p_glAttachShader(prog, v);
		p_glAttachShader(prog, f);
		p_glLinkProgram(prog);
		return prog;
	}

	void submit(ViewId _id, ProgramHandle _program, uint32_t, uint8_t)
	{
		const uint64_t _state = g_state;
		{
			static const char* t = getenv("PDS_TRACE_GL");
			static unsigned n = 0;
			++n;
			if (t && n < 40)
				fprintf(stderr, "[gles] submit #%u view %u program %u vb %u ib %u\n",
				        n, _id, _program, g_vb, g_ib);
		}
		prepareView(_id);
		if (!isProgHandle(_program) || !progSlot(_program)->used) return;
		Program& prog = (*progSlot(_program));
		static const char* forceSolid = getenv("PD_FORCE_SOLID");
		GLuint solid = forceSolid ? solidProgram() : 0;
		p_glUseProgram(solid ? solid : prog.id);

		// state
		const bool writeR = (_state & BGFX_STATE_WRITE_R) != 0;
		const bool writeG = (_state & BGFX_STATE_WRITE_G) != 0;
		const bool writeB = (_state & BGFX_STATE_WRITE_B) != 0;
		const bool writeA = (_state & BGFX_STATE_WRITE_A) != 0;
		const bool writeZ = (_state & BGFX_STATE_WRITE_Z) != 0;
		(void)writeR; (void)writeG; (void)writeB;
		p_glDepthMask(writeZ ? GL_TRUE : GL_FALSE);

		const uint64_t depthTest = _state & BGFX_STATE_DEPTH_TEST_MASK;
		if (depthTest == 0)
		{
			p_glDisable(GL_DEPTH_TEST);
		}
		else
		{
			p_glEnable(GL_DEPTH_TEST);
			GLenum fn = GL_ALWAYS;
			switch (depthTest)
			{
			case BGFX_STATE_DEPTH_TEST_LESS: fn = GL_LESS; break;
			case BGFX_STATE_DEPTH_TEST_LEQUAL: fn = GL_LEQUAL; break;
			case BGFX_STATE_DEPTH_TEST_EQUAL: fn = GL_EQUAL; break;
			case BGFX_STATE_DEPTH_TEST_GEQUAL: fn = GL_GEQUAL; break;
			case BGFX_STATE_DEPTH_TEST_GREATER: fn = GL_GREATER; break;
			case BGFX_STATE_DEPTH_TEST_NOTEQUAL: fn = GL_NOTEQUAL; break;
			case BGFX_STATE_DEPTH_TEST_NEVER: fn = GL_NEVER; break;
			default: fn = GL_ALWAYS; break;
			}
			p_glDepthFunc(fn);
		}
		(void)writeA;

		// blending: bgfx packs src|dst in the high 32 bits
		const uint32_t blendSrc = (uint32_t)(_state >> 32) & 0x1F;
		const uint32_t blendDst = (uint32_t)(_state >> 32) >> 8 & 0x1F;
		if (blendSrc || blendDst)
		{
			auto mapBlend = [](uint32_t b) -> GLenum {
				switch (b)
				{
				case BGFX_STATE_BLEND_ZERO: return GL_ZERO;
				case BGFX_STATE_BLEND_ONE: return GL_ONE;
				case BGFX_STATE_BLEND_SRC_COLOR: return GL_SRC_COLOR;
				case BGFX_STATE_BLEND_INV_SRC_COLOR: return GL_ONE_MINUS_SRC_COLOR;
				case BGFX_STATE_BLEND_SRC_ALPHA: return GL_SRC_ALPHA;
				case BGFX_STATE_BLEND_INV_SRC_ALPHA: return GL_ONE_MINUS_SRC_ALPHA;
				case BGFX_STATE_BLEND_DST_ALPHA: return GL_DST_ALPHA;
				case BGFX_STATE_BLEND_INV_DST_ALPHA: return GL_ONE_MINUS_DST_ALPHA;
				case BGFX_STATE_BLEND_DST_COLOR: return GL_DST_COLOR;
				case BGFX_STATE_BLEND_INV_DST_COLOR: return GL_ONE_MINUS_DST_COLOR;
				case BGFX_STATE_BLEND_SRC_ALPHA_SAT: return GL_SRC_ALPHA_SATURATE;
				default: return GL_ONE;
				}
			};
			p_glEnable(GL_BLEND);
			p_glBlendFunc(mapBlend(blendSrc), mapBlend(blendDst));
		}
		else
		{
			p_glDisable(GL_BLEND);
		}

		// culling
		if (_state & BGFX_STATE_CULL_CW) { p_glEnable(GL_CULL_FACE); p_glCullFace(GL_BACK); p_glFrontFace(GL_CW); }
		else if (_state & BGFX_STATE_CULL_CCW) { p_glEnable(GL_CULL_FACE); p_glCullFace(GL_BACK); p_glFrontFace(GL_CCW); }
		else p_glDisable(GL_CULL_FACE);

		// textures
		for (auto& pt : g_pendingTextures)
		{
			if (!isUniHandle(pt.sampler) || !(*uniSlot(pt.sampler)).used) continue;
			const std::string& sname = (*uniSlot(pt.sampler)).name;
			auto it = prog.uniforms.find(sname);
			if (it == prog.uniforms.end() || it->second < 0) continue;

			p_glActiveTexture((GLenum)(GL_TEXTURE0 + pt.stage));
			GLuint tex = 0;
			if (isTexHandle(pt.texture) && (*texSlot(pt.texture)).used) tex = (*texSlot(pt.texture)).id;
			p_glBindTexture(GL_TEXTURE_2D, tex);
			p_glUniform1i(it->second, (GLint)pt.stage);
			{
				static const char* t4 = getenv("PDS_TRACE_TEX");
				static unsigned tb = 0;
				if (t4 && tb < 12) { ++tb; fprintf(stderr, "[gles]   bind stage %u %s -> handle %u gl id %u loc %d\n", pt.stage, sname.c_str(), pt.texture, tex, it->second); }
			}
		}

		// uniforms
		for (auto& pu : g_pendingUniforms)
		{
			if (!isUniHandle(pu.handle) || !(*uniSlot(pu.handle)).used) continue;
			Uniform& u = (*uniSlot(pu.handle));
			auto it = prog.uniforms.find(u.name);
			if (it == prog.uniforms.end() || it->second < 0) continue;
			switch (u.type)
			{
			case UniformType::Vec4: p_glUniform4fv(it->second, 1, u.f); break;
			case UniformType::Mat3: p_glUniformMatrix3fv(it->second, 1, GL_FALSE, u.f); break;
			case UniformType::Mat4: p_glUniformMatrix4fv(it->second, 1, GL_FALSE, u.f); break;
			case UniformType::Sampler: p_glUniform1i(it->second, (GLint)u.u[0]); break;
			default: break;
			}
		}

		// geometry
		const bool lines = (_state & BGFX_STATE_PT_LINES) != 0;
		const bool points = (_state & BGFX_STATE_PT_POINTS) != 0;
		const GLenum mode = points ? GL_POINTS : (lines ? GL_LINES : GL_TRIANGLES);

		{
			static const char* t2 = getenv("PDS_TRACE_GL");
			static unsigned d = 0;
			if (t2 && d < 6)
			{
				++d;
				fprintf(stderr, "[gles]   draw: tvb=%p tib=%p vb=%u ib=%u mode=%u count16=%zu\n",
				        (void*)g_tvb, (void*)g_tib, g_vb, g_ib, mode,
				        g_tib ? g_tib->size / sizeof(uint16_t) : (size_t)0);
			}
		}
		if (g_tvb && g_tib && g_tvb->data && g_tib->data)
		{
			// bind vertex layout for the transient chunk
			p_glBindVertexArray(g_transientVao);
			p_glBindBuffer(GL_ARRAY_BUFFER, g_transientVbo);
			p_glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)(g_tvb->data - g_transientVboMem),
			                  (GLsizeiptr)g_tvb->size, g_tvb->data);
			p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_transientIbo);
			p_glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, (GLintptr)(g_tib->data - g_transientIboMem),
			                  (GLsizeiptr)g_tib->size, g_tib->data);

			const VertexLayout& lay = g_tvb->layout;
			for (int a = 0; a < Attrib::Count; ++a)
			{
				if (!lay.has((Attrib::Enum)a)) continue;
				GLint loc = p_glGetAttribLocation(solid ? solid : prog.id, s_attribNames[a]);
				if (loc < 0) continue;
				const VertexLayout::Attr& at = lay.m_attr[a];
				GLenum ty = GL_FLOAT;
				GLboolean norm = at.normalized ? GL_TRUE : GL_FALSE;
				switch (at.type)
				{
				case AttribType::Uint8: ty = GL_UNSIGNED_BYTE; break;
				case AttribType::Int16: ty = GL_SHORT; break;
				case AttribType::Half: ty = 0x140B /* GL_HALF_FLOAT */; break;
				default: ty = GL_FLOAT; break;
				}
				p_glEnableVertexAttribArray((GLuint)loc);
				p_glVertexAttribPointer((GLuint)loc, at.num, ty, norm, lay.getStride(),
				                        (const void*)(uintptr_t)at.offset);
			}
			const GLsizei count = (GLsizei)(g_tib->size / sizeof(uint16_t));
			p_glDrawElements(mode, count, GL_UNSIGNED_SHORT, (const void*)0);
			p_glBindVertexArray(0);
		}
		else if (isBufHandle(g_vb) && (*bufSlot(g_vb)).used && isBufHandle(g_ib) && (*bufSlot(g_ib)).used)
		{
			Buffer& vb = (*bufSlot(g_vb));
			Buffer& ib = (*bufSlot(g_ib));
			static GLuint s_vao = 0;
			if (!s_vao) p_glGenVertexArrays(1, &s_vao);
			p_glBindVertexArray(s_vao);
			p_glBindBuffer(GL_ARRAY_BUFFER, vb.id);
			for (int a = 0; a < Attrib::Count; ++a)
			{
				if (!vb.layout.has((Attrib::Enum)a)) continue;
				GLint loc = p_glGetAttribLocation(solid ? solid : prog.id, s_attribNames[a]);
				{
					static const char* t3 = getenv("PDS_TRACE_GL");
					static unsigned q = 0;
					if (t3 && q < 8) { ++q; fprintf(stderr, "[gles]     attrib %s -> %d (prog %u)\n", s_attribNames[a], loc, _program); }
				}
				if (loc < 0) continue;
				const VertexLayout::Attr& at = vb.layout.m_attr[a];
				GLenum ty = at.type == AttribType::Uint8 ? GL_UNSIGNED_BYTE
				          : at.type == AttribType::Int16 ? GL_SHORT : GL_FLOAT;
				p_glEnableVertexAttribArray((GLuint)loc);
				p_glVertexAttribPointer((GLuint)loc, at.num, ty,
				                        at.normalized ? GL_TRUE : GL_FALSE,
				                        vb.layout.getStride(), (const void*)(uintptr_t)at.offset);
			}
			p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib.id);
			GLsizei count = (GLsizei)(g_ibCount == UINT32_MAX ? ib.count - g_ibFirst : g_ibCount);
			p_glDrawElements(mode, count, GL_UNSIGNED_SHORT,
			                 (const void*)(uintptr_t)(g_ibFirst * sizeof(uint16_t)));
			p_glBindVertexArray(0);
			if (glDiagEnabled())
				if (GLenum e = p_glGetError())
					fprintf(stderr, "[gles]   drawElements(%d) gl error 0x%x (prog %u)\n", count, e, _program);
		}

		{
			// Diagnostic: draws per view, to catch a layer that renders far more
			// often than the one quad it is supposed to.
			static const bool traceDraws = getenv("PD_TRACE_DRAWS") != nullptr;
			if (traceDraws)
			{
				static unsigned perView[256] = {0};
				static unsigned lastTick = 0;
				perView[_id & 0xFF]++;
				const unsigned now = (unsigned)(clock() / (CLOCKS_PER_SEC / 1000));
				if (now - lastTick >= 1000)
				{
					lastTick = now;
					fprintf(stderr, "[draws]");
					for (int i = 0; i < 32; ++i)
						if (perView[i]) fprintf(stderr, " v%d=%u", i, perView[i]);
					fprintf(stderr, "\n");
					memset(perView, 0, sizeof(perView));
				}
			}
		}

		g_pendingTextures.clear();
		g_pendingUniforms.clear();
		g_tvb = NULL;
		g_tib = NULL;
		g_vb = kInvalidHandle;
		g_ib = kInvalidHandle;
	}

	UniformHandle createUniform(const char* _name, UniformType::Enum _type, uint16_t)
	{
		uint16_t handle = kInvalidHandle;
		Uniform* u = allocSlot(g_uniforms, handle, kUniBase);
		if (!u) return kInvalidHandle;
		u->name = _name ? _name : "";
		u->type = _type;
		return handle;
	}

	Encoder* begin(bool) { return NULL; }
	void end(Encoder*) {}
	void Encoder::setState(uint64_t s, uint32_t c) { setState(s, c); }
	void Encoder::setUniform(UniformHandle h, const void* v, uint16_t n) { setUniform(h, v, n); }
	void Encoder::setTexture(uint8_t st, UniformHandle sh, TextureHandle th, uint32_t f) { setTexture(st, sh, th, f); }
	void Encoder::setVertexBuffer(uint8_t st, VertexBufferHandle h, uint32_t sv, uint32_t nv) { setVertexBuffer(st, h, sv, nv); }
	void Encoder::setIndexBuffer(IndexBufferHandle h, uint32_t fi, uint32_t ni) { setIndexBuffer(h, fi, ni); }
	void Encoder::submit(ViewId id, ProgramHandle p, uint32_t d, uint8_t f) { submit(id, p, d, f); }
}

// ---------------------------------------------------------------------------
// view preparation (called from submit/touch)
// ---------------------------------------------------------------------------
static void prepareView(bgfx::ViewId id)
{
	if (id >= 256) return;
	View& v = g_views[id];

	const GLuint target = (v.fbo != bgfx::kInvalidHandle && isFboHandle(v.fbo)
	                       && (*fboSlot(v.fbo)).used) ? (*fboSlot(v.fbo)).fbo : g_defaultFbo;
	p_glBindFramebuffer(GL_FRAMEBUFFER, target);

	const int vw = v.w ? v.w : g_width;
	const int vh = v.h ? v.h : g_height;

	// The engine sizes its output as (W, W*3/4) — a 4:3 frame — and draws the
	// composite quad over all of it, but the game picture only occupies part of
	// that frame. Measured on the device (720x540 frame): content sits at
	// x 32..687, y 0..422, i.e. 656x423, leaving a black inset all round and
	// running off the bottom of a 720x480 panel.
	//
	// So the full-frame view is mapped so that measured content rect lands on
	// the whole panel: 656x423 -> 720x480 (a 3% horizontal difference from a
	// uniform scale, i.e. the picture is not visibly stretched).
	// PD_VIEW_FIT=0 disables it; PD_FRAME_CONTENT="x,y,w,h" overrides the rect.
	{
		static bool inited = false;
		static float sx = 0, sy = 0, ox = 0, oy = 0;
		static bool fit = false;
		if (!inited)
		{
			inited = true;
			const char* off = getenv("PD_VIEW_FIT");
			fit = !(off && off[0] == '0');
			// Content rect inside the 720x540 frame, measured from device
			// captures of the intro/field scenes.
			float cx = 32, cy = 42, cw = 656, ch = 374;
			if (const char* env = getenv("PD_FRAME_CONTENT"))
				sscanf(env, "%f,%f,%f,%f", &cx, &cy, &cw, &ch);
			// The composite frame is (g_width, g_width*3/4), not the height of
			// whichever view happens to be prepared first — using vh here put the
			// picture mostly off-screen.
			const float frameH = (float)g_width * 0.75f;
			if (cw > 0 && ch > 0 && g_width > 0 && g_height > 0)
			{
				// One uniform scale so the picture is not stretched: fill the
				// panel width and let the height land wherever the content
				// aspect puts it, then centre it.
				sx = (float)g_width / cw;
				sy = sx;
				ox = -cx * sx;
				// Vertical nudge so the picture lands centred on the panel;
				// the engine's inset inside the frame is not a constant, so this
				// is measured, not derived. PD_VIEW_YCENTER overrides it.
				float ycenter = 25.0f;
				if (const char* yc = getenv("PD_VIEW_YCENTER")) ycenter = (float)atof(yc);
				oy = -((frameH - cy - ch) * sy) + ((float)g_height - ch * sy) * 0.5f + ycenter;
			}
			if (fit)
				fprintf(stderr, "[port] picture fit: content %.0fx%.0f in %dx%d -> scale %.3fx%.3f offset %.0f,%.0f\n",
				        cw, ch, g_width, g_height, sx, sy, ox, oy);
		}
		if (fit && v.w == g_width && v.h >= g_height && sx > 0)
		{
			const float frameH = (float)g_width * 0.75f;
			p_glViewport((int)(v.x + ox), (int)(v.y + oy),
			             (int)(vw * sx + 0.5f), (int)(frameH * sy + 0.5f));
		}
		else
		{
			p_glViewport(v.x, v.y, vw, vh);
		}
	}

	if (v.hasScissor)
	{
		p_glEnable(GL_SCISSOR_TEST);
		p_glScissor(v.sx, v.sy, v.sw, v.sh);
	}
	else
	{
		p_glDisable(GL_SCISSOR_TEST);
	}

	if (!v.touchedThisFrame)
	{
		v.touchedThisFrame = true;
		GLbitfield bits = 0;
		if (v.clearFlags & BGFX_CLEAR_COLOR) bits |= GL_COLOR_BUFFER_BIT;
		if (v.clearFlags & BGFX_CLEAR_DEPTH) bits |= GL_DEPTH_BUFFER_BIT;
		if (v.clearFlags & BGFX_CLEAR_STENCIL) bits |= GL_STENCIL_BUFFER_BIT;
		if (bits)
		{
			const uint32_t c = v.clearColor;
			p_glClearColor(((c >> 0) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f,
			               ((c >> 16) & 0xFF) / 255.0f, ((c >> 24) & 0xFF) / 255.0f);
			p_glClearDepthf(v.clearDepth);
			p_glClearStencil(v.clearStencil);
			p_glDepthMask(GL_TRUE);
			p_glClear(bits);
		}
	}
}

// ---------------------------------------------------------------------------
// Offscreen presentation — real double buffering over fbdev.
//
// The fbdev EGL surface renders straight into the framebuffer the panel is
// scanning out, so a frame is displayed while GL is still writing it: captured
// on the device, a single frame held two different movie frames side by side
// (tearing), which reads as flashing. libmali rejects glBlitFramebuffer
// (GL_INVALID_OPERATION), so the finished frame is presented by drawing the
// offscreen colour texture into the default framebuffer with one fullscreen
// triangle — plain textured draw, supported everywhere.
// ---------------------------------------------------------------------------
static GLuint g_presentFbo = 0, g_presentTex = 0, g_presentProg = 0, g_presentDepth = 0;
static int g_presentW = 0, g_presentH = 0;

static GLuint presentProgram()
{
	if (g_presentProg) return g_presentProg;
	// compileStage() prepends bgfx's preamble (including #version), so these
	// sources must not carry their own.
	static const char* vs =
	    "out highp vec2 v_uv;\n"
	    "void main() {\n"
	    "  vec2 p = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);\n"
	    "  v_uv = (p + 1.0) * 0.5;\n"
	    "  gl_Position = vec4(p, 0.0, 1.0);\n"
	    "}\n";
	static const char* fs =
	    "precision highp float;\n"
	    "in highp vec2 v_uv;\n"
	    "uniform sampler2D u_frame;\n"
	    "out mediump vec4 o_color;\n"
	    "void main() { o_color = texture(u_frame, v_uv); }\n";
	GLuint v = bgfx::compileStage(GL_VERTEX_SHADER, vs, "pd_present_vs");
	GLuint f = bgfx::compileStage(GL_FRAGMENT_SHADER, fs, "pd_present_fs");
	if (!v || !f) return 0;
	GLuint prog = p_glCreateProgram();
	p_glAttachShader(prog, v);
	p_glAttachShader(prog, f);
	p_glLinkProgram(prog);
	GLint linked = 0;
	p_glGetProgramiv(prog, 0x8B82 /* GL_LINK_STATUS */, &linked);
	if (!linked)
	{
		fprintf(stderr, "[present] offscreen program failed to link\n");
		return 0;
	}
	g_presentProg = prog;
	return g_presentProg;
}

// Redirect the backbuffer to an offscreen colour texture of _w x _h.
extern "C" int pdgles_offscreen_begin(int _w, int _h)
{
	if (_w <= 0 || _h <= 0) return 0;
	if (g_presentFbo && g_presentW == _w && g_presentH == _h) return 1;
	if (!presentProgram() || !p_glGenFramebuffers || !p_glGenTextures ||
	    !p_glFramebufferTexture2D || !p_glBindFramebuffer)
		return 0;

	if (g_presentFbo) { p_glDeleteFramebuffers(1, &g_presentFbo); g_presentFbo = 0; }
	if (g_presentTex) { p_glDeleteTextures(1, &g_presentTex); g_presentTex = 0; }
	if (g_presentDepth) { p_glDeleteRenderbuffers(1, &g_presentDepth); g_presentDepth = 0; }

	p_glGenTextures(1, &g_presentTex);
	p_glBindTexture(GL_TEXTURE_2D, g_presentTex);
	p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, _w, _h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	p_glBindTexture(GL_TEXTURE_2D, 0);

	// Depth-stencil is required: the title screen is 2D and looked fine without
	// it, but the field is 3D and drew a black screen (depth tests against a
	// missing buffer). Costs one renderbuffer at panel size.
	p_glGenRenderbuffers(1, &g_presentDepth);
	p_glBindRenderbuffer(GL_RENDERBUFFER, g_presentDepth);
	p_glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, _w, _h);
	p_glBindRenderbuffer(GL_RENDERBUFFER, 0);

	p_glGenFramebuffers(1, &g_presentFbo);
	p_glBindFramebuffer(GL_FRAMEBUFFER, g_presentFbo);
	p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_presentTex, 0);
	p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, g_presentDepth);
	const GLenum status = p_glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if (status != 0x8CD5 /* GL_FRAMEBUFFER_COMPLETE */)
	{
		fprintf(stderr, "[present] offscreen FBO incomplete (0x%x)\n", status);
		p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
		p_glDeleteFramebuffers(1, &g_presentFbo); g_presentFbo = 0;
		p_glDeleteTextures(1, &g_presentTex); g_presentTex = 0;
		return 0;
	}

	g_presentW = _w;
	g_presentH = _h;
	// Everything the engine thinks it draws to "the backbuffer" now lands here.
	g_defaultFbo = g_presentFbo;
	fprintf(stderr, "[present] offscreen backbuffer %dx%d (double buffered)\n", _w, _h);
	return 1;
}

// Publish the finished offscreen frame into the real framebuffer.
extern "C" void pdgles_offscreen_present(void)
{
	if (!g_presentFbo || !g_presentProg) return;

	p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
	p_glViewport(0, 0, g_width, g_height);
	p_glDisable(GL_SCISSOR_TEST);
	p_glDisable(GL_DEPTH_TEST);
	p_glDisable(GL_BLEND);
	p_glDisable(GL_CULL_FACE);
	if (p_glColorMask) p_glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	p_glDepthMask(GL_TRUE);

	p_glUseProgram(g_presentProg);
	p_glActiveTexture(GL_TEXTURE0);
	p_glBindTexture(GL_TEXTURE_2D, g_presentTex);
	const GLint loc = p_glGetUniformLocation(g_presentProg, "u_frame");
	if (loc >= 0) p_glUniform1i(loc, 0);

	p_glBindVertexArray(0);
	p_glDrawArrays(GL_TRIANGLES, 0, 3);

	p_glBindFramebuffer(GL_FRAMEBUFFER, g_presentFbo);
}

// ---------------------------------------------------------------------------
// platform hooks (used by platform/arm64/renderer_gles.cpp)
// ---------------------------------------------------------------------------
extern "C" void pdgles_set_swap_hook(void (*fn)(void))
{
	g_swapHook = fn;
}

// Drawn after the game's last submit and before the buffer swap: the port's own
// overlay (menu, FPS) lives here so it never interleaves with engine draws.
extern "C" void pdgles_set_pre_swap_hook(void (*fn)(void))
{
	g_preSwapHook = fn;
}

// ---------------------------------------------------------------------------
// test hook: count back-buffer pixels near a colour (used by tests/)
// ---------------------------------------------------------------------------
extern "C" int pdgles_debug_count_pixels(int w, int h, int r, int g, int b, int tol)
{
	if (!p_glReadPixels) return -1;
	std::vector<uint8_t> px((size_t)w * h * 4, 0);
	p_glBindFramebuffer(GL_FRAMEBUFFER, g_defaultFbo);
	p_glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
	int n = 0;
	for (size_t i = 0; i < (size_t)w * h; ++i)
	{
		const int dr = (int)px[i * 4 + 0] - r;
		const int dg = (int)px[i * 4 + 1] - g;
		const int db = (int)px[i * 4 + 2] - b;
		if (abs(dr) <= tol && abs(dg) <= tol && abs(db) <= tol) ++n;
	}
	return n;
}

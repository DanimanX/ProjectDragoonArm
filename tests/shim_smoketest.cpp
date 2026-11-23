// shim_smoketest.cpp — proves the GLES bgfx layer actually rasterises.
//
// The engine renders nothing on either backend (bgfx and this shim), so before
// blaming the engine, this test drives the shim directly with a hand-built
// shader blob and checks the pixels it produces. It is deliberately free of
// engine code: SDL3 window + bgfx::init + one full-screen triangle.
//
// Build:  cmake --build build-host --target shim_smoketest
// Run:    DISPLAY=:90 ./build-host/shim_smoketest        (or headless with
//         PD_HEADLESS=1 once an EGL pbuffer path is enabled)

#include <bgfx/bgfx.h>
#include <bgfx/embedded_shader.h>

#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <vector>

// ---------------------------------------------------------------------------
// A minimal bgfx shader blob is:
//   u32 magic('VSH'/'FSH' with version in the top byte) | u32 hashIn |
//   u32 hashOut | u16 uniformCount | …uniforms… | u32 sourceSize | source
// This helper writes version-11 blobs with no uniforms.
// ---------------------------------------------------------------------------
static std::vector<uint8_t> makeBlob(char kind, const char* source)
{
    std::vector<uint8_t> blob;
    const uint32_t magic = ((uint32_t)('V' == kind ? 'V' : 'F')) | ((uint32_t)'S' << 8)
                         | ((uint32_t)'H' << 16) | (11u << 24);
    auto push32 = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i) blob.push_back((uint8_t)(v >> (i * 8)));
    };
    auto push16 = [&](uint16_t v) {
        blob.push_back((uint8_t)(v & 0xFF));
        blob.push_back((uint8_t)(v >> 8));
    };
    push32(magic);
    push32(0);          // hashIn
    push32(0);          // hashOut (version >= 6)
    push16(0);          // uniform count (names come from the GLSL text)
    const uint32_t len = (uint32_t)strlen(source);
    push32(len);
    blob.insert(blob.end(), source, source + len);
    return blob;
}

static std::vector<uint8_t> g_vsBlob;
static std::vector<uint8_t> g_fsBlob;

static bgfx::EmbeddedShader g_table[3];

static void buildTable()
{
    // The blobs are built in main(), so the table must be filled then too:
    // static initialisation would capture empty vectors' null data().
    memset(g_table, 0, sizeof(g_table));
    g_table[0].name = "smoke_vs";
    g_table[0].data[0].type = bgfx::RendererType::OpenGLES;
    g_table[0].data[0].data = g_vsBlob.data();
    g_table[0].data[0].size = (uint32_t)g_vsBlob.size();
    g_table[1].name = "smoke_fs";
    g_table[1].data[0].type = bgfx::RendererType::OpenGLES;
    g_table[1].data[0].data = g_fsBlob.data();
    g_table[1].data[0].size = (uint32_t)g_fsBlob.size();
    g_table[2].name = NULL;
    g_table[2].data[0].type = bgfx::RendererType::Count;
}

extern "C" int pdgles_debug_count_pixels(int w, int h, int r, int g, int b, int tol);

static const char* kVS =
    "in highp vec3 a_position;\n"
    "void main() { gl_Position = vec4(a_position, 1.0); }\n";

static const char* kFS =
    "precision highp float;\n"
    "precision highp usampler2D;\n"
    "uniform usampler2D s_probe;\n"
    "out mediump vec4 bgfx_FragColor;\n"
    "void main() {\n"
    "  uint v = texelFetch(s_probe, ivec2(0, 0), 0).x;\n"
    "  bgfx_FragColor = vec4(float(v) / 255.0, 0.25, 0.75, 1.0);\n"
    "}\n";

int main()
{
    g_vsBlob = makeBlob('V', kVS);
    g_fsBlob = makeBlob('F', kFS);
    buildTable();
    bgfx::EmbeddedShader* table = g_table;

    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        fprintf(stderr, "smoke: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);

    SDL_Window* win = SDL_CreateWindow("shim smoke", 320, 240, SDL_WINDOW_OPENGL);
    if (!win)
    {
        fprintf(stderr, "smoke: window failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    if (!ctx)
    {
        fprintf(stderr, "smoke: context failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_MakeCurrent(win, ctx);

    bgfx::Init init;
    init.type = bgfx::RendererType::OpenGLES;
    init.platformData.context = (void*)SDL_GL_GetCurrentContext();
    if (!bgfx::init(init))
    {
        fprintf(stderr, "smoke: bgfx::init failed\n");
        return 1;
    }

    // full-screen triangle in clip space
    static const float verts[] = {
        -1.0f, -1.0f, 0.0f,
         3.0f, -1.0f, 0.0f,
        -1.0f,  3.0f, 0.0f,
    };
    static const uint16_t indices[] = { 0, 1, 2 };

    bgfx::VertexLayout layout;
    layout.begin().add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float).end();

    bgfx::VertexBufferHandle vb = bgfx::createVertexBuffer(
        bgfx::copy(verts, sizeof(verts)), layout);
    bgfx::IndexBufferHandle ib = bgfx::createIndexBuffer(
        bgfx::copy(indices, sizeof(indices)));

    // integer texture with one known byte: 128 at (0,0)
    static uint8_t probeData[4] = { 128, 64, 32, 16 };
    bgfx::TextureHandle probeTex = bgfx::createTexture2D(2, 2, false, 1, bgfx::TextureFormat::R8U,
                                                          BGFX_TEXTURE_NONE,
                                                          bgfx::copy(probeData, sizeof(probeData)));
    bgfx::UniformHandle probeSampler = bgfx::createUniform("s_probe", bgfx::UniformType::Sampler);

    bgfx::ShaderHandle vs = bgfx::createEmbeddedShader(table, bgfx::RendererType::OpenGLES, "smoke_vs");
    bgfx::ShaderHandle fs = bgfx::createEmbeddedShader(table, bgfx::RendererType::OpenGLES, "smoke_fs");
    bgfx::ProgramHandle prog = bgfx::createProgram(vs, fs, true);
    if (prog == bgfx::kInvalidHandle)
    {
        fprintf(stderr, "smoke: program creation failed\n");
        return 1;
    }

    const bgfx::ViewId view = 0;
    bgfx::setViewRect(view, 0, 0, 320, 240);
    bgfx::setViewClear(view, BGFX_CLEAR_COLOR, 0x000000ff);
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
    bgfx::setTexture(0, probeSampler, probeTex);
    bgfx::setVertexBuffer(0, vb);
    bgfx::setIndexBuffer(ib);
    bgfx::submit(view, prog);

    // Draw, then read the back buffer through the shim's own helper (the shim
    // owns the GL function pointers, so the test never touches GL directly).
    bgfx::setViewRect(view, 0, 0, 320, 240);
    bgfx::setViewClear(view, BGFX_CLEAR_COLOR, 0x000000ff);
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
    bgfx::setTexture(0, probeSampler, probeTex);
    bgfx::setVertexBuffer(0, vb);
    bgfx::setIndexBuffer(ib);
    bgfx::submit(view, prog);

    const int matches = pdgles_debug_count_pixels(320, 240, 128, 64, 191, 12);
    printf("smoke: pixels matching the shader colour: %d of %d\n", matches, 320 * 240);

    bgfx::frame();
    SDL_GL_SwapWindow(win);
    bgfx::shutdown();
    SDL_GL_DestroyContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();

    if (matches < (320 * 240) / 2)
    {
        fprintf(stderr, "smoke: FAIL - the shim did not rasterise\n");
        return 1;
    }
    printf("smoke: PASS\n");
    return 0;
}

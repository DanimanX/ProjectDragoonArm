// renderer_gles.cpp — ProjectDragoonArm platform backend.
//
// Drop-in replacement for engine/AzelLib/renderer/renderer_gl.cpp. It supplies
// the exact symbols that file provided (SDL_ES3_backend, gWindowBGFX, and the
// imguiCreate/imguiBeginFrame/imguiEndFrame trio declared by compat/imguiBGFX.h)
// but drives OpenGL ES 3.x directly through platform/arm64/bgfx instead of bgfx
// plus its example ImGui renderer.
//
// Two windowing paths, one code path for the renderer:
//   * desktop/host: SDL3 + X11/Wayland, GLES context via SDL_GL.
//   * handheld: SDL3 + KMSDRM, same SDL_GL ES context.
// Whichever wins, the context is handed to bgfx::init() through
// platformData.context, so game rendering and ImGui share it.

#include "PDS.h"

#include <imgui_impl_sdl3.h>
#include <imgui_impl_opengl3.h>

extern "C" void pd_menu_install(void);

#include "renderer/renderer_gl.h"
#include "imguiBGFX.h"

extern "C" void pdgles_set_swap_hook(void (*fn)(void));

SDL_Window* gWindowBGFX = nullptr;
static SDL_GLContext g_glContext = nullptr;
static bool g_imguiReady = false;

extern "C" int pdgles_window_width();
extern "C" int pdgles_window_height();

int pdgles_window_width()
{
    int w = 0, h = 0;
    if (gWindowBGFX) SDL_GetWindowSizeInPixels(gWindowBGFX, &w, &h);
    return w;
}

int pdgles_window_height()
{
    int w = 0, h = 0;
    if (gWindowBGFX) SDL_GetWindowSizeInPixels(gWindowBGFX, &w, &h);
    return h;
}

static void swapWindow()
{
    if (gWindowBGFX && g_glContext) SDL_GL_SwapWindow(gWindowBGFX);
}

static bool createWindowAndContext()
{
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
    {
        fprintf(stderr, "[port] SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }

    // GLES 3.0 is the floor: the VDP2 layer shaders use integer textures and
    // texelFetch (see docs/PORTING-NOTES.md §4). Mali-G31 reports 3.2.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    SDL_WindowFlags flags = SDL_WINDOW_OPENGL;
    const char* fullscreen = getenv("PD_FULLSCREEN");
    if (fullscreen && fullscreen[0] && fullscreen[0] != '0')
        flags |= SDL_WINDOW_FULLSCREEN;

    // RG34XX panel is 720x480; on the host this is just a window size.
    const char* wEnv = getenv("PD_WIDTH");
    const char* hEnv = getenv("PD_HEIGHT");
    const int width = wEnv ? atoi(wEnv) : 720;
    const int height = hEnv ? atoi(hEnv) : 480;

    gWindowBGFX = SDL_CreateWindow("Panzer Dragoon Saga", width, height, flags);
    if (!gWindowBGFX)
    {
        // Retry without the OpenGL flag: on some KMS setups SDL wants the
        // driver to pick the platform itself.
        gWindowBGFX = SDL_CreateWindow("Panzer Dragoon Saga", width, height,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN);
    }
    if (!gWindowBGFX)
    {
        fprintf(stderr, "[port] SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }

    g_glContext = SDL_GL_CreateContext(gWindowBGFX);
    if (!g_glContext)
    {
        fprintf(stderr, "[port] SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        return false;
    }
    if (!SDL_GL_MakeCurrent(gWindowBGFX, g_glContext))
    {
        fprintf(stderr, "[port] SDL_GL_MakeCurrent failed: %s\n", SDL_GetError());
        return false;
    }
    // Adaptive vsync: the engine paces to the Saturn's 30 Hz internally.
    SDL_GL_SetSwapInterval(0);
    return true;
}

bool SDL_ES3_backend::init()
{
    if (!createWindowAndContext()) return false;

    SDL_PropertiesID props = SDL_GetWindowProperties(gWindowBGFX);

    bgfx::Init initParam;
    initParam.type = bgfx::RendererType::OpenGLES;
    // Hand our existing context to the backend; do not let it create one.
    initParam.platformData.context = (void*)SDL_GL_GetCurrentContext();
    initParam.platformData.nwh = nullptr;
    initParam.platformData.ndt = nullptr;
#if defined(SDL_PROP_WINDOW_X11_DISPLAY_POINTER)
    if (void* x11Display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, NULL))
        initParam.platformData.ndt = x11Display;
#endif

    if (!bgfx::init(initParam))
    {
        fprintf(stderr, "[port] bgfx::init failed\n");
        return false;
    }
    fprintf(stderr, "[port] renderer: %s\n", bgfx::getRendererName(bgfx::getRendererType()));

    pdgles_set_swap_hook(swapWindow);
    pd_menu_install();

    imguiCreate();
    fprintf(stderr, "[port] step: imguiCreate done (context %p)\n", (void*)ImGui::GetCurrentContext());
    ImGui_ImplSDL3_InitForOpenGL(gWindowBGFX, g_glContext);
    fprintf(stderr, "[port] step: sdl3 backend ready\n");
    ImGui_ImplOpenGL3_Init("#version 300 es");
    fprintf(stderr, "[port] step: opengl3 backend ready\n");
    g_imguiReady = true;

    return true;
}

backend* SDL_ES3_backend::create()
{
    if (init()) return new SDL_ES3_backend();
    return nullptr;
}

void SDL_ES3_backend::bindBackBuffer()
{
}

void SDL_ES3_backend::ImGUI_NewFrame()
{
}

void SDL_ES3_backend::ImGUI_RenderDrawData(ImDrawData* pDrawData)
{
    if (g_imguiReady) ImGui_ImplOpenGL3_RenderDrawData(pDrawData);
}

// ---------------------------------------------------------------------------
// imgui bridge (declared by compat/imguiBGFX.h)
// ---------------------------------------------------------------------------
void imguiCreate(float _fontSize, bx::AllocatorI*)
{
    // bgfx's example bridge created the context; this backend must too.
    if (!ImGui::GetCurrentContext()) ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;      // no imgui.ini next to the game data
    io.LogFilename = nullptr;
    io.FontGlobalScale = 1.0f;
    if (_fontSize > 0.0f)
    {
        ImFontConfig cfg;
        cfg.SizePixels = _fontSize;
        io.Fonts->AddFontDefault(&cfg);
    }
    fprintf(stderr, "[port] imgui ready (%d font atlas pages)\n", io.Fonts->Fonts.Size);
}

void imguiDestroy()
{
    if (!g_imguiReady) return;
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    g_imguiReady = false;
}

void imguiBeginFrame(int32_t _mx, int32_t _my, uint8_t _button, int32_t _scroll,
                     uint16_t _width, uint16_t _height, int _inputChar, bgfx::ViewId)
{
    if (!g_imguiReady) return;
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)_width, (float)_height);
    io.AddMousePosEvent((float)_mx, (float)_my);
    io.AddMouseButtonEvent(0, (_button & IMGUI_MBUT_LEFT) != 0);
    io.AddMouseButtonEvent(1, (_button & IMGUI_MBUT_RIGHT) != 0);
    io.AddMouseButtonEvent(2, (_button & IMGUI_MBUT_MIDDLE) != 0);
    if (_scroll) io.AddMouseWheelEvent(0.0f, (float)_scroll);
    if (_inputChar >= 0) io.AddInputCharacter((unsigned int)_inputChar);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
}

void imguiEndFrame()
{
    if (!g_imguiReady) return;
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

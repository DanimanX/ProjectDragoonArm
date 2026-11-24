// renderer_gles.cpp — ProjectDragoonArm platform backend.
//
// Drop-in replacement for engine/AzelLib/renderer/renderer_gl.cpp. It supplies
// the exact symbols that file provided (SDL_ES3_backend, gWindowBGFX, and the
// imguiCreate/imguiBeginFrame/imguiEndFrame trio declared by compat/imguiBGFX.h)
// but drives OpenGL ES 3.x directly through platform/arm64/bgfx instead of bgfx
// plus its example ImGui renderer.
//
// Three windowing paths, one code path for the renderer:
//   * desktop/host: SDL3 + X11/Wayland, GLES context via SDL_GL.
//   * handheld with a real SDL3 video backend: SDL3 + KMSDRM, same SDL_GL ES
//     context.
//   * Knulli/Batocera on the H700: SDL3 has no working video backend there
//     (the firmware ships SDL2 with its own "Mali EGL" fbdev driver, and there
//     is no libdrm/libgbm, so KMSDRM cannot be used). In that case the port
//     drives libmali's EGL itself over /dev/fb0 and installs its own buffer
//     swap, which is exactly what the firmware's SDL2 driver does.
// Whichever wins, the context is handed to bgfx::init() through
// platformData.context, so game rendering and ImGui share it.

#include "PDS.h"

#include <imgui_impl_sdl3.h>
#include <imgui_impl_opengl3.h>

#include <dlfcn.h>
#include <execinfo.h>
#include <signal.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <linux/fb.h>
#include <linux/input.h>

extern "C" void pd_menu_install(void);

#include "renderer/renderer_gl.h"
#include "imguiBGFX.h"

extern "C" void pdgles_set_swap_hook(void (*fn)(void));
extern "C" int  pdgles_offscreen_begin(int w, int h);
extern "C" void pdgles_offscreen_present(void);

SDL_Window* gWindowBGFX = nullptr;
static SDL_GLContext g_glContext = nullptr;
static bool g_imguiReady = false;
static bool g_imguiSDL = false;

// ---------------------------------------------------------------------------
// Minimal EGL binding for the fbdev path (no EGL headers in the cross sysroot).
// ---------------------------------------------------------------------------
typedef void*     PD_EGLDisplay;
typedef void*     PD_EGLSurface;
typedef void*     PD_EGLContext;
typedef void*     PD_EGLConfig;
typedef unsigned  PD_EGLBoolean;

#define PD_EGL_SURFACE_TYPE        0x302D
#define PD_EGL_WINDOW_BIT          0x0004
#define PD_EGL_NONE                0x3038
#define PD_EGL_RENDERABLE_TYPE     0x3040
#define PD_EGL_OPENGL_ES3_BIT      0x0040
#define PD_EGL_OPENGL_ES2_BIT      0x0004
#define PD_EGL_RED_SIZE            0x3024
#define PD_EGL_GREEN_SIZE          0x3023
#define PD_EGL_BLUE_SIZE           0x3022
#define PD_EGL_ALPHA_SIZE          0x3021
#define PD_EGL_DEPTH_SIZE          0x3025
#define PD_EGL_STENCIL_SIZE        0x3026
#define PD_EGL_CONTEXT_CLIENT_VERSION 0x3098
#define PD_EGL_OPENGL_ES_API       0x30A0
#define PD_EGL_DEFAULT_DISPLAY     ((void*)0)
#define PD_EGL_NO_SURFACE          ((PD_EGLSurface)0)

static PD_EGLDisplay g_eglDisplay = nullptr;
static PD_EGLSurface g_eglSurface = nullptr;
static PD_EGLContext g_eglContext = nullptr;
extern "C" void pdgles_set_surface_fbo(unsigned fbo);

// True when the surface is a Mali fbdev_window and eglSwapBuffers presents by
// itself (no CPU readback needed).
static bool g_eglDirect = false;
static bool g_offscreenPresent = false;
static void (*g_eglSwapBuffersFn)(PD_EGLDisplay, PD_EGLSurface) = nullptr;
static int (*g_eglGetErrorFn)(void) = nullptr;

// ---------------------------------------------------------------------------
// Presentation over /dev/fb0.
//
// Measured on the RG34XX: eglSwapBuffers() on this libmali returns EGL_TRUE but
// never reaches the panel — the game renders into the EGL surface and the screen
// keeps showing whatever was there. Writing the framebuffer pages directly is
// visible immediately (verified on the device), and the firmware's own SDL2
// fbdev driver does the same. So: render as usual, read the frame back, and
// publish it to the off-screen page, then flip.
// ---------------------------------------------------------------------------
typedef void (*PD_GLFN_void)(void);
typedef void (*PD_PFN_glReadPixels)(int, int, int, int, unsigned, unsigned, void*);
typedef void (*PD_PFN_glBindFramebuffer)(unsigned, unsigned);
typedef void (*PD_PFN_glPixelStorei)(unsigned, int);

static int g_fbFd = -1;
static unsigned char* g_fbMap = nullptr;
static size_t g_fbMapLen = 0;
static unsigned g_fbStride = 0;
static unsigned g_fbWidth = 0, g_fbHeight = 0, g_fbVirtHeight = 0;
static unsigned g_fbPages = 1;
static unsigned g_curPage = 0;
static PD_PFN_glReadPixels g_readPixels = nullptr;
static PD_GLFN_void g_finishFn = nullptr;
static PD_PFN_glBindFramebuffer g_bindFramebufferFn = nullptr;
static PD_PFN_glPixelStorei g_pixelStoreFn = nullptr;
typedef unsigned (*PD_PFN_glCreateShader)(unsigned);
typedef void (*PD_PFN_glGenFramebuffers)(int, unsigned*);
typedef void (*PD_PFN_glBindFramebuffer)(unsigned, unsigned);
typedef void (*PD_PFN_glGenTextures)(int, unsigned*);
typedef void (*PD_PFN_glBindTexture)(unsigned, unsigned);
typedef void (*PD_PFN_glTexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void*);
typedef void (*PD_PFN_glTexParameteri)(unsigned, unsigned, int);
typedef void (*PD_PFN_glFramebufferTexture2D)(unsigned, unsigned, unsigned, unsigned, int);
typedef void (*PD_PFN_glGenRenderbuffers)(int, unsigned*);
typedef void (*PD_PFN_glBindRenderbuffer)(unsigned, unsigned);
typedef void (*PD_PFN_glRenderbufferStorage)(unsigned, unsigned, int, int);
typedef void (*PD_PFN_glFramebufferRenderbuffer)(unsigned, unsigned, unsigned, unsigned);
typedef unsigned (*PD_PFN_glCheckFramebufferStatus)(unsigned);
typedef void (*PD_PFN_glBlitFramebuffer)(int,int,int,int,int,int,int,int,unsigned,unsigned);
typedef void (*PD_PFN_glClear)(unsigned);
typedef void (*PD_PFN_glClearColor)(float, float, float, float);
typedef void (*PD_PFN_glViewport)(int, int, int, int);
typedef void (*PD_PFN_glDisable)(unsigned);
static PD_PFN_glClear g_clearFn = nullptr;
static PD_PFN_glClearColor g_clearColorFn = nullptr;
static PD_PFN_glViewport g_viewportFn = nullptr;
static PD_PFN_glDisable g_disableFn = nullptr;
static PD_PFN_glGenFramebuffers g_genFbos = nullptr;
static PD_PFN_glBindFramebuffer g_bindFbo = nullptr;
static PD_PFN_glGenTextures g_genTex = nullptr;
static PD_PFN_glBindTexture g_bindTex = nullptr;
static PD_PFN_glTexImage2D g_texImage2D = nullptr;
static PD_PFN_glTexParameteri g_texParam = nullptr;
static PD_PFN_glFramebufferTexture2D g_fboTex2D = nullptr;
static PD_PFN_glGenRenderbuffers g_genRb = nullptr;
static PD_PFN_glBindRenderbuffer g_bindRb = nullptr;
static PD_PFN_glRenderbufferStorage g_rbStorage = nullptr;
static PD_PFN_glFramebufferRenderbuffer g_fboRb = nullptr;
static PD_PFN_glCheckFramebufferStatus g_checkFbo = nullptr;
static PD_PFN_glBlitFramebuffer g_blitFbo = nullptr;
static unsigned g_offFbo = 0;
static unsigned char* g_presentBuf = nullptr;

// Control fd for /dev/fb0, used to wait for vblank before publishing a frame.
// The panel scans the very buffer GL draws into (the driver has no page flip),
// so a frame drawn outside vblank is torn: captured on the device, a single
// frame held two different movie frames, top and bottom.
static int g_vsyncFd = -1;

// Adaptive vblank wait.
//
// FBIO_WAITFORVSYNC keeps the flip tear-free, but on this driver it can block
// far longer than one refresh once the panel stops paging (measured: ~1 fps in
// scenes that present differently). So the wait is timed, and if it overruns a
// couple of refreshes it switches itself off instead of stalling the game.
// PD_NO_VSYNC=1 forces it off; PD_VSYNC_THRESHOLD_MS tunes the limit.
static void fbdevWaitVsync()
{
    static const bool forcedOff = getenv("PD_NO_VSYNC") != nullptr;
    static bool disabled = false;
    static double thresholdMs = -1;
    if (forcedOff || disabled) return;
    if (thresholdMs < 0)
    {
        const char* t = getenv("PD_VSYNC_THRESHOLD_MS");
        thresholdMs = t ? atof(t) : 50.0;
    }
    if (g_vsyncFd < 0) g_vsyncFd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
    if (g_vsyncFd < 0) return;

    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    int zero = 0;
    ioctl(g_vsyncFd, FBIO_WAITFORVSYNC, &zero);
    clock_gettime(CLOCK_MONOTONIC, &b);

    const double ms = (b.tv_sec - a.tv_sec) * 1000.0 + (b.tv_nsec - a.tv_nsec) / 1e6;
    if (ms > thresholdMs)
    {
        disabled = true;
        fprintf(stderr, "[present] vsync wait took %.1fms (> %.0f), disabling it "
                        "to protect the frame rate\n", ms, thresholdMs);
    }
}

// EGL entry points kept for teardown (createFbdevEglContext resolves them).
static PD_EGLBoolean (*p_eglDestroySurface_fn)(PD_EGLDisplay, PD_EGLSurface) = nullptr;
static PD_EGLBoolean (*p_eglDestroyContext_fn)(PD_EGLDisplay, PD_EGLContext) = nullptr;
static PD_EGLBoolean (*p_eglMakeCurrent_fn)(PD_EGLDisplay, PD_EGLSurface, PD_EGLSurface,
                                            PD_EGLContext) = nullptr;
static PD_EGLBoolean (*p_eglTerminate_fn)(PD_EGLDisplay) = nullptr;
static PD_EGLBoolean (*p_eglWaitClient_fn)(void) = nullptr;

// Picks which framebuffer page to publish to: the one that is not on screen.
static void fbdev_present()
{
    if (g_fbFd < 0 && g_fbMapLen == 0) return;
    if (!g_readPixels || !g_fbMap) return;

    static const bool trace = getenv("PD_TRACE_FRAME") != nullptr;
    static double sumRead = 0, sumCopy = 0, sumPan = 0;
    static unsigned frames = 0;
    static struct timespec tPrev = {0, 0}, tNow;
    double dRead = 0, dCopy = 0, dPan = 0, dFrame = 0;
    clock_gettime(CLOCK_MONOTONIC, &tNow);
    if (tPrev.tv_sec)
        dFrame = (tNow.tv_sec - tPrev.tv_sec) * 1000.0 + (tNow.tv_nsec - tPrev.tv_nsec) / 1e6;
    tPrev = tNow;

    if (g_finishFn) g_finishFn();
    if (g_bindFramebufferFn) g_bindFramebufferFn(0x8D40, 0);
    if (g_pixelStoreFn) g_pixelStoreFn(0x0D05 /* GL_PACK_ALIGNMENT */, 1);

    const unsigned w = g_fbWidth, h = g_fbHeight;
    struct timespec a, b, c, d;
    clock_gettime(CLOCK_MONOTONIC, &a);
    g_readPixels(0, 0, (int)w, (int)h, 0x1908 /* GL_RGBA */, 0x1401 /* GL_UNSIGNED_BYTE */,
                 g_presentBuf);
    clock_gettime(CLOCK_MONOTONIC, &b);

    // Our own page bookkeeping. Reading vi.yoffset is not reliable here: the pan
    // requested with FB_ACTIVATE_VBL only takes effect at the next vblank, so a
    // frame that arrives first would compute the same target page again and
    // overwrite the page about to be shown — that is the tearing/flicker.
    const unsigned dstPage = (g_fbPages > 1) ? ((g_curPage + 1) % g_fbPages) : 0;

    unsigned char* page = g_fbMap + (size_t)g_fbStride * h * dstPage;
    const size_t srcStride = (size_t)w * 4;
    for (unsigned y = 0; y < h; ++y)
    {
        // glReadPixels is bottom-up; the framebuffer is top-down.
        const unsigned char* src = g_presentBuf + (size_t)(h - 1 - y) * srcStride;
        unsigned char* dst = page + (size_t)y * g_fbStride;
        if (g_fbStride == srcStride) memcpy(dst, src, srcStride);
        else memcpy(dst, src, srcStride < g_fbStride ? srcStride : g_fbStride);
    }
    clock_gettime(CLOCK_MONOTONIC, &c);

    if (g_fbPages > 1)
    {
        struct fb_var_screeninfo vi;
        if (ioctl(g_fbFd, FBIOGET_VSCREENINFO, &vi) == 0)
        {
            vi.yoffset = h * dstPage;
            vi.activate = FB_ACTIVATE_VBL;
            ioctl(g_fbFd, FBIOPAN_DISPLAY, &vi);
        }
        g_curPage = dstPage;
        // Wait for the flip to land. Without this the next frame can start
        // writing while the panel still scans the page we are about to reuse.
        int zero = 0;
        ioctl(g_fbFd, FBIO_WAITFORVSYNC, &zero);
    }
    clock_gettime(CLOCK_MONOTONIC, &d);

    dRead = (b.tv_sec - a.tv_sec) * 1000.0 + (b.tv_nsec - a.tv_nsec) / 1e6;
    dCopy = (c.tv_sec - b.tv_sec) * 1000.0 + (c.tv_nsec - b.tv_nsec) / 1e6;
    dPan = (d.tv_sec - c.tv_sec) * 1000.0 + (d.tv_nsec - c.tv_nsec) / 1e6;
    sumRead += dRead; sumCopy += dCopy; sumPan += dPan;
    if (trace && ++frames % 30 == 0)
    {
        fprintf(stderr,
                "[present] %ux%u frames=%u  frame=%.1fms  read=%.1fms  copy=%.1fms  pan+vsync=%.1fms\n",
                w, h, frames, (sumRead + sumCopy + sumPan) / 30.0,
                sumRead / 30.0, sumCopy / 30.0, sumPan / 30.0);
        fprintf(stderr, "[present]   interval=%.1fms (%.1f fps)\n",
                dFrame, dFrame > 0 ? 1000.0 / dFrame : 0.0);
        sumRead = sumCopy = sumPan = 0;
    }
}

static bool fbdev_open()
{
    const char* path = getenv("PD_FBDEV");
    if (!path || !path[0]) path = "/dev/fb0";
    g_fbFd = open(path, O_RDWR | O_CLOEXEC);
    if (g_fbFd < 0)
    {
        fprintf(stderr, "[port] fb-present: cannot open %s\n", path);
        return false;
    }

    struct fb_var_screeninfo vi;
    struct fb_fix_screeninfo fi;
    if (ioctl(g_fbFd, FBIOGET_VSCREENINFO, &vi) != 0 ||
        ioctl(g_fbFd, FBIOGET_FSCREENINFO, &fi) != 0)
    {
        fprintf(stderr, "[port] fb-present: ioctl failed on %s\n", path);
        return false;
    }
    if (vi.bits_per_pixel != 32 && vi.bits_per_pixel != 16)
    {
        fprintf(stderr, "[port] fb-present: unsupported %u bpp\n", vi.bits_per_pixel);
        return false;
    }

    g_fbWidth = vi.xres;
    g_fbHeight = vi.yres;
    g_fbVirtHeight = vi.yres_virtual ? vi.yres_virtual : vi.yres;
    g_fbStride = fi.line_length;
    g_fbPages = (vi.yres > 0 && g_fbVirtHeight >= vi.yres) ? g_fbVirtHeight / vi.yres : 1;
    g_fbMapLen = (size_t)g_fbStride * g_fbVirtHeight;
    g_fbMap = (unsigned char*)mmap(nullptr, g_fbMapLen, PROT_READ | PROT_WRITE,
                                   MAP_SHARED, g_fbFd, 0);
    if (g_fbMap == MAP_FAILED)
    {
        g_fbMap = nullptr;
        fprintf(stderr, "[port] fb-present: mmap failed\n");
        return false;
    }

    g_presentBuf = (unsigned char*)malloc((size_t)g_fbWidth * g_fbHeight * 4);
    if (!g_presentBuf)
    {
        fprintf(stderr, "[port] fb-present: out of memory for the present buffer\n");
        return false;
    }

    // Start from the page the panel is scanning right now, so the first flip
    // targets the other one.
    g_curPage = (g_fbHeight > 0) ? (vi.yoffset / g_fbHeight) % (g_fbPages ? g_fbPages : 1) : 0;

    fprintf(stderr, "[port] fb-present: %s %ux%u (%u page(s)), stride %u, %u bpp, page %u\n",
            path, g_fbWidth, g_fbHeight, g_fbPages, g_fbStride, vi.bits_per_pixel, g_curPage);
    return true;
}

// The fbdev surface renders straight into the visible framebuffer (measured:
// drawing without eglSwapBuffers changes what is on screen), so every partial
// frame the game produces is visible as tearing/flashes. Bind an offscreen FBO
// instead and present the finished frame in one blit.
static bool createOffscreenTarget(unsigned w, unsigned h)
{
    if (!g_genFbos || !g_bindFbo || !g_genTex || !g_bindTex || !g_texImage2D ||
        !g_fboTex2D || !g_genRb || !g_rbStorage || !g_fboRb || !g_checkFbo || !g_blitFbo)
        return false;

    unsigned tex = 0, rb = 0, fbo = 0;
    g_genTex(1, &tex);
    g_bindTex(0x0DE1 /* GL_TEXTURE_2D */, tex);
    g_texImage2D(0x0DE1, 0, 0x8058 /* GL_RGBA8 */, (int)w, (int)h, 0,
                 0x1908 /* GL_RGBA */, 0x1401 /* GL_UNSIGNED_BYTE */, nullptr);
    if (g_texParam)
    {
        g_texParam(0x0DE1, 0x2801 /* MIN_FILTER */, 0x2600 /* NEAREST */);
        g_texParam(0x0DE1, 0x2800 /* MAG_FILTER */, 0x2600);
    }

    g_genRb(1, &rb);
    g_bindRb(0x8D41 /* GL_RENDERBUFFER */, rb);
    g_rbStorage(0x8D41, 0x88F0 /* GL_DEPTH24_STENCIL8 */, (int)w, (int)h);

    g_genFbos(1, &fbo);
    g_bindFbo(0x8D40 /* GL_FRAMEBUFFER */, fbo);
    g_fboTex2D(0x8D40, 0x8CE0 /* GL_COLOR_ATTACHMENT0 */, 0x0DE1, tex, 0);
    g_fboRb(0x8D40, 0x8D00 /* GL_DEPTH_STENCIL_ATTACHMENT */, 0x8D41, rb);

    const unsigned status = g_checkFbo(0x8D40);
    if (status != 0x8CD5 /* GL_FRAMEBUFFER_COMPLETE */)
    {
        fprintf(stderr, "[port] offscreen FBO incomplete (0x%x), using direct output\n", status);
        g_bindFbo(0x8D40, 0);
        return false;
    }
    g_offFbo = fbo;
    fprintf(stderr, "[port] offscreen render target %ux%u bound (FBO %u)\n", w, h, fbo);
    return true;
}

static bool loadGLPresent()
{
    void* lib = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL);
    if (!lib) lib = dlopen("libmali.so.0", RTLD_NOW | RTLD_GLOBAL);
    if (!lib)
    {
        fprintf(stderr, "[port] fb-present: no GLES library for readback\n");
        return false;
    }
    g_readPixels = (PD_PFN_glReadPixels)dlsym(lib, "glReadPixels");
    g_clearFn = (PD_PFN_glClear)dlsym(lib, "glClear");
    g_genFbos = (PD_PFN_glGenFramebuffers)dlsym(lib, "glGenFramebuffers");
    g_bindFbo = (PD_PFN_glBindFramebuffer)dlsym(lib, "glBindFramebuffer");
    g_genTex = (PD_PFN_glGenTextures)dlsym(lib, "glGenTextures");
    g_bindTex = (PD_PFN_glBindTexture)dlsym(lib, "glBindTexture");
    g_texImage2D = (PD_PFN_glTexImage2D)dlsym(lib, "glTexImage2D");
    g_texParam = (PD_PFN_glTexParameteri)dlsym(lib, "glTexParameteri");
    g_fboTex2D = (PD_PFN_glFramebufferTexture2D)dlsym(lib, "glFramebufferTexture2D");
    g_genRb = (PD_PFN_glGenRenderbuffers)dlsym(lib, "glGenRenderbuffers");
    g_bindRb = (PD_PFN_glBindRenderbuffer)dlsym(lib, "glBindRenderbuffer");
    g_rbStorage = (PD_PFN_glRenderbufferStorage)dlsym(lib, "glRenderbufferStorage");
    g_fboRb = (PD_PFN_glFramebufferRenderbuffer)dlsym(lib, "glFramebufferRenderbuffer");
    g_checkFbo = (PD_PFN_glCheckFramebufferStatus)dlsym(lib, "glCheckFramebufferStatus");
    g_blitFbo = (PD_PFN_glBlitFramebuffer)dlsym(lib, "glBlitFramebuffer");
    g_clearColorFn = (PD_PFN_glClearColor)dlsym(lib, "glClearColor");
    g_viewportFn = (PD_PFN_glViewport)dlsym(lib, "glViewport");
    g_disableFn = (PD_PFN_glDisable)dlsym(lib, "glDisable");
    g_finishFn = (PD_GLFN_void)dlsym(lib, "glFinish");
    g_bindFramebufferFn = (PD_PFN_glBindFramebuffer)dlsym(lib, "glBindFramebuffer");
    g_pixelStoreFn = (PD_PFN_glPixelStorei)dlsym(lib, "glPixelStorei");
    if (!g_readPixels)
    {
        fprintf(stderr, "[port] fb-present: glReadPixels missing\n");
        return false;
    }
    return true;
}

// Creates the GLES 3 context on the firmware's Mali EGL over the framebuffer.
// Used only when SDL could not give us one; returns false so the caller keeps
// its own diagnostics.
static bool createFbdevEglContext()
{
    const char* disable = getenv("PD_EGL_FBDEV");
    if (disable && disable[0] == '0') return false;

    // Prefer the system EGL (libglvnd on Knulli, which dispatches to libmali);
    // fall back to the Mali blob itself so the port does not depend on a
    // glvnd vendor config being present.
    void* lib = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!lib) lib = dlopen("libEGL.so", RTLD_NOW | RTLD_GLOBAL);
    if (!lib) lib = dlopen("libmali.so.0", RTLD_NOW | RTLD_GLOBAL);
    if (!lib)
    {
        fprintf(stderr, "[port] egl-fbdev: libEGL not found: %s\n", dlerror());
        return false;
    }

    typedef PD_EGLDisplay (*FnGetDisplay)(void*);
    typedef PD_EGLBoolean (*FnInitialize)(PD_EGLDisplay, int*, int*);
    typedef PD_EGLBoolean (*FnChooseConfig)(PD_EGLDisplay, const int*, PD_EGLConfig*, int, int*);
    typedef PD_EGLContext (*FnCreateContext)(PD_EGLDisplay, PD_EGLConfig, PD_EGLContext, const int*);
    typedef PD_EGLSurface (*FnCreateWindowSurface)(PD_EGLDisplay, PD_EGLConfig, void*, const int*);
    typedef PD_EGLBoolean (*FnMakeCurrent)(PD_EGLDisplay, PD_EGLSurface, PD_EGLSurface, PD_EGLContext);
    typedef PD_EGLBoolean (*FnBindAPI)(unsigned);
    typedef PD_EGLBoolean (*FnSwapInterval)(PD_EGLDisplay, int);
    typedef int (*FnGetError)(void);

    FnGetDisplay getDisplay = (FnGetDisplay)dlsym(lib, "eglGetDisplay");
    FnInitialize initialize = (FnInitialize)dlsym(lib, "eglInitialize");
    FnChooseConfig chooseConfig = (FnChooseConfig)dlsym(lib, "eglChooseConfig");
    FnCreateContext createContext = (FnCreateContext)dlsym(lib, "eglCreateContext");
    FnCreateWindowSurface createWindowSurface =
        (FnCreateWindowSurface)dlsym(lib, "eglCreateWindowSurface");
    FnMakeCurrent makeCurrent = (FnMakeCurrent)dlsym(lib, "eglMakeCurrent");
    FnBindAPI bindAPI = (FnBindAPI)dlsym(lib, "eglBindAPI");
    FnSwapInterval swapInterval = (FnSwapInterval)dlsym(lib, "eglSwapInterval");
    FnGetError getError = (FnGetError)dlsym(lib, "eglGetError");
    g_eglGetErrorFn = getError;
    g_eglSwapBuffersFn = (void (*)(PD_EGLDisplay, PD_EGLSurface))dlsym(lib, "eglSwapBuffers");
    p_eglDestroySurface_fn = (PD_EGLBoolean (*)(PD_EGLDisplay, PD_EGLSurface))dlsym(lib, "eglDestroySurface");
    p_eglDestroyContext_fn = (PD_EGLBoolean (*)(PD_EGLDisplay, PD_EGLContext))dlsym(lib, "eglDestroyContext");
    p_eglMakeCurrent_fn = (PD_EGLBoolean (*)(PD_EGLDisplay, PD_EGLSurface, PD_EGLSurface,
                                             PD_EGLContext))dlsym(lib, "eglMakeCurrent");
    p_eglTerminate_fn = (PD_EGLBoolean (*)(PD_EGLDisplay))dlsym(lib, "eglTerminate");
    p_eglWaitClient_fn = (PD_EGLBoolean (*)(void))dlsym(lib, "eglWaitClient");

    if (!getDisplay || !initialize || !chooseConfig || !createContext ||
        !createWindowSurface || !makeCurrent || !g_eglSwapBuffersFn)
    {
        fprintf(stderr, "[port] egl-fbdev: libEGL is missing entry points\n");
        return false;
    }

    if (bindAPI) bindAPI(PD_EGL_OPENGL_ES_API);
    g_eglDisplay = getDisplay(PD_EGL_DEFAULT_DISPLAY);
    if (!g_eglDisplay)
    {
        fprintf(stderr, "[port] egl-fbdev: eglGetDisplay failed (0x%x)\n",
                getError ? getError() : 0);
        return false;
    }

    int major = 0, minor = 0;
    if (!initialize(g_eglDisplay, &major, &minor))
    {
        fprintf(stderr, "[port] egl-fbdev: eglInitialize failed (0x%x)\n",
                getError ? getError() : 0);
        return false;
    }
    fprintf(stderr, "[port] egl-fbdev: EGL %d.%d on EGL_DEFAULT_DISPLAY\n", major, minor);

    // Measured on this device (RG34XX, libmali Bifrost r20p0, EGL 1.4):
    // every one of the 25 configs reports surface_type == 0, and passing
    // EGL_SURFACE_TYPE in the attribute list makes eglChooseConfig fail with
    // EGL_BAD_ATTRIBUTE (0x3004). Asking for a renderable type and depth is
    // accepted, and eglCreateWindowSurface(display, config, NULL, NULL) then
    // succeeds — the native window is literally NULL on fbdev, which is what
    // the firmware's own SDL2 "Mali EGL" driver does.
    static const int cfgAttrs[] = {
        PD_EGL_RENDERABLE_TYPE, PD_EGL_OPENGL_ES2_BIT,
        PD_EGL_DEPTH_SIZE, 24, PD_EGL_STENCIL_SIZE, 8,
        PD_EGL_NONE
    };

    PD_EGLConfig config = nullptr;
    int numConfigs = 0;
    if (!chooseConfig(g_eglDisplay, cfgAttrs, &config, 1, &numConfigs) || numConfigs < 1)
    {
        // Last resort: enumerate the configs and take the first one with depth
        // (needed by the engine's z-tested 3D pipeline).
        static const int cfgAttrsBare[] = { PD_EGL_NONE };
        if (!chooseConfig(g_eglDisplay, cfgAttrsBare, &config, 1, &numConfigs) || numConfigs < 1)
        {
            fprintf(stderr, "[port] egl-fbdev: eglChooseConfig failed (0x%x)\n",
                    getError ? getError() : 0);
            return false;
        }
    }
    fprintf(stderr, "[port] egl-fbdev: config chosen (%d candidate(s))\n", numConfigs);

    static const int ctxAttrs[] = { PD_EGL_CONTEXT_CLIENT_VERSION, 3, PD_EGL_NONE };
    g_glContext = (SDL_GLContext)createContext(g_eglDisplay, config, nullptr, ctxAttrs);
    g_eglContext = (PD_EGLContext)g_glContext;
    if (!g_glContext)
    {
        static const int ctxAttrsEs2[] = { PD_EGL_CONTEXT_CLIENT_VERSION, 2, PD_EGL_NONE };
        g_glContext = (SDL_GLContext)createContext(g_eglDisplay, config, nullptr, ctxAttrsEs2);
        if (!g_glContext)
        {
            fprintf(stderr, "[port] egl-fbdev: eglCreateContext failed (0x%x)\n",
                    getError ? getError() : 0);
            return false;
        }
        fprintf(stderr, "[port] egl-fbdev: warning, got a GLES2 context\n");
    }

    // Mali's fbdev EGL wants a pointer to a fbdev_window as the native window;
    // eglSwapBuffers then presents straight to /dev/fb0 in the GPU. Measured on
    // the device: creation succeeds, a clear+swap reaches both framebuffer
    // pages, and eglSwapInterval(1) really blocks on vsync (21 ms/swap).
    // Creating the surface with a NULL handle also "works" but presents
    // nothing, which is why this used to need a 35 ms/frame glReadPixels.
    // Panel mode for the fbdev_window. Read it from /dev/fb0 rather than
    // trusting the environment; PD_WIDTH/PD_HEIGHT are only a fallback.
    if (!g_fbWidth || !g_fbHeight)
    {
        const char* path = getenv("PD_FBDEV");
        if (!path || !path[0]) path = "/dev/fb0";
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd >= 0)
        {
            struct fb_var_screeninfo vi;
            if (ioctl(fd, FBIOGET_VSCREENINFO, &vi) == 0)
            {
                g_fbWidth = vi.xres;
                g_fbHeight = vi.yres;
            }
            close(fd);
        }
        if (!g_fbWidth || !g_fbHeight)
        {
            const char* we = getenv("PD_WIDTH");
            const char* he = getenv("PD_HEIGHT");
            g_fbWidth = we ? (unsigned)atoi(we) : 720;
            g_fbHeight = he ? (unsigned)atoi(he) : 480;
        }
    }

    static struct { unsigned short width, height; } s_fbdevWindow;
    s_fbdevWindow.width = (unsigned short)g_fbWidth;
    s_fbdevWindow.height = (unsigned short)g_fbHeight;
    if (g_fbWidth && g_fbHeight)
    {
        g_eglSurface = createWindowSurface(g_eglDisplay, config, (void*)&s_fbdevWindow, nullptr);
        g_eglDirect = (g_eglSurface != nullptr);
        if (g_eglDirect)
            fprintf(stderr, "[port] egl-fbdev: direct GPU presentation %ux%u\n",
                    s_fbdevWindow.width, s_fbdevWindow.height);
    }

    if (!g_eglSurface)
    {
        // Fallback: offscreen surface + readback presentation.
        g_eglSurface = createWindowSurface(g_eglDisplay, config, (void*)0, nullptr);
        g_eglDirect = false;
    }
    if (!g_eglSurface)
    {
        fprintf(stderr, "[port] egl-fbdev: eglCreateWindowSurface(fbdev) failed (0x%x)\n",
                getError ? getError() : 0);
        return false;
    }

    if (!makeCurrent(g_eglDisplay, g_eglSurface, g_eglSurface, g_glContext))
    {
        fprintf(stderr, "[port] egl-fbdev: eglMakeCurrent failed (0x%x)\n",
                getError ? getError() : 0);
        return false;
    }

    // Ask the driver which framebuffer the window surface actually is. libmali
    // over fbdev does not report 0 here, and the port used to hardcode 0 when
    // publishing the finished frame: every draw in that path went to an
    // unrelated framebuffer, so the panel stayed black while the offscreen
    // frame was perfect. Resolve glGetIntegerv from this context and read it
    // before anything else binds a framebuffer.
    {
        void* gllib = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL);
        typedef void (*FnGetIntegerv)(unsigned, int*);
        FnGetIntegerv getInt = gllib ? (FnGetIntegerv)dlsym(gllib, "glGetIntegerv") : nullptr;
        if (getInt)
        {
            int bound = -1;
            getInt(0x8CA6 /* GL_FRAMEBUFFER_BINDING */, &bound);
            pdgles_set_surface_fbo((unsigned)bound);
            fprintf(stderr, "[port] egl-fbdev: surface framebuffer id %d\n", bound);
        }
        else
        {
            fprintf(stderr, "[port] egl-fbdev: cannot query the surface framebuffer\n");
        }
    }

    // The engine paces itself to 30 Hz; a vsynced swap keeps the flips tear-free
    // without adding another wait.
    // Off by default: this libmali rejects glBlitFramebuffer (measured:
    // GL_INVALID_OPERATION), and the composite already redraws the whole panel
    // in a single quad, so direct rendering is both correct and cheapest.
    // PD_OFFSCREEN_FBO=1 enables the offscreen path for experiments.
    // Do not let the driver block on vsync: the engine already paces to 30 Hz
    // and the frame is presented in one blit.
    if (swapInterval) swapInterval(g_eglDisplay, 0);
    fprintf(stderr, "[port] egl-fbdev: framebuffer GLES3 context ready\n");

    // Present through /dev/fb0 only when the driver's own swap cannot.
    if (g_eglDirect)
    {
        fprintf(stderr, "[port] fb-present: using eglSwapBuffers (no readback)\n");
        return true;
    }
    const char* present = getenv("PD_FB_PRESENT");
    if (!present || present[0] != '0')
    {
        if (loadGLPresent() && fbdev_open())
            fprintf(stderr, "[port] fb-present: publishing frames to the framebuffer\n");
        else
            fprintf(stderr, "[port] fb-present: unavailable, frames will not be visible\n");
    }
    return true;
}

extern "C" int pdgles_window_width();
extern "C" int pdgles_window_height();

int pdgles_window_width()
{
    int w = 0, h = 0;
    if (gWindowBGFX) SDL_GetWindowSizeInPixels(gWindowBGFX, &w, &h);
    // No SDL window (fbdev path): report the framebuffer mode we render at, so
    // the overlay does not get a zero-sized display.
    if (w <= 0)
    {
        const char* e = getenv("PD_WIDTH");
        w = e ? atoi(e) : 720;
    }
    return w;
}

int pdgles_window_height()
{
    int w = 0, h = 0;
    if (gWindowBGFX) SDL_GetWindowSizeInPixels(gWindowBGFX, &w, &h);
    if (h <= 0)
    {
        const char* e = getenv("PD_HEIGHT");
        h = e ? atoi(e) : 480;
    }
    return h;
}

// Input diagnostics: PD_TRACE_INPUT=1 logs what the pad actually reports, once
// per second, into log.txt. Lets a play session answer "which button is which"
// without a debugger on the handheld.
static void pdTraceInput()
{
    static const bool enabled = getenv("PD_TRACE_INPUT") != nullptr;
    if (!enabled) return;
    // Edge-triggered, not sampled: a button press lasts ~100 ms and a once
    //   per-second sample misses it, which is exactly what hid the mapping bug.
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long long now = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    static long long lastLogMs = -1;
    const bool wantHeartbeat = (lastLogMs < 0) || (now - lastLogMs >= 5000);

    static SDL_Gamepad* pad = nullptr;
    if (!pad)
    {
        int n = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&n);
        if (ids)
        {
            if (n > 0) pad = SDL_OpenGamepad(ids[0]);
            SDL_free(ids);
        }
        fprintf(stderr, "[input] gamepads now: %d, opened=%s\n", n, pad ? "yes" : "no");
        if (!pad) return;
        // Log the mapping SDL actually resolved: if the launcher's
        // SDL_GAMECONTROLLERCONFIG did not take, this shows the auto one and
        // every binding is shifted (the pad reports ESC and the volume keys
        // before its real buttons).
        fprintf(stderr, "[input] mapping: %s\n", SDL_GetGamepadMapping(pad));
    }

    SDL_Joystick* j = SDL_GetGamepadJoystick(pad);
    if (!j) return;
    char buttons[128]; int len = 0;
    const int nb = SDL_GetNumJoystickButtons(j);
    for (int b = 0; b < nb && len < 100; b++)
        if (SDL_GetJoystickButton(j, b)) len += snprintf(buttons + len, sizeof(buttons) - len, "b%d ", b);
    if (!len) snprintf(buttons, sizeof(buttons), "-");

    int ax[8] = {0};
    const int na = SDL_GetNumJoystickAxes(j);
    for (int a = 0; a < na && a < 8; a++) ax[a] = SDL_GetJoystickAxis(j, a);
    const int hat = SDL_GetJoystickHat(j, 0);

    static char lastButtons[128] = "\0";
    static int lastAx[8] = {-999999, -999999, -999999, -999999, -999999, -999999, -999999, -999999};
    static int lastHat = -1;
    bool changed = strcmp(buttons, lastButtons) != 0 || hat != lastHat;
    for (int i = 0; i < na && i < 8 && !changed; ++i)
        if (abs(ax[i] - lastAx[i]) > 4000) changed = true;
    if (!changed && !wantHeartbeat) return;
    lastLogMs = now;
    snprintf(lastButtons, sizeof(lastButtons), "%s", buttons);
    for (int i = 0; i < 8; ++i) lastAx[i] = ax[i];
    lastHat = hat;

    fprintf(stderr, "[input] buttons=%s| axes=%d,%d,%d hat=%d | SOUTH=%d EAST=%d NORTH=%d WEST=%d START=%d\n",
            buttons, ax[0], ax[1], ax[2], hat,
            SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH),
            SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_EAST),
            SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_NORTH),
            SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_WEST),
            SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_START));
}

static void swapWindow()
{
    pdTraceInput();
    // Test hook: PD_EXIT_AFTER_MS=<n> leaves through the normal exit path (the
    // same one SDL_EVENT_QUIT uses) so the ordered shutdown can be exercised
    // without a gamepad in front of the device.
    static long exitAfterMs = -2;
    static long long startMs = -1;
    if (exitAfterMs == -2)
    {
        const char* env = getenv("PD_EXIT_AFTER_MS");
        exitAfterMs = (env && env[0]) ? atol(env) : 0;
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        startMs = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    }
    if (exitAfterMs > 0)
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        long long now = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
        if (now - startMs >= exitAfterMs)
        {
            fprintf(stderr, "[port] PD_EXIT_AFTER_MS reached, leaving through exit()\n");
            exit(0);
        }
    }

    if (g_eglDirect && g_offscreenPresent)
    {
        // The frame was rendered offscreen; publish it during vblank so the
        // raster is not mid-scan while the buffer is rewritten.
        static const bool trace = getenv("PD_TRACE_FRAME") != nullptr;
        static struct timespec prev = {0, 0};
        static double sum = 0;
        static unsigned n = 0;
        struct timespec a, b;
        clock_gettime(CLOCK_MONOTONIC, &a);
        fbdevWaitVsync();
        pdgles_offscreen_present();
        g_eglSwapBuffersFn(g_eglDisplay, g_eglSurface);
        {
            // A swap that fails is silent in GL terms and leaves the panel showing
            // whatever it had. Report it instead of guessing at the driver.
            static const bool traceSwap = getenv("PD_TRACE_PRESENT") != nullptr;
            static unsigned n = 0;
            if (traceSwap && ++n % 60 == 1)
            {
                const int err = g_eglGetErrorFn ? g_eglGetErrorFn() : -1;
                fprintf(stderr, "[swap] offscreen swap done, eglError 0x%x\n", (unsigned)err);
            }
        }
        clock_gettime(CLOCK_MONOTONIC, &b);
        if (trace)
        {
            sum += (b.tv_sec - a.tv_sec) * 1000.0 + (b.tv_nsec - a.tv_nsec) / 1e6;
            if (prev.tv_sec)
            {
                double dt = (b.tv_sec - prev.tv_sec) * 1000.0 + (b.tv_nsec - prev.tv_nsec) / 1e6;
                if (++n % 30 == 0)
                {
                    fprintf(stderr, "[present] present avg=%.2fms  interval=%.1fms (%.1f fps)\n",
                            sum / 30.0, dt, dt > 0 ? 1000.0 / dt : 0.0);
                    sum = 0;
                }
            }
            prev = b;
        }
        return;
    }
    if (g_eglDirect && g_eglSurface && g_eglSwapBuffersFn)
    {
        // The driver presents to /dev/fb0 itself; vsync was set to 1 above.
        static const bool trace = getenv("PD_TRACE_FRAME") != nullptr;
        static struct timespec prev = {0, 0};
        static double sum = 0;
        static unsigned n = 0;
        struct timespec a, b;
        clock_gettime(CLOCK_MONOTONIC, &a);
        if (g_offFbo && g_blitFbo && g_bindFbo)
        {
            // Publish the finished offscreen frame in one GPU blit, then swap.
            g_bindFbo(0x8D40, 0);
            if (g_disableFn) g_disableFn(0x0C11 /* GL_SCISSOR_TEST */);
            if (g_viewportFn) g_viewportFn(0, 0, (int)g_fbWidth, (int)g_fbHeight);
            g_blitFbo(0, 0, (int)g_fbWidth, (int)g_fbHeight,
                      0, 0, (int)g_fbWidth, (int)g_fbHeight,
                      0x00004000 /* GL_COLOR_BUFFER_BIT */, 0x2600 /* GL_NEAREST */);
        }
        g_eglSwapBuffersFn(g_eglDisplay, g_eglSurface);
        clock_gettime(CLOCK_MONOTONIC, &b);

        if (g_offFbo && g_bindFbo)
        {
            // Back to rendering offscreen, and blank it: views only clear their
            // own scissored rect, so anything the game does not redraw would
            // leave stale pixels (the SEGA logo showed over the intro scene).
            g_bindFbo(0x8D40, g_offFbo);
            if (g_disableFn) g_disableFn(0x0C11 /* GL_SCISSOR_TEST */);
            if (g_viewportFn) g_viewportFn(0, 0, (int)g_fbWidth, (int)g_fbHeight);
            if (g_clearColorFn) g_clearColorFn(0.0f, 0.0f, 0.0f, 1.0f);
            if (g_clearFn) g_clearFn(0x00004000 /* GL_COLOR_BUFFER_BIT */);
        }

        if (trace)
        {
            double sw = (b.tv_sec - a.tv_sec) * 1000.0 + (b.tv_nsec - a.tv_nsec) / 1e6;
            sum += sw;
            if (prev.tv_sec)
            {
                double dt = (b.tv_sec - prev.tv_sec) * 1000.0 + (b.tv_nsec - prev.tv_nsec) / 1e6;
                if (++n % 30 == 0)
                {
                    fprintf(stderr, "[present] direct swap avg=%.2fms  interval=%.1fms (%.1f fps)\n",
                            sum / 30.0, dt, dt > 0 ? 1000.0 / dt : 0.0);
                    sum = 0;
                }
            }
            prev = b;
        }
        return;
    }
    if (g_fbMap)
    {
        fbdev_present();
        return;
    }
    if (g_eglSurface)
    {
        g_eglSwapBuffersFn(g_eglDisplay, g_eglSurface);
        return;
    }
    if (gWindowBGFX && g_glContext) SDL_GL_SwapWindow(gWindowBGFX);
}

// ---------------------------------------------------------------------------
// Direct evdev input.
//
// Measured on this firmware: SDL enumerates the pad (SDL_GetGamepads() == 1) but
// SDL_GetGamepadButton() reports 0 forever and no SDL_EVENT_GAMEPAD_* ever
// arrives inside the game process, while a standalone probe reading
// /dev/input/eventN gets every press. Rather than fight SDL's joystick layer in
// a process that has already initialised video + EGL + bgfx, the port reads the
// pad itself. Same device, same codes, deterministic.
// ---------------------------------------------------------------------------
static int   g_evFd = -1;
static unsigned g_evMask = 0;

// Saturn pad bits the engine expects (see renderer.cpp).
enum { PD_A = 0x0001, PD_B = 0x0002, PD_C = 0x0004, PD_START = 0x0008,
       PD_UP = 0x0010, PD_DOWN = 0x0020, PD_LEFT = 0x0040, PD_RIGHT = 0x0080,
       PD_X = 0x0800, PD_Y = 0x4000, PD_Z = 0x2000, PD_R = 0x1000, PD_L = 0x8000,
       // Not a Saturn bit: only the port's overlay menu looks at it.
       PD_SELECT = 0x10000 };

static int evdevOpenPad()
{
    char path[64];
    for (int i = 0; i < 16; ++i)
    {
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        char name[128] = {0};
        if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) > 0 &&
            (strstr(name, "RG34XX") || strstr(name, "Anbernic") || strstr(name, "Controller")))
        {
            g_evFd = fd;
            g_evMask = 0;
            fprintf(stderr, "[input] evdev pad: %s (%s)\n", name, path);
            return fd;
        }
        close(fd);
    }
    fprintf(stderr, "[input] evdev: no controller found\n");
    return -1;
}

static void evdevPollPad()
{
    if (g_evFd < 0 && evdevOpenPad() < 0) return;
    struct input_event ev;
    while (read(g_evFd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev))
    {
        if (ev.type == EV_KEY)
        {
            const bool down = ev.value != 0;
            unsigned bit = 0;
            switch (ev.code)
            {
            case 304: bit = PD_A; break;       // BTN_SOUTH
            case 305: bit = PD_B; break;       // BTN_EAST
            case 306: bit = PD_C; break;       // BTN_C
            case 307: bit = PD_C; break;       // BTN_NORTH (pad label "Y")
            case 308: bit = PD_X; break;       // BTN_WEST  (pad label "X")
            case 310: bit = PD_L; break;       // BTN_TL    (L1)
            case 311: bit = PD_R; break;       // BTN_TR    (R1)
            case 312: bit = PD_Z; break;       // BTN_TL2   (L2)
            case 313: bit = PD_Y; break;       // BTN_TR2   (R2)
            case 314: bit = PD_SELECT; break;  // BTN_SELECT (menu only)
            case 315: bit = PD_START; break;   // BTN_START
            default: break;
            }
            if (bit)
            {
                if (down) g_evMask |= bit;
                else      g_evMask &= ~bit;
            }
        }
        else if (ev.type == EV_ABS)
        {
            // ABS_HAT0X = 16, ABS_HAT0Y = 17; the stick is RX/RY (3/4) and is
            // reported separately through the analog path.
            if (ev.code == 16)
            {
                g_evMask &= ~(unsigned)(PD_LEFT | PD_RIGHT);
                if (ev.value < 0) g_evMask |= PD_LEFT;
                else if (ev.value > 0) g_evMask |= PD_RIGHT;
            }
            else if (ev.code == 17)
            {
                g_evMask &= ~(unsigned)(PD_UP | PD_DOWN);
                if (ev.value < 0) g_evMask |= PD_UP;
                else if (ev.value > 0) g_evMask |= PD_DOWN;
            }
        }
        else if (ev.type == EV_SYN && ev.code == SYN_DROPPED)
        {
            // The kernel dropped events: re-sync rather than keep a stuck bit.
            g_evMask = 0;
        }
    }
}

// Drain the GPU. GL calls are queued, so a CPU-side timer measures queueing,
// not work: the stall surfaces wherever the CPU first waits. Calling this at the
// top of a frame attributes the previous frame's GPU time correctly, which is
// how the per-phase timings were caught blaming the wrong pass.
extern "C" void pd_gpu_sync(void)
{
    if (g_finishFn) g_finishFn();
    else if (p_eglWaitClient_fn) p_eglWaitClient_fn();
}

extern "C" unsigned pd_evdev_pad_mask(void)
{
    evdevPollPad();
    return g_evMask;
}

// ---------------------------------------------------------------------------
// Optional crash diagnostics (PD_CRASH_TRACE=1).
//
// The engine has no teardown at all: main() returns 0 and static destructors
// run. When something in that path dies the log just says "Segmentation
// fault", which is useless on a handheld with no debugger. This prints a
// backtrace to stderr (captured into log.txt) instead.
// ---------------------------------------------------------------------------
static void pdCrashHandler(int sig)
{
    void* frames[48];
    int n = backtrace(frames, 48);

    if (sig == SIGTERM || sig == SIGINT)
    {
        // EmulationStation stops a port with SIGTERM. Running the full renderer
        // teardown from a signal handler is unsafe (the signal can arrive inside
        // a GL call), but skipping static destructors is exactly what avoids the
        // libmali crash, so leave the process cleanly and let the kernel reclaim
        // the context.
        const char* verbose = getenv("PD_CRASH_TRACE");
        if (verbose && verbose[0] != '0')
        {
            static const char msg[] = "\n[port] SIGTERM, exiting\n";
            ssize_t ignored = write(2, msg, sizeof(msg) - 1);
            (void)ignored;
            backtrace_symbols_fd(frames, n, 2);
        }
        fflush(nullptr);
        _exit(0);
    }

    const char* what = sig == SIGSEGV ? "\n[crash] SIGSEGV\n"
                     : sig == SIGABRT ? "\n[crash] SIGABRT\n"
                     : sig == SIGBUS  ? "\n[crash] SIGBUS\n"
                                      : "\n[crash] signal\n";
    ssize_t ignored = write(2, what, strlen(what));
    (void)ignored;
    backtrace_symbols_fd(frames, n, 2);
    fflush(nullptr);
    _exit(128 + sig);
}

static void installCrashTrace()
{
    // Always: a clean exit on the signals EmulationStation / the launcher use to
    // stop a port. Without this the default action kills the process mid-GL-call
    // and the launcher reports a crash.
    signal(SIGTERM, pdCrashHandler);
    signal(SIGINT, pdCrashHandler);

    const char* env = getenv("PD_CRASH_TRACE");
    if (!env || env[0] == '0') return;
    signal(SIGSEGV, pdCrashHandler);
    signal(SIGABRT, pdCrashHandler);
    signal(SIGBUS, pdCrashHandler);
    fprintf(stderr, "[port] crash trace enabled\n");
}

// ---------------------------------------------------------------------------
// Ordered shutdown.
//
// The engine's main() just returns 0 (engine/PDS/PDS.cpp has no teardown), so
// every static destructor and libmali's own unload run in an order nobody
// controls. On the RG34XX that ends in a SIGSEGV inside libmali after a clean
// play session, which the launcher reports as "Segmentation fault".
//
// This handler is registered late (during backend init), so with atexit's
// reverse ordering it runs before the engine's static destructors: tear the
// renderer down ourselves, flush the log, then _exit() so nothing else in the
// process gets a chance to crash on the way out.
// ---------------------------------------------------------------------------
static void pdOrderedShutdown();

static void pdOrderedShutdown()
{
    static bool done = false;
    if (done) return;
    done = true;

    const char* optOut = getenv("PD_ORDERED_EXIT");
    if (optOut && optOut[0] == '0') return;

    fprintf(stderr, "[port] ordered shutdown\n");

    if (g_imguiReady) imguiDestroy();

    bgfx::shutdown();

    if (g_eglSurface && p_eglMakeCurrent_fn)
        p_eglMakeCurrent_fn(g_eglDisplay, nullptr, nullptr, nullptr);
    if (g_eglSurface && p_eglDestroySurface_fn)
        p_eglDestroySurface_fn(g_eglDisplay, g_eglSurface);
    g_eglSurface = nullptr;
    if (g_eglContext && p_eglDestroyContext_fn)
        p_eglDestroyContext_fn(g_eglDisplay, g_eglContext);
    g_eglContext = nullptr;
    g_glContext = nullptr;
    if (g_eglDisplay && p_eglTerminate_fn) p_eglTerminate_fn(g_eglDisplay);
    g_eglDisplay = nullptr;

    if (gWindowBGFX)
    {
        SDL_GL_DestroyContext(g_glContext);
        SDL_DestroyWindow(gWindowBGFX);
        gWindowBGFX = nullptr;
        SDL_Quit();
    }

    if (g_fbMap) { munmap(g_fbMap, g_fbMapLen); g_fbMap = nullptr; }
    if (g_fbFd >= 0) { close(g_fbFd); g_fbFd = -1; }
    free(g_presentBuf);
    g_presentBuf = nullptr;

    fprintf(stderr, "[port] shutdown complete\n");
    fflush(nullptr);
    _exit(0);
}

// Interpose exit() so the teardown above always wins, whoever calls it: main()
// returning through __libc_start_main, an explicit exit() in the engine, or our
// own hooks. Static destructors never get a chance to run into libmali.
extern "C" void exit(int code) __attribute__((noreturn));
extern "C" void exit(int code)
{
    pdOrderedShutdown();
    _exit(code);
}

static bool createWindowAndContext()
{    // Initialisation order matters on this firmware, measured against a
    // standalone probe that works: set the video driver hint FIRST, bring up
    // VIDEO, and only then GAMEPAD. The combined SDL_Init(VIDEO|GAMEPAD) fails
    // here ("No available video device") and, if gamepad is initialised first,
    // SDL ends up enumerating the pad but delivering no events and reporting
    // frozen button state — which is why the game saw no input at all.
    if (!SDL_WasInit(SDL_INIT_VIDEO))
    {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
        if (!SDL_Init(SDL_INIT_VIDEO))
        {
            fprintf(stderr, "[port] SDL_Init(VIDEO, offscreen) failed: %s\n", SDL_GetError());
            if (!SDL_Init(SDL_INIT_VIDEO))
                fprintf(stderr, "[port] SDL_Init(VIDEO) failed: %s\n", SDL_GetError());
        }
    }
    if (!SDL_WasInit(SDL_INIT_GAMEPAD))
    {
        if (!SDL_Init(SDL_INIT_GAMEPAD))
            fprintf(stderr, "[port] SDL_Init(GAMEPAD) failed: %s\n", SDL_GetError());
        else
            fprintf(stderr, "[port] SDL gamepad subsystem up\n");
    }
    // Drain whatever queued during init so the first frame does not see stale
    // device-addition events.
    {
        SDL_Event flush;
        while (SDL_PollEvent(&flush)) {}
    }

    {
        const char* driver = SDL_GetCurrentVideoDriver();
        fprintf(stderr, "[port] SDL video driver: %s\n", driver ? driver : "(none)");
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
        // Retry without the OpenGL flag: the firmware's SDL3 may expose only the
        // dummy/offscreen drivers (Knulli on the H700), which can still give us
        // a window object for input, ImGui and sizes — just no GL context.
        fprintf(stderr, "[port] SDL_CreateWindow(OPENGL) failed: %s\n", SDL_GetError());
        // Also drop FULLSCREEN here: asking the offscreen driver for a fullscreen
        // window makes it adopt the dummy display mode (1024x768 on this
        // firmware), and the engine reads SDL_GetWindowSize() to size the whole
        // renderer — that mismatch is what pushed the picture off-centre with
        // black bands. The fbdev EGL surface we create is the panel size anyway.
        gWindowBGFX = SDL_CreateWindow("Panzer Dragoon Saga", width, height,
                                       flags & ~(SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN));
        if (gWindowBGFX) SDL_SetWindowSize(gWindowBGFX, width, height);
    }
    if (!gWindowBGFX)
    {
        fprintf(stderr, "[port] SDL_CreateWindow failed: %s\n", SDL_GetError());
        if (!createFbdevEglContext()) return false;
        fprintf(stderr, "[port] continuing without an SDL window (fbdev EGL)\n");
        return true;
    }

    g_glContext = SDL_GL_CreateContext(gWindowBGFX);
    if (!g_glContext)
    {
        fprintf(stderr, "[port] SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        // Knulli/Batocera: SDL3 has no video backend on this firmware, so take
        // the context from libmali's EGL over the framebuffer instead.
        if (!createFbdevEglContext()) return false;
    }
    else if (!SDL_GL_MakeCurrent(gWindowBGFX, g_glContext))
    {
        fprintf(stderr, "[port] SDL_GL_MakeCurrent failed: %s\n", SDL_GetError());
        return false;
    }
    if (!g_eglSurface)
    {
        // Adaptive vsync: the engine paces to the Saturn's 30 Hz internally.
        SDL_GL_SetSwapInterval(0);
    }
    return true;
}

bool SDL_ES3_backend::init()
{
    installCrashTrace();
    // Runs before the engine's static destructors (see pdOrderedShutdown).
    atexit(pdOrderedShutdown);
    if (!createWindowAndContext()) return false;

    SDL_PropertiesID props = SDL_GetWindowProperties(gWindowBGFX);

    bgfx::Init initParam;
    initParam.type = bgfx::RendererType::OpenGLES;
    // Hand our existing context to the backend; do not let it create one.
    // On the fbdev path the context came from libmali directly, not from
    // SDL_GL, so pass that handle instead of asking SDL for it.
    initParam.platformData.context = g_eglContext ? (void*)g_eglContext
                                                  : (void*)SDL_GL_GetCurrentContext();
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

    // Offscreen backbuffer has to be created after bgfx::init(): that is where
    // the GL entry points are resolved, and calling them earlier segfaults.
    // Disable with PD_OFFSCREEN=0 to render straight to the panel.
    if (g_eglDirect)
    {
        const char* off = getenv("PD_OFFSCREEN");
        if (!off || off[0] != '0')
            g_offscreenPresent = pdgles_offscreen_begin((int)g_fbWidth, (int)g_fbHeight) != 0;
        if (!g_offscreenPresent)
            fprintf(stderr, "[present] offscreen unavailable, rendering directly\n");
    }

    pdgles_set_swap_hook(swapWindow);
    pd_menu_install();

    imguiCreate();
    fprintf(stderr, "[port] step: imguiCreate done (context %p)\n", (void*)ImGui::GetCurrentContext());
    if (gWindowBGFX)
    {
        g_imguiSDL = ImGui_ImplSDL3_InitForOpenGL(gWindowBGFX, g_glContext);
        fprintf(stderr, "[port] step: sdl3 backend ready (%d)\n", (int)g_imguiSDL);
    }
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
    if (g_imguiSDL) ImGui_ImplSDL3_Shutdown();
    g_imguiSDL = false;
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

// menu.cpp — in-game menu for the handheld port.
//
// The engine's own debug UI is compiled out with SHIPPING_BUILD, so this is the
// port's menu layer: a Dear ImGui overlay drawn on the same GLES context right
// before the buffer swap. It is state-driven (SDL keyboard/gamepad queries, not
// events) because the engine already consumes the event queue.
//
// Nothing here talks to bgfx: the overlay is drawn with imgui_impl_opengl3, so
// it works regardless of which game renderer is active.

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" void pdgles_set_pre_swap_hook(void (*fn)(void));

// Read by the engine (see patches/README.md #5): while the menu is open the
// game must not also consume the pad, otherwise the overlay behaves like a
// transparent window instead of a pause screen.
bool pdMenuOwnsInput = false;

// provided by renderer_gles.cpp
extern "C" int pdgles_window_width();
extern "C" int pdgles_window_height();

// engine globals the menu can adjust (defined in AzelLib/renderer.cpp)
extern float gVolume;

namespace
{
	struct MenuState
	{
		bool open = false;
		bool quitRequested = false;
		bool fpsOverlay = false;
		int scaleMode = 0;          // 0 = fit 4:3, 1 = fill, 2 = integer
		float masterVolume = 1.0f;
		bool showControls = false;
		bool showAbout = false;
		long openAfterMs = 0;
		Uint64 openAt = 0;
		double lastFps = 0.0;
		Uint64 lastFrame = 0;
		int framesSinceSample = 0;
		double fpsAccum = 0.0;
	} g_menu;

	// Edge detection helpers: the engine may leave buttons held, so the menu
	// keys on transitions rather than levels.
	struct Edge
	{
		bool prev = false;
		bool pressed(bool now) { bool p = now && !prev; prev = now; return p; }
	};

	Edge g_menuToggle, g_padToggle, g_quitCombo;

	// One handle for the whole session. SDL_OpenGamepad takes a reference each
	// call, and this used to run once per frame from three places — hundreds of
	// references per minute, and it fights the engine, which opens the same pad
	// for gameplay.
	// The pad is read through evdev in the platform layer: SDL's joystick layer
	// in this process delivers no events and reports frozen state, so every
	// SDL_GetGamepadButton() here was permanently 0.
	extern "C" unsigned pd_evdev_pad_mask(void);

	unsigned padMask() { return pd_evdev_pad_mask(); }

	SDL_Gamepad* pad()
	{
		static SDL_Gamepad* cached = nullptr;
		static Uint64 lastTry = 0;
		if (cached && SDL_GamepadConnected(cached)) return cached;
		const Uint64 now = SDL_GetTicks();
		if (now - lastTry < 1000) return cached;
		lastTry = now;
		int n = 0;
		if (SDL_JoystickID* ids = SDL_GetGamepads(&n))
		{
			for (int i = 0; i < n; ++i)
				if (SDL_Gamepad* gp = SDL_OpenGamepad(ids[i])) { cached = gp; break; }
			SDL_free(ids);
		}
		return cached;
	}

	void drawMenu()
	{
		const ImGuiViewport* vp = ImGui::GetMainViewport();
		const float w = vp->Size.x;
		const float h = vp->Size.y;

		// Dim the whole frame first. The game keeps running behind the overlay
		// (it is a transparent window), and without this the moving picture reads
		// as broken graphics behind the menu text.
		ImDrawList* bg = ImGui::GetBackgroundDrawList();
		// Opaque, not translucent: the game keeps rendering behind the overlay
		// (movies play in their own small viewport), and anything less than
		// opaque reads as graphical corruption behind the menu.
		bg->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), IM_COL32(0, 0, 0, 255));

		ImGui::SetNextWindowPos(ImVec2(w * 0.5f, h * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(w * 0.86f, 0.0f), ImGuiCond_Always);
		ImGui::Begin("ProjectDragoonArm", nullptr,
		             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
		             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

		if (ImGui::Button("Resume", ImVec2(-1, 0)))
			g_menu.open = false;

		ImGui::SeparatorText("Display");
		ImGui::RadioButton("Fit 4:3 (letterbox)", &g_menu.scaleMode, 0);
		ImGui::RadioButton("Fill 3:2", &g_menu.scaleMode, 1);
		ImGui::RadioButton("Integer scale", &g_menu.scaleMode, 2);
		ImGui::Checkbox("FPS overlay", &g_menu.fpsOverlay);

		ImGui::SeparatorText("Audio");
		if (ImGui::SliderFloat("Master", &g_menu.masterVolume, 0.0f, 1.0f))
			gVolume = g_menu.masterVolume;

		ImGui::SeparatorText("Session");
		if (ImGui::Button("Controls", ImVec2(-1, 0))) g_menu.showControls = true;
		if (ImGui::Button("About", ImVec2(-1, 0))) g_menu.showAbout = true;

		ImGui::Separator();
		if (ImGui::Button("Exit to Ports", ImVec2(-1, 0)))
			g_menu.quitRequested = true;

		ImGui::TextDisabled("Menu: Start+Select  |  %.1f FPS", g_menu.lastFps);
		ImGui::End();

		if (g_menu.showControls)
		{
			ImGui::SetNextWindowSize(ImVec2(w * 0.86f, 0), ImGuiCond_Always);
			ImGui::SetNextWindowPos(ImVec2(w * 0.5f, h * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
			ImGui::Begin("Controls", &g_menu.showControls,
			             ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
			ImGui::TextUnformatted("D-Pad        Move / navigate");
			ImGui::TextUnformatted("A            Confirm / interact");
			ImGui::TextUnformatted("B            Cancel / back");
			ImGui::TextUnformatted("C, X         Action (context)");
			ImGui::TextUnformatted("Y            Dragon / menu shortcut");
			ImGui::TextUnformatted("L1 / R1      Rotate camera");
			ImGui::TextUnformatted("R2 (held)    Fast-forward");
			ImGui::TextUnformatted("Start+Select Menu");
			ImGui::End();
		}

		if (g_menu.showAbout)
		{
			ImGui::SetNextWindowSize(ImVec2(w * 0.86f, 0), ImGuiCond_Always);
			ImGui::SetNextWindowPos(ImVec2(w * 0.5f, h * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
			ImGui::Begin("About", &g_menu.showAbout,
			             ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
			ImGui::TextUnformatted("ProjectDragoonArm");
			ImGui::TextUnformatted("Panzer Dragoon Saga, AArch64 / OpenGL ES.");
			ImGui::TextUnformatted("Engine: yaz0r/Azel (MIT). Disc identity: blythos/atolm.");
			ImGui::TextUnformatted("Not affiliated with SEGA. Supply your own disc.");
			ImGui::End();
		}
	}

	void drawFps()
	{
		if (!g_menu.fpsOverlay) return;
		ImGui::SetNextWindowPos(ImVec2(6, 6), ImGuiCond_Always);
		ImGui::Begin("##fps", nullptr,
		             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
		             ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
		             ImGuiWindowFlags_NoBackground);
		ImGui::Text("%.1f FPS", g_menu.lastFps);
		ImGui::End();
	}
}

	// Pad navigation for the overlay. Without this the menu is a dead window:
	// it is drawn but nothing can be selected, and since the game's input is
	// suspended while it is open the game cannot be driven either.
	void feedPadNavMask(unsigned pm)
	{
		ImGuiIO& io = ImGui::GetIO();
		io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

		io.AddKeyEvent(ImGuiKey_GamepadDpadUp,    (pm & 0x0010u) != 0);
		io.AddKeyEvent(ImGuiKey_GamepadDpadDown,  (pm & 0x0020u) != 0);
		io.AddKeyEvent(ImGuiKey_GamepadDpadLeft,  (pm & 0x0040u) != 0);
		io.AddKeyEvent(ImGuiKey_GamepadDpadRight, (pm & 0x0080u) != 0);
		io.AddKeyEvent(ImGuiKey_GamepadFaceDown,  (pm & 0x0001u) != 0);  // A
		io.AddKeyEvent(ImGuiKey_GamepadFaceRight, (pm & 0x0002u) != 0);  // B
	}

// Called once per presented frame, immediately before the swap.
extern "C" void pd_menu_frame(void)
{
	static bool installed = false;
	if (!installed)
	{
		installed = true;
		g_menu.lastFrame = SDL_GetTicks();
		// PD_MENU_OPEN=1 starts with the menu visible (screenshots/CI)
		if (const char* o = getenv("PD_MENU_OPEN")) g_menu.open = o[0] && o[0] != '0';
		// PD_MENU_AFTER_MS=<n> opens the menu into a running game, which is how
		// the mid-gameplay overlay state is reproduced deterministically.
		if (const char* o = getenv("PD_MENU_AFTER_MS")) g_menu.openAfterMs = atol(o);
		g_menu.openAt = SDL_GetTicks() + (Uint64)g_menu.openAfterMs;
		if (const char* v = getenv("PD_FPS_OVERLAY")) g_menu.fpsOverlay = v[0] && v[0] != '0';
	}

	const bool* keys = SDL_GetKeyboardState(nullptr);
	bool keyboardToggle = keys && (keys[SDL_SCANCODE_ESCAPE] || keys[SDL_SCANCODE_F1]);

	const unsigned pm = padMask();
	// Start+Select toggles; Select alone also opens (it is menu-only, so it
	// never reaches the game with a meaning of its own).
	const bool padStart = (pm & 0x0008u) != 0;
	const bool padSelect = (pm & 0x10000u) != 0;
	const bool padToggle = padStart && padSelect;
	const bool padQuit = false;

	if (g_menu.openAfterMs > 0 && !g_menu.open && SDL_GetTicks() >= g_menu.openAt)
	{
		g_menu.open = true;
		g_menu.openAfterMs = 0;
		fprintf(stderr, "[menu] auto-opened at %llu ms\n", (unsigned long long)SDL_GetTicks());
	}

	// B (or Escape) always closes the menu: the overlay suspends game input, so
	// there must be a way out that does not depend on any one button mapping.
	const bool padBackEdge = (pm & 0x0002u) != 0;   // B closes the menu
	static Edge backEdge;
	const bool backPressed = backEdge.pressed(padBackEdge);

	if (g_menuToggle.pressed(keyboardToggle || padToggle) ||
	    (g_menu.open && (backPressed || (keys && keys[SDL_SCANCODE_ESCAPE]))))
	{
		g_menu.open = !g_menu.open;
		fprintf(stderr, "[menu] %s\n", g_menu.open ? "opened (game input suspended)" : "closed");
	}

	pdMenuOwnsInput = g_menu.open;

	// FPS accounting (the engine paces to 30 Hz; this shows the presented rate)
	const Uint64 now = SDL_GetTicks();
	++g_menu.framesSinceSample;
	if (now - g_menu.lastFrame >= 500)
	{
		g_menu.lastFps = g_menu.framesSinceSample * 1000.0 / (double)(now - g_menu.lastFrame);
		g_menu.framesSinceSample = 0;
		g_menu.lastFrame = now;
	}

	// The overlay owns the ImGui frame in shipping builds: the engine's own
	// ImGui calls are compiled out there.
	if (!g_menu.open && !g_menu.fpsOverlay) return;

	if (g_menu.open)
		feedPadNavMask(pm);

	ImGuiIO& io = ImGui::GetIO();
	io.DisplaySize = ImVec2((float)pdgles_window_width(), (float)pdgles_window_height());
	ImGui_ImplOpenGL3_NewFrame();
	ImGui::NewFrame();

	if (g_menu.open) drawMenu();
	drawFps();

	ImGui::Render();
	ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

	if (g_menu.quitRequested || (padQuit && g_quitCombo.pressed(true)))
	{
		// Set the engine's own quit flag, NOT bContinue. This hook runs inside
		// azelSdl_EndFrame(), and loopIteration() assigns `bContinue =
		// azelSdl_EndFrame()` right after — so writing bContinue here was
		// overwritten with true on the same frame and "Exit to Ports" did
		// nothing. closeApp is what EndFrame returns !closeApp from, so it
		// survives and the main loop breaks as intended.
		extern bool closeApp;
		closeApp = true;
		fprintf(stderr, "[menu] exit requested\n");
	}
}

extern "C" void pd_menu_install(void)
{
	pdgles_set_pre_swap_hook(pd_menu_frame);
}

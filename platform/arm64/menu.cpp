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

	void drawMenu()
	{
		const ImGuiViewport* vp = ImGui::GetMainViewport();
		const float w = vp->Size.x;
		const float h = vp->Size.y;

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
		if (const char* v = getenv("PD_FPS_OVERLAY")) g_menu.fpsOverlay = v[0] && v[0] != '0';
	}

	const bool* keys = SDL_GetKeyboardState(nullptr);
	bool keyboardToggle = keys && (keys[SDL_SCANCODE_ESCAPE] || keys[SDL_SCANCODE_F1]);

	bool padToggle = false, padQuit = false;
	int numPads = 0;
	if (SDL_JoystickID* pads = SDL_GetGamepads(&numPads))
	{
		for (int i = 0; i < numPads; ++i)
		{
			if (SDL_Gamepad* gp = SDL_OpenGamepad(pads[i]))
			{
				const bool start = SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_START);
				const bool back = SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_BACK);
				if (start && back) padToggle = true;
				if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_GUIDE)) padQuit = true;
			}
		}
		SDL_free(pads);
	}

	if (g_menuToggle.pressed(keyboardToggle || padToggle))
		g_menu.open = !g_menu.open;

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
		extern bool bContinue;
		bContinue = false;
	}
}

extern "C" void pd_menu_install(void)
{
	pdgles_set_pre_swap_hook(pd_menu_frame);
}

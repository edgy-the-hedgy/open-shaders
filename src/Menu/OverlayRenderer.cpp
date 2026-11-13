#include "OverlayRenderer.h"
#include "BackgroundBlur.h"
#include "HomePageRenderer.h"
#include "ThemeManager.h"

#include <array>
#include <cmath>
#include <dxgi.h>
#include <dxgi1_4.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <imgui_internal.h>
#include <unordered_map>
#include <unordered_set>

#include "CSEditor/EditorWindow.h"
#include "Feature.h"
#include "FeatureIssues.h"
#include "Menu/DesktopMirrorDrawData.h"
#if defined(ENABLE_EFFECTS11)
#	include "Features/Effects11/EffectManager.h"
#	include "Features/Effects11/Editor/Effects11Editor.h"
#endif
#include "Features/HDRDisplay.h"
#include "Features/RenderDoc.h"
#include "Features/SceneSelector.h"
#include "Features/Upscaling.h"
#include "Features/VR.h"
#include "Globals.h"
#include "GpuPass.h"
#include "I18n/I18n.h"
#include "Menu.h"
<<<<<<< HEAD
#include "Menu/CursorLoader.h"
||||||| 12b261724
=======
#include "Menu/CursorLoader.h"
#include "Menu/FontAtlasState.h"
#include "Menu/SettingsTabRenderer.h"
#include "Presets/UnifiedPresetCatalog.h"
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
#include "ShaderCache.h"
#include "State.h"
#include "Util.h"
#include "Utils/Notifications.h"
#include "Utils/UISurface.h"

#include "Features/PerformanceOverlay.h"
#include "Features/PerformanceOverlay/ABTesting/ABTesting.h"

namespace
{
	DesktopMirrorDrawData pendingDesktopMirror;
	ImDrawData* pendingDesktopMirrorSource = nullptr;

	bool IsSupportedDesktopMirrorFormat(const DXGI_FORMAT format)
	{
		return format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
		       format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
	}

	/**
	 * @brief Keeps a status card from covering the menu: when the main menu window is displayed behind the card, the
	 * card is moved just behind it. Z-order follows the window wherever it is moved or undocked, unlike a fixed position.
	 */
	void KeepBehindMainWindow(ImGuiWindow* card)
	{
		ImGuiContext& g = *ImGui::GetCurrentContext();
		ImGuiWindow* menu = nullptr;
		int cardIndex = -1;
		int menuIndex = -1;
		for (int i = 0; i < g.Windows.Size; i++) {
			ImGuiWindow* win = g.Windows[i];
			if (win == card)
				cardIndex = i;
			else if (!menu && !win->ParentWindow && win->WasActive && IsMainWindow(win)) {
				menu = win;
				menuIndex = i;
			}
		}
		if (menu && cardIndex > menuIndex)
			ImGui::BringWindowToDisplayBehind(card, menu);
	}

	/**
	 * @brief Begins the top-left status window in the toast card style: themed WindowBg at OVERLAP_MIN_ALPHA,
	 * no frame, with the accent border and left stripe drawn by EndOverlayCard.
	 * @return False when the window is collapsed or clipped; EndOverlayCard must still be called.
	 */
	bool BeginOverlayCard(const ImVec2& position)
	{
		using Constants = ThemeManager::Constants;
		const float scale = Util::GetUIScale();
		ImGui::SetNextWindowPos(position);
		ImGui::SetNextWindowBgAlpha(0.0f);  // The surface is drawn by EndOverlayCard
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, ImGui::GetStyle().FrameRounding * Constants::TOAST_ROUNDING_SCALE);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
			ImVec2((Constants::TOAST_PADDING_X + Constants::TOAST_STRIPE_WIDTH) * scale, Constants::TOAST_PADDING_Y * scale));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
		const bool visible = ImGui::Begin("ShaderCompilationInfo", nullptr,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
		KeepBehindMainWindow(ImGui::GetCurrentWindow());
		if (visible) {
			// Surface backing, drawn before the content. The card auto-sizes, so it uses last frame's rect.
			Util::Surface::Options surface;
			surface.rounding = ImGui::GetStyle().WindowRounding;
			const ImVec2 p0 = ImGui::GetWindowPos();
			const ImVec2 size = ImGui::GetWindowSize();
			Util::Surface::Draw(ImGui::GetWindowDrawList(), p0, ImVec2(p0.x + size.x, p0.y + size.y),
				Util::Surface::Role::OverlayCard, surface);
		}
		return visible;
	}

	/** @brief Draws the card's thin accent border and left stripe, then ends the window begun by BeginOverlayCard. */
	void EndOverlayCard(const ImVec4& accentColor)
	{
		using Constants = ThemeManager::Constants;
		const float scale = Util::GetUIScale();
		ImDrawList* draw = ImGui::GetWindowDrawList();
		const ImVec2 p0 = ImGui::GetWindowPos();
		const ImVec2 size = ImGui::GetWindowSize();
		const ImVec2 p1(p0.x + size.x, p0.y + size.y);
		const float rounding = ImGui::GetStyle().WindowRounding;
		const ImU32 accent = ImGui::ColorConvertFloat4ToU32(ImVec4(accentColor.x, accentColor.y, accentColor.z, Constants::TOAST_ACCENT_ALPHA));
		Util::Surface::DrawAccentFrame(draw, p0, p1, accent, rounding, Constants::TOAST_BORDER_THICKNESS * scale, Constants::TOAST_STRIPE_WIDTH * scale);
		ImGui::End();
		ImGui::PopStyleVar(3);
	}

	/** @brief Effects 11 effects that failed to compile, or 0 while the effect manager is not up. */
	uint32_t GetFailedEffectCount()
	{
		auto& effectManager = EffectManager::GetSingleton();
		return effectManager.IsInitialized() ? effectManager.GetFailedEffectCount() : 0;
	}

	void DrawShaderCompilationFailures(uint64_t failed, const Menu::ThemeSettings& themeSettings)
	{
		if (failed) {
<<<<<<< HEAD
			ImGui::TextColored(themeSettings.StatusPalette.Error,
				T("overlay.shaders_failed", "ERROR: %llu shaders failed to compile. Check installation and OpenShaders.log"),
				static_cast<unsigned long long>(failed));
||||||| 12b261724
			ImGui::TextColored(themeSettings.StatusPalette.Error,
				"ERROR: %llu shaders failed to compile. Check installation and CommunityShaders.log",
				static_cast<unsigned long long>(failed));
=======
			ImGui::TextColored(themeSettings.StatusPalette.Error, "%s",
				I18n::GetSingleton()->Format("overlay.shaders_failed", { { "count", std::to_string(failed) } },
										"ERROR: {count} shaders failed to compile. Check installation and CommunityShaders.log")
					.c_str());
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb

			if (FeatureIssues::HasPotentialShaderModifyingFeatures()) {
				ImGui::TextColored(themeSettings.StatusPalette.Error, "%s", T("overlay.modified_features", "Features that may have modified shaders detected. Check Feature Issues in the Menu."));
			}
		}
	}

#if defined(ENABLE_EFFECTS11)
	void DrawEffects11Errors(const Menu::ThemeSettings& themeSettings)
	{
		const uint32_t effectFailed = GetFailedEffectCount();
		if (effectFailed) {
			ImGui::TextColored(themeSettings.StatusPalette.Error, "%s",
				I18n::GetSingleton()->Format("overlay.effects_failed", { { "count", std::to_string(effectFailed) } },
										"ERROR: {count} effect(s) failed to compile")
					.c_str());
			for (const auto& err : EffectManager::GetSingleton().GetAllErrors())
				ImGui::TextColored(themeSettings.StatusPalette.Error, "  %s", err.c_str());
		}
	}
#endif

<<<<<<< HEAD
||||||| 12b261724
	bool IsVisibleRootWindow(ImGuiWindow* win)
	{
		if (!win || !win->WasActive || win->Hidden)
			return false;
		return !(win->ParentWindow && !win->DockIsActive) && !(win->Flags & SKIP_WINDOW_FLAGS);
	}

	// Patches DrawList background vertices for windows involved in overlap.
	void PatchOverlappingWindowBackgrounds()
	{
		auto* ctx = ImGui::GetCurrentContext();
		if (!ctx)
			return;

		using C = ThemeManager::Constants;
		const float dt = ImGui::GetIO().DeltaTime;

		struct WinInfo
		{
			ImGuiWindow* win;
			ImRect rect;
		};
		std::vector<WinInfo> windows;
		for (int i = 0; i < ctx->Windows.Size; i++) {
			auto* win = ctx->Windows[i];
			if (IsVisibleRootWindow(win))
				windows.push_back({ win, win->Rect() });
		}

		std::unordered_set<ImGuiID> overlapping;
		for (size_t i = 0; i < windows.size(); i++)
			for (size_t j = i + 1; j < windows.size(); j++)
				if (windows[i].rect.Overlaps(windows[j].rect)) {
					auto* a = windows[i].win;
					auto* b = windows[j].win;
					// Main CS window never dims; other windows yield to it
					if (IsMainWindow(a))
						overlapping.insert(b->ID);
					else if (IsMainWindow(b))
						overlapping.insert(a->ID);
					else
						overlapping.insert(a->FocusOrder > b->FocusOrder ? a->ID : b->ID);
				}

		const ImU32 bgRGB = ImGui::GetColorU32(ImGuiCol_WindowBg) & ~IM_COL32_A_MASK;

		for (auto& [win, rect] : windows) {
			const float target = overlapping.count(win->ID) ? C::OVERLAP_MIN_ALPHA : 0.0f;
			float& alpha = s_windowOverlapAlpha[win->ID];
			const float speed = (target > alpha) ? C::OVERLAP_FADEIN_SPEED : C::OVERLAP_FADEOUT_SPEED;
			alpha += (target - alpha) * (std::min)(1.0f, dt * speed);

			if (alpha < C::OVERLAP_ALPHA_EPSILON) {
				alpha = 0.0f;
				continue;
			}

			auto* dl = win->DrawList;
			if (!dl || dl->VtxBuffer.Size == 0)
				continue;

			// Clamp background rect vertex alpha (contiguous bgRGB block at start of DrawList)
			const ImU32 minA = static_cast<ImU32>(alpha * 255.0f);
			for (int v = 0; v < dl->VtxBuffer.Size; v++) {
				auto& vtx = dl->VtxBuffer[v];
				if ((vtx.col & ~IM_COL32_A_MASK) != bgRGB)
					break;
				ImU32 a = (vtx.col >> IM_COL32_A_SHIFT) & 0xFF;
				if (a > 0 && a < minA)
					vtx.col = bgRGB | (minA << IM_COL32_A_SHIFT);
			}
		}

		// Prune stale entries
		for (auto it = s_windowOverlapAlpha.begin(); it != s_windowOverlapAlpha.end();)
			it->second < C::OVERLAP_ALPHA_EPSILON ? it = s_windowOverlapAlpha.erase(it) : ++it;
	}
=======
	bool IsVisibleRootWindow(ImGuiWindow* win)
	{
		if (!win || !win->WasActive || win->Hidden)
			return false;
		return !(win->ParentWindow && !win->DockIsActive) && !(win->Flags & SKIP_WINDOW_FLAGS);
	}

	// Patches DrawList background vertices for windows involved in overlap.
	void PatchOverlappingWindowBackgrounds()
	{
		auto* ctx = ImGui::GetCurrentContext();
		if (!ctx)
			return;

		using C = ThemeManager::Constants;
		const float dt = ImGui::GetIO().DeltaTime;

		struct WinInfo
		{
			ImGuiWindow* win;
			ImRect rect;
		};
		std::vector<WinInfo> windows;
		for (int i = 0; i < ctx->Windows.Size; i++) {
			auto* win = ctx->Windows[i];
			if (IsVisibleRootWindow(win))
				windows.push_back({ win, win->Rect() });
		}

		std::unordered_set<ImGuiID> overlapping;
		for (size_t i = 0; i < windows.size(); i++)
			for (size_t j = i + 1; j < windows.size(); j++)
				if (windows[i].rect.Overlaps(windows[j].rect)) {
					auto* a = windows[i].win;
					auto* b = windows[j].win;
					// Main CS window never dims; other windows yield to it
					if (IsMainWindow(a))
						overlapping.insert(b->ID);
					else if (IsMainWindow(b))
						overlapping.insert(a->ID);
					else
						overlapping.insert(a->FocusOrder > b->FocusOrder ? a->ID : b->ID);
				}

		const ImU32 bgRGB = ImGui::GetColorU32(ImGuiCol_WindowBg) & ~IM_COL32_A_MASK;

		for (auto& [win, rect] : windows) {
			const float target = overlapping.count(win->ID) ? C::OVERLAP_MIN_ALPHA : 0.0f;
			float& alpha = s_windowOverlapAlpha[win->ID];
			const float speed = (target > alpha) ? C::OVERLAP_FADEIN_SPEED : C::OVERLAP_FADEOUT_SPEED;
			alpha += (target - alpha) * (std::min)(1.0f, dt * speed);

			if (alpha < C::OVERLAP_ALPHA_EPSILON) {
				alpha = 0.0f;
				continue;
			}

			auto* dl = win->DrawList;
			if (!dl || dl->VtxBuffer.Size == 0)
				continue;

			// Clamp background rect vertex alpha (contiguous bgRGB block at start of DrawList)
			const ImU32 minA = static_cast<ImU32>(alpha * 255.0f);
			for (int v = 0; v < dl->VtxBuffer.Size; v++) {
				auto& vtx = dl->VtxBuffer[v];
				if ((vtx.col & ~IM_COL32_A_MASK) != bgRGB)
					break;
				ImU32 a = (vtx.col >> IM_COL32_A_SHIFT) & 0xFF;
				if (a > 0 && a < minA)
					vtx.col = bgRGB | (minA << IM_COL32_A_SHIFT);
			}
		}

		// Prune stale entries
		for (auto it = s_windowOverlapAlpha.begin(); it != s_windowOverlapAlpha.end();)
			it->second < C::OVERLAP_ALPHA_EPSILON ? it = s_windowOverlapAlpha.erase(it) : ++it;
	}

	/** @brief Blur strength this frame: the editor's fullscreen blur follows its widgets, other windows follow the menu fade. */
	float GetBlurFade()
	{
		auto* editorWindow = EditorWindow::GetSingleton();
		if (editorWindow->open)
			return editorWindow->IsViewportActive() ? editorWindow->widgetsFade : 1.0f;

		const float menuFade = Menu::GetSingleton()->openFade;
		return menuFade > 0.0f ? menuFade : 1.0f;
	}
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
}  // namespace

void OverlayRenderer::RenderOverlay(
	Menu& menu,
	const std::function<void()>& processInputEventQueue,
	const std::function<void()>& drawSettings,
	const std::function<const char*(std::vector<InputCombo>)>& keyIdToString,
	float& cachedFontSize,
	float currentFontSize)
{
<<<<<<< HEAD
	BackgroundBlur::RestoreRetainedBuffers();

	// Apply the VR panel size before pumping input: PumpInput reads
	// io.DisplaySize to map wand UV to pixels for this frame.
	ApplyVRPanelDisplaySize();
||||||| 12b261724
=======
	// Always sync, including frames that skip drawing, so closing the last CS UI restores vanity.
	menu.SyncVanityCamera();

>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
	processInputEventQueue();

	auto& io = ImGui::GetIO();
	const bool acceptsInput = !menu.IsPreviewFlying() &&
	                          (menu.ShouldSwallowInput() || globals::features::vr.HelperRequestsRender());
	io.SetAppAcceptingEvents(acceptsInput);
	if (!acceptsInput) {
		io.ClearEventsQueue();
		io.ClearInputKeys();
		io.ClearInputMouse();
		io.WantSetMousePos = false;
		ImGui::ClearActiveID();
	}

	// Before the skip check so the editor's close edge is never missed.
	auto* editorWindow = EditorWindow::GetSingleton();
	if (editorWindow->open && !EditorWindow::CanBeOpen()) {
		editorWindow->open = false;
		if (editorWindow->IsInPreviewMode())
			editorWindow->ExitPreviewMode();
	}
	editorWindow->UpdateOpenState();
	menu.UpdateOpenFade(io.DeltaTime);

	if (ShouldSkipRendering()) {
		EditorWindow::GetSingleton()->FinishGameHourSliderFrame(false);
		io.ClearInputKeys();
		io.ClearEventsQueue();
		EditorWindow::GetSingleton()->UpdateOpenState();
		return;
	}

	HandleFontReload(menu, cachedFontSize, currentFontSize);
	InitializeImGuiFrame(menu);

	RenderShaderCompilationStatus(keyIdToString);
	RenderShaderBlockingStatus();

<<<<<<< HEAD
	auto* editorWindow = EditorWindow::GetSingleton();
	if (editorWindow->open && !EditorWindow::CanBeOpen()) {
		editorWindow->open = false;
		if (editorWindow->IsInPreviewMode())
			editorWindow->ExitPreviewMode();
	}
	editorWindow->UpdateOpenState();
#if defined(ENABLE_EFFECTS11)
||||||| 12b261724
	auto* editorWindow = EditorWindow::GetSingleton();
	if (editorWindow->open && !EditorWindow::CanBeOpen()) {
		editorWindow->open = false;
		if (editorWindow->IsInPreviewMode())
			editorWindow->ExitPreviewMode();
	}
	editorWindow->UpdateOpenState();

	// The Effects 11 editor, the CS Editor and the Community Shaders menu are exclusive
=======
	// The Effects 11 editor and the Community Shaders menu are exclusive; the CS Editor hosts it.
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
	auto& effects11Editor = Effects11Editor::GetSingleton();
	if (effects11Editor.IsOpen() && menu.IsOpen())
		effects11Editor.Close(false);
#endif
	if (editorWindow->open) {
		bool flying = editorWindow->IsPreviewFlying();
		io.MouseDrawCursor = !flying;
		if (flying)
			io.MousePos = { -FLT_MAX, -FLT_MAX };  // prevent hover/tooltips during active flying
		editorWindow->Draw();
#if defined(ENABLE_EFFECTS11)
	} else if (effects11Editor.IsOpen()) {
		io.MouseDrawCursor = true;
		effects11Editor.Draw();
<<<<<<< HEAD
#endif
	} else if (menu.IsEnabled || HomePageRenderer::ShouldShowFirstTimeSetup() ||
			   globals::features::vr.HelperRequestsRender()) {
		ImGui::GetIO().MouseDrawCursor = true;
		// Helper-requested render: the helper's in-scene focus model routed
		// focus here. Draw the settings UI even though Menu::IsEnabled hasn't
		// been flipped by the local TAB hotkey. This honors the
		// kClientFlag_RendersOnFocus contract we advertised at registration.
		if (menu.IsEnabled || globals::features::vr.HelperRequestsRender()) {
||||||| 12b261724
	} else if (menu.IsEnabled || HomePageRenderer::ShouldShowFirstTimeSetup()) {
		ImGui::GetIO().MouseDrawCursor = true;
		if (menu.IsEnabled) {
=======
	} else if (menu.IsOpen() || menu.openFade > 0.0f || HomePageRenderer::ShouldShowFirstTimeSetup()) {
		ImGui::GetIO().MouseDrawCursor = menu.IsOpen() || HomePageRenderer::ShouldShowFirstTimeSetup();
		if (menu.IsOpen() || menu.openFade > 0.0f) {
			const auto& context = *ImGui::GetCurrentContext();
			const bool editedBefore = context.ActiveIdHasBeenEditedThisFrame;
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
			drawSettings();
			// Only frames that edited something pay for the check; search boxes edit too, so it compares.
			const bool edited = !editedBefore && context.ActiveIdHasBeenEditedThisFrame;
			if (edited)
				globals::state->CheckCoreSettingsChanged();
			SettingsTabRenderer::CheckThemeChanged(edited || ImGui::IsMouseReleased(ImGuiMouseButton_Left) || context.NavActivateId != 0);
		}
	} else {
		ImGui::GetIO().MouseDrawCursor = false;
	}

	// Enabling a Baseline pack can happen from either the menu or the CS editor, and the answer outlives the page.
	if (editorWindow->open || menu.IsOpen())
		UnifiedPresetCatalog::GetSingleton().DrawBaselineOverlapPrompt();

	// The editor draws the notifications in its own frame; while it is closed they still need a host,
	// e.g. the "cannot open in a loading screen" warning raised by the toggle hotkey.
	if (!editorWindow->open)
		Util::Notifications::Draw();

	RenderFeatureOverlays();
	RenderFirstTimeSetupOverlay();
	HandleABTesting();
	editorWindow->FinishGameHourSliderFrame(true);
	FinalizeImGuiFrame();
}

bool OverlayRenderer::ShouldSkipRendering()
{
#if defined(ENABLE_EFFECTS11)
	if (Effects11Editor::GetSingleton().IsOpen())
		return false;
#endif
	auto shaderCache = globals::shaderCache;
	auto failed = shaderCache->GetCurrentFailedCount();
	auto hide = shaderCache->IsHideErrors();
	auto* abTestingManager = ABTestingManager::GetSingleton();
	auto* renderDoc = RenderDoc::GetSingleton();

<<<<<<< HEAD
#if defined(ENABLE_EFFECTS11)
	uint32_t effectFailed = EffectManager::GetSingleton().IsInitialized() ? EffectManager::GetSingleton().GetFailedEffectCount() : 0;
#else
	uint32_t effectFailed = 0;
#endif
||||||| 12b261724
	uint32_t effectFailed = EffectManager::GetSingleton().IsInitialized() ? EffectManager::GetSingleton().GetFailedEffectCount() : 0;
=======
	const uint32_t effectFailed = GetFailedEffectCount();
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb

	return !(shaderCache->IsCompiling() ||
			 Menu::GetSingleton()->IsOpen() ||
			 Menu::GetSingleton()->openFade > 0.0f ||
			 HomePageRenderer::ShouldShowFirstTimeSetup() ||
			 EditorWindow::GetSingleton()->open ||
<<<<<<< HEAD
||||||| 12b261724
			 Effects11Editor::GetSingleton().IsOpen() ||
=======
			 Util::Notifications::HasAny() ||
			 Effects11Editor::GetSingleton().IsOpen() ||
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
			 abTestingManager->IsEnabled() ||
			 (failed && !hide) ||
			 effectFailed ||
			 globals::features::performanceOverlay.settings.ShowInOverlay ||
<<<<<<< HEAD
			 globals::features::sceneSelector.IsOverlayVisible() ||
			 renderDoc->IsAvailable() ||
			 globals::features::vr.HelperRequestsRender());
||||||| 12b261724
			 renderDoc->IsAvailable());
=======
			 globals::features::performanceOverlay.settings.ShowWeatherPicker ||
			 renderDoc->ShouldShowOverlay());
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
}

void OverlayRenderer::HandleFontReload(Menu& menu, float& cachedFontSize, float currentFontSize)
{
	bool fontSizeChanged = std::abs(cachedFontSize - currentFontSize) > ThemeManager::Constants::FONT_CACHE_EPSILON;
	std::string desiredSignature = menu.BuildFontSignature(currentFontSize);
	bool signatureChanged = desiredSignature != MenuFonts::GetAtlasState().cachedFontSignature;

	if (fontSizeChanged || signatureChanged) {
		if (!ThemeManager::ReloadFont(menu, cachedFontSize)) {
			logger::warn("OverlayRenderer::HandleFontReload() - Font reload failed");
		}
	}
}

bool OverlayRenderer::ApplyVRPanelDisplaySize()
{
	uint32_t panelW = 0, panelH = 0;
	if (!globals::game::isVR || !globals::features::vr.GetHelperPanelSize(panelW, panelH))
		return false;

	// VR: canvas must equal the helper panel's pixel size 1:1, or wand
	// clicks (mapped via the same DisplaySize-based UV) drift from the
	// resized content. The desktop mirror shares this size and can clip on
	// a differing swapchain resolution; pre-existing, not a regression.
	auto& io = ImGui::GetIO();
	io.DisplaySize = ImVec2(static_cast<float>(panelW), static_cast<float>(panelH));
	io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
	return true;
}

void OverlayRenderer::InitializeImGuiFrame(Menu& menu)
{
	// Start the Dear ImGui frame
	ImGui_ImplDX11_NewFrame();
	ImGui_ImplWin32_NewFrame();

	// ImGui_ImplWin32_NewFrame() above overwrites DisplaySize from the window
	// rect, so the panel size must be re-applied here before ImGui::NewFrame.
	const bool vrPanel = ApplyVRPanelDisplaySize();
	if (!vrPanel) {
		DXGI_SWAP_CHAIN_DESC desc{};
		globals::d3d::swapChain->GetDesc(&desc);
		const float displayW = static_cast<float>(desc.BufferDesc.Width);
		const float displayH = static_cast<float>(desc.BufferDesc.Height);
		Util::UpdateImGuiInput(desc.OutputWindow, displayW, displayH);
	}
	// The wand drives the cursor in VR (PumpInput), so skip the desktop
	// cursor injection above for the panel case, since it would fight the wand position.

	ImGui::NewFrame();

	// Detect display size change to reset window layout. The VR canvas size
	// is not invariant (it tracks panel resolution/supersampling), which is
	// exactly the kind of change this catches.
	const float2 currentDisplaySize{ ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y };
	if (menu.lastDisplaySize.x > 0.f && menu.lastDisplaySize != currentDisplaySize) {
		logger::info("Display size changed: {}x{} -> {}x{}, resetting window layout",
			menu.lastDisplaySize.x, menu.lastDisplaySize.y, currentDisplaySize.x, currentDisplaySize.y);
		menu.resetLayout = true;
		EditorWindow::GetSingleton()->resetLayout = true;
#if defined(ENABLE_EFFECTS11)
		Effects11Editor::GetSingleton().RequestLayoutReset();
#endif
		globals::features::performanceOverlay.ResetWindowLayout();
		globals::features::sceneSelector.ResetWindowLayout();
	}
	menu.lastDisplaySize = currentDisplaySize;

	ThemeManager::SetupImGuiStyle(menu);
}

void OverlayRenderer::RenderShaderCompilationStatus(const std::function<const char*(std::vector<InputCombo>)>& keyIdToString)
{
	auto shaderCache = globals::shaderCache;
	auto failed = shaderCache->GetCurrentFailedCount();
	auto hide = shaderCache->IsHideErrors();

	const float scale = Util::GetUIScale();
	const float pos = ThemeManager::Constants::OVERLAY_WINDOW_POSITION * scale;

	uint64_t totalShaders = shaderCache->GetTotalTasks();
	uint64_t compiledShaders = shaderCache->GetCompletedTasks();

	auto state = globals::state;
	auto& themeSettings = Menu::GetSingleton()->GetTheme();
	auto* renderDoc = RenderDoc::GetSingleton();
	bool renderDocAvailable = renderDoc->ShouldShowOverlay();
	// Runs every frame, so only build the strings a visible card shows
	const auto renderDocInformation = renderDocAvailable ? renderDoc->GetOverlayWarningMessage() : std::string();

<<<<<<< HEAD
	std::string compilePrefix = shaderCache->backgroundCompilation ? T("overlay.background_prefix", "Background ") : "";
	std::string shaderStats = shaderCache->GetShaderStatsString(!state->IsDeveloperMode());
	auto progressTitle = std::vformat(T("overlay.compiling_shaders", "{}Compiling Shaders: {}"),
		std::make_format_args(compilePrefix, shaderStats));
	auto percent = (float)compiledShaders / (float)totalShaders;
	auto progressOverlay = fmt::format("{}/{} ({:2.1f}%)", compiledShaders, totalShaders, 100 * percent);
||||||| 12b261724
	auto progressTitle = fmt::format("{}Compiling Shaders: {}",
		shaderCache->backgroundCompilation ? "Background " : "",
		shaderCache->GetShaderStatsString(!state->IsDeveloperMode()).c_str());
	auto percent = (float)compiledShaders / (float)totalShaders;
	auto progressOverlay = fmt::format("{}/{} ({:2.1f}%)", compiledShaders, totalShaders, 100 * percent);
=======
	const uint32_t effectFailed = GetFailedEffectCount();
	const bool showFailures = (failed && !hide) || effectFailed;
	const bool compiling = shaderCache->IsCompiling();
	if (!compiling && !showFailures && !renderDocAvailable)
		return;
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb

<<<<<<< HEAD
#if defined(ENABLE_EFFECTS11)
	uint32_t effectFailed = EffectManager::GetSingleton().IsInitialized() ? EffectManager::GetSingleton().GetFailedEffectCount() : 0;
#else
	uint32_t effectFailed = 0;
#endif
||||||| 12b261724
	uint32_t effectFailed = EffectManager::GetSingleton().IsInitialized() ? EffectManager::GetSingleton().GetFailedEffectCount() : 0;
=======
	// Card accent: errors win, then the RenderDoc warning, else plain progress
	const ImVec4 accent = showFailures       ? themeSettings.StatusPalette.Error :
	                      renderDocAvailable ? themeSettings.StatusPalette.Warning :
	                                           themeSettings.StatusPalette.InfoColor;

	if (!BeginOverlayCard(ImVec2(pos, pos))) {
		EndOverlayCard(accent);
		return;
	}

	if (compiling) {
		const std::unordered_map<std::string, std::string> statsArg{ { "stats", shaderCache->GetShaderStatsString(!state->IsDeveloperMode()) } };
		const auto progressTitle = shaderCache->backgroundCompilation ?
		                               I18n::GetSingleton()->Format("overlay.compiling_shaders_background", statsArg, "Background Compiling Shaders: {stats}") :
		                               I18n::GetSingleton()->Format("overlay.compiling_shaders", statsArg, "Compiling Shaders: {stats}");
		auto percent = (float)compiledShaders / (float)totalShaders;
		auto progressOverlay = fmt::format("{}/{} ({:2.1f}%)", compiledShaders, totalShaders, 100 * percent);
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb

<<<<<<< HEAD
	if (shaderCache->IsCompiling()) {
		// VR immersion: suppress only the routine background-compile readout; the
		// blocking-compile path and anything exceptional still show below.
		const bool hideRoutineHud = globals::game::isVR && shaderCache->backgroundCompilation &&
		                            Menu::GetSingleton()->GetSettings().HideCompilationHUDInVR;
		const bool hasExceptionalInfo = shaderCache->IsDiskCacheHeld() || shaderCache->HasFeatureSetChanges() || FeatureIssues::CountLoadFailures() > 0 ||
		                                (failed && !hide) || effectFailed || renderDocAvailable || state->IsDeveloperMode();
		if (hideRoutineHud && !hasExceptionalInfo)
			return;

		ImGui::SetNextWindowPos(ImVec2(pos, pos));
		if (!ImGui::Begin("ShaderCompilationInfo", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
			ImGui::End();
			return;
		}
		if (!hideRoutineHud) {
			ImGui::TextUnformatted(progressTitle.c_str());
			ImGui::ProgressBar(percent, ImVec2(0.0f, 0.0f), progressOverlay.c_str());
		}
		if (shaderCache->HasFeatureSetRevertPending()) {
			ImGui::TextColored(themeSettings.StatusPalette.Warning, "%s",
				T("overlay.revert_pending",
					"Previous cache restored.\n"
					"Restart to use it."));
		} else if (shaderCache->HasFeatureSetChanges()) {
			if (shaderCache->HasFeatureSetCacheBackup()) {
				ImGui::TextColored(themeSettings.StatusPalette.Warning, "%s",
					T("overlay.feature_changed_backup",
						"Feature setup changed. Building a new shader cache for this setup.\n"
						"Previous cache saved."));
			} else {
				ImGui::TextColored(themeSettings.StatusPalette.Warning, "%s",
					T("overlay.feature_changed_no_backup",
						"Feature setup changed. Building shaders in memory until the cache can be rebuilt.\n"
						"Previous cache is not available for restore."));
			}
		} else if (shaderCache->IsDiskCacheHeld()) {
			ImGui::TextColored(themeSettings.StatusPalette.Warning, "%s",
				T("overlay.cache_held",
					"Saved shader cache cannot be used.\n"
					"A required feature is missing or failed to load."));
		}
		// Surface bad-install feature problems during the compile itself; otherwise
		// the user only learns from the Feature Issues tab after baking the wrong set.
		if (const size_t issueCount = FeatureIssues::CountLoadFailures()) {
			ImGui::TextColored(themeSettings.StatusPalette.Error,
				T("overlay.feature_issues_compiling",
					"WARNING: %zu feature(s) failed to load (bad install or version mismatch).\n"
					"Quit, fix them in the menu's Feature Issues tab, and restart - compiling now\n"
					"bakes the wrong shaders and you will have to recompile after fixing."),
				issueCount);
		}
||||||| 12b261724
	if (shaderCache->IsCompiling()) {
		ImGui::SetNextWindowPos(ImVec2(pos, pos));
		if (!ImGui::Begin("ShaderCompilationInfo", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
			ImGui::End();
			return;
		}
		ImGui::TextUnformatted(progressTitle.c_str());
		ImGui::ProgressBar(percent, ImVec2(0.0f, 0.0f), progressOverlay.c_str());
=======
		ImGui::TextUnformatted(progressTitle.c_str());
		ImGui::ProgressBar(percent, ImVec2(0.0f, 0.0f), progressOverlay.c_str());
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
		if (state->IsDeveloperMode()) {
			int32_t threadLimit = shaderCache->backgroundCompilation ? shaderCache->backgroundCompilationThreadCount : shaderCache->compilationThreadCount;
			int compilationRunning = (int)shaderCache->compilationPool.get_tasks_running();
			int heavyInFlight = shaderCache->GetHeavyTasksInFlight();
			int heavyLimit = static_cast<int>(Util::GetPerformanceCoreCount());
			uint64_t slow = shaderCache->GetSlowTasks();
			uint64_t verySlow = shaderCache->GetVerySlowTasks();
<<<<<<< HEAD
			ImGui::Text(T("overlay.threads_status", "Threads: %d / %d limit | Heavy: %d / %d P-cores | %d workers"),
||||||| 12b261724
			ImGui::Text("Threads: %d / %d limit | Heavy: %d / %d P-cores | %d workers",
=======
			ImGui::Text(T("overlay.compile_threads", "Threads: %d / %d limit | Heavy: %d / %d P-cores | %d workers"),
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
				compilationRunning,
				threadLimit,
				heavyInFlight,
				heavyLimit,
				(int)shaderCache->compilationPool.get_thread_count());
			if (slow > 0) {
				ImGui::Text(T("overlay.slow_shaders", "Slow shaders: %llu (very slow: %llu)"), slow, verySlow);
			}
		}
		if (!shaderCache->backgroundCompilation && shaderCache->menuLoaded) {
<<<<<<< HEAD
			const char* skipKeyName = keyIdToString(Menu::GetSingleton()->GetSettings().SkipCompilationKey);
			auto skipShadersText = std::vformat(
				T("overlay.skip_compilation", "Press {} to proceed without completing shader compilation. "),
				std::make_format_args(skipKeyName));
||||||| 12b261724
			auto skipShadersText = fmt::format(
				"Press {} to proceed without completing shader compilation. ",
				keyIdToString(Menu::GetSingleton()->GetSettings().SkipCompilationKey));
=======
			const auto skipShadersText = I18n::GetSingleton()->Format("overlay.skip_compilation",
				{ { "key", keyIdToString(Menu::GetSingleton()->GetSettings().SkipCompilationKey) } },
				"Press {key} to proceed without completing shader compilation.");
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
			ImGui::TextUnformatted(skipShadersText.c_str());
			ImGui::TextUnformatted(T("overlay.uncompiled_warning", "WARNING: Uncompiled shaders will have visual errors or cause stuttering when loading."));
		}
<<<<<<< HEAD
		if (failed && !hide)
			DrawShaderCompilationFailures(failed, themeSettings);
#if defined(ENABLE_EFFECTS11)
		DrawEffects11Errors(themeSettings);
#endif

		if (renderDocAvailable)
			ImGui::TextColored(themeSettings.StatusPalette.Warning, "%s", renderDocInformation.c_str());

		ImGui::End();
		return;
||||||| 12b261724
		if (failed && !hide)
			DrawShaderCompilationFailures(failed, themeSettings);
		DrawEffects11Errors(themeSettings);

		if (renderDocAvailable)
			ImGui::TextColored(themeSettings.StatusPalette.Warning, "%s", renderDocInformation.c_str());

		ImGui::End();
		return;
=======
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
	}

<<<<<<< HEAD
	if ((failed && !hide) || effectFailed) {
		ImGui::SetNextWindowPos(ImVec2(pos, pos));
		if (!ImGui::Begin("ShaderCompilationInfo", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
			ImGui::End();
			return;
		}

		DrawShaderCompilationFailures(failed, themeSettings);
#if defined(ENABLE_EFFECTS11)
||||||| 12b261724
	if ((failed && !hide) || effectFailed) {
		ImGui::SetNextWindowPos(ImVec2(pos, pos));
		if (!ImGui::Begin("ShaderCompilationInfo", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
			ImGui::End();
			return;
		}

		DrawShaderCompilationFailures(failed, themeSettings);
=======
	if (showFailures) {
		DrawShaderCompilationFailures(hide ? 0 : failed, themeSettings);
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
		DrawEffects11Errors(themeSettings);
<<<<<<< HEAD
#endif
||||||| 12b261724
=======
	}
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb

	if (renderDocAvailable)
		ImGui::TextColored(themeSettings.StatusPalette.Warning, "%s", renderDocInformation.c_str());

	EndOverlayCard(accent);
}

void OverlayRenderer::RenderFeatureOverlays()
{
	// load overlays
	for (Feature* feat : Feature::GetFeatureList()) {
		if (feat && feat->loaded) {
			if (auto* overlay = dynamic_cast<OverlayFeature*>(feat)) {
				overlay->DrawOverlay();
			}
		}
	}
}

void OverlayRenderer::HandleABTesting()
{
	// A/B Testing management
	auto* abTestingManager = ABTestingManager::GetSingleton();
	abTestingManager->Update();

	// Always update test data during TEST phase, regardless of overlay visibility
	if (abTestingManager->IsEnabled()) {
		globals::features::performanceOverlay.UpdateAllShaderTestData();

		// Add A/B test aggregator data collection here
		auto& overlay = globals::features::performanceOverlay;
		auto [mainRows, summaryRows] = overlay.BuildDrawCallRows();
		std::vector<DrawCallRow> allRows = mainRows;
		allRows.insert(allRows.end(), summaryRows.begin(), summaryRows.end());

		// Update the A/B test aggregator with current frame data
		abTestingManager->RecordFrame(allRows);
	}

	// Draw A/B testing overlay
	abTestingManager->DrawOverlayUI();
}

void OverlayRenderer::FinalizeImGuiFrame()
{
	if (auto* menu = Menu::GetSingleton();
		menu && menu->GetSettings().Theme.UseCustomCursor && Util::CursorLoader::GetLoadedCount() > 0) {
		Util::CursorLoader::DrawCustomCursor(*menu);
	}

	ImGui::Render();

<<<<<<< HEAD
	auto* drawData = ImGui::GetDrawData();
	if (!RenderDesktopMirror(drawData) && !BackgroundBlur::RenderDrawData(drawData))
		ImGui_ImplDX11_RenderDrawData(drawData);
||||||| 12b261724
	// Apply background blur behind ImGui windows before rendering them
	BackgroundBlur::RenderBackgroundBlur();
=======
	BackgroundBlur::SetFade(GetBlurFade());

	// Apply background blur behind ImGui windows before rendering them
	BackgroundBlur::RenderBackgroundBlur();
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb

	// Render the same draw data into the ImGuiVRHelper's panel RTV so the
	// helper can composite our menu as a 3D quad in the HMD. The helper owns
	// VR overlay compositing — Community Shaders no longer submits its own.
	globals::features::vr.RenderHelperToPanel();
}

bool OverlayRenderer::RenderDesktopMirror(ImDrawData* drawData)
{
	pendingDesktopMirror.Reset();
	pendingDesktopMirrorSource = nullptr;
	uint32_t panelWidth = 0;
	uint32_t panelHeight = 0;
	const bool helperPanelActive = globals::game::isVR && globals::features::vr.GetHelperPanelSize(panelWidth, panelHeight);
	if (!helperPanelActive || !drawData || !globals::d3d::swapChain || !globals::features::vr.settings.FitMenuToDesktop)
		return false;
	if (globals::features::upscaling.d3d12SwapChainActive ||
		(globals::features::hdrDisplay.loaded && globals::features::hdrDisplay.settings.enableHDR))
		return false;

	DXGI_SWAP_CHAIN_DESC swapChainDesc{};
	if (FAILED(globals::d3d::swapChain->GetDesc(&swapChainDesc)))
		return false;
	if (!IsSupportedDesktopMirrorFormat(swapChainDesc.BufferDesc.Format))
		return false;

	const ImVec2 desktopSize(
		static_cast<float>(swapChainDesc.BufferDesc.Width),
		static_cast<float>(swapChainDesc.BufferDesc.Height));
	const double sourceWidth = static_cast<double>(drawData->DisplaySize.x) * drawData->FramebufferScale.x;
	const double sourceHeight = static_cast<double>(drawData->DisplaySize.y) * drawData->FramebufferScale.y;
	const bool sourceCanvasValid = std::isfinite(sourceWidth) && std::isfinite(sourceHeight) && sourceWidth > 0.0 && sourceHeight > 0.0;
	const bool dimensionsDiffer = !sourceCanvasValid || std::abs(desktopSize.x - sourceWidth) > 0.5 || std::abs(desktopSize.y - sourceHeight) > 0.5;
	if (!dimensionsDiffer || BackgroundBlur::HasHDRSceneImageCallback(drawData))
		return false;

	// Fit the helper-sized canvas for the desktop target; keep the original for the HMD panel.
	if (!pendingDesktopMirror.Build(*drawData, desktopSize))
		return false;

	pendingDesktopMirrorSource = drawData;
	return true;
}

void OverlayRenderer::RenderDesktopMirrorAtPresent(IDXGISwapChain* swapChain, const bool hdrReady, const bool frameGenActive)
{
	const auto clearPending = []() {
		pendingDesktopMirror.Reset();
		pendingDesktopMirrorSource = nullptr;
	};
	if (!pendingDesktopMirror.Get())
		return;
	const SKSE::stl::scope_exit consumePending([&]() noexcept {
		pendingDesktopMirror.Reset();
		pendingDesktopMirrorSource = nullptr;
	});
	if (!swapChain || !pendingDesktopMirrorSource || hdrReady || frameGenActive || globals::features::upscaling.d3d12SwapChainActive ||
		(globals::features::hdrDisplay.loaded && globals::features::hdrDisplay.settings.enableHDR)) {
		clearPending();
		return;
	}
	if (!globals::d3d::device || !globals::d3d::context) {
		clearPending();
		return;
	}

	DXGI_SWAP_CHAIN_DESC swapChainDesc{};
	if (FAILED(swapChain->GetDesc(&swapChainDesc))) {
		clearPending();
		return;
	}
	UINT bufferIndex = 0;
	winrt::com_ptr<IDXGISwapChain3> swapChain3;
	if (SUCCEEDED(swapChain->QueryInterface(IID_PPV_ARGS(swapChain3.put())))) {
		bufferIndex = swapChain3->GetCurrentBackBufferIndex();
	} else if (swapChainDesc.BufferCount > 1 && swapChainDesc.SwapEffect != DXGI_SWAP_EFFECT_DISCARD) {
		clearPending();
		return;
	}

	winrt::com_ptr<ID3D11Texture2D> backBuffer;
	if (FAILED(swapChain->GetBuffer(bufferIndex, IID_PPV_ARGS(backBuffer.put()))) || !backBuffer) {
		clearPending();
		return;
	}
	D3D11_TEXTURE2D_DESC backBufferDesc{};
	backBuffer->GetDesc(&backBufferDesc);
	auto* mirrorDrawData = pendingDesktopMirror.Get();
	const bool fitsMirror = mirrorDrawData && IsSupportedDesktopMirrorFormat(backBufferDesc.Format) && backBufferDesc.SampleDesc.Count == 1 &&
	                        static_cast<float>(backBufferDesc.Width) == mirrorDrawData->DisplaySize.x &&
	                        static_cast<float>(backBufferDesc.Height) == mirrorDrawData->DisplaySize.y;

	winrt::com_ptr<ID3D11RenderTargetView> backBufferRTV;
	if (FAILED(globals::d3d::device->CreateRenderTargetView(backBuffer.get(), nullptr, backBufferRTV.put()))) {
		clearPending();
		return;
	}
	Util::SetResourceName(backBufferRTV.get(), "OverlayRenderer::DesktopMirror RTV");

	auto context = globals::d3d::context;
	std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> rawPreviousRTVs{};
	std::array<winrt::com_ptr<ID3D11RenderTargetView>, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> previousRTVs{};
	winrt::com_ptr<ID3D11DepthStencilView> previousDSV;
	context->OMGetRenderTargets(static_cast<UINT>(rawPreviousRTVs.size()), rawPreviousRTVs.data(), previousDSV.put());
	for (size_t i = 0; i < rawPreviousRTVs.size(); ++i)
		previousRTVs[i].attach(rawPreviousRTVs[i]);
	D3D11_VIEWPORT previousViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
	UINT previousViewportCount = ARRAYSIZE(previousViewports);
	context->RSGetViewports(&previousViewportCount, previousViewports);
	D3D11_RECT previousScissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
	UINT previousScissorCount = ARRAYSIZE(previousScissors);
	context->RSGetScissorRects(&previousScissorCount, previousScissors);
	const SKSE::stl::scope_exit restoreOutput([&]() noexcept {
		std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> targets{};
		for (size_t i = 0; i < previousRTVs.size(); ++i)
			targets[i] = previousRTVs[i].get();
		context->OMSetRenderTargets(static_cast<UINT>(targets.size()), targets.data(), previousDSV.get());
		context->RSSetViewports(previousViewportCount, previousViewports);
		context->RSSetScissorRects(previousScissorCount, previousScissors);
	});

	auto* target = backBufferRTV.get();
	context->OMSetRenderTargets(1, &target, nullptr);
	const D3D11_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(backBufferDesc.Width), static_cast<float>(backBufferDesc.Height), 0.0f, 1.0f };
	context->RSSetViewports(1, &viewport);
	const D3D11_RECT scissor{ 0, 0, static_cast<LONG>(backBufferDesc.Width), static_cast<LONG>(backBufferDesc.Height) };
	context->RSSetScissorRects(1, &scissor);

	CS_GPU_PASS("OverlayRenderer::DesktopMirror");
	if (!fitsMirror) {
		ImGui_ImplDX11_RenderDrawData(pendingDesktopMirrorSource);
		return;
	}
	if (!BackgroundBlur::RenderDrawDataForDesktopMirror(mirrorDrawData, *pendingDesktopMirrorSource))
		ImGui_ImplDX11_RenderDrawData(mirrorDrawData);
	clearPending();
}

void OverlayRenderer::RenderFirstTimeSetupOverlay()
{
	if (HomePageRenderer::ShouldShowFirstTimeSetup()) {
		HomePageRenderer::RenderFirstTimeSetupDialog();
	}
}

void OverlayRenderer::RenderShaderBlockingStatus()
{
	auto shaderCache = globals::shaderCache;
	auto state = globals::state;

	if (!state->IsDeveloperMode() || shaderCache->blockedKey.empty()) {
		return;
	}

	const float scale = Util::GetUIScale();
	const float pos = ThemeManager::Constants::OVERLAY_WINDOW_POSITION * scale;

	// Stack below shader compilation window if visible
	float yPos = pos;
	if (auto* shaderWin = ImGui::FindWindowByName("ShaderCompilationInfo")) {
		if (shaderWin->Active) {
			yPos = shaderWin->Pos.y + shaderWin->Size.y + ImGui::GetStyle().ItemSpacing.y;
		}
	}
	// Also stack below water cache overlay if visible
	if (auto* waterWin = ImGui::FindWindowByName("UWCacheCreationInfo")) {
		if (waterWin->Active && waterWin->Pos.y + waterWin->Size.y > yPos) {
			yPos = waterWin->Pos.y + waterWin->Size.y + ImGui::GetStyle().ItemSpacing.y;
		}
	}
	ImGui::SetNextWindowPos(ImVec2(pos, yPos));
	if (!ImGui::Begin("ShaderBlockingInfo", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
		ImGui::End();
		return;
	}

<<<<<<< HEAD
	Util::Text::Error("%s", T("overlay.shader_blocking_active", "Shader Blocking Active"));
	ImGui::Text(T("overlay.blocked_key", "Blocked: %s"), shaderCache->blockedKey.c_str());
||||||| 12b261724
	Util::Text::Error(T("overlay.shader_blocking_active", "Shader Blocking Active"));
	ImGui::Text("Blocked: %s", shaderCache->blockedKey.c_str());
=======
	Util::Text::Error(T("overlay.shader_blocking_active", "Shader Blocking Active"));
	ImGui::Text(T("overlay.blocked_shader", "Blocked: %s"), shaderCache->blockedKey.c_str());
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb

	// Try to get more details from active shaders
	auto activeShaders = shaderCache->GetActiveShaders();

	// Find the index of the blocked shader in the active list (or show N/A if not found)
	size_t blockedIndex = 0;
	bool foundBlocked = false;
	for (size_t i = 0; i < activeShaders.size(); ++i) {
		if (activeShaders[i].key == shaderCache->blockedKey) {
			blockedIndex = i + 1;  // 1-based indexing for display
			foundBlocked = true;
			break;
		}
	}

	if (foundBlocked) {
		ImGui::Text(T("overlay.blocked_index", "Index: %zu/%zu"), blockedIndex, activeShaders.size());
	} else {
<<<<<<< HEAD
		ImGui::Text(T("overlay.blocked_index_na", "Index: N/A (%zu active)"), activeShaders.size());
||||||| 12b261724
		ImGui::Text("Index: N/A (%zu active)", activeShaders.size());
=======
		ImGui::Text(T("overlay.blocked_index_unknown", "Index: N/A (%zu active)"), activeShaders.size());
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
	}

	for (const auto& shader : activeShaders) {
		if (shader.key == shaderCache->blockedKey) {
<<<<<<< HEAD
			ImGui::Text(T("overlay.blocked_shader_detail", "Type: %s | Class: %s | Descriptor: 0x%X"),
||||||| 12b261724
			ImGui::Text("Type: %s | Class: %s | Descriptor: 0x%X",
=======
			ImGui::Text(T("overlay.blocked_details", "Type: %s | Class: %s | Descriptor: 0x%X"),
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
				magic_enum::enum_name(shader.shaderType).data(),
				magic_enum::enum_name(shader.shaderClass).data(),
				shader.descriptor);
			break;
		}
	}

	ImGui::End();
}

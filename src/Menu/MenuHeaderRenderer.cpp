#include "MenuHeaderRenderer.h"
#include "Menu/IconLoader.h"

#include <imgui.h>
#include <imgui_internal.h>

#include "Features/Upscaling.h"
#include "Fonts.h"
#include "Globals.h"
#include "I18n/I18n.h"
#include "IconsTabler.h"
#include "Menu.h"
#include "Menu/FeatureListRenderer.h"
#include "Menu/GoToPalette.h"
#include "Menu/HomePageRenderer.h"
#include "Menu/IssuesPageRenderer.h"
#include "Menu/MenuIcons.h"
#include "Menu/MenuLinks.h"
#include "Plugin.h"
#include "SaveManager.h"
#include "ShaderCache.h"
#include "State.h"
#include "ThemeManager.h"
#include "Util.h"
#include "Utils/UIAnim.h"

namespace
{
	using RoleFontGuard = MenuFonts::FontRoleGuard;

	/** @brief Icon tint for the header's glyph buttons: the theme text colour in monochrome mode, otherwise white. */
	ImVec4 BaseIconTint()
	{
		const auto& theme = globals::menu->GetSettings().Theme;
		return theme.UseMonochromeIcons ? theme.Palette.Text : ImVec4(1, 1, 1, 1);
	}

	/** @brief Draws an icon glyph centred in a square, sized to match the texture icons. */
	void DrawIconGlyph(ImDrawList* drawList, ImVec2 min, float size, Icons::GlyphRef glyph, ImU32 color)
	{
		Icons::DrawCenteredGlyph(drawList, min, ImVec2(size, size), size * ThemeManager::Constants::HEADER_GLYPH_ICON_SCALE, glyph, color);
	}

	/** @brief Draw list for the docked header buttons: the tab bar's host window, so popups and tooltips stay above them. */
	ImDrawList* DockedHeaderDrawList()
	{
		const ImGuiDockNode* node = ImGui::GetCurrentWindowRead()->DockNode;
		return node && node->HostWindow ? node->HostWindow->DrawList : ImGui::GetForegroundDrawList();
	}

	/** @brief Hit test for a docked header button, which is not an ImGui item: inside this pane and not under a popup or another window. */
	bool IsDockedHeaderButtonHovered(const ImRect& rect)
	{
		const ImGuiWindow* window = ImGui::GetCurrentWindowRead();
		const ImGuiWindow* hovered = ImGui::GetCurrentContext()->HoveredWindow;
		return hovered && hovered->RootWindowDockTree == window->RootWindowDockTree && window->Rect().Contains(ImGui::GetIO().MousePos) &&
		       ImGui::IsMouseHoveringRect(rect.Min, rect.Max, false);
	}
}

<<<<<<< HEAD
void MenuHeaderRenderer::RenderHeader(bool isDocked, bool showLogo, bool canShowIcons, float uiScale, const Menu::UIIcons& uiIcons, bool& sidebarVisible)
||||||| 12b261724
void MenuHeaderRenderer::RenderHeader(bool isDocked, bool showLogo, bool canShowIcons, float uiScale, const Menu::UIIcons& uiIcons)
=======
bool MenuHeaderRenderer::GlyphIconButton(const char* id, Icons::GlyphRef glyph, float glyphBox, const char* tooltip, bool active)
{
	// Square like the nav buttons: the vertical padding on both axes
	const float pad = ImGui::GetStyle().FramePadding.y;
	const ImVec2 buttonSize(glyphBox + pad * 2.0f, glyphBox + pad * 2.0f);

	ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
	// Clear at rest, except the current page's button, which keeps the hover fill
	ImGui::PushStyleColor(ImGuiCol_Button, active ? ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered) : ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
	const bool clicked = Util::Button(id, buttonSize);
	ImGui::PopStyleColor();
	ImGui::PopStyleVar();
	const ImRect rect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

	const ImVec4 tint = active ? Util::Colors::GetAccent() : BaseIconTint();
	DrawIconGlyph(ImGui::GetWindowDrawList(), ImVec2(rect.Min.x + pad, rect.Min.y + pad), glyphBox, glyph, ImGui::GetColorU32(tint));
	if (tooltip) {
		Util::HoverTip(tooltip);
	}
	return clicked;
}

bool MenuHeaderRenderer::RenderNavButton(bool docked, ImVec2 min, float size, const char* id, Icons::GlyphRef glyph, const char* tooltip,
	bool current, const ImVec4* tintOverride, const std::function<void()>& onClick)
{
	const ImRect rect(min, ImVec2(min.x + size, min.y + size));

	bool hovered;
	bool down;
	bool clicked;
	if (docked) {
		hovered = IsDockedHeaderButtonHovered(rect);
		down = hovered && ImGui::IsMouseDown(ImGuiMouseButton_Left);
		clicked = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
	} else {
		const ImVec2 saved = ImGui::GetCursorScreenPos();
		ImGui::SetCursorScreenPos(min);
		clicked = ImGui::InvisibleButton(id, ImVec2(size, size));
		hovered = ImGui::IsItemHovered();
		down = ImGui::IsItemActive();
		ImGui::SetCursorScreenPos(saved);
	}

	ImDrawList* drawList = docked ? DockedHeaderDrawList() : ImGui::GetWindowDrawList();
	Util::DrawAnimatedButtonHighlight(rect, ImGui::GetID(id), hovered, down, current, drawList);

	ImVec4 tint = BaseIconTint();
	if (tintOverride)
		tint = *tintOverride;
	else if (current)
		tint = Util::Colors::GetAccent();
	if (!current && !hovered)
		tint.w *= ThemeManager::Constants::HEADER_ICON_IDLE_ALPHA;
	if (tintOverride)  // The issues icon breathes gently to draw the eye
		tint.w *= 1.0f - ThemeManager::Constants::ANIM_PULSE_AMPLITUDE * Util::Anim::Pulse();
	const float pad = ThemeManager::Constants::DOCKED_ICON_PADDING_REDUCTION;
	DrawIconGlyph(drawList, ImVec2(min.x + pad, min.y + pad), size - pad * 2.0f, glyph, ImGui::GetColorU32(tint));

	if (hovered)
		ImGui::SetTooltip("%s", tooltip);
	if (clicked)
		onClick();
	return clicked;
}

float MenuHeaderRenderer::RenderNavButtons(bool docked, ImVec2 min, float size, float gap)
{
	float x = min.x;
	RenderNavButton(docked, ImVec2(x, min.y), size, "##HeaderHome", Icons::TI(MenuIcons::kHome), T("menu.header.home_tooltip", "Home"),
		HomePageRenderer::IsShowing() && !HomePageRenderer::IsAttentionFocused(), nullptr,
		[]() { globals::menu->SelectFeatureMenu(std::string(MenuPageIds::kHome)); });
	x += size + gap;

	if (const auto issues = IssuesPageRenderer::GetFeatureIssueCount()) {
		const ImVec4 error = Util::Colors::GetError();
		const auto tooltip = I18n::GetSingleton()->Format("menu.header.issues_tooltip", { { "count", std::to_string(issues) } },
			"{count} feature issue(s) - click to view");
		RenderNavButton(docked, ImVec2(x, min.y), size, "##HeaderIssues", Icons::TI(MenuIcons::kIssues), tooltip.c_str(), false, &error,
			[]() { HomePageRenderer::FocusNeedsAttention(); });
		x += size + gap;
	}
	return x - min.x;
}

std::string MenuHeaderRenderer::GetTitleText()
{
	return std::format("Community Shaders {}", Util::GetFormattedVersion(Plugin::VERSION));
}

std::string MenuHeaderRenderer::GetBuildTagText()
{
	const auto expectedTag = std::format("v{}", Util::GetFormattedVersion(Plugin::VERSION));
	return Plugin::BUILD_DESCRIBE != expectedTag ? Util::GetBuildLabel() : std::string();
}

std::string MenuHeaderRenderer::GetGameVersionText()
{
	return I18n::GetSingleton()->Format("menu.footer.game_version",
		{ { "runtime", std::string(magic_enum::enum_name(REL::Module::GetRuntime())) },
			{ "version", Util::GetFormattedVersion(REL::Module::get().version()) } },
		"Game Version: {runtime} {version}");
}

std::string MenuHeaderRenderer::GetSwapChainText()
{
	return I18n::GetSingleton()->Format("menu.footer.d3d12_swap_chain",
		{ { "status", globals::features::upscaling.d3d12SwapChainActive ? std::string(T("common.active", "Active")) : std::string(T("common.inactive", "Inactive")) } },
		"D3D12 Swap Chain: {status}");
}

std::string MenuHeaderRenderer::GetGpuText()
{
	return I18n::GetSingleton()->Format("menu.footer.gpu", { { "name", globals::state->adapterDescription } }, "GPU: {name}");
}

void MenuHeaderRenderer::RenderHeader(bool isDocked, bool showLogo, float uiScale, const Util::IconLoader::UIIcons& uiIcons,
	const std::vector<MenuShortcuts::Shortcut>& shortcuts)
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
{
	if (!globals::menu) {
		logger::error("MenuHeaderRenderer::RenderHeader: globals::menu is null, cannot render header");
		return;
	}

<<<<<<< HEAD
	auto versionStr = Util::GetFormattedVersion(Plugin::VERSION);
	auto expectedTag = std::format("v{}", versionStr);
	auto title = Plugin::BUILD_DESCRIBE == expectedTag ? std::format("{} {}", Plugin::DISPLAY_NAME, versionStr) : std::format("{} {} [{}]", Plugin::DISPLAY_NAME, versionStr, Plugin::BUILD_DESCRIBE);
	auto actionIcons = BuildActionIcons(canShowIcons, uiIcons);
	const char* sidebarTooltip = sidebarVisible ? T("menu.hide_sidebar", "Hide Sidebar") : T("menu.show_sidebar", "Show Sidebar");
	const float sidebarIconSize = ImGui::GetFontSize() * ThemeManager::Constants::SIDEBAR_ICON_SIZE_MULTIPLIER * uiScale;
	const ImVec2 sidebarContentSize = uiIcons.sidebar.texture ? ImVec2(sidebarIconSize, sidebarIconSize) : ImGui::CalcTextSize(sidebarTooltip);
	const ImVec2 padding = ImGui::GetStyle().FramePadding;
	const ImVec2 sidebarButtonSize(sidebarContentSize.x + padding.x * 2.0f, sidebarContentSize.y + padding.y * 2.0f);
||||||| 12b261724
	auto versionStr = Util::GetFormattedVersion(Plugin::VERSION);
	auto expectedTag = std::format("v{}", versionStr);
	auto title = Plugin::BUILD_DESCRIBE == expectedTag ? std::format("Community Shaders {}", versionStr) : std::format("Community Shaders {} [{}]", versionStr, Plugin::BUILD_DESCRIBE);
	auto actionIcons = BuildActionIcons(canShowIcons, uiIcons);
=======
	const auto title = GetTitleText();
	auto actionIcons = BuildActionIcons(uiIcons, shortcuts);

	// Dev builds: muted build label after the title
	const auto buildTag = GetBuildTagText();
	const bool showBuildTag = !buildTag.empty();
	const float buildTagWidth = showBuildTag ? ImGui::GetStyle().ItemSpacing.x + ImGui::CalcTextSize(buildTag.c_str()).x : 0.0f;
	float titleInkScreenY = 0.0f;  // Screen y of the title's ink centre; the build label centres on it
	auto drawBuildTag = [&]() {
		if (!showBuildTag)
			return;
		ImGui::SameLine();
		ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, titleInkScreenY - Util::GetTextInkCenterY()));
		ImGui::TextDisabled("%s", buildTag.c_str());
	};
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb

	if (isDocked) {
		// When docked, draw logo as a background watermark if available
		if (showLogo && uiIcons.logo.texture) {
			RenderWatermarkLogo(uiIcons);
		}

		// Buttons that do not fit a narrow pane are clipped, matching their hit test
		ImDrawList* barDrawList = DockedHeaderDrawList();
		const ImRect pane = ImGui::GetCurrentWindowRead()->Rect();
		barDrawList->PushClipRect(pane.Min, pane.Max, true);

		// Draw action icons in the title bar area
<<<<<<< HEAD
		RenderDockedIcons(actionIcons, uiScale);
		RenderSidebarToggle(sidebarButtonSize, uiIcons, sidebarTooltip, sidebarVisible);
||||||| 12b261724
		RenderDockedIcons(actionIcons, uiScale);
=======
		const float iconsLeft = RenderDockedIcons(actionIcons, uiScale);

		const ImVec2 windowPos = ImGui::GetWindowPos();
		const float titleBarHeight = ImGui::GetFrameHeight();
		const float homeSize = GetHeaderIconSize(true, uiScale);
		const float gap = ThemeManager::Constants::DOCKED_ICON_SPACING * uiScale;
		const int navCount = IssuesPageRenderer::GetFeatureIssueCount() ? 3 : 2;  // Home, issues, search
		const float navWidth = (homeSize + gap) * static_cast<float>(navCount);

		// Home and search follow the node's last tab; a trailing spacer keeps the tabs short of the header buttons
		ImGuiWindow* window = ImGui::GetCurrentWindow();
		float tabsRight = window->DC.DockTabItemRect.Max.x;
		if (ImGuiDockNode* node = window->DockNode) {
			for (const ImGuiWindow* tabWindow : node->Windows)
				tabsRight = std::max(tabsRight, tabWindow->DC.DockTabItemRect.Max.x);
			barDrawList->PopClipRect();  // The amend re-enters the host window, which would inherit the pane clip
			if (node->TabBar && ImGui::DockNodeBeginAmendTabBar(node)) {
				ImGui::TabItemSpacing("##HeaderButtons", ImGuiTabItemFlags_Trailing, std::max(0.0f, node->TabBar->BarRect.Max.x - iconsLeft) + navWidth);
				ImGui::DockNodeEndAmendTabBar();
			}
			barDrawList->PushClipRect(pane.Min, pane.Max, true);
		}
		const float homeLeft = std::min(tabsRight + gap, iconsLeft - navWidth);
		float titleRight = homeLeft + RenderNavButtons(true, ImVec2(homeLeft, windowPos.y + (titleBarHeight - homeSize) * 0.5f), homeSize, gap);
		RenderSearchPill(true, titleRight, iconsLeft, windowPos.y, titleBarHeight, uiScale,
			MenuShortcuts::GetHint(shortcuts, MenuShortcuts::Ids::kGoTo));
		barDrawList->PopClipRect();
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
	} else {
		ImGui::BeginGroup();
		const ImVec2 headerOrigin = ImGui::GetCursorScreenPos();
		ImGui::Dummy(sidebarButtonSize);
		ImGui::SameLine();

		// When not docked, show the custom header
		bool centerHeader = globals::menu->GetTheme().CenterHeader;

		if (ImGui::BeginTable("##HeaderLayout", 2, ImGuiTableFlags_SizingStretchProp)) {
			ImGui::TableSetupColumn("Title", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("Buttons", ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableNextColumn();  // Title on the left with logo

			// Determine scaling based on GlobalScale setting and font size
			const float currentFontSize = ImGui::GetFontSize();
			const float baseTextScale = ThemeManager::Constants::HEADER_BASE_TEXT_SCALE;
			const float baseIconSize = currentFontSize * ThemeManager::Constants::HEADER_BASE_ICON_MULTIPLIER;

			// Apply UI scale to the base scaling factors
			const float textScaleFactor = baseTextScale * uiScale;
			const float logoSize = baseIconSize * uiScale;  // Match action icon size

			// Home button at the column's left edge, centred on the title row; the title makes room for it
			const float homeSize = GetHeaderIconSize(false, uiScale);
			const float navGap = ImGui::GetStyle().ItemSpacing.x;
			const size_t navCount = IssuesPageRenderer::GetFeatureIssueCount() > 0 ? 2 : 1;
			const float homeReserve = static_cast<float>(navCount) * (homeSize + navGap);
			const ImVec2 columnStart = ImGui::GetCursorScreenPos();
			const float estimatedRowHeight = std::max(logoSize, ImGui::GetFontSize() * textScaleFactor);
			RenderNavButtons(false, ImVec2(columnStart.x, columnStart.y + (estimatedRowHeight - homeSize) * 0.5f), homeSize, navGap);

			// Width of the title block (logo, text and build label)
			float contentWidth = 0.0f;
			if (showLogo) {
				float logoAspectRatio = uiIcons.logo.size.x / uiIcons.logo.size.y;
				contentWidth = (logoSize * logoAspectRatio) + ImGui::GetStyle().ItemSpacing.x;
			}
			{
				RoleFontGuard titleFont(Menu::FontRole::Title);
				ImGui::SetWindowFontScale(textScaleFactor);
				contentWidth += ImGui::CalcTextSize(title.c_str()).x;
				ImGui::SetWindowFontScale(1.0f);
			}
			contentWidth += buildTagWidth;

			// The search pill follows the nav buttons; the title takes the room that is left
			const float columnWidth = ImGui::GetContentRegionAvail().x;
			const float pillEnd = RenderSearchPill(false, columnStart.x + homeReserve, columnStart.x + columnWidth - contentWidth, columnStart.y,
				estimatedRowHeight, uiScale, MenuShortcuts::GetHint(shortcuts, MenuShortcuts::Ids::kGoTo));
			const float reserve = std::max(homeReserve, pillEnd - columnStart.x);

			if (centerHeader) {
				// Centred in the space right of the pill; slides left rather than clip into the icon column
				const float offset = reserve + std::max(0.0f, (columnWidth - reserve - contentWidth) * 0.5f);
				ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);
			} else {
				// Add padding for left-aligned layout
				ImGui::SetCursorPosX(ImGui::GetCursorPosX() + reserve + ThemeManager::Constants::CURSOR_POSITION_PADDING);
			}

			// Centre the title's ink (not its line box) on the row the nav buttons and pill are centred on
			{
				RoleFontGuard titleFont(Menu::FontRole::Title);
				ImGui::SetWindowFontScale(textScaleFactor);
				ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, Util::CenterTextTopY(columnStart.y, estimatedRowHeight)));
				titleInkScreenY = ImGui::GetCursorScreenPos().y + Util::GetTextInkCenterY();
				ImGui::SetWindowFontScale(1.0f);
			}

			// Always display logo if texture is available
			if (showLogo) {
				float logoAspectRatio = uiIcons.logo.size.x / uiIcons.logo.size.y;
				ImVec2 logoSizeVec(logoSize * logoAspectRatio, logoSize);

				// Determine tint color for logo
				ImU32 logoTint = ImGui::GetColorU32(IM_COL32_WHITE);
				if (globals::menu->GetSettings().Theme.UseMonochromeLogo) {
					ImVec4 textColor = globals::menu->GetSettings().Theme.Palette.Text;
					logoTint = ImGui::GetColorU32(textColor);
				}

				// Use our helper to render aligned logo and text with perfect vertical alignment
				{
					RoleFontGuard titleFont(Menu::FontRole::Title);
					Util::DrawAlignedTextWithLogo(
						uiIcons.logo.texture,
						logoSizeVec,
						title.c_str(),
						textScaleFactor,
						logoTint);
				}
			} else {
				// No logo, just render the text with proper alignment
				ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
				{
					RoleFontGuard titleFont(Menu::FontRole::Title);
					Util::DrawSharpText(title.c_str(), true, textScaleFactor);
				}
				ImGui::PopStyleVar();
			}
			drawBuildTag();

			// Buttons on the right
			ImGui::TableNextColumn();
			RenderUndockedIcons(actionIcons, uiScale);

			ImGui::EndTable();
		}
		ImGui::EndGroup();
		const float headerHeight = ImGui::GetItemRectSize().y;
		const ImVec2 cursorAfterHeader = ImGui::GetCursorScreenPos();
		ImGui::SetCursorScreenPos(ImVec2(headerOrigin.x, headerOrigin.y + (headerHeight - sidebarButtonSize.y) * 0.5f));
		RenderSidebarToggle(sidebarButtonSize, uiIcons, sidebarTooltip, sidebarVisible);
		ImGui::SetCursorScreenPos(cursorAfterHeader);
	}

	// No separator needed for docked mode since icons are in title bar
	if (!isDocked) {
<<<<<<< HEAD
		// First separator - always shown when not docked
		ImGui::SeparatorEx(ImGuiSeparatorFlags_Horizontal, ThemeManager::Constants::SEPARATOR_THICKNESS);
		ImGui::Spacing();
	}

	// If icons are disabled or missing, show action buttons as text between separators (only when not docked)
	auto shaderCache = globals::shaderCache;
	if (!canShowIcons && !isDocked) {
		if (ImGui::BeginTable("##ActionButtons", 4, ImGuiTableFlags_SizingStretchSame)) {
			// Save Settings Button
			ImGui::TableNextColumn();
			if (Util::ButtonWithFlash(T("menu.save_settings", "Save Settings"), { -1, 0 })) {
				globals::state->Save();
			}

			// Restore Saved Settings Button
			ImGui::TableNextColumn();
			if (ImGui::Button(T("menu.restore_settings", "Restore Saved Settings"), { -1, 0 })) {
				globals::state->Load();
			}

			// Clear Shader Cache Button
			ImGui::TableNextColumn();
			{
				const bool capturing = shaderCache->IsCapturingActiveShaders();
				const bool awaitingMenuClose = shaderCache->IsAwaitingMenuCloseCapture();
				ImGui::BeginDisabled(capturing || awaitingMenuClose);
				std::string label = awaitingMenuClose ?
				                        T("menu.clear_shader_cache_awaiting_close", "Close the menu to finish") :
				                    capturing ?
				                        std::format("{} {}", T("menu.clear_shader_cache_capturing", "Capturing..."), shaderCache->GetActiveShaderCaptureFramesRemaining()) :
				                        T("menu.clear_shader_cache", "Clear Shader Cache");
				if (ImGui::Button(label.c_str(), { -1, 0 })) {
					Util::RequestClearShaderCacheConfirmation(Util::ResolveShaderCacheClearScope());
				}
				ImGui::EndDisabled();
			}
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", Util::GetClearShaderCacheTooltip());
				ImGui::Text("%s", T("menu.clear_shader_cache_modifier_hint", "Shift-click for the other clear mode."));
			}

			// Error message toggle if needed
			if (shaderCache->GetFailedTasks()) {
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				if (ImGui::Button(T("menu.toggle_error_message", "Toggle Error Message"), { -1, 0 })) {
					shaderCache->ToggleErrorMessages();
				}
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("%s", T("menu.toggle_error_message_tooltip",
										  "Hide or show the shader failure message. "
										  "Your installation is broken and will likely see errors in game. "
										  "Please double check you have updated all features and that your load order is correct. "
										  "See OpenShaders.log for details and check the Nexus Mods page or Discord server."));
				}
			}

			ImGui::EndTable();
		}

		// Second separator - only shown if icons are disabled/missing or if there are failed tasks (and not docked)
		if (!isDocked) {
			ImGui::Spacing();
			ImGui::SeparatorEx(ImGuiSeparatorFlags_Horizontal, ThemeManager::Constants::SEPARATOR_THICKNESS);
			ImGui::Spacing();
		}
	} else if (shaderCache->GetFailedTasks() && !isDocked) {
		// If icons are enabled but there are failed tasks, show error toggle button
		// and add the second separator (only when not docked)
		if (ImGui::Button(T("menu.toggle_error_message", "Toggle Error Message"), { -1, 0 })) {
			shaderCache->ToggleErrorMessages();
		}
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T("menu.toggle_error_message_tooltip",
								  "Hide or show the shader failure message. "
								  "Your installation is broken and will likely see errors in game. "
								  "Please double check you have updated all features and that your load order is correct. "
								  "See OpenShaders.log for details and check the Nexus Mods page or Discord server."));
		}

		// Add second separator when showing error button
		ImGui::Spacing();
||||||| 12b261724
		// First separator - always shown when not docked
		ImGui::SeparatorEx(ImGuiSeparatorFlags_Horizontal, ThemeManager::Constants::SEPARATOR_THICKNESS);
		ImGui::Spacing();
	}

	// If icons are disabled or missing, show action buttons as text between separators (only when not docked)
	auto shaderCache = globals::shaderCache;
	if (!canShowIcons && !isDocked) {
		if (ImGui::BeginTable("##ActionButtons", 4, ImGuiTableFlags_SizingStretchSame)) {
			// Save Settings Button
			ImGui::TableNextColumn();
			if (Util::ButtonWithFlash(T("menu.save_settings", "Save Settings"), { -1, 0 })) {
				globals::state->Save();
			}

			// Restore Saved Settings Button
			ImGui::TableNextColumn();
			if (ImGui::Button(T("menu.restore_settings", "Restore Saved Settings"), { -1, 0 })) {
				globals::state->Load();
			}

			// Clear Shader Cache Button
			ImGui::TableNextColumn();
			if (ImGui::Button(T("menu.clear_shader_cache", "Clear Shader Cache"), { -1, 0 })) {
				Util::RequestClearShaderCacheConfirmation();
			}
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T("menu.clear_shader_cache_tooltip",
									  "Clears the shader cache and disk cache (if enabled). "
									  "The Shader Cache is the collection of compiled shaders which replace the vanilla shaders at runtime. "
									  "The Disk Cache is a collection of compiled shaders on disk. "
									  "Clearing will mean that shaders are recompiled only when the game re-encounters them."));
			}

			// Error message toggle if needed
			if (shaderCache->GetFailedTasks()) {
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				if (ImGui::Button(T("menu.toggle_error_message", "Toggle Error Message"), { -1, 0 })) {
					shaderCache->ToggleErrorMessages();
				}
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("%s", T("menu.toggle_error_message_tooltip",
										  "Hide or show the shader failure message. "
										  "Your installation is broken and will likely see errors in game. "
										  "Please double check you have updated all features and that your load order is correct. "
										  "See CommunityShaders.log for details and check the Nexus Mods page or Discord server."));
				}
			}

			ImGui::EndTable();
		}

		// Second separator - only shown if icons are disabled/missing or if there are failed tasks (and not docked)
		if (!isDocked) {
			ImGui::Spacing();
			ImGui::SeparatorEx(ImGuiSeparatorFlags_Horizontal, ThemeManager::Constants::SEPARATOR_THICKNESS);
			ImGui::Spacing();
		}
	} else if (shaderCache->GetFailedTasks() && !isDocked) {
		// If icons are enabled but there are failed tasks, show error toggle button
		// and add the second separator (only when not docked)
		if (ImGui::Button(T("menu.toggle_error_message", "Toggle Error Message"), { -1, 0 })) {
			shaderCache->ToggleErrorMessages();
		}
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T("menu.toggle_error_message_tooltip",
								  "Hide or show the shader failure message. "
								  "Your installation is broken and will likely see errors in game. "
								  "Please double check you have updated all features and that your load order is correct. "
								  "See CommunityShaders.log for details and check the Nexus Mods page or Discord server."));
		}

		// Add second separator when showing error button
		ImGui::Spacing();
=======
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
		ImGui::SeparatorEx(ImGuiSeparatorFlags_Horizontal, ThemeManager::Constants::SEPARATOR_THICKNESS);
		ImGui::Spacing();
	}
}

<<<<<<< HEAD
void MenuHeaderRenderer::RenderSidebarToggle(const ImVec2& buttonSize, const Menu::UIIcons& uiIcons, const char* tooltip, bool& sidebarVisible)
{
	if (uiIcons.sidebar.texture) {
		const ImVec2 padding = ImGui::GetStyle().FramePadding;
		if (ImGui::InvisibleButton("##SidebarToggle", buttonSize, ImGuiButtonFlags_EnableNav))
			sidebarVisible = !sidebarVisible;
		Util::DrawCurrentItemRoundedButtonHighlight();
		const ImVec2 min = ImGui::GetItemRectMin();
		ImGui::GetWindowDrawList()->AddImage(uiIcons.sidebar.texture,
			ImVec2(min.x + padding.x, min.y + padding.y),
			ImVec2(min.x + buttonSize.x - padding.x, min.y + buttonSize.y - padding.y),
			ImVec2(0, 0), ImVec2(1, 1), ImGui::GetColorU32(ImGuiCol_Text));
	} else {
		const auto buttonStyle = Util::TransparentIconButtonStyle();
		if (ImGui::Button(std::format("{}###SidebarToggle", tooltip).c_str(), buttonSize))
			sidebarVisible = !sidebarVisible;
	}
	if (auto tooltipScope = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(tooltip);
}

float MenuHeaderRenderer::GetCompactActionsWidth(const Menu::UIIcons& uiIcons)
{
	const auto icons = BuildActionIcons(true, uiIcons, true);
	float width = ImGui::GetStyle().ItemSpacing.x * static_cast<float>(icons.size() - 1);
	for (const auto& icon : icons)
		width += icon.texture ? ImGui::GetFrameHeight() : ImGui::CalcTextSize(icon.tooltip).x + ImGui::GetStyle().FramePadding.x * 2.0f;
	return width;
}

void MenuHeaderRenderer::RenderCompactActions(const Menu::UIIcons& uiIcons)
{
	const auto icons = BuildActionIcons(true, uiIcons, true);
	const auto buttonStyle = Util::TransparentIconButtonStyle();
	const float size = ImGui::GetFrameHeight();
	const float imageSize = ImGui::GetFontSize();
	const auto tint = ImGui::GetColorU32(Util::GetIconTint());
	for (size_t i = 0; i < icons.size(); ++i) {
		if (i != 0)
			ImGui::SameLine();
		const auto& icon = icons[i];
		const bool clearing = i == icons.size() - 1;
		const bool disabled = clearing && (globals::shaderCache->IsCapturingActiveShaders() || globals::shaderCache->IsAwaitingMenuCloseCapture());
		ImGui::BeginDisabled(disabled);
		ImGui::PushID(static_cast<int>(i));
		if (icon.texture) {
			if (ImGui::InvisibleButton("##MenuAction", { size, size }, ImGuiButtonFlags_EnableNav))
				icon.callback();
			Util::DrawCurrentItemRoundedButtonHighlight();
			const auto min = ImGui::GetItemRectMin();
			const float inset = (size - imageSize) * 0.5f;
			ImGui::GetWindowDrawList()->AddImage(icon.texture, { min.x + inset, min.y + inset },
				{ min.x + inset + imageSize, min.y + inset + imageSize }, { 0, 0 }, { 1, 1 }, tint);
		} else if (ImGui::Button(icon.tooltip)) {
			icon.callback();
		}
		ImGui::PopID();
		ImGui::EndDisabled();
		if (auto tooltip = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(icon.tooltip);
		}
	}
}

std::vector<MenuHeaderRenderer::ActionIcon> MenuHeaderRenderer::BuildActionIcons(bool canShowIcons, const Menu::UIIcons& uiIcons, bool includeMissing)
||||||| 12b261724
std::vector<MenuHeaderRenderer::ActionIcon> MenuHeaderRenderer::BuildActionIcons(bool canShowIcons, const Menu::UIIcons& uiIcons)
=======
std::vector<MenuHeaderRenderer::ActionIcon> MenuHeaderRenderer::BuildActionIcons(const Util::IconLoader::UIIcons& uiIcons,
	const std::vector<MenuShortcuts::Shortcut>& shortcuts)
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
{
	// In display order. An icon whose texture failed to load falls back to its glyph.
	std::vector<ActionIcon> actionIcons;

<<<<<<< HEAD
	if (!canShowIcons) {
		return actionIcons;
	}

	// Build list of available action icons (in display order)
	if (uiIcons.saveSettings.texture || includeMissing) {
		actionIcons.push_back({ uiIcons.saveSettings.texture,
			T("menu.save_settings", "Save Settings"),
			[]() {
				globals::state->Save();
			} });
	}
	if (uiIcons.loadSettings.texture || includeMissing) {
		actionIcons.push_back({ uiIcons.loadSettings.texture,
			T("menu.restore_settings", "Restore Saved Settings"),
			[]() {
				globals::state->Load();
			} });
	}
	if (uiIcons.clearCache.texture || includeMissing) {
		actionIcons.push_back({ uiIcons.clearCache.texture,
			Util::ResolveShaderCacheClearScope() == Util::ShaderCacheClearScope::ActiveOnly ?
				T("menu.clear_active_shaders", "Clear Active Shaders") :
				T("menu.clear_shader_cache", "Clear Shader Cache"),
			[]() {
				Util::RequestClearShaderCacheConfirmation(Util::ResolveShaderCacheClearScope());
			} });
	}

||||||| 12b261724
	if (!canShowIcons) {
		return actionIcons;
	}

	// Build list of available action icons (in display order)
	if (uiIcons.saveSettings.texture) {
		actionIcons.push_back({ uiIcons.saveSettings.texture,
			T("menu.save_settings", "Save Settings"),
			[]() {
				globals::state->Save();
			} });
	}
	if (uiIcons.loadSettings.texture) {
		actionIcons.push_back({ uiIcons.loadSettings.texture,
			T("menu.restore_settings", "Restore Saved Settings"),
			[]() {
				globals::state->Load();
			} });
	}
	if (uiIcons.clearCache.texture) {
		actionIcons.push_back({ uiIcons.clearCache.texture,
			T("menu.clear_shader_cache_tooltip",
				"Clears the shader cache and disk cache (if enabled). "
				"The Shader Cache is the collection of compiled shaders which replace the vanilla shaders at runtime. "
				"The Disk Cache is a collection of compiled shaders on disk. "
				"Clearing will mean that shaders are recompiled only when the game re-encounters them."),
			[]() {
				Util::RequestClearShaderCacheConfirmation();
			} });
	}

=======
	// Tooltips name the action's shortcut, e.g. "Save settings (Ctrl+S)"
	auto* i18n = I18n::GetSingleton();
	const bool unsaved = globals::state->HasUnsavedChanges();
	const std::unordered_map<std::string, std::string> saveHint{ { "shortcut", MenuShortcuts::GetHint(shortcuts, MenuShortcuts::Ids::kSave) } };
	actionIcons.push_back({ uiIcons.saveSettings.texture, Icons::TI(MenuIcons::kSave),
		unsaved ? i18n->Format("menu.header.save_unsaved_tooltip", saveHint, "Save unsaved changes ({shortcut})") :
				  i18n->Format("menu.header.save_tooltip", saveHint, "Save settings ({shortcut})"),
		[]() {
			SaveManager::SaveAll();
		},
		unsaved });
	actionIcons.push_back({ uiIcons.loadSettings.texture, Icons::TI(MenuIcons::kRestore),
		T("menu.restore_settings", "Restore Saved Settings"),
		[]() {
			Util::RequestRestoreSavedSettingsConfirmation();
		} });
	actionIcons.push_back({ uiIcons.clearCache.texture, Icons::TI(MenuIcons::kClearCache),
		T("menu.clear_shader_cache", "Clear Shader Cache"),  // the confirmation dialog explains what is cleared
		[]() {
			Util::RequestClearShaderCacheConfirmation();
		} });
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
	return actionIcons;
}

namespace
{
	/**
	 * @brief Draws the unsaved-changes box behind an action icon: the status colour at low alpha with a thin border, in
	 * the style of the CS Editor's lock badge. The box is the icon's hover rect; the icon keeps its own colours, since
	 * tinting it turned its blue purple.
	 */
	void DrawUnsavedBox(ImDrawList* drawList, ImVec2 min, ImVec2 max)
	{
		using Constants = ThemeManager::Constants;
		const float rounding = std::min(ImGui::GetStyle().FrameRounding, (max.x - min.x) * 0.35f);
		ImVec4 fill = Util::Colors::GetUnsaved();
		fill.w = Constants::HEADER_UNSAVED_BOX_FILL_ALPHA;
		ImVec4 border = Util::Colors::GetUnsaved();
		border.w = Constants::HEADER_UNSAVED_BOX_BORDER_ALPHA;
		drawList->AddRectFilled(min, max, ImGui::GetColorU32(fill), rounding);
		drawList->AddRect(min, max, ImGui::GetColorU32(border), rounding);
	}
}

float MenuHeaderRenderer::GetHeaderIconSize(bool docked, float uiScale)
{
	using Constants = ThemeManager::Constants;
	const float multiplier = docked ? Constants::DOCKED_ICON_SIZE_MULTIPLIER : Constants::HEADER_BASE_ICON_MULTIPLIER;
	return ImGui::GetFontSize() * multiplier * Constants::HEADER_ICON_SCALE * uiScale;
}

float MenuHeaderRenderer::RenderDockedIcons(const std::vector<ActionIcon>& actionIcons, float uiScale)
{
	if (actionIcons.empty())
		return ImGui::GetWindowPos().x + ImGui::GetWindowSize().x;

	// Docked: larger icons drawn over the tab bar
	const float iconSize = GetHeaderIconSize(true, uiScale);
	const float iconSpacing = ThemeManager::Constants::DOCKED_ICON_SPACING * uiScale;
	const float rightMargin = ThemeManager::Constants::DOCKED_RIGHT_MARGIN * uiScale;

	// Get window position and calculate title bar area
	ImVec2 windowPos = ImGui::GetWindowPos();
	ImVec2 windowSize = ImGui::GetWindowSize();
	float titleBarHeight = ImGui::GetFrameHeight();

	ImDrawList* barDrawList = DockedHeaderDrawList();

	// Calculate icon positions (right to left from close button)
	float iconX = windowPos.x + windowSize.x - rightMargin;
	float iconY = windowPos.y + (titleBarHeight - iconSize) * 0.5f;

	// Draw icons from right to left
	for (auto it = actionIcons.rbegin(); it != actionIcons.rend(); ++it) {
		iconX -= iconSize + iconSpacing;

		// Slightly reduce the icon rendering area to minimize any transparent padding
		const float paddingReduction = ThemeManager::Constants::DOCKED_ICON_PADDING_REDUCTION * uiScale;
		ImVec2 iconMin(iconX + paddingReduction, iconY + paddingReduction);
		ImVec2 iconMax(iconX + iconSize - paddingReduction, iconY + iconSize - paddingReduction);

		// Use the full area for mouse interaction (including padding)
		ImRect interactionRect({ iconX, iconY }, { iconX + iconSize, iconY + iconSize });

		// Check mouse interaction against full area
		const bool isHovered = IsDockedHeaderButtonHovered(interactionRect);
		auto unsavedTint = Util::StateHoverButtonStyle(Util::Colors::GetUnsaved(), it->unsaved);  // the unsaved save hovers in its colour
		Util::DrawAnimatedButtonHighlight(interactionRect, ImGui::GetID(it->tooltip.c_str()), isHovered, isHovered && ImGui::IsMouseDown(ImGuiMouseButton_Left), false, barDrawList);

		// Draw icon with hover effect, using reduced area to minimize padding
		ImU32 tintColor;
		if (globals::menu->GetSettings().Theme.UseMonochromeIcons) {
			// Use theme text color for monochrome icons
			ImVec4 textColor = globals::menu->GetSettings().Theme.Palette.Text;
			if (!isHovered) {
				textColor.w *= ThemeManager::Constants::HEADER_ICON_IDLE_ALPHA;
			}
			tintColor = ImGui::GetColorU32(textColor);
		} else {
			// Use white/gray tint for colored icons
			tintColor = ImGui::GetColorU32(isHovered ? IM_COL32_WHITE : ThemeManager::Constants::HEADER_COLOR_ICON_IDLE);
		}
		if (it->unsaved)
			DrawUnsavedBox(barDrawList, interactionRect.Min, interactionRect.Max);
		if (it->texture) {
			// Whole-pixel rect, and the mip level nearest this size, so the PNG edges stay crisp
			const ImVec2 snappedMin = Util::IconLoader::SnapToPixel(iconMin);
			const ImVec2 snappedSize = Util::IconLoader::SnapToPixel(ImVec2(iconMax.x - iconMin.x, iconMax.y - iconMin.y));
			barDrawList->AddImage(Util::IconLoader::ViewForSize(it->texture, snappedSize), snappedMin,
				ImVec2(snappedMin.x + snappedSize.x, snappedMin.y + snappedSize.y), ImVec2(0, 0), ImVec2(1, 1), tintColor);
		} else
			DrawIconGlyph(barDrawList, iconMin, iconMax.x - iconMin.x, it->glyph, tintColor);

		// Handle interaction
		if (isHovered) {
			if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
				it->callback();
			}

			// Set tooltip manually since we're drawing outside normal ImGui flow
			ImGui::SetTooltip("%s", it->tooltip.c_str());
		}
	}
	return iconX;
}

float MenuHeaderRenderer::RenderSearchPill(bool docked, float left, float right, float top, float rowHeight, float uiScale,
	const std::string& shortcutHint)
{
	using Constants = ThemeManager::Constants;
	// Same size as the Home / issues buttons it sits beside
	const float size = GetHeaderIconSize(docked, uiScale);
	const float gap = Constants::DOCKED_ICON_SPACING * uiScale;
	if (size <= 0.0f || right - gap - left < size)
		return left;

	const std::string tooltip = shortcutHint.empty() ? std::string(T("menu.palette.pill_tooltip", "Go to...")) :
	                                                   std::format("{} ({})", T("menu.palette.pill_tooltip", "Go to..."), shortcutHint);
	RenderNavButton(docked, ImVec2(left, top + (rowHeight - size) * 0.5f), size, "##HeaderGoTo", Icons::TI(MenuIcons::kSearch), tooltip.c_str(),
		false, nullptr, []() { GoToPalette::Open(); });
	return left + size + gap;
}

void MenuHeaderRenderer::RenderUndockedIcons(const std::vector<ActionIcon>& actionIcons, float uiScale)
{
	if (actionIcons.empty())
		return;

	// Undocked: square buttons in a table column, drawn like the nav buttons (eased theme highlight, then the icon)
	const float iconSize = GetHeaderIconSize(false, uiScale);
	const float paddingReduction = ThemeManager::Constants::UNDOCKED_ICON_PADDING_REDUCTION * uiScale;
	const ImVec2 imageSize(iconSize - paddingReduction, iconSize - paddingReduction);
	const float pad = ImGui::GetStyle().FramePadding.y;
	const ImVec2 buttonSize(imageSize.x + pad * 2.0f, imageSize.y + pad * 2.0f);
	const ImU32 tint = ImGui::GetColorU32(BaseIconTint());

	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ThemeManager::Constants::UNDOCKED_ICON_ITEM_SPACING, 0.0f));
	for (size_t i = 0; i < actionIcons.size(); ++i) {
		const auto& icon = actionIcons[i];
		const bool clicked = ImGui::InvisibleButton(std::format("##ActionBtn{}", i).c_str(), buttonSize, ImGuiButtonFlags_EnableNav);
		const ImRect rect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		if (icon.unsaved)
			DrawUnsavedBox(drawList, rect.Min, rect.Max);
		auto unsavedTint = Util::StateHoverButtonStyle(Util::Colors::GetUnsaved(), icon.unsaved);  // the unsaved save hovers in its colour
		Util::DrawAnimatedButtonHighlight(rect, ImGui::GetItemID(), ImGui::IsItemHovered(), ImGui::IsItemActive(), false, drawList);
		const ImVec2 contentMin(rect.Min.x + pad, rect.Min.y + pad);
		if (icon.texture) {
			// Whole-pixel rect, and the mip level nearest this size, so the PNG edges stay crisp
			const ImVec2 snappedMin = Util::IconLoader::SnapToPixel(contentMin);
			const ImVec2 snappedSize = Util::IconLoader::SnapToPixel(imageSize);
			drawList->AddImage(Util::IconLoader::ViewForSize(icon.texture, snappedSize), snappedMin,
				ImVec2(snappedMin.x + snappedSize.x, snappedMin.y + snappedSize.y), ImVec2(0, 0), ImVec2(1, 1), tint);
		} else {
			DrawIconGlyph(drawList, contentMin, imageSize.x, icon.glyph, tint);
		}
		if (clicked)
			icon.callback();
		Util::HoverTip(icon.tooltip.c_str());

		if (i < actionIcons.size() - 1)
			ImGui::SameLine();
	}
	ImGui::PopStyleVar();
}

void MenuHeaderRenderer::RenderWatermarkLogo(const Util::IconLoader::UIIcons& uiIcons)
{
	// Get current window's drawable area (excluding title bar)
	ImVec2 windowPos = ImGui::GetWindowPos();
	ImVec2 windowSize = ImGui::GetWindowSize();
	float titleBarHeight = ImGui::GetFrameHeight();

	// Calculate content area (below title bar)
	ImVec2 contentPos(windowPos.x, windowPos.y + titleBarHeight);
	ImVec2 contentSize(windowSize.x, windowSize.y - titleBarHeight);

	// Calculate watermark logo size - base it on height for consistent sizing
	const float watermarkHeightPercent = ThemeManager::Constants::WATERMARK_HEIGHT_PERCENT;
	float watermarkHeight = contentSize.y * watermarkHeightPercent;
	float logoAspectRatio = uiIcons.logo.size.x / uiIcons.logo.size.y;
	float watermarkWidth = watermarkHeight * logoAspectRatio;

	// Position watermark in the center of the content area
	float logoX = contentPos.x + (contentSize.x - watermarkWidth) * 0.5f;   // Horizontally centered
	float logoY = contentPos.y + (contentSize.y - watermarkHeight) * 0.5f;  // Vertically centered

	// Draw watermark logo with transparency and blending
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	ImVec2 logoMin(logoX, logoY);
	ImVec2 logoMax(logoX + watermarkWidth, logoY + watermarkHeight);

	// Determine watermark color based on monochrome logo setting
	ImU32 watermarkColor;
	if (globals::menu->GetSettings().Theme.UseMonochromeLogo) {
		ImVec4 textColor = globals::menu->GetSettings().Theme.Palette.Text;
		textColor.w = ThemeManager::Constants::WATERMARK_MONO_ALPHA;
		watermarkColor = ImGui::GetColorU32(textColor);
	} else {
		watermarkColor = ImGui::GetColorU32(ThemeManager::Constants::WATERMARK_COLOR);
	}

	const ImVec2 logoDrawMin = Util::IconLoader::SnapToPixel(logoMin);
	const ImVec2 logoDrawSize = Util::IconLoader::SnapToPixel(ImVec2(logoMax.x - logoMin.x, logoMax.y - logoMin.y));
	drawList->AddImage(Util::IconLoader::ViewForSize(uiIcons.logo.texture, logoDrawSize), logoDrawMin,
		ImVec2(logoDrawMin.x + logoDrawSize.x, logoDrawMin.y + logoDrawSize.y), ImVec2(0, 0), ImVec2(1, 1), watermarkColor);
}

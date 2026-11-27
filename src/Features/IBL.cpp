#include "IBL.h"
#include "Features/DX12/DX12Control.h"
#include "Features/NativeIBL/NativeIBL.h"
#include "Features/IBL/IBLSettingsInput.h"

#include "Deferred.h"
#include "DynamicCubemaps.h"
#include "LODShadows.h"
#include "LinearLighting.h"
#include "ShaderCache.h"
#include "State.h"

#if defined(ENABLE_EFFECTS11)
#	include "Effects11.h"
#	include "Effects11/SettingManager.h"
#endif
#include "Globals.h"

#include "../I18n/I18n.h"
#include "GpuPass.h"
#include <DDSTextureLoader.h>
#include <DirectXTex.h>

#define I18N_KEY_PREFIX "feature.ibl."

namespace
{
	constexpr float kIBLScaleMin = 0.0f;
	constexpr float kIBLScaleMax = 2.0f;
	constexpr uint kDALCLuminanceRatioMode = 0;
	constexpr uint kDALCColorRatioMode = 1;
	constexpr uint kDALCPlusSkyMode = 2;
	constexpr uint kDALCPlusSkyDirectionalMode = 3;

	bool IsDALCModeDisabled(const IBL::Settings& settings)
	{
		return settings.DALCAmount <= 0.0f;
	}

	uint GetEffectiveDALCMode(const IBL::Settings& settings)
	{
		if (IsDALCModeDisabled(settings) && settings.DALCMode >= kDALCPlusSkyMode)
			return kDALCLuminanceRatioMode;

		return settings.DALCMode;
	}

	/** @brief Flags Lighting passes drawn into the inventory, magic or book 3D menu scene so shaders can skip IBL for them. */
	void UpdateMenuItemDescriptor(const RE::BSRenderPass* a_pass)
	{
		constexpr auto menuItem = static_cast<uint32_t>(State::ExtraShaderDescriptors::MenuItem);
		constexpr auto worldDraw = static_cast<uint32_t>(State::ExtraShaderDescriptors::InWorld) |
		                           static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections);
		auto& descriptor = globals::state->permutationData.ExtraShaderDescriptor;
		descriptor &= ~menuItem;
		if ((descriptor & worldDraw) || !a_pass || !a_pass->shader || a_pass->shader->shaderType.get() != RE::BSShader::Type::Lighting)
			return;

		const auto* menuScene = RE::UI3DSceneManager::GetSingleton();
		const auto* accumulator = *globals::game::currentAccumulator.get();
		if (!menuScene || !menuScene->shadowSceneNode || !accumulator ||
			accumulator->GetRuntimeData().activeShadowSceneNode != menuScene->shadowSceneNode)
			return;

		const auto scheme = menuScene->currentlightScheme;
		if (scheme == RE::INTERFACE_LIGHT_SCHEME::kInventory || scheme == RE::INTERFACE_LIGHT_SCHEME::kInventoryMagic ||
			scheme == RE::INTERFACE_LIGHT_SCHEME::kBook)
			descriptor |= menuItem;
	}

	void DrawEnableCheckbox(const char* label, bool& disableSetting)
	{
		bool enableSetting = !disableSetting;
		if (ImGui::Checkbox(label, &enableSetting)) {
			disableSetting = !enableSetting;
		}
	}
}

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	IBL::Settings,
	EnableIBL,
	PreserveFogLuminance,
	UseStaticIBL,
	DALCAmount,
	EnvIBLScale,
	SkyIBLScale,
	EnvIBLSaturation,
	SkyIBLSaturation,
	FogAmount,
	DALCMode,
	DisableInInteriors,
	DisableInWorldMap,
	DisableInLoadingScreen)

void IBL::DrawSettings()
{
	const bool managedByENB = IsManagedByENB();
	const Util::LockedSection enbLock(managedByENB,
		T("common.settings_managed_by_enb", "This setting is managed by Effects 11."));
	const Util::DefaultsWhileActive<Settings> shownDefaults(settings, managedByENB);

	Util::CheckboxFlag(T(TKEY("enable_ibl"), "Enable IBL"), settings.EnableIBL);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("enable_ibl_tooltip"), "Toggle IBL. When enabled, ambient lighting is derived from cubemap spherical harmonics instead of the vanilla system."));
	}
	if (ImGui::TreeNodeEx(T(TKEY("enable_ibl_options"), "Enable IBL Options"), ImGuiTreeNodeFlags_None)) {
		DrawEnableCheckbox(T(TKEY("enable_in_interiors"), "Enable Interior IBL"), settings.DisableInInteriors);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("enable_in_interiors_tooltip"), "Enables IBL in interior cells."));
		}
		DrawEnableCheckbox(T(TKEY("enable_in_world_map"), "Enable World Map IBL"), settings.DisableInWorldMap);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("enable_in_world_map_tooltip"), "Enables IBL while the world map is open."));
		}
		DrawEnableCheckbox(T(TKEY("enable_in_loading_screen"), "Enable Loading Screen IBL"), settings.DisableInLoadingScreen);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("enable_in_loading_screen_tooltip"), "Enables IBL during loading screens and the main menu."));
		}
		ImGui::TreePop();
	}
	ImGui::SliderFloat(T(TKEY("env_ibl_scale"), "Env IBL Scale"), &settings.EnvIBLScale, kIBLScaleMin, kIBLScaleMax, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("env_ibl_scale_tooltip"), "Intensity multiplier for the environment IBL (from Dynamic Cubemaps).\nControls how strongly the surrounding environment contributes to ambient lighting."));
	}
	ImGui::SliderFloat(T(TKEY("sky_ibl_scale"), "Sky IBL Scale"), &settings.SkyIBLScale, kIBLScaleMin, kIBLScaleMax, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("sky_ibl_scale_tooltip"), "Intensity multiplier for the sky IBL (from the game's native reflections cubemap).\nControls how strongly the sky contributes to ambient lighting."));
	}
	ImGui::SliderFloat(T(TKEY("env_ibl_saturation"), "Env IBL Saturation"), &settings.EnvIBLSaturation, 0.0f, 2.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("env_ibl_saturation_tooltip"), "Color saturation of the environment IBL.\nLower values produce more neutral ambient light; higher values produce more vivid color."));
	}
	ImGui::SliderFloat(T(TKEY("sky_ibl_saturation"), "Sky IBL Saturation"), &settings.SkyIBLSaturation, 0.0f, 2.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("sky_ibl_saturation_tooltip"), "Color saturation of the sky IBL.\nLower values produce more neutral ambient light; higher values produce more vivid color."));
	}
	ImGui::SliderFloat(T(TKEY("dalc_amount"), "DALC Amount"), &settings.DALCAmount, 0.0f, 1.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("dalc_amount_tooltip"),
							  "Blends the IBL brightness toward the game's vanilla ambient (DALC) level.\n"
							  "0 = no matching (pure IBL brightness), 1 = fully matched to vanilla ambient."));
	}
	{
		struct DALCModeOption
		{
			uint value;
			const char* label;
		};

		const DALCModeOption dalcModeOptions[][2] = {
			{
				{ kDALCLuminanceRatioMode, T(TKEY("dalc_mode_luminance_ratio"), "Luminance Ratio") },
				{ kDALCPlusSkyMode, T(TKEY("dalc_mode_dalc_plus_sky"), "DALC + Sky") },
			},
			{
				{ kDALCColorRatioMode, T(TKEY("dalc_mode_color_ratio"), "Color Ratio") },
				{ kDALCPlusSkyDirectionalMode, T(TKEY("dalc_mode_dalc_plus_sky_directional"), "DALC + Sky (Directional)") },
			},
		};
		int dalcMode = static_cast<int>(settings.DALCMode);

		auto showDALCModeTooltip = []() {
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T(TKEY("dalc_mode_tooltip"),
									  "Luminance Ratio: Scalar ratio from overall luminance (loses DALC color tint).\n"
									  "Color Ratio: Per-channel ratio (preserves DALC color tint).\n"
									  "DALC + Sky: Uses vanilla ambient as base, sky IBL on top. Skylighting only affects sky.\n"
									  "DALC + Sky (Directional): Same, but Skylighting also dims vanilla ambient per-direction."));
			}
		};

		const bool dalcModeDisabled = IsDALCModeDisabled(settings);
		const auto disabledGuard = Util::DisableGuard(dalcModeDisabled);
		ImGui::Text("%s:", T(TKEY("dalc_mode"), "DALC Mode"));
		showDALCModeTooltip();
		if (ImGui::BeginTable("##DALCModeOptions", 2, ImGuiTableFlags_SizingStretchSame)) {
			for (const auto& row : dalcModeOptions) {
				for (const auto& option : row) {
					ImGui::TableNextColumn();
					if (ImGui::RadioButton(option.label, &dalcMode, static_cast<int>(option.value))) {
						settings.DALCMode = static_cast<uint>(dalcMode);
					}
					showDALCModeTooltip();
				}
			}
			ImGui::EndTable();
		}
	}
	Util::CheckboxFlag(T(TKEY("use_static_ibl"), "Use Static IBL For Out-of-World Objects"), settings.UseStaticIBL);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("use_static_ibl_tooltip"), "Uses pre-baked static IBL cubemap textures for objects rendered outside the game world (e.g. inventory items, loading screens)."));
	}
	ImGui::SliderFloat(T(TKEY("fog_mix"), "Fog Mix"), &settings.FogAmount, 0.0f, 1.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("fog_mix_tooltip"), "Blends the fog color toward the IBL ambient color.\n0 = vanilla fog, 1 = fog fully tinted by IBL."));
	}
	Util::CheckboxFlag(T(TKEY("preserve_fog_luminance"), "Preserve Fog Luminance"), settings.PreserveFogLuminance);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("preserve_fog_luminance_tooltip"), "When Fog Mix is active, rescales the IBL-tinted fog to keep the original fog brightness.\nPrevents fog from becoming too bright or too dark."));
	}
}

#undef I18N_KEY_PREFIX

void IBL::LoadSettings(json& o_json)
{
	settings = o_json;
}

void IBL::SaveSettings(json& o_json)
{
	o_json = settings;
}

void IBL::RestoreDefaultSettings()
{
	settings = {};
}

IBL::PerFrame IBLInput::ResolveSettings(const IBL::Settings& settings)
{
	const bool sceneDisabled = IsDisabledForCurrentScene(settings);
	const bool managedByENB = IsManagedByENB();
	const IBL::Settings source = managedByENB ? IBL::Settings{} : settings;
	IBL::PerFrame data = {
		.EnableIBL = sceneDisabled ? 0u : source.EnableIBL,
		.PreserveFogLuminance = source.PreserveFogLuminance,
		.UseStaticIBL = source.UseStaticIBL,
		.DALCAmount = source.DALCAmount,
		.EnvIBLScale = source.EnvIBLScale,
		.SkyIBLScale = source.SkyIBLScale,
		.EnvIBLSaturation = source.EnvIBLSaturation,
		.SkyIBLSaturation = source.SkyIBLSaturation,
		.FogAmount = source.FogAmount,
		.DALCMode = GetEffectiveDALCMode(source),
#if defined(ENABLE_EFFECTS11)
		.ENBSkyFalloff = globals::features::effects11.OwnsFeature(E11Handoff::Feature::IBL)
#else
		.ENBSkyFalloff = false
#endif
	};

#if defined(ENABLE_EFFECTS11)
	if (!sceneDisabled && managedByENB) {
		auto& settingManager = SettingManager::GetSingleton();
		data.EnableIBL = Util::IsInterior() ? 0u : 1u;
		data.EnvIBLScale = 0.0f;
		data.SkyIBLScale = settingManager.GetInterpolatedTimeOfDayValue("MultiplicativeAmount", "IMAGEBASEDLIGHTING");
		data.DALCAmount = 1.0f;
		data.EnvIBLSaturation = 1.0f;
		data.SkyIBLSaturation = 1.0f;
		data.DALCMode = kDALCPlusSkyDirectionalMode;
		data.FogAmount = 0.0f;
	}
#endif

	return data;
}

IBL::PerFrame IBL::GetCommonBufferData() const
{
	auto data = IBLInput::ResolveSettings(settings);
	const bool useStatic = settings.UseStaticIBL && !globals::state->inWorld && !globals::state->activeReflections;
	if (!useStatic && !dynamicIBLValid)
		data.EnableIBL = 0;
	return data;
}

bool IBLInput::IsManagedByENB()
{
#if defined(ENABLE_EFFECTS11)
	auto& enb = globals::features::effects11;
	if (!enb.OwnsFeature(E11Handoff::Feature::IBL))
		return false;
	return SettingManager::GetSingleton().GetValue<bool>("EnableImageBasedLighting", "EFFECT");
#else
	return false;
#endif
}

bool IBL::IsManagedByENB() const
{
	return IBLInput::IsManagedByENB();
}

bool IBLInput::IsDisabledForCurrentScene(const IBL::Settings& settings)
{
	const auto state = globals::state;
	if (!state)
		return false;

	if (state->IsMainOrLoadingMenuOpen())
		return settings.DisableInLoadingScreen;

	if (state->isMapMenuOpen)
		return settings.DisableInWorldMap;

	return settings.DisableInInteriors && Util::IsInterior();
}

bool IBL::IsDisabledForCurrentScene() const
{
	return IBLInput::IsDisabledForCurrentScene(settings);
}

void IBL::ReflectionsPrepass()
{
	if (loaded) {
		auto context = globals::d3d::context;

		bool sceneDisabled = IsDisabledForCurrentScene();
		// dynamicIBLValid is false when the diffuse IBL compute shader failed to
		// compile -- envIBLTexture/skyIBLTexture were never written this frame.
		bool bindDynamic = !sceneDisabled && dynamicIBLValid;

		// Set PS shader resource
		{
			std::array<ID3D11ShaderResourceView*, 4> srvs = {
				bindDynamic ? envIBLTexture->srv.get() : nullptr,
				bindDynamic ? skyIBLTexture->srv.get() : nullptr,
				staticDiffuseIBLTexture ? staticDiffuseIBLTexture->srv.get() : nullptr,
				staticSpecularIBLTexture ? staticSpecularIBLTexture->srv.get() : nullptr
			};
			context->PSSetShaderResources(76, 4, srvs.data());
		}
	}
}

bool IBL::UsesReflectionRadiance() const
{
	return loaded && capture.Allocated() && globals::shaderCache->IsEnabled() &&
	       (globals::game::isVR || globals::features::lodShadows.loaded) &&
	       !IsDisabledForCurrentScene() && !Util::IsInterior() && (settings.EnableIBL || IsManagedByENB());
}

std::function<void()> IBL::OnReflectionsRenderBegin()
{
	if (globals::features::dx12Control.WantsMaterialPreparation())
		return nullptr;  // NativeIBL owns the reflection input hook in this session.
	if (!capture.Allocated())
		capture.Setup();
	return capture.Begin(UsesReflectionRadiance());
}

void IBL::OnSceneTransitionReset(bool)
{
	capture.Invalidate();
}

json IBL::GetDiagnostics()
{
	return {
		{ "radianceValidFaces", capture.ValidFaces() },
		{ "radianceCaptureFailed", capture.Failed() },
		{ "radianceSize", capture.Width() },
		{ "dynamicIBLValid", dynamicIBLValid }
	};
}

std::function<void()> IBL::OnRenderPassBegin(const RE::BSRenderPass* a_pass)
{
	UpdateMenuItemDescriptor(a_pass);
	return capture.OnRenderPassBegin();
}

void IBL::OnDraw()
{
	capture.OnDraw();
}

void IBL::Prepass()
{
	if (IsDisabledForCurrentScene())
		return;

	auto context = globals::d3d::context;

	auto& dynamicCubemaps = globals::features::dynamicCubemaps;

	auto& envTexture = dynamicCubemaps.envTexture;

	// Unset PS shader resource
	{
		ID3D11ShaderResourceView* views[2]{ nullptr, nullptr };
		context->PSSetShaderResources(76, 2, views);
	}

	std::array<ID3D11ShaderResourceView*, 1> srvs = { dynamicCubemaps.loaded && envTexture ? envTexture->srv.get() : nullptr };
	std::array<ID3D11UnorderedAccessView*, 1> uavs = { envIBLTexture->uav.get() };
	std::array<ID3D11SamplerState*, 1> samplers = { Deferred::GetSingleton()->linearSampler };

	auto* diffuseIBLShader = GetDiffuseIBLCS();
	dynamicIBLValid = diffuseIBLShader != nullptr;

	// IBL - Environment cubemap SH projection (skip for DALC-based modes that don't use EnvIBL)
	if (GetEffectiveDALCMode(settings) < kDALCPlusSkyMode) {
		samplers[0] = Deferred::GetSingleton()->linearSampler;

		context->CSSetSamplers(0, (uint)samplers.size(), samplers.data());
		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		if (diffuseIBLShader) {
			context->CSSetShader(diffuseIBLShader, nullptr, 0);
			{
				CS_GPU_PASS("IBL::EnvDiffuseIBL");
				context->Dispatch(1, 1, 1);
			}
		}
	} else {
		// Still need to set sampler and shader for sky IBL dispatch below
		context->CSSetSamplers(0, (uint)samplers.size(), samplers.data());
		if (diffuseIBLShader)
			context->CSSetShader(diffuseIBLShader, nullptr, 0);
	}

	// Visible reflections receive LOD shadows while sky irradiance retains terrain and cloud shadows.
	{
		const auto& reflections = globals::game::renderer->GetRendererData().cubemapRenderTargets[RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS];
		auto* radiance = globals::features::dx12Control.WantsMaterialPreparation() ?
			globals::features::nativeIBL.GetRasterSkyView() : capture.View();
		srvs.at(0) = radiance ? radiance : Util::AsReal(reflections.SRV);
		uavs.at(0) = skyIBLTexture->uav.get();

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		if (diffuseIBLShader) {
			context->CSSetShader(diffuseIBLShader, nullptr, 0);
			CS_GPU_PASS("IBL::SkyDiffuseIBL");
			context->Dispatch(1, 1, 1);
		}
	}

	// Reset
	{
		srvs.fill(nullptr);
		uavs.fill(nullptr);
		samplers.fill(nullptr);

		context->CSSetSamplers(0, (uint)samplers.size(), samplers.data());
		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(nullptr, nullptr, 0);
	}

	// Set PS shader resource
	{
		ID3D11ShaderResourceView* views[2]{ envIBLTexture->srv.get(), skyIBLTexture->srv.get() };
		context->PSSetShaderResources(76, 2, views);
	}
}

void IBL::SetupResources()
{
	GetDiffuseIBLCS();
	if (!globals::features::dx12Control.WantsMaterialPreparation())
		capture.Setup();

	{
		D3D11_TEXTURE2D_DESC texDesc{
			.Width = 3,
			.Height = 1,
			.MipLevels = 1,
			.ArraySize = 1,
			.Format = DXGI_FORMAT_R16G16B16A16_FLOAT,
			.SampleDesc = { 1, 0 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
			.CPUAccessFlags = 0,
			.MiscFlags = 0
		};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = {
				.MostDetailedMip = 0,
				.MipLevels = texDesc.MipLevels }
		};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};

		envIBLTexture = new Texture2D(texDesc, "IBL::EnvironmentIrradiance");
		envIBLTexture->CreateSRV(srvDesc);
		envIBLTexture->CreateUAV(uavDesc);
		skyIBLTexture = new Texture2D(texDesc, "IBL::SkyIrradiance");
		skyIBLTexture->CreateSRV(srvDesc);
		skyIBLTexture->CreateUAV(uavDesc);
	}

	auto device = globals::d3d::device;

	logger::debug("Loading static Diffuse IBL textures...");
	{
		DirectX::ScratchImage image;
		try {
			std::filesystem::path path = "Data\\Shaders\\IBL\\DiffuseIBL.dds";

			DX::ThrowIfFailed(LoadFromDDSFile(path.c_str(), DirectX::DDS_FLAGS_NONE, nullptr, image));
		} catch (const DX::com_exception& e) {
			logger::error("{}", e.what());
			return;
		}

		ID3D11Resource* pResource = nullptr;
		try {
			DX::ThrowIfFailed(CreateTexture(device,
				image.GetImages(), image.GetImageCount(),
				image.GetMetadata(), &pResource));
		} catch (const DX::com_exception& e) {
			logger::error("{}", e.what());
			return;
		}

		staticDiffuseIBLTexture = eastl::make_unique<Texture2D>(reinterpret_cast<ID3D11Texture2D*>(pResource), "IBL::StaticDiffuse");

		staticDiffuseIBLTexture->desc.MiscFlags |= D3D11_RESOURCE_MISC_TEXTURECUBE;

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = staticDiffuseIBLTexture->desc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE,
			.TextureCube = {
				.MostDetailedMip = 0,
				.MipLevels = 1 }
		};
		staticDiffuseIBLTexture->CreateSRV(srvDesc);
	}

	logger::debug("Loading static Specular IBL textures...");
	{
		DirectX::ScratchImage image;
		try {
			std::filesystem::path path = "Data\\Shaders\\IBL\\SpecIBL.dds";

			DX::ThrowIfFailed(LoadFromDDSFile(path.c_str(), DirectX::DDS_FLAGS_NONE, nullptr, image));
		} catch (const DX::com_exception& e) {
			logger::error("{}", e.what());
			return;
		}

		ID3D11Resource* pResource = nullptr;
		try {
			DX::ThrowIfFailed(CreateTexture(device,
				image.GetImages(), image.GetImageCount(),
				image.GetMetadata(), &pResource));
		} catch (const DX::com_exception& e) {
			logger::error("{}", e.what());
			return;
		}

		staticSpecularIBLTexture = eastl::make_unique<Texture2D>(reinterpret_cast<ID3D11Texture2D*>(pResource), "IBL::StaticSpecular");

		staticSpecularIBLTexture->desc.MiscFlags |= D3D11_RESOURCE_MISC_TEXTURECUBE;

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = staticSpecularIBLTexture->desc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE,
			.TextureCube = {
				.MostDetailedMip = 0,
				.MipLevels = 8 }
		};
		staticSpecularIBLTexture->CreateSRV(srvDesc);
	}
}

void IBL::ClearShaderCache()
{
	diffuseIBLCS.Reset();
	dynamicIBLValid = false;
	capture.Invalidate();
}

ID3D11ComputeShader* IBL::GetDiffuseIBLCS()
{
	std::vector<std::pair<const char*, const char*>> defines;
	if (globals::features::dynamicCubemaps.loaded)
		defines.push_back({ "DYNAMIC_CUBEMAPS", nullptr });
	return diffuseIBLCS.Get(L"Data\\Shaders\\IBL\\DiffuseIBLCS.hlsl", defines, "cs_5_0");
}

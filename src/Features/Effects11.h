#pragma once

#include "Buffer.h"
#include "Effects11/SceneHandoff.h"

#include <array>
#include <memory>
#include <winrt/base.h>

// C4324: the aligned PerFrame cache member pads the struct
#pragma warning(push)
#pragma warning(disable: 4324)

struct Effects11 : Feature
{
public:
	virtual inline std::string GetName() override { return "Effects11"; }
	virtual inline std::string GetShortName() override { return "Effects11"; }
	virtual inline std::string GetDisplayName() override { return "Effects 11"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kPostProcessing; }
	virtual inline std::string_view GetShaderDefineName() override { return "EFFECTS11"; }
<<<<<<< HEAD
	virtual inline bool HasShaderDefine(RE::BSShader::Type) override { return true; }
	virtual bool SupportsVR() override { return true; }
||||||| 12b261724
	virtual inline bool HasShaderDefine(RE::BSShader::Type) override { return true; }
=======
	bool HasShaderDefine(RE::BSShader::Type) override;
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
<<<<<<< HEAD
			T("feature.effects11.description", "Effects 11 loads ENBSeries-compatible FX files, including ENB Extender presets.\nThis allows for advanced post-processing effects and visual enhancements using DirectX 11 Effect (.fx) files."),
			{ T("feature.effects11.key_feature_1", "ENBSeries and ENB Extender preset support"),
				T("feature.effects11.key_feature_2", "DirectX 11 Effect file loading"),
				T("feature.effects11.key_feature_3", "Advanced post-processing pipeline"),
				T("feature.effects11.key_feature_4", "Custom technique execution"),
||||||| 12b261724
			T("feature.effects11.description", "Effects 11 provides a framework for loading and executing ENBSeries-compatible FX effect files.\nThis allows for advanced post-processing effects and visual enhancements using DirectX 11 Effect (.fx) files."),
			{ T("feature.effects11.key_feature_1", "ENBSeries-compatible FX support"),
				T("feature.effects11.key_feature_2", "DirectX 11 Effect file loading"),
				T("feature.effects11.key_feature_3", "Advanced post-processing pipeline"),
				T("feature.effects11.key_feature_4", "Custom technique execution"),
=======
			T("feature.effects11.description", "Effects 11 loads ENBSeries-compatible FX post-processing. Install multiple presets under Data/SKSE/Plugins/CommunityShaders/Effects11/Presets/<Name>/ (each with enbseries.ini + enbseries/), use a classic root/Data enbseries install as Legacy, or ship effects11/ inside a unified Presets pack. Hotswap does not modify root files."),
			{ T("feature.effects11.key_feature_1", "ENBSeries-compatible FX support"),
				T("feature.effects11.key_feature_2", "Multi-preset library with in-game hotswap"),
				T("feature.effects11.key_feature_3", "Legacy root/Data enbseries support"),
				T("feature.effects11.key_feature_4", "Advanced post-processing pipeline"),
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
				T("feature.effects11.key_feature_5", "Dynamic UI variable system") }
		};
	}

<<<<<<< HEAD
	struct Settings
	{
		bool IgnorePresetParticles = false;
		std::string presetLocation;  // relative to game root (see PresetManager::ToRelativeKey); "" = auto-resolve
	};
	Settings settings;

||||||| 12b261724
=======
	/** @brief The active preset belongs to the active preset pack, so nothing goes in the user config. */
	virtual bool UsesMainSettings() const override { return false; }

>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
	struct alignas(16) PerFrame
	{
		uint Enable;
		float ColorPow;
		float LightSpriteIntensity;
		float FireIntensity;

		float FireCurve;
		float pad0[3];

		float CloudsCurve;
		float CloudsDesaturation;
		float CloudsEdgeIntensity;
		float CloudsEdgeMoonMultiplier;

		uint EnableProceduralSun;
		float ProceduralSunDiskCos;
		float ProceduralSunDiskRadiance;
		float ProceduralSunEdgeSoftness;

<<<<<<< HEAD
		float ProceduralSunCoronaFalloff;
		float ProceduralSunCoronaScale;
		float SkyScaleMultiplier = 1.0f;
		float ProceduralSunPad;

||||||| 12b261724
		float ProceduralSunCoronaFalloff;
		float ProceduralSunCoronaScale;
=======
		float ProceduralSunHaloCos;
		float ProceduralSunHaloIntensity;
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
		uint UseProceduralGradientWeights;
		float ProceduralGradientWeightCurve;
		float LightSpriteCurve;
<<<<<<< HEAD
		uint EnableParticle;
||||||| 12b261724
		float pad1[3];
=======
		float ProceduralSunHaloFalloff;
		float ProceduralSunRadianceLimit;
		float pad1;
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb

		float ParticleIntensity;
		float ParticleLightingInfluence;
		float ParticleAmbientInfluence;
		float ParticlePointLightingInfluence;

		uint EnableVolumetricRays;
		float VolumetricRaysIntensity;
		float VolumetricRaysDensity;
		float VolumetricRaysSkyColorAmount;

		float VolumetricRaysDesaturation;
		float3 VolumetricRaysColorFilter;

		uint EnableWater;
		float WaterWavesAmplitude;
		float WaterMuddiness;
		float WaterSunLightingMultiplier;

		float WaterSunSpecularMultiplier;
		float WaterFresnelMin;
		float WaterFresnelMax;
		float WaterFresnelMultiplier;

		float WaterReflectionAmount;
		float WaterPad0;
		float WaterPad1;
		float WaterPad2;

		uint EnableCloudsScattering;
		float SkyScatteringIntensity;
		float SkyScatteringShadowAmount;
		float SkyScatteringAmount;

		float3 SkyScatteringColor;
		float SkyScatteringDustDarkening;

		float3 SkyScatteringDustTint;
		float SkyScatteringDustVolume;

		float3 SkyScatteringSunDirection;
		float SkyScatteringSunVisibility;

		float SkyScatteringHorizonRange;
		float SkyScatteringAtmosphereThickness;
		float SkyScatteringAirGlowIntensity;
		float SkyScatteringAirGlowRange;

		float SkyScatteringSunGlowIntensity;
		float SkyScatteringSunGlowRange;
		float SkyScatteringMoonGlowAmount;
		float SkyScatteringMoonGlowRange;

		float SkyScatteringSunIntensity;
		float CloudsLightingSunIntensity;
		float CloudsLightingMoonIntensity;
		uint EnableCloudsLightingFromMoon;

		uint CalculateCloudsEdgeFromScattering;
		float CloudsLightingDesaturation;
		float CloudsLightingForwardScattering;
		float CloudsLightingDensity;

		float3 CloudsColorFilter;
		float CloudsIntensity;

		float CloudsVertexAlphaBoost;
		float CloudsEdgeClamp;
		float CloudsEdgeFadePower;
		float SunBillboardTan;

		float MasserBillboardTan;
		float SecundaBillboardTan;
<<<<<<< HEAD
		float SkyScatteringPad0;
		float SkyScatteringPad1;
||||||| 12b261724
		float2 SkyScatteringPad0;

		uint EnableWater;
		float WaterWavesAmplitude;
		float WaterMuddiness;
		float WaterSunLightingMultiplier;

		float WaterSunSpecularMultiplier;
		float WaterFresnelMin;
		float WaterFresnelMax;
		float WaterFresnelMultiplier;

		float WaterReflectionAmount;
		float WaterPad0;
		float WaterPad1;
		float WaterPad2;

		float StarsCurve;
		float StarsIntensity;
		float MoonCurve;
		uint EnableAnimatedStars;

		float StarsAnimationTime;
		float StarsAnimationDensity;
		float StarsAnimationIntensity;
		float AuroraIntensity;

		float AuroraCurve;
		uint FixBlackCrush;
		float2 NightSkyPad0;
=======
		float SkyScaleMultiplier = 1.0f;
		float SkyScatteringPad0;

		uint EnableWater;
		float WaterWavesAmplitude;
		float WaterMuddiness;
		float WaterSunLightingMultiplier;

		float WaterSunSpecularMultiplier;
		float WaterFresnelMin;
		float WaterFresnelMax;
		float WaterFresnelMultiplier;

		float WaterReflectionAmount;
		float WaterPad0;
		float WaterPad1;
		float WaterPad2;

		float StarsCurve;
		float StarsIntensity;
		float MoonCurve;
		uint EnableAnimatedStars;

		float StarsAnimationTime;
		float StarsAnimationDensity;
		float StarsAnimationIntensity;
		float AuroraIntensity;

		float AuroraCurve;
		uint FixBlackCrush;
		float2 NightSkyPad0;
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb

		float3 VolumetricFogColorFilter;
		float VolumetricFogIntensity;

		float VolumetricFogCurve;
		float VolumetricFogOpacity;
		float VolumetricFogShadowAmount;
		uint VolumetricFogEnableLighting;

		float3 VolumetricRaysSkyColor;  ///< The weather's horizon colour
		uint VolumetricFogEnableShadows;

		float StarsCurve;
		float StarsIntensity;
		float MoonCurve;
		uint EnableAnimatedStars;

		float StarsAnimationTime;
		float StarsAnimationDensity;
		float StarsAnimationIntensity;
		float AuroraIntensity;

		float AuroraCurve;
		float3 NightSkyPad;

		uint VolumetricRaysSampleCount;
		uint GameVolumetricRaysDisableDithering;
		float2 GameVolumetricRaysPad0;

		float3 GameVolumetricRaysLightScale;  ///< Ungraded over graded sunlight, so game VL stays lit by the weather's own sunlight
		float GameVolumetricRaysCustomContribution;

		float3 GameVolumetricRaysCustomColor;
		float GameVolumetricRaysPad1;
	};
	static_assert(sizeof(PerFrame) % 16 == 0);
	static_assert(offsetof(PerFrame, StarsCurve) % 16 == 0);
	static_assert(offsetof(PerFrame, VolumetricFogColorFilter) % 16 == 0);
	static_assert(offsetof(PerFrame, VolumetricRaysSkyColor) % 16 == 0);
	static_assert(offsetof(PerFrame, VolumetricRaysSampleCount) % 16 == 0);
	static_assert(offsetof(PerFrame, GameVolumetricRaysLightScale) % 16 == 0);
	static_assert(offsetof(PerFrame, GameVolumetricRaysCustomColor) % 16 == 0);
	static_assert(offsetof(PerFrame, EnableCloudsScattering) % 16 == 0);
	static_assert(offsetof(PerFrame, SkyScatteringColor) % 16 == 0);
	static_assert(offsetof(PerFrame, SkyScatteringDustTint) % 16 == 0);
	static_assert(offsetof(PerFrame, SkyScatteringSunDirection) % 16 == 0);
	static_assert(offsetof(PerFrame, SkyScatteringSunIntensity) % 16 == 0);
	static_assert(offsetof(PerFrame, CloudsColorFilter) % 16 == 0);
	static_assert(offsetof(PerFrame, MasserBillboardTan) % 16 == 0);

	bool enableEffect = false;

<<<<<<< HEAD
	bool volumetricRaysFailed = false;
	bool sunRaysFailed = false;
	bool tonemapFailed = false;
	winrt::com_ptr<ID3D11PixelShader> raymarchVolumetricRaysPS;
	winrt::com_ptr<ID3D11PixelShader> applyVolumetricRaysPS;
	winrt::com_ptr<ID3D11ComputeShader> blurHCS;
	winrt::com_ptr<ID3D11ComputeShader> blurVCS;
	winrt::com_ptr<ID3D11BlendState> scatteringBlendState;
||||||| 12b261724
	ID3D11PixelShader* raymarchVolumetricRaysPS = nullptr;
	ID3D11PixelShader* applyVolumetricRaysPS = nullptr;
	ID3D11ComputeShader* blurHCS = nullptr;
	ID3D11ComputeShader* blurVCS = nullptr;
	winrt::com_ptr<ID3D11BlendState> additiveBlendState;
=======
	/** @brief Whether the active preset hands this feature back to CS through its manifest sceneControl. */
	bool IsHandedOff(E11Handoff::Feature feature) const { return handedOff[static_cast<size_t>(feature)]; }
	/** @brief Sets the runtime handoff flag; UnifiedPresetCatalog owns persisting it to the manifest. */
	void SetHandedOff(E11Handoff::Feature feature, bool value) { handedOff[static_cast<size_t>(feature)] = value; }
	/** @brief Whether Effects 11 drives a feature it can hand to CS. */
	bool OwnsFeature(E11Handoff::Feature feature) const { return loaded && enableEffect && !IsHandedOff(feature); }

	ID3D11PixelShader* raymarchVolumetricRaysPS = nullptr;
	ID3D11PixelShader* applyVolumetricRaysPS = nullptr;
	ID3D11ComputeShader* blurHCS = nullptr;
	ID3D11ComputeShader* blurVCS = nullptr;
	winrt::com_ptr<ID3D11BlendState> additiveBlendState;
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb

	std::unique_ptr<Texture2D> vlTexA;
	std::unique_ptr<Texture2D> vlTexB;
	std::unique_ptr<Texture2D> vlDepthHalf;
	std::unique_ptr<ConstantBuffer> vlBlurCB;

	winrt::com_ptr<ID3D11PixelShader> sunRaysMaskPS;
	winrt::com_ptr<ID3D11PixelShader> sunRaysBlurPS;
	winrt::com_ptr<ID3D11PixelShader> sunRaysCompositePS;
	std::unique_ptr<Texture2D> sunRaysTexA;
	std::unique_ptr<Texture2D> sunRaysTexB;
	std::unique_ptr<ConstantBuffer> sunRaysCB;

	/** @brief Sun color after the preset's sun desaturation and filter, normalized to a peak of 1; tints the sky scattering. */
	float3 scatteringSunColor = { 1.0f, 1.0f, 1.0f };
	/** @brief Sun direction used for sky scattering; mirrored below the horizon while the sun disc is hidden. */
	float3 scatteringSunDirection = { 0.0f, 0.0f, 1.0f };
	/** @brief Angular radius of the [PROCEDURALSUN] disc, in radians; the adaptation sun mask covers it. */
	float proceduralSunAngularRadius = 0.0f;
	/** @brief Per-channel sunlight before over after the [ENVIRONMENT] direct lighting grade, from the last OverrideWeather. */
	float3 directLightingUngradeScale = { 1.0f, 1.0f, 1.0f };

	PerFrame GetCommonBufferData();
	void UpdateSkyScattering(PerFrame& a_data);

	virtual void DrawSettings() override;
<<<<<<< HEAD
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;
||||||| 12b261724
=======
	virtual void Load() override;
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
	virtual void SetupResources() override;
<<<<<<< HEAD
||||||| 12b261724
	virtual void Prepass() override;
=======
	virtual void PostSetupResources() override;
	virtual void Reset() override;
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
	virtual void ClearShaderCache() override;

<<<<<<< HEAD
	/** @brief Resolves settings.presetLocation against PresetManager's freshly rescanned
	 *  locations and calls SetActiveLocation. Must run after both Rescan() and
	 *  LoadSettings() -- see Initialize(). */
	void ResolveActivePresetLocation();

	/** @brief One-time preset-location discovery + selection resolution; called from
	 *  SetupResources() after settings have been loaded and before EffectManager::Initialize(). */
	void Initialize();

	/** @copydoc Feature::RegisterUxActions */
	void RegisterUxActions() override;

	/** @brief Flips the "UseEffect" GLOBAL setting; bound to the Effects 11 toggle hotkey. */
||||||| 12b261724
	/** @brief Flips the "UseEffect" GLOBAL setting; bound to the Effects 11 toggle hotkey. */
=======
	/** @brief Switches between Effects 11 and the pipeline that ran before it; bound to the Effects 11 toggle hotkey. */
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
	void ToggleEnabled();
	/** @brief Writes the "UseEffect" GLOBAL setting; go through PostProcessingMode::Set to keep the pipelines exclusive. */
	void SetUseEffect(bool enabled);
	/** @brief UseEffect is on in enbseries.ini; does not require a compiled preset. */
	bool IsUseEffectEnabled() const;
	/** @brief UseEffect on and a compiled preset is ready (Effects 11 actually drives the frame). */
	bool IsPresetEnabled() const;
	bool IsActive() const { return presetActive; }

	void DrawVolumetricRays();

	/** @brief Draws the ENB [RAYS] screen-space shafts from the sun, or from a moon at night, additively onto the main target. */
	void DrawSunRays();
	/** @brief Raymarches and composites preset volumetric scattering. */
	void DrawVolumetricScattering();

	void OnSkyUpdateColors(RE::Sky* a_sky);
	void OverrideWeather(RE::Sky* a_sky);
	void CheckCommonData();
	/** @copydoc Feature::WantsPointLightColorOverride */
	bool WantsPointLightColorOverride() const override { return true; }
	/** @copydoc Feature::OverridePointLightColor */
	void OverridePointLightColor(float3& a_color) override;

	struct DirectionalAmbientColors
	{
		RE::NiColor directionalAmbientColors[3][2];
	};
	void OverrideAmbientLighting(DirectionalAmbientColors& DirectionalAmbientColors);
	/**
	 * @brief During a transition between weathers with different presets, rescales the graded ambient cube towards
	 * the blend of each weather graded by its own preset values.
	 * @param a_sky The sky whose directional ambient cube a_graded was graded from.
	 * @param a_graded Ambient colours already graded at the blended preset values; corrected in place.
	 */
	void BlendGradedAmbient(RE::Sky* a_sky, DirectionalAmbientColors& a_graded);

	DirectionalAmbientColors vanillaAmbientCache{};
	DirectionalAmbientColors gradedAmbientCache{};
	RE::NiColor ambientSpecularTintCache{};
	bool ambientSpecularTintCacheValid = false;
	float ambientSpecularFresnelCache = 0.0f;
	bool ambientGradeCacheValid = false;

	void ModifySky(RE::BSRenderPass* Pass);
<<<<<<< HEAD

	/** @brief Whether Effects11 wants to replace the vanilla tonemap this frame. */
	bool WantsTonemapOwnership();
	/** @brief Runs the Effects11 chain in place of the vanilla tonemap pass. */
	bool RenderTonemap(RE::RENDER_TARGET a_input, RE::RENDER_TARGET a_output);
	/** @brief True when Effects11 produced the SDR scene for the frame being presented. */
||||||| 12b261724
	bool HandleTonemapRender(RE::RENDER_TARGET a_input, RE::RENDER_TARGET a_output);
	/** @brief True when the effect chain replaced ISHDR for the frame being presented, leaving an SDR scene for HDR Display to expand. */
=======
	/**
	 * @brief Whether Effects11 wants to replace the vanilla tonemap this frame.
	 *
	 * Queried by State::GetTonemapOwner() to arbitrate against Post Processing. Does not
	 * render anything; refreshes per-frame common data as a side effect.
	 */
	bool WantsTonemapOwnership();

	/**
	 * @brief Runs the ENB effect chain in place of the vanilla tonemap pass.
	 * @param a_input Render target holding the scene color to tonemap.
	 * @param a_output Render target receiving the tonemapped result.
	 * @return True only if the chain wrote the output; false means the caller must fall
	 *         back to the vanilla pass.
	 */
	bool RenderTonemap(RE::RENDER_TARGET a_input, RE::RENDER_TARGET a_output);

	/** @brief True when the effect chain replaced ISHDR for the frame being presented, leaving an SDR scene for HDR Display to expand. */
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
	bool ReplacedTonemapperThisFrame() const;

private:
<<<<<<< HEAD
	/** @brief Creates the scattering blend state shared by the ray passes on first use; false if creation failed. */
	bool EnsureScatteringBlendState();
||||||| 12b261724
	/** @brief Creates the additive blend state shared by the ray passes on first use; false if creation failed. */
	bool EnsureAdditiveBlendState();
=======
	/**
	 * @brief Grades the weather colours in place with the current preset values.
	 * @return The sun scattering tint (scatteringSunColor unchanged when the effect is off).
	 */
	float3 GradeSkyColors(std::span<RE::NiColor, RE::TESWeather::ColorTypes::kTotal> a_colors) const;
	/** @brief Per-weather counterpart of BlendGradedAmbient for the sky colours graded by GradeSkyColors. */
	void BlendGradedWeathers(RE::Sky* a_sky, std::span<RE::NiColor, RE::TESWeather::ColorTypes::kTotal> a_graded) const;
	bool presetActive = false;
	bool resourcesReady = false;
	/** @brief Creates the additive blend state shared by the ray passes on first use; false if creation failed. */
	bool EnsureAdditiveBlendState();
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
	/** @brief Compiles the sun ray shaders and (re)creates their half-res targets; false if anything failed. */
	bool EnsureSunRaysResources(uint32_t a_width, uint32_t a_height);

	uint tonemapReplacedFrame = UINT32_MAX;  ///< frameCount when the effect chain last wrote the tonemap output
	std::array<bool, static_cast<size_t>(E11Handoff::Feature::Count)> handedOff{};

	/** @brief Point light settings, resolved once per frame in CheckCommonData since OverridePointLightColor runs per light. */
	struct PointLightingParams
	{
		float curve = 1.0f;
		float desaturation = 0.0f;
		float intensity = 1.0f;
	} pointLighting;

	// The feature buffer is rebuilt several times per frame, so GetCommonBufferData's lookups are replayed from here
	PerFrame perFrameCache{};
	Util::FrameChecker perFrameCacheChecker;
};

#pragma warning(pop)

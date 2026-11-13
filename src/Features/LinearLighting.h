#pragma once

struct LinearLighting : Feature
{
	static LinearLighting* GetSingleton()
	{
		static LinearLighting singleton;
		return &singleton;
	}

	virtual inline std::string GetName() override { return "Linear Lighting"; }
	virtual std::string GetDisplayName() override { return T("feature.linear_lighting.name", "Linear Lighting"); }
	virtual inline std::string GetShortName() override { return "LinearLighting"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kLighting; }
	/** @brief Returns a localized description and list of key features for the UI summary panel. */
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.linear_lighting.description", "Linear Lighting does internal color space conversion to improve lighting calculation accuracy."),
<<<<<<< HEAD
			{ T("feature.linear_lighting.key_feature_1", "Semantic authored-color conversion"),
||||||| 12b261724
			{ T("feature.linear_lighting.key_feature_1", "Customizable gamma correction"),
=======
			{ T("feature.linear_lighting.key_feature_1", "Managed color input encoding"),
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
				T("feature.linear_lighting.key_feature_2", "Corrects lighting calculations"),
				T("feature.linear_lighting.key_feature_3", "Makes PBR really work") } };
	};

	virtual bool SupportsVR() override { return true; };
	virtual bool IsCore() const override { return true; };

	/** @brief ENABLE_LL is a compile-time define; emit it only when the feature is enabled. */
	virtual inline std::string_view GetShaderDefineName() override { return "ENABLE_LL"; }
	virtual inline bool HasShaderDefine(RE::BSShader::Type) override { return IsLinearLightingActive(); }
	virtual std::vector<std::pair<std::string_view, std::string_view>> GetShaderDefineOptions() override;
	virtual std::vector<std::pair<std::string_view, std::string_view>> GetCommonShaderDefines() override;

	static constexpr float FireEffectCurveMin = 0.1f;
	static constexpr float FireEffectCurveMax = 8.0f;

	struct Settings
	{
<<<<<<< HEAD
		uint enableLinearLighting = false;
		uint enableACEScg = false;
		float conversionSaturation = 0.92f;
||||||| 12b261724
		uint enableLinearLighting = false;
		float lightGamma = 1.8f;
		float colorGamma = 1.8f;
		float emitColorGamma = 1.8f;
		float glowmapGamma = 1.8f;
		float ambientGamma = 1.8f;
		float fogGamma = 1.97f;
		float fogAlphaGamma = 1.8f;
		float effectGamma = 1.4f;
		float effectAlphaGamma = 1.55f;
		float skyGamma = 1.8f;
		float waterGamma = 1.8f;
		float vlGamma = 1.8f;
=======
		uint enableLinearLighting = true;
		uint enableACEScg = false;
		float gameGamma = 1.6f;
		float diffuseGamma = 2.2f;
		float diffuseMidReflectance = 0.2871746f;
		float diffuseWhiteReflectance = 1.0f;
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb

		// Lighting multipliers
<<<<<<< HEAD
		float ambientMult = 0.32f;
||||||| 12b261724
		float vanillaDiffuseColorMult = 1.0f;
		float directionalLightMult = 1.0f;
		float pointLightMult = 1.0f;
		float ambientMult = 1.0f;
		float emitColorMult = 1.0f;
		float glowmapMult = 0.66f;

		// Effect multipliers
		float effectLightingMult = 0.32f;
		float membraneEffectMult = 1.0f;
		float bloodEffectMult = 1.0f;
		float projectedEffectMult = 1.0f;
		float deferredEffectMult = 1.0f;
		float otherEffectMult = 1.0f;
=======
		float directionalLightMult = 1.0f;
		float pointLightMult = 1.0f;
		float ambientMult = 1.0f;
		float emitColorMult = 1.0f;
		float glowmapMult = 0.66f;

		// Effect multipliers
		float effectLightingMult = 0.32f;
		float membraneEffectMult = 1.0f;
		float bloodEffectMult = 1.0f;
		float projectedEffectMult = 1.0f;
		float deferredEffectMult = 1.0f;
		float otherEffectMult = 1.0f;

		// Effect classes Effects 11 derives from shader flags
		float particleEffectMult = 1.0f;
		float lightSpriteEffectMult = 1.0f;
		float fireEffectMult = 1.0f;
		float fireEffectCurve = 1.0f;
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
	} settings;

	struct alignas(16) PerFrameData
	{
<<<<<<< HEAD
		uint enableLinearLighting;
		uint enableACEScg;
		uint isDirLightLinear;
		float dirLightMult;
		float diffuseGamma;
		float diffuseCurve;
		float diffuseWhiteReflectance;
		float conversionSaturation;
		RE::NiColor effectLightingColor;
||||||| 12b261724
		uint enableLinearLighting;
		uint isDirLightLinear;
		float dirLightMult;
		float lightGamma;
		float colorGamma;
		float emitColorGamma;
		float glowmapGamma;
		float ambientGamma;
		float fogGamma;
		float fogAlphaGamma;
		float effectGamma;
		float effectAlphaGamma;
		float skyGamma;
		float waterGamma;
		float vlGamma;
		float vanillaDiffuseColorMult;
		float directionalLightMult;
		float pointLightMult;
=======
		uint isMainOrLoadingMenu;
		float diffuseGamma;
		float directionalLightMult;
		float pointLightMult;
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
		float ambientMult;
<<<<<<< HEAD
		RE::NiColor skyStaticsColor;
		float pad;
||||||| 12b261724
		float emitColorMult;
		float glowmapMult;
		float effectLightingMult;
		float membraneEffectMult;
		float bloodEffectMult;
		float projectedEffectMult;
		float deferredEffectMult;
		float otherEffectMult;
		uint pad0;
=======
		float glowmapMult;
		float effectLightingMult;
		float membraneEffectMult;
		float bloodEffectMult;
		float projectedEffectMult;
		float deferredEffectMult;
		float otherEffectMult;
		float particleEffectMult;
		float lightSpriteEffectMult;
		float fireEffectMult;
		float fireEffectCurve;
		float diffuseCurve;
		float diffuseWhiteReflectance;
		float2 pad;
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
	};
	STATIC_ASSERT_ALIGNAS_16(PerFrameData);
	static_assert(sizeof(PerFrameData) == 0x40);
	static_assert(offsetof(PerFrameData, conversionSaturation) == 0x1C);
	static_assert(offsetof(PerFrameData, skyStaticsColor) == 0x30);

	static constexpr std::array<float, 3> kNoProjectedMaterialColorScale{ -1.0f, -1.0f, -1.0f };

<<<<<<< HEAD
	struct alignas(16) PerGeometryData
	{
		float emissiveMult;
		std::array<float, 3> projectedMaterialColorScale = kNoProjectedMaterialColorScale;
	};
	static_assert(sizeof(PerGeometryData) == 16);

	std::array<std::array<float, 3>, 2> lodProjectedMaterialColorScales{ kNoProjectedMaterialColorScale, kNoProjectedMaterialColorScale };

	ConstantBuffer* PerGeometryCB = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> sceneGammaDecodeCS;
	bool sceneGammaActive = false;

	uint isDirLightLinear = false;
	float dirLightMult = 1.0f;
	RE::NiColor effectLightingColor{ 1.0f, 1.0f, 1.0f };
	RE::NiColor skyStaticsColor{ 1.0f, 1.0f, 1.0f };
	RE::NiColor weatherEffectLightingSource{};
	RE::NiColor weatherSkyStaticsSource{};
	bool weatherLightingColorsInitialized = false;
	float weatherConversionSaturation = 1.0f;

	/** @brief Draws the Linear Lighting controls and lighting multipliers. */
||||||| 12b261724
	struct alignas(16) PerGeometryData
	{
		float emissiveMult;
		float pad0[3];
	};

	ConstantBuffer* PerGeometryCB = nullptr;

	uint isDirLightLinear = false;
	float dirLightMult = 1.0f;

	/** @brief Draws the ImGui settings UI for gamma correction and lighting multiplier configuration. */
=======
	/** @brief Draws the ImGui settings UI for color management and lighting multiplier configuration. */
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
	virtual void DrawSettings() override;
	virtual void PostSetupResources() override;
	/** @brief Applies pending shader settings at the frame boundary. */
	virtual void Reset() override;
	virtual void ClearShaderCache() override;
	virtual void ModifySharedLighting(SharedLighting& lighting) override;
	virtual void Load() override;

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;

	virtual void RestoreDefaultSettings() override;

<<<<<<< HEAD
	/** @brief Reads the directional light multiplier and projected LOD material colors. */
	virtual void Prepass() override;
	/** @brief Installs the lighting geometry hook. */
	virtual void PostPostLoad() override;

	/** @brief Creates the emissive data buffer and compiles the scene gamma decode shader. */
	virtual void SetupResources() override;
	/** @brief Recompiles the scene gamma decode shader after a shader-cache clear. */
	virtual void ClearShaderCache() override;
	/** @brief Marks kMAIN as gamma-domain storage for the main world-rendering interval. */
	virtual void OnWorldRenderBegin() override;
	/** @brief Decodes the completed gamma-domain world scene in place. */
	virtual void OnWorldRenderEnd(RE::RENDER_TARGET a_renderTarget) override;
	/** @brief Finishes any pending scene decode before post-processing consumes its input. */
	virtual void OnBeforePostProcessing(RE::RENDER_TARGET a_renderTarget) override;
	/** @brief Uses a linear clear color and target for cubemap rendering, restoring both on scope exit. */
	virtual std::function<void()> OnReflectionsRenderBegin() override;

	/** @brief Populates and returns the per-frame constant buffer data with gamma and multiplier settings. */
||||||| 12b261724
	/** @brief Reads the directional light multiplier from ImageSpaceManager during the prepass. */
	virtual void Prepass() override;
	/** @brief Installs the BSLightingShader geometry setup hook. */
	virtual void PostPostLoad() override;

	/** @brief Creates the per-geometry constant buffer for emissive multiplier data. */
	virtual void SetupResources() override;

	/** @brief Populates and returns the per-frame constant buffer data with gamma and multiplier settings. */
=======
	/** @brief Populates and returns the per-frame constant buffer data with color-management and multiplier settings. */
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
	PerFrameData GetCommonBufferData();
	/** @brief Returns whether the engine and shaders should currently use linear lighting data. */
	bool IsLinearLightingActive() const;
	/** @brief Compiles the scene gamma decode shader when the target format supports typed UAV loads. */
	void CompileSceneGammaDecodeShader();

	/** @brief Caches linear copies of the interpolated weather colors used by effect meshes. */
	virtual void OnWeatherColorsUpdated(RE::Sky* a_sky) override;

<<<<<<< HEAD
	/**
	 * @brief Decodes an authored Skyrim color into linear sRGB.
	 * @param inColor The input color in gamma space.
	 * @param saturation Luminance-preserving saturation of the decoded input.
	 * @return The color converted to linear space.
	 */
	static RE::NiColor DecodeAuthoredColor(RE::NiColor inColor, float saturation = 1.0f);
||||||| 12b261724
	/**
	 * @brief Converts an NiColor from gamma space to linear space using the specified gamma value.
	 * @param inColor The input color in gamma space.
	 * @param gamma The gamma exponent to apply.
	 * @return The color converted to linear space.
	 */
	RE::NiColor ColorToLinear(RE::NiColor inColor, float gamma);
=======
	bool IsLinearLightingActive() const;
	bool IsACEScgActive() const { return IsLinearLightingActive() && configuredACEScg; }
	RE::NiColor SRGBToWorking(RE::NiColor color) const;
	void SRGBToWorking(float* color) const;
	RE::NiColor LightColorToWorking(const RE::NiLight* light, bool effect = false) const;
	void SetSunlightColor(RE::NiLight* light, RE::NiColor color);
	void ClearSunlightColor(const RE::NiLight* light);
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb

<<<<<<< HEAD
	/**
	 * @brief Uploads emissive and projected material data during lighting geometry setup.
	 * @param a_pass The render pass whose lighting properties to read.
	 */
	void BSLightingShader_SetupGeometry(RE::BSRenderPass* a_pass);
||||||| 12b261724
	/**
	 * @brief Uploads emissive multiplier data to the per-geometry constant buffer during shader setup.
	 * @param a_pass The render pass whose lighting properties to read.
	 */
	void BSLightingShader_SetupGeometry(RE::BSRenderPass* a_pass);
=======
private:
	void ApplyGameGamma();
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb

<<<<<<< HEAD
	/** @brief Contains the lighting shader hook implementation. */
	struct Hooks;
||||||| 12b261724
	/** @brief Contains the BSLightingShader geometry setup hook implementation. */
	struct Hooks;
=======
	std::string gameGammaDefine = "1.6";
	float configuredGameGamma = 1.6f;
	bool applyGameGamma = false;
	const RE::NiLight* workingSunlight = nullptr;
	RE::NiColor workingSunlightColor{};

	bool configuredLinearLighting = false;
	bool configuredACEScg = false;
	bool resourcesReady = false;
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
};

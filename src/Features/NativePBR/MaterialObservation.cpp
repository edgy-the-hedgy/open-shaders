#include "MaterialObservation.h"

#include "TruePBR/BSLightingShaderMaterialPBR.h"
#include "TruePBR/BSLightingShaderMaterialPBRLandscape.h"
#include "Util.h"

namespace
{
	NativePBR::Glint SnapshotGlint(const GlintParameters& a_glint)
	{
		return { a_glint.enabled, a_glint.screenSpaceScale, a_glint.logMicrofacetDensity,
			a_glint.microfacetRoughness, a_glint.densityRandomization };
	}

	void SnapshotPBRBase(const RE::BSLightingShaderMaterialBase& a_material, NativePBR::MaterialData& a_result)
	{
		for (size_t index = 0; index < a_result.uvOffsetScale.size(); ++index) {
			a_result.uvOffsetScale[index] = { a_material.texCoordOffset[index].x, a_material.texCoordOffset[index].y,
				a_material.texCoordScale[index].x, a_material.texCoordScale[index].y };
		}
		a_result.materialAlpha = a_material.materialAlpha;
		a_result.clampMode = static_cast<uint32_t>(a_material.textureClampMode);
	}

	NativePBR::MaterialData SnapshotPBRMesh(const BSLightingShaderMaterialPBR& a_material)
	{
		NativePBR::MaterialData result;
		result.kind = NativePBR::MaterialKind::Mesh;
		SnapshotPBRBase(a_material, result);
		result.pbrFlags = a_material.pbrFlags.underlying();
		result.nifThinSurfaceFlags = a_material.nifThinSurfaceFlags.underlying();
		result.subsurfaceBalance = a_material.subsurfaceBalance;
		result.thinSurfaceParameters = a_material.thinSurfaceParameters;
		result.fuzzRoughness = a_material.fuzzRoughness;
		result.fuzzRoughnessTexture = a_material.fuzzRoughnessTexture;
		result.roughnessScale = a_material.GetRoughnessScale();
		result.displacementScale = a_material.GetDisplacementScale();
		result.specularLevel = a_material.GetSpecularLevel();
		if (a_material.pbrFlags.any(PBRFlags::TwoLayer)) {
			const auto& color = a_material.GetCoatColor();
			result.coatColor = { color.red, color.green, color.blue };
			result.coatStrength = a_material.GetCoatStrength();
			result.coatRoughness = a_material.GetCoatRoughness();
			result.coatSpecularLevel = a_material.GetCoatSpecularLevel();
		} else {
			if (a_material.pbrFlags.any(PBRFlags::Subsurface)) {
				const auto& color = a_material.GetSubsurfaceColor();
				result.subsurfaceColor = { color.red, color.green, color.blue };
				result.subsurfaceOpacity = a_material.GetSubsurfaceOpacity();
			}
			if (a_material.pbrFlags.any(PBRFlags::Fuzz)) {
				const auto& color = a_material.GetFuzzColor();
				result.fuzzColor = { color.red, color.green, color.blue };
				result.fuzzWeight = a_material.GetFuzzWeight();
			}
		}
		result.glint = SnapshotGlint(a_material.GetGlintParameters());

		const std::array textures{ a_material.diffuseTexture.get(), a_material.normalTexture.get(), a_material.rmaosTexture.get(),
			a_material.emissiveTexture.get(), a_material.displacementTexture.get(), a_material.featuresTexture0.get(), a_material.featuresTexture1.get() };
		for (size_t role = 0; role < textures.size(); ++role) {
			if (!textures[role])
				continue;
			result.textureResourceNames[role] = textures[role]->name.c_str();
			const auto* rendererTexture = textures[role]->rendererTexture;
			if (!rendererTexture || !rendererTexture->texture)
				continue;
			winrt::com_ptr<ID3D11Texture2D> texture;
			if (FAILED(Util::AsReal(rendererTexture->texture)->QueryInterface(IID_PPV_ARGS(texture.put()))))
				continue;
			D3D11_TEXTURE2D_DESC desc{};
			texture->GetDesc(&desc);
			result.textureResources[role] = { desc.Width, desc.Height, desc.MipLevels, desc.ArraySize, uint32_t(desc.Format),
				desc.MiscFlags, desc.BindFlags, uint32_t(desc.Usage), desc.SampleDesc.Count, true };
		}

		auto* textureSet = a_material.textureSet.get();
		if (!textureSet)
			return result;
		static const auto* textureSetRTTI = REL::Relocation<const RE::NiRTTI*>{ RE::BSTextureSet::Ni_RTTI }.get();
		const auto* rtti = textureSet->GetRTTI();
		if (!rtti || !rtti->IsKindOf(textureSetRTTI))
			return result;
		constexpr std::array<uint32_t, 7> textureSlots{ 0, 1, 5, 2, 3, 7, 6 };
		for (size_t index = 0; index < textureSlots.size(); ++index) {
			const auto* path = textureSet->GetTexturePath(static_cast<RE::BSTextureSet::Texture>(textureSlots[index]));
			if (path)
				result.texturePaths[index] = path;
		}
		result.texturePathsComplete = !result.texturePaths[0].empty() &&
		                              !result.texturePaths[1].empty() && !result.texturePaths[2].empty();
		return result;
	}

	NativePBR::MaterialData SnapshotPBRLandscape(const BSLightingShaderMaterialPBRLandscape& a_material)
	{
		NativePBR::MaterialData result;
		result.kind = NativePBR::MaterialKind::Landscape;
		SnapshotPBRBase(a_material, result);
		result.landscapeTileCount = std::min(uint32_t(a_material.numLandscapeTextures),
			uint32_t(BSLightingShaderMaterialPBRLandscape::NumTiles));
		for (uint32_t tile = 0; tile < result.landscapeTileCount; ++tile) {
			auto& output = result.landscapeTiles[tile];
			output.isPbr = a_material.isPbr[tile];
			output.roughnessScale = a_material.roughnessScales[tile];
			output.displacementScale = a_material.displacementScales[tile];
			output.specularLevel = a_material.specularLevels[tile];
			const std::array textures{ a_material.landscapeBaseColorTextures[tile].get(),
				a_material.landscapeNormalTextures[tile].get(), a_material.landscapeRMAOSTextures[tile].get(),
				a_material.landscapeDisplacementTextures[tile].get() };
			for (size_t role = 0; role < textures.size(); ++role) {
				if (textures[role])
					output.textureNames[role] = textures[role]->name.c_str();
			}
			output.glint = SnapshotGlint(a_material.glintParameters[tile]);
		}
		return result;
	}
}

void NativePBR::ObserveLightingMaterial(Registry& a_registry, const RE::BSLightingShaderMaterialBase& a_material, MaterialKind a_kind)
{
	const std::array<std::array<float, 4>, 2> uv{ { { a_material.texCoordOffset[0].x, a_material.texCoordOffset[0].y,
														a_material.texCoordScale[0].x, a_material.texCoordScale[0].y },
		{ a_material.texCoordOffset[1].x, a_material.texCoordOffset[1].y,
			a_material.texCoordScale[1].x, a_material.texCoordScale[1].y } } };
	if (a_kind == MaterialKind::Landscape) {
		const auto* pbr = static_cast<const BSLightingShaderMaterialPBRLandscape*>(&a_material);
		std::array<const void*, 24> textures{};
		for (size_t tile = 0; tile < std::min(pbr->numLandscapeTextures, BSLightingShaderMaterialPBRLandscape::NumTiles); ++tile) {
			textures[tile * 4] = pbr->landscapeBaseColorTextures[tile].get();
			textures[tile * 4 + 1] = pbr->landscapeNormalTextures[tile].get();
			textures[tile * 4 + 2] = pbr->landscapeRMAOSTextures[tile].get();
			textures[tile * 4 + 3] = pbr->landscapeDisplacementTextures[tile].get();
		}
		if (auto capture = a_registry.ObserveAndBegin(&a_material, uv, a_material.materialAlpha, uint32_t(a_material.textureClampMode), textures))
			a_registry.Publish(&a_material, capture, SnapshotPBRLandscape(*pbr));
	} else if (a_kind == MaterialKind::Mesh) {
		const auto* pbr = static_cast<const BSLightingShaderMaterialPBR*>(&a_material);
		auto observeTextures = [pbr] {
			const std::array sources{ pbr->diffuseTexture.get(), pbr->normalTexture.get(), pbr->rmaosTexture.get(),
				pbr->emissiveTexture.get(), pbr->displacementTexture.get(), pbr->featuresTexture0.get(), pbr->featuresTexture1.get() };
			std::array<const void*, 8> textures{};
			std::array<const void*, 14> resources{};
			for (size_t index = 0; index < sources.size(); ++index) {
				textures[index] = sources[index];
				if (sources[index] && sources[index]->rendererTexture) {
					resources[index * 2] = sources[index]->rendererTexture;
					resources[index * 2 + 1] = sources[index]->rendererTexture->texture;
				}
			}
			textures.back() = pbr->textureSet.get();
			return std::pair{ textures, resources };
		};
		const auto [textures, resources] = observeTextures();
		auto capture = a_registry.ObserveAndBegin(&a_material, uv, a_material.materialAlpha, uint32_t(a_material.textureClampMode), textures, resources);
		if (capture && pbr->diffuseTexture && pbr->normalTexture && pbr->rmaosTexture) {
			auto snapshot = SnapshotPBRMesh(*pbr);
			const auto [currentTextures, currentResources] = observeTextures();
			NativePBR::MaterialData current;
			SnapshotPBRBase(a_material, current);
			a_registry.ObserveAndBegin(&a_material, current.uvOffsetScale, current.materialAlpha, current.clampMode, currentTextures, currentResources);
			a_registry.Publish(&a_material, capture, std::move(snapshot));
		}
	}
}

void NativePBR::ObserveVisibleInstance(Registry& a_registry, const RE::BSLightingShaderProperty& a_property, RE::BSGeometry& a_geometry)
{
	if (a_geometry.name.empty())
		return;
	if (auto* reference = a_geometry.GetUserData(); reference && !reference->IsDynamicForm()) {
		if (const auto* file = reference->GetFile(0)) {
			const auto& geometryData = a_geometry.GetGeometryRuntimeData();
			InstanceProperties instance;
			instance.shaderFlags = a_property.flags.underlying();
			instance.skinned = bool(geometryData.skinInstance);
			if (const auto* alpha = geometryData.alphaProperty.get()) {
				instance.alphaProperty = true;
				instance.alphaFlags = alpha->alphaFlags;
				instance.alphaThreshold = alpha->alphaThreshold;
				instance.opacity = InstanceProperties::Opacity::NonOpaque;
			}
			if (a_property.material &&
				static_cast<const RE::BSLightingShaderMaterialBase*>(a_property.material)->materialAlpha < 1.0f)
				instance.opacity = InstanceProperties::Opacity::NonOpaque;
			if (a_property.flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kVertexLighting) &&
				reference->data.objectReference && reference->data.objectReference->formType == RE::FormType::Static) {
				const auto* stat = static_cast<const RE::TESObjectSTAT*>(reference->data.objectReference);
				if (const auto* mato = stat->data.materialObj; mato && mato->directionalData.singlePass)
					if (const char* editorId = mato->GetFormEditorID(); editorId)
						instance.materialObjectEditorId = editorId;
			}
			if (const auto* color = a_property.emissiveColor; color && std::isfinite(color->red) && std::isfinite(color->green) &&
																 std::isfinite(color->blue) && std::isfinite(a_property.emissiveMult)) {
				instance.emissionKnown = true;
				instance.emissiveColor = { color->red, color->green, color->blue };
				instance.emissiveMultiplier = a_property.emissiveMult;
			}
			a_registry.ObserveInstance(a_property.material,
				std::format("{:06X}:{}", reference->GetLocalFormID(), file->GetFilename()), a_geometry.name.c_str(), instance);
		}
	}
}

#include "DX12PreparedMaterialFrame.h"
#include "DX12NativeSceneInputs.h"
#include "Features/NativePBR/MaterialCatalog.h"
#include "Features/NativePBR/MaterialInterpretation.h"
#include "RE/B/BSShaderProperty.h"

#include <algorithm>
#include <array>

namespace DX12Native
{
	namespace
	{
		using F = RE::BSShaderProperty::EShaderPropertyFlag;

		bool HasFlag(uint64_t flags, F flag)
		{
			return (flags & static_cast<uint64_t>(flag)) != 0;
		}

		void InterpretAuthoredFlags(uint64_t flags, NativePBR::MaterialData& material)
		{
			const auto has = [flags](F flag) { return HasFlag(flags, flag); };
			const auto add = [&](PBRFlags flag) { material.pbrFlags |= uint32_t(flag); };
			if (has(F::kMultiLayerParallax)) {
				add(PBRFlags::TwoLayer);
				material.coatStrength = 1.0f;
				if (has(F::kSoftLighting)) add(PBRFlags::InterlayerParallax);
				if (has(F::kBackLighting)) add(PBRFlags::CoatNormal);
				if (has(F::kEffectLighting)) add(PBRFlags::ColoredCoat);
			} else if (has(F::kBackLighting)) {
				add(PBRFlags::HairMarschner);
			} else {
				if (has(F::kAnisotropicLighting)) add(PBRFlags::ThinSurface);
				if (has(F::kEffectLighting)) add(PBRFlags::Foliage);
				material.nifThinSurfaceFlags = material.pbrFlags;
				if (has(F::kRimLighting)) add(PBRFlags::Subsurface);
				if (has(F::kSoftLighting)) add(PBRFlags::Fuzz);
				else if (has(F::kFitSlope)) material.glint.enabled = true;
			}
		}

		bool ValidVertexInputs(const DX12::PreparedMaterialSource& source)
		{
			const auto valid = [&](uint32_t offset) {
				return source.vertexStride >= 16 && offset >= 16 && offset <= source.vertexStride - 4;
			};
			return source.vertexCount && uint64_t(source.vertexCount) * source.vertexStride == source.vertexBytes.size() &&
				valid(source.uvOffset) && valid(source.normalOffset) &&
				(source.tangentOffset == UINT32_MAX || valid(source.tangentOffset)) &&
				(source.colorOffset == UINT32_MAX || valid(source.colorOffset));
		}
	}

	std::shared_ptr<const PreparedMaterialFrame> CompilePreparedMaterialFrame(const SceneInputs& inputs)
	{
		auto result = std::make_shared<PreparedMaterialFrame>();
		result->geometryRevision = inputs.geometryRevision;
		if (inputs.residency)
			result->scope = inputs.residency->requested.front().scope;
		const auto catalog = NativePBR::MaterialCatalog::Shared().GetSnapshot();
		const auto missingBinding = NativePBR::PackGpuDrawBinding(0, 0, UINT32_MAX, 0);
		result->bindings.assign(inputs.geometry.size(), missingBinding);
		result->candidateIndices.assign(inputs.geometry.size(), UINT32_MAX);
		constexpr std::array<size_t, 7> slots{ 0, 1, 5, 2, 3, 7, 6 };
		NativePBR::GpuTextureLocations locations{};
		for (auto& location : locations)
			location = { NativePBR::kGpuMissingTexture, 0 };

		for (size_t i = 0; i < inputs.geometry.size(); ++i) {
			const auto& shape = inputs.geometry[i];
			if (!shape || !shape->material) {
				++result->missing;
				continue;
			}
			++result->sources;
			const auto& source = *shape->material;
			// An observed legacy flag or a slot-5 path does not establish PBR authorship.
			if (!source.authoredShaderFlags || !HasFlag(*source.authoredShaderFlags, F::kMenuScreen)) {
				++result->unknownAuthored;
				continue;
			}
			const auto flags = *source.authoredShaderFlags;
			if (HasFlag(flags, F::kLODLandscape) || HasFlag(flags, F::kMultiTextureLandscape) ||
				HasFlag(flags, F::kFace) || HasFlag(flags, F::kHairTint) || HasFlag(flags, F::kTreeAnim) ||
				HasFlag(flags, F::kRefraction) || HasFlag(flags, F::kTempRefraction) ||
				HasFlag(flags, F::kVertexAlpha) || HasFlag(flags, F::kGrayscaleToPaletteAlpha) ||
				!ValidVertexInputs(source) || source.alpha != 1.0f || source.clampMode < 0 || source.clampMode > 3) {
				++result->limited;
				continue;
			}
			NativePBR::MaterialData material;
			material.materialAlpha = source.alpha;
			material.clampMode = uint32_t(source.clampMode);
			material.uvOffsetScale[0] = source.uvTransform;
			material.uvOffsetScale[1] = source.uvTransform;
			InterpretAuthoredFlags(flags, material);
			for (size_t role = 0; role < slots.size(); ++role)
				material.texturePaths[role] = source.texturePaths[slots[role]];
			material.texturePathsComplete = true;
			if (catalog && !source.textureSetEditorId.empty()) {
				if (const auto* config = catalog->FindTextureSet(source.textureSetEditorId)) {
					material = NativePBR::InterpretTextureSet(material, *config);
					++result->resolvedTxst;
				}
			}
			if (!(material.pbrFlags & (uint32_t(PBRFlags::TwoLayer) | uint32_t(PBRFlags::HairMarschner)))) {
				if (source.nifThinSurface)
					NativePBR::InterpretNifThinSurface(*source.nifThinSurface, material);
				if (source.nifFuzz)
					NativePBR::InterpretNifFuzz(*source.nifFuzz, material);
			}
			if (catalog && !shape->materialObjectEditorId.empty()) {
				if (const auto* config = catalog->FindMaterialObject(shape->materialObjectEditorId)) {
					const auto projected = NativePBR::InterpretMaterialObject(shape->materialObjectEditorId, *config);
					material.projectedColor = projected.baseColorScale;
					material.projectedRoughness = projected.roughness;
					material.projectedSpecularLevel = projected.specularLevel;
					material.projectedGlint = projected.glint;
					++result->resolvedMato;
				}
			}
			// Per-instance identity and geometry revision deliberately avoid cross-placement MATO aliasing.
			auto recipe = NativePBR::CompileRecipe({ shape->identity, inputs.geometryRevision, std::move(material) });
			const auto capability = NativePBR::ClassifyScreenSurface(recipe, HasFlag(source.observedShaderFlags, F::kProjectedUV));
			if (capability.issues || recipe.issues & (NativePBR::InvalidSnapshot | NativePBR::InvalidTexturePath | NativePBR::MissingRequiredTextures))
				++result->limited;
			const auto index = uint32_t(result->recipes.size());
			result->candidateIndices[i] = index;
			result->records.push_back(NativePBR::PackGpuMaterialRecord(recipe, locations));
			result->bindings[i] = NativePBR::PackGpuDrawBinding(recipe.identity, recipe.revision, UINT32_MAX, 0);
			result->recipes.push_back(std::move(recipe));
		}
		return result;
	}
}

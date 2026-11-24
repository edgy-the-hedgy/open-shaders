#include "DX12NativeRuntimeCapture.h"

#include "Globals.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstring>
#include <format>
#include <ranges>
#include <string_view>
#include <DirectXPackedVector.h>
#include "RE/B/BSLightingShaderMaterialBase.h"
#include "RE/B/BSTextureSet.h"

namespace DX12Native
{
	namespace
	{
		constexpr uint64_t kSingleCopyBytes = 1024ull * 1024;
		constexpr uint32_t kFrameCopyBytes = 2u * 1024u * 1024u;
		constexpr size_t kPacketLimit = 128;
		constexpr uint64_t kFnvPrime = 1099511628211ull;
		constexpr uint64_t kFnvOffset = 14695981039346656037ull;

		uint64_t StableObjectKey(const RuntimeGeometryCandidate& source)
		{
			if (!source.objectIdentity)
				return 0;  // No reliable per-object key: never retire a similarly named sibling.
			uint64_t hash = kFnvOffset;
			const auto add = [&](unsigned char value) { hash = (hash ^ value) * kFnvPrime; };
			for (unsigned char c : source.referenceKey) add(c);
			add(0);
			for (unsigned char c : source.shapeName) add(c);
			add(0);
			for (unsigned n = 0; n < 8; ++n)
				add(static_cast<unsigned char>(source.objectIdentity >> (n * 8)));
			return hash ? hash : 1;
		}

		uint64_t Identity(const RuntimeGeometryCandidate& source)
		{
			uint64_t hash = kFnvOffset;
			auto add = [&](unsigned char value) { hash = (hash ^ value) * kFnvPrime; };
			for (unsigned char c : source.referenceKey) add(c);
			add(0);
			for (unsigned char c : source.shapeName) add(c);
			add(0);
			// Sibling meshes may share a name, transform and triangle count.
			// Pointer identity distinguishes those meshes for this runtime cell.
			for (unsigned n = 0; n < 8; ++n)
				add(static_cast<unsigned char>(source.objectIdentity >> (n * 8)));
			for (float value : source.transform) {
				const uint32_t bits = std::bit_cast<uint32_t>(value);
				for (unsigned n = 0; n < 4; ++n)
					add(static_cast<unsigned char>(bits >> (n * 8)));
			}
			for (uint32_t value : { source.vertexCount, source.triangleCount })
				for (unsigned n = 0; n < 4; ++n)
					add(static_cast<unsigned char>(value >> (n * 8)));
			return hash ? hash : 1;
		}

		bool FromMissingNif(std::string_view referenceKey, const DX12::MissingNifShapeDescriptor& source)
		{
			const std::string recorded = std::format("{:06X}:{}", source.referenceLocal, source.referencePlugin);
			return std::equal(referenceKey.begin(), referenceKey.end(),
				recorded.begin(), recorded.end(), [](char a, char b) {
					return std::tolower(static_cast<unsigned char>(a)) ==
					       std::tolower(static_cast<unsigned char>(b));
				});
		}

		// Copy only generic engine material inputs, never TruePBR extension state.
		// The live PBR marker may be original (menu-screen) or remapped by the
		// active raster backend (vertex-lighting). This is diagnostic observation.
		std::shared_ptr<DX12::PreparedShading> SnapshotMaterial(const RuntimeGeometryCandidate& source)
		{
			const auto* property = source.shadingProperty;
			if (!property || !property->material || property->material->GetType() != RE::BSShaderMaterial::Type::kLighting ||
				source.uvOffset < 16 || source.normalOffset < 16 || source.tangentOffset < 16 ||
				source.uvOffset > source.stride - 4 || source.normalOffset > source.stride - 4 ||
				source.tangentOffset > source.stride - 4 || (source.colorOffset != UINT32_MAX &&
				(source.colorOffset < 16 || source.colorOffset > source.stride - 4))) return {};
			using F = RE::BSShaderProperty::EShaderPropertyFlag;
			const auto flags = static_cast<uint64_t>(property->flags.underlying());
			if (!property->flags.any(F::kMenuScreen, F::kVertexLighting) ||
				property->flags.any(F::kVertexAlpha, F::kGrayscaleToPaletteAlpha, F::kRefraction, F::kTempRefraction,
					F::kTreeAnim, F::kLODLandscape, F::kMultiTextureLandscape, F::kFace, F::kHairTint) ||
				property->alpha != 1.0f) return {};
			const auto* material = static_cast<const RE::BSLightingShaderMaterialBase*>(property->material);
			if (material->materialAlpha != 1.0f || material->textureClampMode < 0 || material->textureClampMode > 3)
				return {};
			auto* textures = material->textureSet.get();
			const auto* expected = REL::Relocation<const RE::NiRTTI*>{ RE::BSTextureSet::Ni_RTTI }.get();
			if (!textures || !expected || !textures->GetRTTI() || !textures->GetRTTI()->IsKindOf(expected)) return {};
			auto shading = std::make_shared<DX12::PreparedShading>();
			shading->runtimeObserved = true;
			shading->stride = 60;
			shading->shapeName = source.shapeName;
			shading->provenance = "runtime engine material";
			shading->shaderFlags1 = uint32_t(flags);
			shading->shaderFlags2 = uint32_t(flags >> 32) | (1u << 23);
			shading->twoSided = property->flags.any(F::kTwoSided);
			shading->clampMode = material->textureClampMode;
			shading->uvTransform = { material->texCoordOffset[0].x, material->texCoordOffset[0].y,
				material->texCoordScale[0].x, material->texCoordScale[0].y };
			if (!std::ranges::all_of(shading->uvTransform, [](float x) { return std::isfinite(x); })) return {};
			for (uint32_t i = 0; i < 8; ++i) {
				const char* path = textures->GetTexturePath(static_cast<RE::BSTextureSet::Texture>(i));
				std::string name = path ? path : "";
				if (name.size() > 260) return {};
				shading->textures.push_back(std::move(name));
			}
			if (shading->textures[0].empty() || shading->textures[1].empty() || shading->textures[5].empty()) return {};
			return shading;
		}

		// Compare actual rigid geometry, not subcell readiness or camera coverage.
		// Companion-produced shapes take precedence over equivalent runtime shapes.
		bool SameShape(const DX12::Shape& a, const DX12::Shape& b)
		{
			if (a.Positions().size() != b.Positions().size() || a.Indices() != b.Indices())
				return false;
			for (size_t i = 0; i < a.transform.size(); ++i)
				if (std::abs(a.transform[i] - b.transform[i]) > 0.01f)
					return false;
			for (size_t i = 0; i < a.Positions().size(); ++i) {
				const auto& x = a.Positions()[i];
				const auto& y = b.Positions()[i];
				if (std::abs(x.x - y.x) > 0.01f ||
					std::abs(x.y - y.y) > 0.01f ||
					std::abs(x.z - y.z) > 0.01f)
					return false;
			}
			return true;
		}
	}

	RuntimeGeometryCapture::RuntimeGeometryCapture() :
		worker([this](std::stop_token stop) { Run(stop); })
	{}

	RuntimeGeometryCapture::~RuntimeGeometryCapture()
	{
		worker.request_stop();
		wake.notify_all();
		if (worker.joinable())
			worker.join();
	}

	void RuntimeGeometryCapture::Reset()
	{
		std::scoped_lock lock(mutex);
		++generation;
		pending.clear();
		captured.clear();
		stableObservations.clear();
		activeCenter = {};
		activeSize = 0;
		lastFrame = UINT32_MAX;
		frameBytes = 0;
	}

	void RuntimeGeometryCapture::Observe(const RuntimeGeometryCandidate& candidate, uint32_t frame,
		const std::shared_ptr<const DX12::Grid>& grid)
	{
		if (!grid || !grid->requested.front().scope || !candidate.vertices || !candidate.indices ||
			candidate.referenceKey.empty() || candidate.shapeName.empty() ||
			candidate.stride < 16 || candidate.stride > 256 ||
			!candidate.vertexCount || !candidate.triangleCount)
			return;
		const uint64_t vertexBytes = uint64_t(candidate.stride) * candidate.vertexCount;
		const uint64_t indexCount = uint64_t(candidate.triangleCount) * 3;
		const uint64_t bytes = vertexBytes + indexCount * sizeof(uint16_t);
		if (!bytes || bytes > kSingleCopyBytes || indexCount > UINT32_MAX)
			return;
		const auto center = grid->requested.front();
		const uint64_t id = Identity(candidate);
		const uint64_t stableKey = StableObjectKey(candidate);
		{
			std::scoped_lock lock(mutex);
			if (activeCenter != center || activeSize != grid->size) {
				++generation;
				activeCenter = center;
				activeSize = grid->size;
				captured.clear();
				stableObservations.clear();
				pending.clear();
				lastFrame = UINT32_MAX;
				frameBytes = 0;
			}
			if (lastFrame != frame) {
				lastFrame = frame;
				frameBytes = 0;
			}
			if (candidate.requiresStability && !captured.contains(id)) {
				// Avoid freezing moving clutter and actor attachments into TLAS.
				// One observation per engine frame, irrespective of pass multiplicity.
				if (stableObservations.size() >= 8192 && !stableObservations.contains(id)) {
					// Drop observations that have not been visible recently instead
					// of permanently locking out later furniture in a large cell.
					std::erase_if(stableObservations, [frame](const auto& entry) {
						return frame - entry.second.first > 120u;
					});
					if (stableObservations.size() >= 8192)
						return;
				}
				auto& seen = stableObservations[id];
				if (seen.first != frame) {
					// Require consecutive visible frames, not three observations
					// scattered across unrelated camera positions.
					if (frame - seen.first > 4u)
						seen.second = 0;
					seen.first = frame;
					seen.second = std::min(seen.second + 1u, 3u);
				}
				if (seen.second < 3)
					return;
			}
			if (bytes > kFrameCopyBytes - frameBytes || pending.size() >= kPacketLimit ||
				!captured.insert(id).second)
				return;
			frameBytes += static_cast<uint32_t>(bytes);
		}

		try {
			Packet packet;
			packet.identity = id;
			packet.runtimeObjectKey = stableKey;
			packet.sourceCenter = center;
			packet.referenceKey = candidate.referenceKey;
			packet.transform = candidate.transform;
			packet.stride = candidate.stride;
			packet.vertexCount = candidate.vertexCount;
			packet.centerX = candidate.centerX;
			packet.centerY = candidate.centerY;
			packet.vertices.resize(static_cast<size_t>(vertexBytes));
			std::memcpy(packet.vertices.data(), candidate.vertices, packet.vertices.size());
			packet.indices.assign(candidate.indices, candidate.indices + indexCount);
			if (candidate.shadingProperty) {
				packet.shading = SnapshotMaterial(candidate);
				packet.uvOffset = candidate.uvOffset;
				packet.normalOffset = candidate.normalOffset;
				packet.tangentOffset = candidate.tangentOffset;
				packet.colorOffset = candidate.colorOffset;
			}
			{
				std::scoped_lock lock(mutex);
				if (activeCenter != center || activeSize != grid->size) {
					captured.erase(id);
					return;
				}
				packet.generation = generation;
				pending.push_back(std::move(packet));
			}
			wake.notify_one();
		} catch (...) {
			std::scoped_lock lock(mutex);
			captured.erase(id);
		}
	}

	void RuntimeGeometryCapture::Run(std::stop_token stop)
	{
		while (!stop.stop_requested()) {
			Packet packet;
			{
				std::unique_lock lock(mutex);
				wake.wait(lock, stop, [&] { return !pending.empty(); });
				if (stop.stop_requested())
					return;
				packet = std::move(pending.front());
				pending.pop_front();
			}
			auto shape = std::make_shared<DX12::Shape>();
			shape->identity = packet.identity;
			shape->runtimeObjectKey = packet.runtimeObjectKey;
			shape->transform = packet.transform;
			shape->positions.reserve(packet.vertexCount);
			bool usable = true;
			for (uint32_t i = 0; i < packet.vertexCount; ++i) {
				float xyz[3];
				std::memcpy(xyz, packet.vertices.data() + size_t(i) * packet.stride, sizeof(xyz));
				if (!std::isfinite(xyz[0]) || !std::isfinite(xyz[1]) || !std::isfinite(xyz[2])) {
					usable = false;
					break;
				}
				shape->positions.push_back({ xyz[0], xyz[1], xyz[2] });
			}
			if (!usable || !std::ranges::all_of(packet.indices,
							   [&](uint16_t index) { return index < packet.vertexCount; }))
				continue;
			shape->indices.assign(packet.indices.begin(), packet.indices.end());
			if (packet.shading) {
				// Skyrim rigid vertex layout: float3 position + bitangent X,
				// half2 UV, packed normal.xyz/bitangent Y, packed tangent.xyz/
				// bitangent Z, optional RGBA8 color. Decode on the CPU worker.
				auto& attributes = packet.shading->attributes;
				attributes.reserve(size_t(packet.vertexCount) * 15);
				const auto sn = [](uint8_t v) { return float(v) * (2.0f / 255.0f) - 1.0f; };
				for (uint32_t i = 0; i < packet.vertexCount; ++i) {
					const auto* vertex = packet.vertices.data() + size_t(i) * packet.stride;
					uint16_t uv[2];
					float bx;
					std::memcpy(uv, vertex + packet.uvOffset, sizeof(uv));
					std::memcpy(&bx, vertex + 12, sizeof(bx));
					const auto* n = vertex + packet.normalOffset;
					const auto* t = vertex + packet.tangentOffset;
					const auto* color = packet.colorOffset == UINT32_MAX ? nullptr : vertex + packet.colorOffset;
					const float u = DirectX::PackedVector::XMConvertHalfToFloat(uv[0]);
					const float v = DirectX::PackedVector::XMConvertHalfToFloat(uv[1]);
					if (!std::isfinite(bx) || !std::isfinite(u) || !std::isfinite(v)) {
						attributes.clear();
						break;
					}
					attributes.insert(attributes.end(), { sn(n[0]), sn(n[1]), sn(n[2]), u, v,
						color ? float(color[0])/255.0f : 1.0f, color ? float(color[1])/255.0f : 1.0f,
						color ? float(color[2])/255.0f : 1.0f, color ? float(color[3])/255.0f : 1.0f,
						sn(t[0]), sn(t[1]), sn(t[2]), bx, sn(n[3]), sn(t[3]) });
				}
				if (!attributes.empty()) shape->shading = std::move(packet.shading);
			}

			const auto releaseIfCurrent = [&]() {
				std::scoped_lock lock(mutex);
				if (packet.generation == generation)
					captured.erase(packet.identity);
			};
			const auto grid = globals::features::dx12Resources.GetGrid();
			if (!grid || !grid->requested.front().scope ||
				grid->requested.front().scope != packet.sourceCenter.scope) {
				releaseIfCurrent();
				continue;
			}
			const auto x = DX12::Coordinate(packet.centerX, grid->size);
			const auto y = DX12::Coordinate(packet.centerY, grid->size);
			if (!x || !y || !std::ranges::contains(grid->requested, DX12::SubcellKey{ grid->requested.front().scope, *x, *y })) {
				releaseIfCurrent();
				continue;
			}
			bool companionOwns = false, ambiguousMatch = false;
			std::shared_ptr<const DX12::Shape> materialMatch;
			for (const auto& tile : grid->resident) {
				if (!tile) continue;
				for (const auto& prepared : tile->shapes) {
					if (!prepared || !SameShape(*prepared, *shape)) continue;
					companionOwns = true;
					if (!grid->fullNative || !shape->shading || prepared->referencePlugin.empty()) continue;
					const auto reference = std::format("{:06X}:{}", prepared->referenceLocal, prepared->referencePlugin);
					if (!std::equal(reference.begin(), reference.end(), packet.referenceKey.begin(), packet.referenceKey.end(),
						[](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); }))
						continue;
					// Match exact geometry and reference; if multiple distinct
					// blocks are indistinguishable, do not guess their materials.
					if (prepared->shading && prepared->shading->shapeName != shape->shading->shapeName) continue;
					if (materialMatch && materialMatch->identity != prepared->identity) ambiguousMatch = true;
					else materialMatch = prepared;
				}
			}
			if (companionOwns) {
				if (materialMatch && !ambiguousMatch &&
					(!materialMatch->shading || (materialMatch->materialOverride && materialMatch->materialOverride->ambiguous))) {
					globals::features::dx12Resources.RecoverMaterial(packet.sourceCenter.scope, materialMatch, shape->shading);
				}
				continue;
			}
			// The companion may have retained the original NIF even when a block
			// was unsupported. Preserve that provenance on the runtime supplement.
			// Block indices are NOT inferred from runtime shape names.
			for (const auto& tile : grid->resident) {
				if (!tile || !shape->sourceNif.empty())
					continue;
				for (const auto& missing : tile->missingShapes) {
					if (FromMissingNif(packet.referenceKey, missing)) {
						shape->sourceNif = missing.nifPath;
						break;
					}
				}
			}
			{
				std::scoped_lock lock(mutex);
				if (packet.generation != generation)
					continue;
			}
			// Existing immutable grid owns residency and geometry revisions.
			globals::features::dx12Resources.PublishRuntimeShape(
				std::move(shape), packet.centerX, packet.centerY);
		}
	}
}

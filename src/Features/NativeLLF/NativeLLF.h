#pragma once

#include "Features/DX12/DX12NativeDirectLighting.h"
#include "Features/DX12/FullNativeFeature.h"
#include "Features/LightLimitFix/ParticleInput.h"
#include "NativeLightMetadataPolicy.h"
#include "NativeLightSpatialPolicy.h"
#include <atomic>
#include <memory>
#include <unordered_map>

#include <unordered_set>

#include <optional>
#include <vector>

namespace NativeLLF
{
	/** @brief Frame-local direct inputs, using an asynchronously computed spatial grouping. */
	struct DirectInputFrame
	{
		uint32_t frame = 0, scope = 0, pluginFrame = 0;
		uint64_t geometryRevision = 0;
		std::vector<DX12Native::DirectPointLight> points;
		DX12Native::DirectDirectionalLight directional;
		bool available = false;
	};

	class Producer final : public DX12::FullNativeFeature
	{
	public:
		std::string GetName() override { return "Native LLF"; }
		std::string GetShortName() override { return "NativeLLF"; }
		void PostPostLoad() override;
		void Reset() override;
		void ResetParticleInput();
		void OnRuntimeDisabled() override;
		void ObserveLightCreation(RE::NiLight* light, const RE::TESObjectLIGH& authored);
		json GetDiagnostics() override;
		void OnSceneTransitionReset(bool opening) override { if (opening) { ResetParticleInput(); ResetSpatialInput(); } }
		std::optional<ParticleInput::Observation> ObserveParticlePass(RE::BSRenderPass* pass);
		std::shared_ptr<const DirectInputFrame> CollectDirectInputs(std::shared_ptr<const DX12::Grid> grid, uint32_t scope, uint64_t geometryRevision, uint32_t frame, bool opaqueReady);

	private:
		struct SpatialWorkerState
		{
			std::atomic<std::shared_ptr<const SpatialPolicy::Publication>> latest;
			std::atomic<bool> busy{false};
			std::atomic<uint64_t> tasks{0}, publications{0}, deferred{0};
		};
		std::atomic<std::shared_ptr<SpatialWorkerState>> spatialWorker;
		struct PreparedCache
		{
			std::shared_ptr<const DX12::Grid> grid;
			std::vector<DX12::PreparedLight> lights;
			std::vector<std::optional<Policy::AuthoredLight>> authored;
			std::unordered_map<uint32_t, std::vector<size_t>> byReference;
			std::vector<size_t> byX;
		};
		std::atomic<std::shared_ptr<const PreparedCache>> preparedCache;
		// Immutable per-frame acquisition; retries must not rewalk engine light arrays.
		std::atomic<std::shared_ptr<const DirectInputFrame>> directInputPublication;
		std::atomic<uint64_t> directInputCacheHits{0};
		std::shared_ptr<const PreparedCache> GetPreparedCache(std::shared_ptr<const DX12::Grid> grid);
		std::atomic<uint32_t> spatialInputCount{0}, spatialOutputCount{0};
		std::atomic<bool> spatialPlanApplied{false};
		void ResetSpatialInput();
		std::shared_ptr<const SpatialPolicy::Publication> UpdateSpatialInput(SpatialPolicy::Signature signature);
		struct MetadataEntry
		{
			// Retain identity until bounded retirement; a recycled address cannot alias a live entry.
			RE::NiPointer<RE::NiLight> identity;
			Policy::AuthoredLight authored;
			uint32_t lastSeen = 0;
		};
		std::mutex metadataMutex;
		std::unordered_map<const RE::NiLight*, MetadataEntry> metadata;
		uint32_t metadataFrame = 0, metadataRejected = 0;
		std::atomic<uint32_t> preparedInputCount{0}, directInputCount{0}, spotInputCount{0}, particleInputCount{0};
		std::atomic<uint32_t> compatibilityMetadataCount{0}, shadowInputCount{0}, nonShadowInputCount{0};
		std::optional<Policy::AuthoredLight> FindMetadata(RE::NiLight* light, uint32_t placed);
		void BeginMetadataFrame();
		struct ParticleEmitter
		{
			float3 position{}, color{};
			float radius = 0.0f;
		};
		ParticleLights configs;
		LightLimitFix::Settings particleSettings;
		ParticleInput::ReferenceCache references;
		std::mutex referenceMutex;
		std::mutex observationMutex;
		std::unordered_set<const RE::BSGeometry*> observed;
		std::vector<ParticleEmitter> pending, current;
		uint32_t currentPluginFrame = UINT32_MAX;
		void CaptureEmitter(const LightLimitFix::ParticleLightInfo& emitter);
	};
}

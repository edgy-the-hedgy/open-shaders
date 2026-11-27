#pragma once

#include "Features/DX12/DX12NativeDirectLighting.h"
#include "Features/DX12/FullNativeFeature.h"
#include "Features/LightLimitFix/ParticleInput.h"
#include "NativeLightMetadataPolicy.h"
#include <unordered_map>

#include <unordered_set>

#include <optional>
#include <vector>

namespace NativeLLF
{
	/** @brief Synchronous CPU light-input snapshot; GPU upload remains owned by DirectLightingTrace. */
	struct DirectInputFrame
	{
		uint32_t frame = 0, scope = 0;
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
		void OnSceneTransitionReset(bool opening) override { if (opening) ResetParticleInput(); }
		std::optional<ParticleInput::Observation> ObserveParticlePass(RE::BSRenderPass* pass);
		std::optional<DirectInputFrame> CollectDirectInputs(const DX12Native::TopLevel& scene, uint32_t frame, bool opaqueReady);

	private:
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

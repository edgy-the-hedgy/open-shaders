#pragma once

#include "Features/DX12/DX12NativeDirectLighting.h"
#include "Features/DX12/FullNativeFeature.h"
#include "Features/LightLimitFix/ParticleInput.h"

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
		void OnRuntimeDisabled() override { ResetParticleInput(); }
		void OnSceneTransitionReset(bool opening) override { if (opening) ResetParticleInput(); }
		void ObserveParticlePass(RE::BSRenderPass* pass);
		std::optional<DirectInputFrame> CollectDirectInputs(const DX12Native::TopLevel& scene, uint32_t frame, bool opaqueReady);

	private:
		struct ParticleEmitter
		{
			float3 position{}, color{};
			float radius = 0.0f;
		};
		ParticleLights configs;
		LightLimitFix::Settings particleSettings;
		ParticleInput::ReferenceCache references;
		std::mutex referenceMutex;
		std::unordered_set<const RE::BSGeometry*> observed;
		std::vector<ParticleEmitter> pending, current;
		uint32_t currentPluginFrame = UINT32_MAX;
		void CaptureEmitter(const LightLimitFix::ParticleLightInfo& emitter);
	};
}

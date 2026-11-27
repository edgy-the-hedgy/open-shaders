#pragma once

#include "Features/DX12/FullNativeFeature.h"

#include "Features/DX12/DX12NativeIndirectLighting.h"
#include "Features/IBL/IBLSettingsInput.h"

namespace NativeIBL
{
	/** @brief Environment-only indirect lighting consumer; owns its DX12 resources. */
	class Producer final : public DX12::FullNativeFeature
	{
	public:
		std::string GetName() override { return "Native IBL"; }
		std::string GetShortName() override { return "NativeIBL"; }
		void PostPostLoad() override;
		json GetDiagnostics() override;
		void SetupResources() override;
		bool WantsDrawHook() const override { return true; }
		bool WantsRenderPassHook() const override { return true; }
		void OnDraw() override { capture.OnDraw(); }
		std::function<void()> OnRenderPassBegin(const RE::BSRenderPass*) override { return capture.OnRenderPassBegin(); }
		std::function<void()> OnReflectionsRenderBegin() override;
		void OnRuntimeDisabled() override { InvalidateGeneration(); Release(); SetCaptureContext({}); }
		void ClearShaderCache() override { InvalidateGeneration(); Release(); SetCaptureContext({}); }
		void SetCaptureContext(const DX12Native::EnvironmentStamp& stamp);
		DX12Native::EnvironmentSource GetSkySource(const DX12Native::EnvironmentStamp& requested) const;
		ID3D11ShaderResourceView* GetRasterSkyView() const { return GetSkySource(context).texture ? capture.View() : nullptr; }
		void Poll(uint32_t frame) { if (lighting) lighting->Poll(frame); }
		void InvalidateGeneration() { if (lighting) lighting->Reset(); }
		void Release() { lighting.reset(); capture.Release(); }
		void Dispatch(const GraphicsBackend& backend,
			std::shared_ptr<const DX12Native::NativeScreenProbe::MaterialFrame> material,
			std::shared_ptr<const DX12Native::EnvironmentFrame> environment,
			const DX12Native::EnvironmentStamp& stamp,
			std::shared_ptr<const DX12Native::CharacterCasterFrame> characters, uint32_t quality);
		std::shared_ptr<const DX12Native::IndirectLightingFrame> Get(uint32_t frame) const
		{
			return lighting ? lighting->Get(frame) : nullptr;
		}

	private:
		IBL::Settings settings;
		IBLInput::ReflectionRadianceCapture capture;
		DX12Native::EnvironmentStamp context{}, publishedStamp{};
		uint64_t skyRevision = 0;
		std::unique_ptr<DX12Native::NativeIndirectLighting> lighting;
	};
}

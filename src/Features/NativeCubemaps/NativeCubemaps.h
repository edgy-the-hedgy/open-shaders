#pragma once

#include "Features/DX12/FullNativeFeature.h"

#include "Features/DX12/DX12NativeEnvironment.h"

namespace NativeCubemaps
{
	/** @brief Owns native environment generations and fenced DX12 environment resources. */
	class Producer final : public DX12::FullNativeFeature
	{
	public:
		std::string GetName() override { return "Native Cubemaps"; }
		std::string GetShortName() override { return "NativeCubemaps"; }
		void OnRuntimeDisabled() override { InvalidateGeneration(); Release(); }
		void ClearShaderCache() override { InvalidateGeneration(); Release(); }
		DX12Native::EnvironmentStamp Prepare(uint32_t scope, uint32_t frame, bool& changed);
		void Capture(const GraphicsBackend& backend);
		void InvalidateGeneration();
		void Release() { environment.reset(); }
		std::shared_ptr<const DX12Native::EnvironmentFrame> Get() const { return environment ? environment->Get() : nullptr; }

		// Transitional acquisition callbacks at the existing legacy capture boundaries.
		void ResetLegacyCubeBridge();
		void RecordLegacyCubeCapture(uint32_t index, bool successful);
		void PublishLegacyCube(uint32_t index, ID3D11Texture2D* texture);
		void ResetLegacySkyBridge();
		void InvalidateLegacySkyFaces();
		void BeginLegacySkyCapture(DX12Native::EnvironmentEncoding encoding);
		void PublishLegacySky(bool failed, uint32_t clearedFaces, uint32_t validFaces, uint32_t allFaces, ID3D11Texture2D* texture);

	private:
		struct RetainedSource
		{
			winrt::com_ptr<ID3D11Texture2D> texture;
			uint64_t revision = 0;
			DX12Native::EnvironmentStamp stamp{};
			DX12Native::EnvironmentSource Get(const DX12Native::EnvironmentStamp& requested) const
			{
				DX12Native::EnvironmentSource source{ texture.get(), revision, stamp };
				return source.IsValidFor(requested) ? source : DX12Native::EnvironmentSource{};
			}
		};
		DX12Native::EnvironmentStamp cubeContext{}, skyContext{}, skyPending{};
		std::array<DX12Native::EnvironmentStamp, 2> cubePending{};
		std::array<RetainedSource, 2> cubeSources{};
		RetainedSource skySource;
		uint64_t cubeRevision = 0, skyRevision = 0;
		uint32_t skyFaces = 0;
		std::unique_ptr<DX12Native::NativeEnvironment> environment;
		DX12Native::EnvironmentStamp currentStamp{};
		uint64_t epoch = 1;
		float lastHours = 0.0f;
	};
}

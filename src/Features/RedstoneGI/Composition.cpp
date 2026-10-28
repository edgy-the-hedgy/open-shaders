#include "RedstoneGI.h"
#include "Globals.h"
#include "State.h"
#include "Util.h"

namespace
{
	constexpr uint32_t kCompositionResourceSlot = 20;
	constexpr uint32_t kCompositionResourceCount = 6;
	constexpr uint32_t kCompositionSettingsSlot = 13;

	struct alignas(16) CompositionSettings
	{
		std::array<uint, 2> inputSize;
		std::array<uint, 2> frameSize;
		float diffuseScale;
		float specularScale;
		uint lightingReady;
		uint occlusionMask = 0;
		float roughnessFloor;
		float diffuseOcclusionStrength;
		float specularOcclusionStrength;
		uint paddingTail = 0;
	};
}

ID3D11ComputeShader* RedstoneGI::GetCompositeShader(bool a_interior, std::vector<std::pair<const char*, const char*>>& a_defines)
{
	auto defines = a_defines;
	std::erase_if(defines, [](const auto& a_define) {
		return a_define.first && (std::string_view(a_define.first) == "SSGI" || std::string_view(a_define.first) == "SSGI_AO_ONLY");
	});
	auto& shader = a_interior ? compositeInteriorShader : compositeShader;
	auto* compiled = compositionSettings ? shader.Get(L"Data\\Shaders\\RedstoneGI\\DeferredCompositeCS.hlsl", defines, "cs_5_0", "main", "RedstoneGI::Composite CS") : nullptr;
	return compiled;
}

void RedstoneGI::SetupCompositeResources()
{
	compositionSettings = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<CompositionSettings>(), "RedstoneGI::CompositionSettings");
}

void RedstoneGI::BindCompositeResources()
{
	if (!compositionSettings)
		return;
	auto* context = globals::d3d::context;
	const auto size = Util::ConvertToDynamic(globals::state->screenSize);
	const bool current = rrLightingReady && globals::game::frameCounter &&
	                     rrOutputFrame == static_cast<uint32_t>(*globals::game::frameCounter);
	compositionSettings->Update(CompositionSettings{
		rrHistoryResolution, { static_cast<uint>(size.x), static_cast<uint>(size.y) },
		diffuseEnabled ? std::exp2(rgiContributionEV + diffuseContributionEV) : 0.0f, specularEnabled ? std::exp2(rgiContributionEV + specularContributionEV) : 0.0f,
		current ? 1u : 0u, (occlusion.diffuseEnabled ? 1u : 0u) | (occlusion.specularEnabled ? 2u : 0u), specularRoughnessFloor, occlusion.diffuseStrength, occlusion.specularStrength, 0 });
	previousCompositeSettings = nullptr;
	context->CSGetConstantBuffers(kCompositionSettingsSlot, 1, previousCompositeSettings.put());
	auto* cb = compositionSettings->CB();
	context->CSSetConstantBuffers(kCompositionSettingsSlot, 1, &cb);
	ID3D11ShaderResourceView* srvs[kCompositionResourceCount]{
		texRRSpecularOutput ? texRRSpecularOutput->srv.get() : nullptr,
		texRRDiffuseOutput ? texRRDiffuseOutput->srv.get() : nullptr,
		texRRLinearDepth ? texRRLinearDepth->srv.get() : nullptr,
		texRRNormalRoughness ? texRRNormalRoughness->srv.get() : nullptr,
		Util::GetCurrentSceneDepthSRV(false),
		texPackedOcclusion ? texPackedOcclusion->srv.get() : nullptr
	};
	context->CSSetShaderResources(kCompositionResourceSlot, kCompositionResourceCount, srvs);
}

void RedstoneGI::FinishComposite(bool a_usedPrivateShader)
{
	if (!compositionSettings)
		return;
	auto* context = globals::d3d::context;
	ID3D11ShaderResourceView* srvs[kCompositionResourceCount]{};
	context->CSSetShaderResources(kCompositionResourceSlot, kCompositionResourceCount, srvs);
	auto* cb = previousCompositeSettings.get();
	context->CSSetConstantBuffers(kCompositionSettingsSlot, 1, &cb);
	previousCompositeSettings = nullptr;
	rrOutputApplied = a_usedPrivateShader && rrLightingReady && rrProviderSucceeded;
}

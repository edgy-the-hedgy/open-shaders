#include "RedstoneGI.h"
#include "Deferred.h"
#include "GpuPass.h"
#include "State.h"
#include "Util.h"

void RedstoneGI::PrepareSurfaceInputs(const std::array<uint, 2>& a_resolution, const std::array<uint, 2>& a_internalResolution)
{
	CS_GPU_PASS("RedstoneGI::SurfaceInputs");
	auto* context = globals::d3d::context;
	auto targets = globals::game::renderer->GetRuntimeData().renderTargets;
	ID3D11ShaderResourceView* reads[2]{};
	ID3D11UnorderedAccessView* writes[5]{};
	const auto unbind = [&] {
		std::fill(std::begin(reads), std::end(reads), nullptr);
		std::fill(std::begin(writes), std::end(writes), nullptr);
		context->CSSetShaderResources(0, 2, reads);
		context->CSSetUnorderedAccessViews(0, 5, writes, nullptr);
	};
	{
		CS_GPU_PASS("RedstoneGI::DepthPyramid");
		reads[0] = Util::GetCurrentSceneDepthSRV();
		for (uint i = 0; i < 5; ++i) writes[i] = uavWorkingDepth[i].get();
		context->CSSetShaderResources(0, 2, reads);
		context->CSSetUnorderedAccessViews(0, 5, writes, nullptr);
		context->CSSetShader(prefilterDepthsCompute.get(), nullptr, 0);
		context->Dispatch((a_resolution[0] + 15u) >> 4, (a_resolution[1] + 15u) >> 4, 1);
		unbind();
	}
	{
		CS_GPU_PASS("RedstoneGI::SourceRadiance");
		reads[0] = Util::AsReal(targets[globals::deferred->forwardRenderTargets[0]].SRV);
		reads[1] = texWorkingDepth->srv.get();
		writes[0] = texRadianceTemp->uav.get();
		context->CSSetShaderResources(0, 2, reads);
		context->CSSetUnorderedAccessViews(0, 1, writes, nullptr);
		context->CSSetShader(sourceRadianceCompute.get(), nullptr, 0);
		context->Dispatch((a_internalResolution[0] + 7u) >> 3, (a_internalResolution[1] + 7u) >> 3, 1);
		unbind();
	}
	{
		CS_GPU_PASS("RedstoneGI::RadiancePyramid");
		reads[0] = texRadianceTemp->srv.get();
		for (uint i = 0; i < 5; ++i) writes[i] = uavRadiance[i].get();
		context->CSSetShaderResources(0, 2, reads);
		context->CSSetUnorderedAccessViews(0, 5, writes, nullptr);
		context->CSSetShader(prefilterRadianceCompute.get(), nullptr, 0);
		context->Dispatch((a_internalResolution[0] + 15u) >> 4, (a_internalResolution[1] + 15u) >> 4, 1);
		unbind();
	}
	{
		CS_GPU_PASS("RedstoneGI::NormalPyramid");
		reads[0] = Util::AsReal(targets[NORMALROUGHNESS].SRV);
		reads[1] = texWorkingDepth->srv.get();
		for (uint i = 0; i < 5; ++i) writes[i] = uavNormal[i].get();
		context->CSSetShaderResources(0, 2, reads);
		context->CSSetUnorderedAccessViews(0, 5, writes, nullptr);
		context->CSSetShader(prefilterNormalCompute.get(), nullptr, 0);
		context->Dispatch((a_internalResolution[0] + 15u) >> 4, (a_internalResolution[1] + 15u) >> 4, 1);
		unbind();
	}
}

void RedstoneGI::DrawEstimators(const std::array<uint, 2>& a_internalResolution)
{
	CS_GPU_PASS("RedstoneGI::IndependentEstimators");
	auto* context = globals::d3d::context;
	auto targets = globals::game::renderer->GetRuntimeData().renderTargets;
	ID3D11ShaderResourceView* reads[]{ texWorkingDepth->srv.get(), Util::AsReal(targets[NORMALROUGHNESS].SRV),
		texRadiance->srv.get(), texNoise->srv.get(), texNormal->srv.get(),
		Util::AsReal(targets[ALBEDO].SRV), Util::AsReal(targets[REFLECTANCE].SRV) };
	ID3D11UnorderedAccessView* writes[]{ texRRSpecularInput ? texRRSpecularInput->uav.get() : nullptr,
		texRRDiffuseInput ? texRRDiffuseInput->uav.get() : nullptr };
	context->CSSetShaderResources(0, ARRAYSIZE(reads), reads);
	context->CSSetUnorderedAccessViews(0, ARRAYSIZE(writes), writes, nullptr);
	context->CSSetShader(giCompute.get(), nullptr, 0);
	context->Dispatch((a_internalResolution[0] + 7u) >> 3, (a_internalResolution[1] + 7u) >> 3, 1);
	std::fill(std::begin(reads), std::end(reads), nullptr);
	std::fill(std::begin(writes), std::end(writes), nullptr);
	context->CSSetShaderResources(0, ARRAYSIZE(reads), reads);
	context->CSSetUnorderedAccessViews(0, ARRAYSIZE(writes), writes, nullptr);
}

void RedstoneGI::DrawOcclusion(const std::array<uint, 2>& a_size)
{
	if (!occlusion.diffuseEnabled && !occlusion.specularEnabled) return;
	CS_GPU_PASS("RedstoneGI::Occlusion");
	auto* context = globals::d3d::context;
	occlusion.useDenoised = 0;
	occlusionSettings->Update(occlusion);
	auto* cb = occlusionSettings->CB();
	context->CSSetConstantBuffers(2, 1, &cb);
	auto targets = globals::game::renderer->GetRuntimeData().renderTargets;
	ID3D11ShaderResourceView* reads[]{ texWorkingDepth->srv.get(), Util::AsReal(targets[NORMALROUGHNESS].SRV),
		texRadiance->srv.get(), texNoise->srv.get(), texNormal->srv.get() };
	ID3D11UnorderedAccessView* writes[]{ texDiffuseOcclusionInput->uav.get(), texSpecularOcclusionInput->uav.get() };
	context->CSSetShaderResources(0, 5, reads);
	context->CSSetUnorderedAccessViews(0, 2, writes, nullptr);
	context->CSSetShader(occlusionCompute.get(), nullptr, 0);
	context->Dispatch((a_size[0] + 7u) >> 3, (a_size[1] + 7u) >> 3, 1);
	std::fill(std::begin(reads), std::end(reads), nullptr);
	std::fill(std::begin(writes), std::end(writes), nullptr);
	context->CSSetShaderResources(0, 5, reads);
	context->CSSetUnorderedAccessViews(0, 2, writes, nullptr);
}

void RedstoneGI::PackOcclusion(const std::array<uint, 2>& a_size, bool a_denoised)
{
	if (!occlusion.diffuseEnabled && !occlusion.specularEnabled) return;
	CS_GPU_PASS_SELECT(a_denoised, "RedstoneGI::PackRuntimeOcclusion", "RedstoneGI::ResolveHostOcclusion");
	auto* context = globals::d3d::context;
	occlusion.useDenoised = a_denoised;
	occlusionSettings->Update(occlusion);
	ID3D11ShaderResourceView* reads[]{ texDiffuseOcclusionOutput->srv.get(), texSpecularOcclusionOutput->srv.get(),
		texDiffuseOcclusionInput->srv.get(), texSpecularOcclusionInput->srv.get(),
		texRRLinearDepth[!rrDepthHistoryIdx]->srv.get(), texRRNormalRoughness->srv.get() };
	auto* write = texPackedOcclusion->uav.get();
	context->CSSetShaderResources(0, 6, reads);
	context->CSSetUnorderedAccessViews(0, 1, &write, nullptr);
	context->CSSetShader(occlusionPackCompute.get(), nullptr, 0);
	context->Dispatch((a_size[0] + 7u) >> 3, (a_size[1] + 7u) >> 3, 1);
	std::fill(std::begin(reads), std::end(reads), nullptr);
	write = nullptr;
	context->CSSetShaderResources(0, 6, reads);
	context->CSSetUnorderedAccessViews(0, 1, &write, nullptr);
}

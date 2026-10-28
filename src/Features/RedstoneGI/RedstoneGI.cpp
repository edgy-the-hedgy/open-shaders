#include "RedstoneGI.h"
#include "Deferred.h"
#include "GpuPass.h"
#include "RuntimeDenoiser.h"
#include "State.h"
#include "Util.h"
#include "Utils/Game.h"
#include <DirectXTex.h>

const char* RedstoneGI::GetRayRegenerationInactiveReason() const
{
	if (!rrEnabled) return "FSR-RR is disabled; using raw signal resolve.";
	if (!loaded || !ShadersOK())
		return "RedstoneGI is disabled or its shaders are unavailable.";
	if (!HasRayRegenerationResources())
		return "Ray Regeneration shaders or resources are unavailable.";
	return denoiser.CanDispatchRayRegeneration() ? nullptr : "Ray Regeneration provider is unavailable.";
}

bool RedstoneGI::HasRayRegenerationResources() const
{
	return occlusionCompute && occlusionPackCompute && occlusionSettings && texPackedOcclusion &&
	       texDiffuseOcclusionInput && texSpecularOcclusionInput && texDiffuseOcclusionOutput && texSpecularOcclusionOutput &&
	       rrPrepareInputsCompute && rrResolveFallbackCompute && texRRSpecularInput && texRRSpecularOutput &&
	       texRRDiffuseInput && texRRDiffuseOutput && texRRLinearDepth &&
	       texRRMotionVectors && texRRNormalRoughness && texRRSpecularAlbedo && texRRDiffuseAlbedo;
}

json RedstoneGI::GetDiagnostics()
{
	const auto* reason = GetRayRegenerationInactiveReason();
	const auto debugView = GetRRDebugView();
	return {
		{ "rayRegenerationReady", reason == nullptr },
		{ "rayRegenerationInactiveReason", reason ? reason : "" },
		{ "rayRegenerationOutputApplied", rrOutputApplied && globals::game::frameCounter && rrOutputFrame == static_cast<uint32_t>(*globals::game::frameCounter) },
		{ "rayRegenerationOutputFrame", rrOutputFrame },
		{ "rrEnabled", rrEnabled },
		{ "diffuseEnabled", diffuseEnabled },
		{ "specularEnabled", specularEnabled },
		{ "rrOutputOnly", IsRROutputOnly() },
		{ "rrInputsOnly", IsRRInputsOnly() },
		{ "rrPreviewUsesSceneContributions", debugView == RRDebugView::Combined || debugView == RRDebugView::Diffuse || debugView == RRDebugView::Specular || debugView == RRDebugView::AllSignals },
		{ "rrDebugView", debugView == RRDebugView::Diffuse ? "diffuse" : debugView == RRDebugView::Specular ? "specular" : debugView == RRDebugView::DiffuseOcclusion ? "diffuseAO" : debugView == RRDebugView::SpecularOcclusion ? "specularOcclusion" : debugView == RRDebugView::CombinedOcclusion ? "combinedOcclusion" : debugView == RRDebugView::AllSignals ? "allSignals" : debugView == RRDebugView::GeometryDepth ? "geometryDepth" : "combined" },
		{ "rrCheckerboardEnabled", rrCheckerboardEnabled },
		{ "rrCheckerboardActive", rrCheckerboardThisFrame },
		{ "rrCheckerboardOrigin", rrCheckerboardOrigin },
		{ "depthMotion", "cameraExpectedDepthWhenNativeXYAgrees; unknown deformation depth uses zero" },
		{ "animatedDepthMotionAvailable", false },
		{ "materialClassesAvailable", false },
		{ "specularHitDistance", "GGX weighted distance of visible screen-space hits; misses remain invalid" },
		{ "surfaceSelection", "fixed native pixel in each RR footprint; no minimum-depth dilation or mixed mip surfaces" },
		{ "diffuseAndSpecular", true },
		{ "diffuseEstimator", "directRGB" },
		{ "diffuseSlices", diffuseSlices },
		{ "diffuseSteps", diffuseSteps },
		{ "specularSlices", specularSlices },
		{ "specularSteps", specularSteps },
		{ "adaptiveSampling", settings.EnableAdaptiveSampling },
		{ "vanillaSSAOEnabled", settings.EnableVanillaSSAO },
		{ "diffuseOcclusionEnabled", occlusion.diffuseEnabled != 0 },
		{ "specularOcclusionEnabled", occlusion.specularEnabled != 0 },
		{ "occlusionPackedRG", true },
		{ "diffuseOcclusionRadius", occlusion.diffuseRadius },
		{ "specularOcclusionRadius", occlusion.specularRadius },
		{ "diffuseOcclusionStrength", occlusion.diffuseStrength },
		{ "specularOcclusionStrength", occlusion.specularStrength },
		{ "diffuseOcclusionSlices", occlusion.diffuseSlices },
		{ "diffuseOcclusionSteps", occlusion.diffuseSteps },
		{ "specularOcclusionSlices", occlusion.specularSlices },
		{ "specularOcclusionSteps", occlusion.specularSteps },
		{ "ilCompensationDistance", giCompensationMaxDistance },
		{ "occluderDepth", occluderDepth },
		{ "specularRoughnessFloor", specularRoughnessFloor },
		{ "lightingReady", rrLightingReady },
		{ "providerSucceeded", rrProviderSucceeded },
		{ "contributionEV", rgiContributionEV },
		{ "specularContributionEV", specularContributionEV },
		{ "diffuseContributionEV", diffuseContributionEV },
		{ "rrPreviewReady", rrPreviewValid },
		{ "rrPreviewFailed", rrPreviewFailed },
		{ "rrPreviewWidth", rrPreviewSize[0] },
		{ "rrPreviewHeight", rrPreviewSize[1] },
		{ "rayRegenerationProvider", denoiser.GetRayRegenerationDiagnostics() }
	};
}

void RedstoneGI::SetupResources()
{
	resourcesReady = false;
	const auto backend = Feature::FindGraphicsBackend();
	if (!backend || !backend->fsrFrameGeneration || globals::game::isVR) return;
	if (!startupGate.checked) startupGate.Select(true, true, [&] { return backend->rayRegenerationHardware; });
	if (!startupGate.eligible || !denoiser.Initialize(*backend)) return;
	auto renderer = globals::game::renderer;
	auto device = globals::d3d::device;

	EnforceFixedPath();

	logger::info("RedstoneGI paired lighting resources");

	logger::debug("Creating buffers...");
	{
		settingsBuffer = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<RGICB>(), "RedstoneGI::CB");
	}

	logger::debug("Creating textures...");
	{
		D3D11_TEXTURE2D_DESC texDesc{
			.Width = 64,
			.Height = 64,
			.MipLevels = 1,
			.ArraySize = 1,
			.Format = DXGI_FORMAT_R32_UINT,
			.SampleDesc = { 1, 0 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
			.CPUAccessFlags = 0,
			.MiscFlags = 0
		};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = {
				.MostDetailedMip = 0,
				.MipLevels = texDesc.MipLevels }
		};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};

		auto mainTex = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
		mainTex.texture->GetDesc(Util::AsW32(&texDesc));
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

		texDesc.MipLevels = srvDesc.Texture2D.MipLevels = 1;
		texDesc.MiscFlags = 0;
		texDesc.Format = srvDesc.Format = uavDesc.Format = DXGI_FORMAT_R11G11B10_FLOAT;
		texRadiance = eastl::make_unique<Texture2D>(texDesc, "RedstoneGI::Radiance");
		texRadiance->CreateSRV(srvDesc);
		texRadiance->CreateUAV(uavDesc);
		texDesc.Format = srvDesc.Format = uavDesc.Format = DXGI_FORMAT_R32_FLOAT;
		texWorkingDepth = eastl::make_unique<Texture2D>(texDesc, "RedstoneGI::WorkingDepth");
		texWorkingDepth->CreateSRV(srvDesc);
		texWorkingDepth->CreateUAV(uavDesc);

		uavDesc.Texture2D.MipSlice = 0;
		texDesc.MipLevels = srvDesc.Texture2D.MipLevels = 1;
		srvDesc.Format = uavDesc.Format = texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		SetupRayRegenerationResources(texDesc);
	}

	logger::debug("Loading noise texture...");
	{
		DirectX::ScratchImage image;
		try {
			std::filesystem::path path{ "Data\\Shaders\\RedstoneGI\\fast_2uges.dds" };

			DX::ThrowIfFailed(LoadFromDDSFile(path.c_str(), DirectX::DDS_FLAGS_NONE, nullptr, image));
		} catch (const DX::com_exception& e) {
			logger::error("{}", e.what());
			return;
		}

		ID3D11Resource* pResource = nullptr;
		try {
			DX::ThrowIfFailed(CreateTexture(device,
				image.GetImages(), image.GetImageCount(),
				image.GetMetadata(), &pResource));
		} catch (const DX::com_exception& e) {
			logger::error("{}", e.what());
			return;
		}

		texNoise = eastl::make_unique<Texture2D>(reinterpret_cast<ID3D11Texture2D*>(pResource), "RedstoneGI::Noise");

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = texNoise->desc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = {
				.MostDetailedMip = 0,
				.MipLevels = 1 }
		};
		texNoise->CreateSRV(srvDesc);
	}

	logger::debug("Creating samplers...");
	{
		D3D11_SAMPLER_DESC samplerDesc = {
			.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR,
			.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP,
			.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP,
			.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP,
			.MaxAnisotropy = 1,
			.MinLOD = 0,
			.MaxLOD = D3D11_FLOAT32_MAX
		};
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, linearClampSampler.put()));
		Util::SetResourceName(linearClampSampler.get(), "RedstoneGI::LinearClampSampler");

		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, pointClampSampler.put()));
		Util::SetResourceName(pointClampSampler.get(), "RedstoneGI::PointClampSampler");
	}

	CompileComputeShaders();
	resourcesReady = IsRuntimeReady();
}

void RedstoneGI::SetupRayRegenerationResources(const D3D11_TEXTURE2D_DESC& a_textureDesc)
{
	rrHistoryValid = false;
	rrOutputApplied = false;
	rrOutputFrame = UINT32_MAX;
	rrHistoryResolution = {};

	try {
		SetupCompositeResources();
		occlusionSettings = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<OcclusionSettings>(), "RedstoneGI::OcclusionSettings");
		const auto createTexture = [](const D3D11_TEXTURE2D_DESC& a_desc, const char* a_name) {
			auto texture = eastl::make_unique<Texture2D>(a_desc, a_name);
			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
			srvDesc.Format = a_desc.Format;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MipLevels = 1;
			texture->CreateSRV(srvDesc);
			D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
			uavDesc.Format = a_desc.Format;
			uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
			texture->CreateUAV(uavDesc);
			return texture;
		};

		texRRSpecularInput = createTexture(a_textureDesc, "RedstoneGI::RRSpecularInput");
		texRRSpecularOutput = createTexture(a_textureDesc, "RedstoneGI::RRSpecularOutput");
		texRRDiffuseInput = createTexture(a_textureDesc, "RedstoneGI::RRDiffuseInput");
		texRRDiffuseOutput = createTexture(a_textureDesc, "RedstoneGI::RRDiffuseOutput");
		auto occlusionDesc = a_textureDesc;
		occlusionDesc.Format = DXGI_FORMAT_R8_UNORM;
		texDiffuseOcclusionInput = createTexture(occlusionDesc, "RedstoneGI::DiffuseOcclusionInput");
		texSpecularOcclusionInput = createTexture(occlusionDesc, "RedstoneGI::SpecularOcclusionInput");
		texDiffuseOcclusionOutput = createTexture(occlusionDesc, "RedstoneGI::DiffuseOcclusionOutput");
		texSpecularOcclusionOutput = createTexture(occlusionDesc, "RedstoneGI::SpecularOcclusionOutput");
		occlusionDesc.Format = DXGI_FORMAT_R8G8_UNORM;
		texPackedOcclusion = createTexture(occlusionDesc, "RedstoneGI::PackedOcclusion");
		texRRNormalRoughness = createTexture(a_textureDesc, "RedstoneGI::RRNormalRoughness");
		texRRMotionVectors = createTexture(a_textureDesc, "RedstoneGI::RRMotionVectors");
		texRRSpecularAlbedo = createTexture(a_textureDesc, "RedstoneGI::RRSpecularAlbedo");
		texRRDiffuseAlbedo = createTexture(a_textureDesc, "RedstoneGI::RRDiffuseAlbedo");

		auto depthDesc = a_textureDesc;
		depthDesc.Format = DXGI_FORMAT_R32_FLOAT;
		texRRLinearDepth = createTexture(depthDesc, "RedstoneGI::RRLinearDepth");
	} catch (const DX::com_exception& e) {
		logger::error("RedstoneGI Ray Regeneration resources unavailable: {}", e.what());
		compositionSettings.reset();
		occlusionSettings.reset();
		texDiffuseOcclusionInput.reset(); texSpecularOcclusionInput.reset();
		texDiffuseOcclusionOutput.reset(); texSpecularOcclusionOutput.reset(); texPackedOcclusion.reset();
		texRRSpecularInput.reset();
		texRRSpecularOutput.reset();
		texRRDiffuseInput.reset();
		texRRDiffuseOutput.reset();
		texRRNormalRoughness.reset();
		texRRMotionVectors.reset();
		texRRSpecularAlbedo.reset();
		texRRDiffuseAlbedo.reset();
		texRRLinearDepth.reset();
	}
}

void RedstoneGI::ClearShaderCache()
{
	switchFailed = false;
	if (!compositionSettings) return;
	compositeShader.Reset();
	compositeInteriorShader.Reset();
	rrPreviewShader.Reset();
	geometryDepthReduceShader.Reset();
	geometryDepthPreviewShader.Reset();
	rrPreviewValid = false;
	rrPreviewFailed = false;
	Util::ClearShaders<ID3D11ComputeShader>({ prefilterDepthsCompute,
		sourceRadianceCompute, giCompute, rrPrepareInputsCompute, rrResolveFallbackCompute, occlusionCompute, occlusionPackCompute });
	rrHistoryValid = false;
	CompileComputeShaders();
	resourcesReady = IsRuntimeReady();
	ResetRuntimeHistory();
}

void RedstoneGI::CompileComputeShaders()
{
	struct ShaderCompileInfo
	{
		winrt::com_ptr<ID3D11ComputeShader>* program;
		const char* filename;
	};
	const ShaderCompileInfo shaders[]{
		{ &prefilterDepthsCompute, "prefilterDepths.cs.hlsl" },
		{ &sourceRadianceCompute, "sourceRadiance.cs.hlsl" },
		{ &occlusionCompute, "occlusion.cs.hlsl" },
		{ &occlusionPackCompute, "occlusionPack.cs.hlsl" },
		{ &giCompute, "gi.cs.hlsl" },
		{ &rrPrepareInputsCompute, "rrPrepareInputs.cs.hlsl" },
		{ &rrResolveFallbackCompute, "rrResolveFallback.cs.hlsl" }
	};
	for (const auto& shader : shaders) {
		std::vector<std::pair<const char*, const char*>> defines;
		if (settings.ResolutionMode == 1) defines.emplace_back("HALF_RES", "");
		if (settings.ResolutionMode == 2) defines.emplace_back("QUARTER_RES", "");
		if (settings.EnableAdaptiveSampling) defines.emplace_back("ADAPTIVE_SAMPLING", "");
		auto path = std::filesystem::path("Data\\Shaders\\RedstoneGI") / shader.filename;
		if (auto* raw = static_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), defines, "cs_5_0")))
			shader.program->attach(raw);
	}
	recompileFlag = false;
}

bool RedstoneGI::ShadersOK() const
{
	return texNoise && prefilterDepthsCompute &&
	       sourceRadianceCompute && giCompute && HasRayRegenerationResources();
}

void RedstoneGI::UpdateSB()
{
	RGICB data{};
	const float2 size{ static_cast<float>(texWorkingDepth->desc.Width), static_cast<float>(texWorkingDepth->desc.Height) };
	const auto dynamic = Util::ConvertToDynamic(size);
	const auto& projection = globals::game::frameBufferCached.GetCameraProj();
	const float projX = projection._11;
	const float projY = projection._22;
	data.NDCToViewMul = { 2.0f / projX, -2.0f / projY };
	data.NDCToViewAdd = { -1.0f / projX, 1.0f / projY };
	data.TexDim = size;
	data.RcpTexDim = { 1.0f / size.x, 1.0f / size.y };
	data.FrameDim = { std::floor(dynamic.x), std::floor(dynamic.y) };
	data.RcpFrameDim = { 1.0f / data.FrameDim.x, 1.0f / data.FrameDim.y };
	data.FrameIndex = globals::state->frameCount;
	data.DiffuseSlices = diffuseEnabled ? diffuseSlices : 0;
	data.DiffuseSteps = diffuseSteps;
	data.SpecularSlices = specularEnabled ? specularSlices : 0;
	data.SpecularSteps = specularSteps;
	data.RRHistoryValid = rrHistoryValid;
	data.RRCheckerboardEnabled = rrCheckerboardThisFrame;
	data.RRCheckerboardOrigin = rrCheckerboardOrigin;
	data.MinScreenRadius = settings.MinScreenRadius * data.FrameDim.x / static_cast<float>(1u << settings.ResolutionMode);
	data.EffectRadius = settings.GIRadius;
	data.GIRadius = settings.GIRadius / data.EffectRadius;
	data.SpecularRoughnessFloor = specularRoughnessFloor;
	data.DepthFadeRange = settings.DepthFadeRange;
	data.DepthFadeScaleConst = 1.0f / std::max(1.0f, settings.DepthFadeRange.y - settings.DepthFadeRange.x);
	data.GISaturation = settings.GISaturation;
	data.GIDistanceCompensation = settings.GIDistanceCompensation;
	data.GICompensationMaxDist = giCompensationMaxDistance;
	data.GIStrength = settings.GIStrength;
	data.OccluderDepth = occluderDepth;
	data.PreviousCameraView = previousCameraView;
	data.PreviousCameraProjection = previousCameraProjection;
	data.PreviousCameraOrigin = previousCameraOrigin;
	data.GeometryHistoryValid = geometryHistoryValid;
	settingsBuffer->Update(data);
}

void RedstoneGI::DrawRayRegeneration(const std::array<uint, 2>& a_engineResolution, const std::array<uint, 2>& a_internalResolution)
{
	denoiser.SetRRDiagnosticsEnabled(rrDiagnosticsEnabled);
	if (!HasRayRegenerationResources() || !a_internalResolution[0] || !a_internalResolution[1]) {
		rrHistoryValid = false;
		return;
	}
	auto context = globals::d3d::context;
	auto rts = globals::game::renderer->GetRuntimeData().renderTargets;
	std::array<ID3D11ShaderResourceView*, 5> srvs{};
	std::array<ID3D11UnorderedAccessView*, 5> uavs{};
	const auto unbindViews = [&]() {
		srvs.fill(nullptr);
		uavs.fill(nullptr);
		context->CSSetShaderResources(0, static_cast<uint>(srvs.size()), srvs.data());
		context->CSSetUnorderedAccessViews(0, static_cast<uint>(uavs.size()), uavs.data(), nullptr);
	};

	{
		CS_GPU_PASS("RedstoneGI::RRPrepareInputs");
		srvs = {
			texWorkingDepth->srv.get(),
			Util::AsReal(rts[NORMALROUGHNESS].SRV),
			Util::AsReal(rts[RE::RENDER_TARGET::kMOTION_VECTOR].SRV),
			Util::AsReal(rts[REFLECTANCE].SRV),
			Util::AsReal(rts[ALBEDO].SRV)
		};
		uavs = {
			texRRLinearDepth->uav.get(),
			texRRMotionVectors->uav.get(),
			texRRNormalRoughness->uav.get(),
			texRRSpecularAlbedo->uav.get(),
			texRRDiffuseAlbedo->uav.get()
		};
		context->CSSetShaderResources(0, static_cast<uint>(srvs.size()), srvs.data());
		context->CSSetUnorderedAccessViews(0, static_cast<uint>(uavs.size()), uavs.data(), nullptr);
		context->CSSetShader(rrPrepareInputsCompute.get(), nullptr, 0);
		context->Dispatch((a_internalResolution[0] + 7u) >> 3, (a_internalResolution[1] + 7u) >> 3, 1);
	}

	unbindViews();
	const bool resetRR = !rrHistoryValid || (globals::state && globals::state->IsMainOrLoadingMenuOpen());
	const RedstoneDenoiser::OcclusionResources occlusionResources{
		texDiffuseOcclusionInput->resource.get(), texSpecularOcclusionInput->resource.get(),
		texDiffuseOcclusionOutput->resource.get(), texSpecularOcclusionOutput->resource.get(),
		(occlusion.diffuseEnabled ? FFX_DENOISER_SIGNAL_AMBIENT_OCCLUSION : 0u) | (occlusion.specularEnabled ? FFX_DENOISER_SIGNAL_SPECULAR_OCCLUSION : 0u) };
	const bool succeeded = rrEnabled && denoiser.DispatchRayRegeneration(
		texRRSpecularInput->resource.get(), texRRLinearDepth->resource.get(),
		texRRMotionVectors->resource.get(), texRRNormalRoughness->resource.get(),
		texRRSpecularAlbedo->resource.get(), texRRDiffuseAlbedo->resource.get(),
		texRRSpecularOutput->resource.get(), texRRDiffuseInput->resource.get(), texRRDiffuseOutput->resource.get(),
		occlusionResources, a_engineResolution, a_internalResolution[0], a_internalResolution[1], resetRR, rrCheckerboardThisFrame, rrCheckerboardOrigin);
	if (!succeeded) {
		CS_GPU_PASS("RedstoneGI::HostResolve");
		srvs[0] = texRRSpecularInput->srv.get();
		srvs[1] = texRRDiffuseInput->srv.get();
		srvs[2] = texRRLinearDepth->srv.get();
		srvs[3] = texRRNormalRoughness->srv.get();
		uavs[0] = texRRSpecularOutput->uav.get();
		uavs[1] = texRRDiffuseOutput->uav.get();
		context->CSSetShaderResources(0, 4, srvs.data());
		context->CSSetUnorderedAccessViews(0, 2, uavs.data(), nullptr);
		context->CSSetShader(rrResolveFallbackCompute.get(), nullptr, 0);
		context->Dispatch((a_internalResolution[0] + 7u) >> 3, (a_internalResolution[1] + 7u) >> 3, 1);
	}
	unbindViews();
	PackOcclusion(a_internalResolution, succeeded);
	rrLightingReady = true;
	rrProviderSucceeded = succeeded;
	rrOutputFrame = static_cast<uint32_t>(*globals::game::frameCounter);
	if (IsRRDebugVisible() && GetRRDebugView() != RRDebugView::GeometryDepth && (succeeded || !rrEnabled || IsRRInputsOnly()))
		PrepareRRDebugPreview(a_internalResolution);
	rrHistoryValid = succeeded;
}

void RedstoneGI::DrawGI()
{
	CS_GPU_PASS("RedstoneGI::DrawGI");
	if (!globals::game::frameCounter) return;
	EnforceFixedPath();
	ID3D11ShaderResourceView* previousInputs[16]{};
	globals::d3d::context->CSSetShaderResources(0, ARRAYSIZE(previousInputs), previousInputs);
	rrPreviewValid = rrOutputApplied = rrLightingReady = rrProviderSucceeded = false;
	if (auto* setting = RE::GetINISetting("bSAOEnable:Display")) setting->data.b = settings.EnableVanillaSSAO;
	auto imageSpaceManager = globals::game::imageSpaceManager;
	GET_INSTANCE_MEMBER_VRPTR(BSImagespaceShaderISSAOBlurH, imageSpaceManager);
	if (auto* sao = BSImagespaceShaderISSAOBlurH) sao->enableSAO = settings.EnableVanillaSSAO;
	if (recompileFlag) ClearShaderCache();
	if (!ShadersOK()) {
		geometryHistoryValid = false;
		rrHistoryValid = false;
		return;
	}
	const auto dynamic = Util::ConvertToDynamic(globals::state->screenSize);
	const std::array<uint, 2> resolution{ static_cast<uint>(dynamic.x), static_cast<uint>(dynamic.y) };
	const std::array<uint, 2> internal{ resolution[0] >> settings.ResolutionMode, resolution[1] >> settings.ResolutionMode };
	if (!internal[0] || !internal[1]) return;
	if (queuedResetHistory.exchange(false) || rrHistoryResolution != internal ||
		!globals::game::frameCounter || (geometryHistoryValid && rrOutputFrame + 1u != static_cast<uint32_t>(*globals::game::frameCounter)))
		geometryHistoryValid = rrHistoryValid = false;
	rrHistoryResolution = internal;
	rrCheckerboardThisFrame = rrCheckerboardEnabled && internal[0] > 1u && !(internal[0] & 1u) && denoiser.CanDispatchRayRegeneration();
	rrCheckerboardOrigin = globals::game::frameCounter ? static_cast<uint32_t>(*globals::game::frameCounter) & 1u : 0u;
	if (globals::state->IsMainOrLoadingMenuOpen()) geometryHistoryValid = false;
	UpdateSB();
	auto* context = globals::d3d::context;
	auto* cb = settingsBuffer->CB();
	context->CSSetConstantBuffers(1, 1, &cb);
	globals::state->BindSharedDataCS(context, false);
	ID3D11SamplerState* samplers[]{ pointClampSampler.get(), linearClampSampler.get() };
	context->CSSetSamplers(0, 2, samplers);
	winrt::com_ptr<ID3D11Buffer> previousOcclusionSettings;
	context->CSGetConstantBuffers(2, 1, previousOcclusionSettings.put());
	PrepareSurfaceInputs(resolution, internal);
	if (IsRRDebugVisible() && GetRRDebugView() == RRDebugView::GeometryDepth)
		PrepareGeometryDebugPreview(resolution);
	DrawEstimators(internal);
	DrawOcclusion(internal);
	DrawRayRegeneration(resolution, internal);
	previousCameraView = globals::game::frameBufferCached.GetCameraView();
	previousCameraProjection = globals::game::frameBufferCached.GetCameraProjUnjittered();
	previousCameraOrigin = globals::game::frameBufferCached.GetCameraPosAdjust();
	geometryHistoryValid = true;
	auto* previousOcclusionCB = previousOcclusionSettings.get();
	context->CSSetConstantBuffers(2, 1, &previousOcclusionCB);
	cb = nullptr;
	samplers[0] = samplers[1] = nullptr;
	context->CSSetConstantBuffers(1, 1, &cb);
	context->CSSetSamplers(0, 2, samplers);
	context->CSSetShader(nullptr, nullptr, 0);
}

#include "GrassOptimizations.h"
<<<<<<< HEAD
#include "GpuPass.h"
#include "GrassCollision.h"
#include "GrassLighting.h"
#include "ShaderCache.h"
#include "State.h"
#include "TerrainBlending.h"  // loaded state selects the scene depth SRV's format
#include "Utils/Game.h"
#include "Wind/Wind.h"
||||||| 12b261724
#include "GrassLighting.h"
#include "TerrainBlending.h"  // loaded state selects the scene depth SRV's format
=======

#include <span>

#include "Profiler.h"
#include "State.h"
#include "TruePBR.h"
#include "TruePBR/BSLightingShaderMaterialPBR.h"
#include "Utils/VersionedRelocation.h"
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb

#define I18N_KEY_PREFIX "feature.grass_optimizations."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	GrassOptimizations::Settings,
	Enabled,
	MinPixelSize,
	FullDetailPixelSize,
	MinDensity,
	MeshCostBias,
	CostBiasStartDistance,
	InvisibleFadeCull,
	RenderDistanceOverride,
	EdgeFadeStart,
	EnableOcclusionCulling,
	OcclusionBias,
	SimpleShadingPixelSize,
	EnableMeshLOD,
	EnableMidLOD,
	MidLODPixelSize,
	EnableFarLOD,
	FarLODPixelSize,
	MeshLODBandPixels)

void GrassOptimizations::LoadSettings(json& o_json)
{
	settings = o_json;
}

void GrassOptimizations::SaveSettings(json& o_json)
{
	o_json = settings;
}

void GrassOptimizations::RestoreDefaultSettings()
{
	settings = {};
}

void GrassOptimizations::DrawSettings()
{
	ImGui::Checkbox(T(TKEY("enabled"), "Enabled"), &settings.Enabled);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("enabled_tooltip"), "Use GPU-driven grass culling and instanced draws. When disabled, grass renders through the vanilla path and all other settings are ignored."));
	}
	auto enabledGuard = Util::DisableGuard(!settings.Enabled);

	ImGui::SeparatorText(T(TKEY("culling"), "Culling & LOD"));

	ImGui::SliderFloat(T(TKEY("full_detail_pixel_size"), "Full-Detail Pixel Size"), &settings.FullDetailPixelSize, 4.0f, 128.0f, "%.1f px");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("full_detail_pixel_size_tooltip"),
							  "Instances whose on-screen radius is above this render at full density. Below it, density thinning increases over the full INI grass distance. A shorter Grass Render Distance cuts this ramp off early without changing density closer to the player. Min Pixel Size remains a hard cutoff. Increasing this setting improves performance by removing closer grass."));
	}

	ImGui::SliderFloat(T(TKEY("min_pixel_size"), "Min Pixel Size"), &settings.MinPixelSize, 1.0f, 32.0f, "%.1f px");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("min_pixel_size_tooltip"),
							  "Individual grass instances that visibly take up less space on the screen than this are dropped entirely. Higher values improve performance by removing far-away grass instances sooner."));
	}

	Util::PercentageSlider(T(TKEY("min_density"), "Minimum Density"), &settings.MinDensity);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("min_density_tooltip"),
							  "The lowest density targeted by pixel-based thinning at the full INI grass distance. Closer grass retains more density, and Min Pixel Size still removes instances below its cutoff."));
	}

	Util::PercentageSlider(T(TKEY("mesh_cost_bias"), "Mesh Cost Bias"), &settings.MeshCostBias);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("mesh_cost_bias_tooltip"),
							  "Culls or removes grass meshes based on their complexity (performance impact). At 0, removal is identical between all grass types regardless of complexity. At 1, heavier and more complex meshes are culled 2-6x sooner than simple ones. Only applies beyond the Cost Bias Start Distance, so nearby grass is never thinned."));
	}

	ImGui::SliderFloat(T(TKEY("cost_bias_start_distance"), "Cost Bias Start Distance"), &settings.CostBiasStartDistance, 0.0f, 20000.0f, "%.0f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		std::vector<std::string> tooltipLines = {
			T(TKEY("cost_bias_start_distance_tooltip"),
				"Distance at which Mesh Cost Bias starts taking effect, ramping to full over the same distance again. Nearer than this, all grass types are treated identically no matter how complex. Zero applies the bias everywhere, including right in front of the player."),
			Util::Units::FormatDistance(settings.CostBiasStartDistance)
		};
		Util::DrawMultiLineTooltip(tooltipLines);
	}

	ImGui::SliderFloat(T(TKEY("render_distance_override"), "Grass Render Distance"), &settings.RenderDistanceOverride, 0.0f, 100000.0f, "%.0f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("render_distance_override_tooltip"),
							  "Max grass render distance in units. 0 = use the game's INI cap (fGrassStartFadeDistance + fGrassFadeRange). Any grass beyond the vanilla range or this range will be removed."));
	}

	Util::PercentageSlider(T(TKEY("edge_fade_start"), "Edge Fade Start"), &settings.EdgeFadeStart);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		std::vector<std::string> tooltipLines = {
			T(TKEY("edge_fade_start_tooltip"),
				"Percent of the grass render distance at which grass starts fading out. The default of 0.85 fades over the last 15%. A lower value results in a longer, smoother fade out, while a value of 1.0 disables the fade and grass pops out at the render distance."),
			Util::Units::FormatDistance(maxGrassDistance * settings.EdgeFadeStart)
		};
		Util::DrawMultiLineTooltip(tooltipLines);
	}

	ImGui::SliderFloat(T(TKEY("invisible_fade_cull"), "Invisible Fade Cull"), &settings.InvisibleFadeCull, 0.0f, 0.5f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("invisible_fade_cull_tooltip"),
							  "Skip drawing grass whose transparency is below this threshold. Grass with a fade value of zero is completely invisible and thus is removed early for performance reasons."));
	}

	ImGui::SliderFloat(T(TKEY("simple_shading_px"), "Simple Shading Below"), &settings.SimpleShadingPixelSize, 0.0f, 32.0f, "%.1f px");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("simple_shading_px_tooltip"),
							  "Grass instances smaller than this size on screen will skip barely visible detail including contact shadows, specular highlights, and other complex grass visual elements. Zero disables this feature."));
	}

<<<<<<< HEAD
||||||| 12b261724
	ImGui::SliderFloat(T(TKEY("collision_distance"), "Collision Distance"), &settings.CollisionDistance, 0.0f, 8192.0f, "%.0f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		std::vector<std::string> tooltipLines = {
			T(TKEY("collision_distance_tooltip"),
				"Grass beyond this distance skips any collision detection. Zero disables collision on all grass. Requires the Grass Collision feature."),
			Util::Units::FormatDistance(settings.CollisionDistance)
		};
		Util::DrawMultiLineTooltip(tooltipLines);
	}

=======
	ImGui::SliderFloat(T(TKEY("collision_distance"), "Collision Distance"), &settings.CollisionDistance, 0.0f, 8192.0f, "%.0f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		std::vector<std::string> tooltipLines = {
			T(TKEY("collision_distance_tooltip"),
				"Grass beyond this distance skips any collision detection. Zero disables collision on all grass. Requires the Grass Collision feature."),
			Util::Units::FormatDistance(settings.CollisionDistance)
		};
		Util::DrawMultiLineTooltip(tooltipLines);
	}

	ImGui::Spacing();

	ImGui::Checkbox(T(TKEY("occlusion_culling"), "Occlusion Culling"), &settings.EnableOcclusionCulling);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("occlusion_culling_tooltip"),
							  "Skips grass hidden behind rocks, buildings and NPCs. Depending on how much grass is not visible, this may cost more than its benefits. If you see grass flickering when moving, try disabling this. Terrain such as hills is not treated as an occluder when Terrain Blending is enabled, which reduces the benefit."));
	}

	ImGui::SliderFloat(T(TKEY("occlusion_bias"), "Occlusion Bias"), &settings.OcclusionBias, 0.0f, 0.05f, "%.4f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("occlusion_bias_tooltip"),
							  "How far behind an occluder grass must sit before Occlusion Culling removes it. Raise this if grass disappears around the edges of rocks and hills, lower it to reclaim more performance. Has no effect unless Occlusion Culling is enabled."));
	}

>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
	ImGui::SeparatorText(T(TKEY("mesh_lod"), "Mesh LOD"));

	ImGui::Checkbox(T(TKEY("enable_mesh_lod"), "Enable Mesh LOD"), &settings.EnableMeshLOD);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("enable_mesh_lod_tooltip"),
							  "Improves performance by swapping distant grass instances for a simpler LOD mesh, in two bands. Requires an LOD .nif per grass type at meshes\\LOD\\Grass\\<source-mesh-name>_LOD0.nif, plus an optional _LOD1.nif for the far band. Grass with no LOD mesh keeps its full mesh."));
	}

	ImGui::BeginDisabled(!settings.EnableMeshLOD);

	ImGui::Checkbox(T(TKEY("enable_mid_lod"), "Enable Middle LOD"), &settings.EnableMidLOD);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("enable_mid_lod_tooltip"),
							  "Swaps mid-distance grass to the _LOD0.nif mesh. With this off, grass stays on its full mesh until the far band takes over."));
	}

	ImGui::SliderFloat(T(TKEY("mid_lod_pixel_size"), "Middle LOD Pixel Size"), &settings.MidLODPixelSize, 1.0f, 64.0f, "%.1f px");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("mid_lod_pixel_size_tooltip"),
							  "Instances whose on-screen radius is below this but above the Far LOD Pixel Size swap to the _LOD0.nif mesh."));
	}

	ImGui::Checkbox(T(TKEY("enable_far_lod"), "Enable Far LOD"), &settings.EnableFarLOD);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("enable_far_lod_tooltip"),
							  "Swaps the most distant grass to the _LOD1.nif mesh. Grass types without that file reuse their _LOD0.nif, so the far band still gets its own brightness."));
	}

	ImGui::SliderFloat(T(TKEY("far_lod_pixel_size"), "Far LOD Pixel Size"), &settings.FarLODPixelSize, 1.0f, 64.0f, "%.1f px");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("far_lod_pixel_size_tooltip"),
							  "Instances whose on-screen radius is below this but above Min Pixel Size swap to the _LOD1.nif mesh. Values above the Middle LOD Pixel Size are clamped to it, since the far band is always the more distant of the two."));
	}

	ImGui::SliderFloat(T(TKEY("mesh_lod_band"), "Mesh LOD Transition Band"), &settings.MeshLODBandPixels, 0.0f, 16.0f, "%.1f px");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("mesh_lod_band_tooltip"),
							  "The range of on-screen sizes over which a random amount of meshes are swapped out before completely transitioning to the next LOD. Applies to both transitions. A wider range results in a smoother transition."));
	}

	ImGui::EndDisabled();
}

bool GrassOptimizations::HasShaderDefine(RE::BSShader::Type shaderType)
{
	return shaderType == RE::BSShader::Type::Grass;
}

void GrassOptimizations::PostPostLoad()
{
	Hooks::Install();
	active = true;
	if (!settings.Enabled)
		ApplyActive(false);
}

void GrassOptimizations::Reset()
{
<<<<<<< HEAD
	switch (shaderType) {
	case RE::BSShader::Type::Grass:
		return active;
	default:
		return false;
	}
||||||| 12b261724
	switch (shaderType) {
	case RE::BSShader::Type::Grass:
		return true;
	default:
		return false;
	}
=======
	// Bucket mutations stay on the render thread, so draw setup needs no lock.
	bucketStore.EndFrame();
	bucketStore.meshLibrary.CommitLoadedLODMeshes();
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
}

void GrassOptimizations::ClearShaderCache()
{
	cullCS = nullptr;
	cullCSFailed = false;
	hiZ.ClearShaderCache();
	bucketStore.meshLibrary.ClearShaderCache();
}

<<<<<<< HEAD
static void ComputeCameraRelativeFrustumPlanes(float (*out)[4], const Matrix& viewProj)
{
	// Matches GrassCullingCS.hlsl's divide-by-zero guard convention (e.g. max(length(dvC), 1e-4)).
	constexpr float kPlaneNormalizeEpsilon = 1e-4f;

	const float rows[4][4] = {
		{ viewProj._11, viewProj._12, viewProj._13, viewProj._14 },
		{ viewProj._21, viewProj._22, viewProj._23, viewProj._24 },
		{ viewProj._31, viewProj._32, viewProj._33, viewProj._34 },
		{ viewProj._41, viewProj._42, viewProj._43, viewProj._44 }
	};

	const auto setPlane = [&](uint32_t index, uint32_t rowA, float signA, uint32_t rowB, float signB) {
		float plane[4];
		for (uint32_t component = 0; component < 4; ++component)
			plane[component] = rows[rowA][component] * signA + rows[rowB][component] * signB;
		const float invLength = 1.0f / std::max(std::sqrt(plane[0] * plane[0] + plane[1] * plane[1] + plane[2] * plane[2]), kPlaneNormalizeEpsilon);
		out[index][0] = plane[0] * invLength;
		out[index][1] = plane[1] * invLength;
		out[index][2] = plane[2] * invLength;
		out[index][3] = -plane[3] * invLength;
	};

	setPlane(0, 2, 1.0f, 2, 0.0f);
	setPlane(1, 3, 1.0f, 2, -1.0f);
	setPlane(2, 3, 1.0f, 0, 1.0f);
	setPlane(3, 3, 1.0f, 0, -1.0f);
	setPlane(4, 3, 1.0f, 1, -1.0f);
	setPlane(5, 3, 1.0f, 1, 1.0f);
}

void GrassOptimizations::UpdateGrass()
||||||| 12b261724
void GrassOptimizations::UpdateGrass()
=======
RE::NiDirectionalLight* GrassOptimizations::GetSunLight()
{
	auto* sceneNode = globals::game::smState->shadowSceneNode[0];
	auto* sun = sceneNode ? sceneNode->GetRuntimeData().sunLight : nullptr;
	return sun ? skyrim_cast<RE::NiDirectionalLight*>(sun->light.get()) : nullptr;
}

void GrassOptimizations::UpdateGrass(const RE::NiCamera* cam)
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
{
	auto* ctx = globals::d3d::context;

	const bool canCull = GetCullCS() && ctx1 && cullParamsCB && GetSunLight() && !globals::game::smState->liteBrite;

	if (auto* grassManager = RE::BGSGrassManager::GetSingleton())
		Hooks::NiNode_OnVisible::grassRoot.store(grassManager->grassNode.get(), std::memory_order_relaxed);

	timeAccum += globals::game::smState->timerValues[1];
	prevTimeBase = timeBase;
	timeBase = globals::game::smState->timerValues[4] * 0.0016666667f * 6.2831802f;

	const auto iniFloat = [](const char* name, float fallback) {
		auto* setting = RE::GetINISetting(name);
		return setting ? setting->GetFloat() : fallback;
	};

	if (fadeInTimeRcp == 0.0f) {
		const float t = iniFloat("fGrassFadeInTime:Grass", 0.0f);
		fadeInTimeRcp = t > 0.0f ? 1.0f / t : 1e6f;
	}

	if (vanillaMaxDistance == 0.0f) {
		grassStartFadeDistance = iniFloat("fGrassStartFadeDistance:Grass", 6000.0f);
		vanillaMaxDistance = grassStartFadeDistance + iniFloat("fGrassFadeRange:Grass", 2000.0f);
	}

	maxGrassDistance = settings.RenderDistanceOverride > 0.0f ? settings.RenderDistanceOverride : vanillaMaxDistance;
	maxDistSq = maxGrassDistance * maxGrassDistance;

	// Start the fade one frame early, matching the engine and avoiding a gap when grass is replaced.
	bucketStore.BeginFrame({ settings.EnableMeshLOD, settings.EnableMidLOD, settings.EnableFarLOD, timeAccum - globals::game::smState->timerValues[1] });

<<<<<<< HEAD
	{
		CS_GPU_PASS("GrassOptimizations::ApplyPending");
		bucketStore.RefreshComplexGrass(globals::features::grassLighting.settings.ComplexGrassThreshold, ctx);
		bucketStore.ApplyPending(device, ctx);
	}
||||||| 12b261724
	globals::profiler->BeginPass("GrassOptimizations::ApplyPending");
	bucketStore.RefreshComplexGrass(globals::features::grassLighting.settings.ComplexGrassThreshold, ctx);
	bucketStore.ApplyPending(device, ctx);
	globals::profiler->EndPass();
=======
	globals::profiler->BeginPass("GrassOptimizations::UploadPending");
	bucketStore.UploadPending(ctx);
	globals::profiler->EndPass();
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb

	if (!canCull || !cam) {
		// Clear stale cull results before returning to engine draws.
		DisableOptimizedDraws();
		return;
	}

<<<<<<< HEAD
	const bool isVR = globals::game::isVR;

	// cam->world with its translate swapped for the given eye's actual position; flat non-VR keeps
	// cam->world untouched since there's only one eye.
	const auto eyeTransform = [&](uint32_t eye) {
		RE::NiTransform t = cam->world;
		if (isVR)
			t.translate = Util::GetEyePosition(eye);
		return t;
	};

	RE::NiFrustumPlanes frustum{};
	ComputeFrustumPlanes(frustum, isVR ? cam->GetVRRuntimeData().eyeFrustums[0] : cam->GetRuntimeData2().viewFrustum, eyeTransform(0));
	const RE::NiPoint3 camPos = cam->world.translate;
	const __m128 camPosV = _mm_setr_ps(camPos.x, camPos.y, camPos.z, 0.0f);

	RE::NiFrustumPlanes frustum1{};
	if (isVR)
		ComputeFrustumPlanes(frustum1, cam->GetVRRuntimeData().eyeFrustums[1], eyeTransform(1));

	FrustumSoA frustumSoAs[2];
	BuildFrustumSoA(frustumSoAs[0], frustum);
	BuildFrustumSoA(frustumSoAs[1], isVR ? frustum1 : frustum);
	const uint32_t frustumCount = isVR ? 2u : 1u;

	if (settings.EnableOcclusionCulling)
		hiZ.Build(device, ctx);
	else
		hiZ.Invalidate();

	{
		CullParamsCB cp{};
		if (isVR) {
			ComputeCameraRelativeFrustumPlanes(cp.frustumPlanes, globals::game::frameBufferCached.GetCameraViewProj(0));
			ComputeCameraRelativeFrustumPlanes(cp.frustumPlanes + 6, globals::game::frameBufferCached.GetCameraViewProj(1));
		} else {
			for (int i = 0; i < 6; ++i) {
				cp.frustumPlanes[i][0] = frustum.cullingPlanes[i].normal.x;
				cp.frustumPlanes[i][1] = frustum.cullingPlanes[i].normal.y;
				cp.frustumPlanes[i][2] = frustum.cullingPlanes[i].normal.z;
				cp.frustumPlanes[i][3] = frustum.cullingPlanes[i].constant;
				std::copy_n(cp.frustumPlanes[i], 4, cp.frustumPlanes[6 + i]);
			}
		}
		cp.eyeCount = frustumCount;

		cp.minPixelSize = settings.MinPixelSize;
		cp.fullDetailPixelSize = settings.FullDetailPixelSize;
		cp.lodMinKeep = settings.MinDensity;
		cp.lodFadeBand = 0.15f;

		const auto& vf = isVR ? cam->GetVRRuntimeData().eyeFrustums[0] : cam->GetRuntimeData2().viewFrustum;
		// Hi-Z uses nominal pixels; density and LOD thresholds use render-resolution pixels.
		const float screenH = Util::ConvertToDynamic(globals::state->screenSize).y;
		const float frustumHeight = 2.0f * std::abs(vf.fTop);
		cp.meshCostBias = settings.MeshCostBias;
		cp.projScale = screenH / frustumHeight;
		cp.hiZProjScale = globals::state->screenSize.y / frustumHeight;
		cp.maxDistSq = maxDistSq;
		cp.edgeFadeStart = std::clamp(settings.EdgeFadeStart, 0.0f, 1.0f);

		cp.alphaParam1 = grassStartFadeDistance;
		cp.alphaParam2 = maxGrassDistance;
		cp.fadeNow = timeAccum;
		cp.fadeInTimeRcp = fadeInTimeRcp;

		cp.invisibleFadeCull = settings.InvisibleFadeCull;
		cp.simpleShadingPixelSize = std::max(0.0f, settings.SimpleShadingPixelSize);
		cp.midLODPixelSize = settings.MidLODPixelSize;
		cp.farLODPixelSize = settings.EnableMidLOD ? std::min(settings.FarLODPixelSize, settings.MidLODPixelSize) : settings.FarLODPixelSize;

		cp.meshLODBandPx = std::max(0.0f, settings.MeshLODBandPixels);
		cp.hiZEnabled = hiZ.IsValid() ? 1.0f : 0.0f;
		cp.hiZSizeX = (float)hiZ.GetWidth();
		cp.hiZSizeY = (float)hiZ.GetHeight();

		cp.hiZTexelPixels = hiZ.GetTexelPixels();
		cp.hiZMipCount = (float)hiZ.GetMipCount();
		cp.occlusionBias = std::max(0.0f, settings.OcclusionBias);
		cp.costBiasStartDist = std::max(0.0f, settings.CostBiasStartDistance);

		cullParamsCB->Update(cp);
	}

	uint32_t visibleBuckets = 0;
	sliceTableCPU.clear();

	{
		// Measures the CPU time spent frustum culling bucket slices
		CS_GPU_PASS("GrassOptimizations::SliceCull");
		for (auto& [key, b] : bucketStore.buckets) {
			b.ResetCullState();
			if (!b.totalInstances || !b.instanceSRV)
				continue;

			if (!b.coarseValid)
				bucketStore.UpdateCoarseBounds(b);

			CullBucketSlices(b, frustumSoAs, frustumCount, camPosV);

			if (!b.cullVisible)
				continue;

			for (uint32_t tier = 0; tier < (uint32_t)GrassMeshLibrary::LODTier::kCount; ++tier)
				b.lodBins[tier].active = bucketStore.EnsureLODBin(b, (GrassMeshLibrary::LODTier)tier, device);
			++visibleBuckets;
		}
	}

	{
		// Measures the slice table upload, the per-bucket constants, and the cull dispatch per visible bucket.
		CS_GPU_PASS("GrassOptimizations::InstanceCull");
		UploadCullState(device, ctx, visibleBuckets);
	}
||||||| 12b261724
	RE::NiFrustumPlanes frustum{};
	ComputeFrustumPlanes(frustum, cam->GetRuntimeData2().viewFrustum, cam->world);
	const RE::NiPoint3 camPos = cam->world.translate;
	const __m128 camPosV = _mm_setr_ps(camPos.x, camPos.y, camPos.z, 0.0f);
	FrustumSoA frustumSoA;
	BuildFrustumSoA(frustumSoA, frustum);

	if (settings.EnableOcclusionCulling)
		hiZ.Build(device, ctx);
	else
		hiZ.Invalidate();

	{
		CullParamsCB cp{};
		for (int i = 0; i < 6; ++i) {
			cp.frustumPlanes[i][0] = frustum.cullingPlanes[i].normal.x;
			cp.frustumPlanes[i][1] = frustum.cullingPlanes[i].normal.y;
			cp.frustumPlanes[i][2] = frustum.cullingPlanes[i].normal.z;
			cp.frustumPlanes[i][3] = frustum.cullingPlanes[i].constant;
		}

		cp.minPixelSize = settings.MinPixelSize;
		cp.fullDetailPixelSize = settings.FullDetailPixelSize;
		cp.lodMinKeep = settings.MinDensity;
		cp.lodFadeBand = 0.15f;

		const auto& vf = cam->GetRuntimeData2().viewFrustum;
		const auto [screenW, screenH] = globals::game::renderer->GetScreenSize();
		cp.meshCostBias = settings.MeshCostBias;
		cp.projScale = screenH / (2.0f * std::abs(vf.fTop));
		cp.maxDistSq = maxDistSq;
		cp.edgeFadeStart = std::clamp(settings.EdgeFadeStart, 0.0f, 1.0f);

		cp.alphaParam1 = grassStartFadeDistance;
		cp.alphaParam2 = maxGrassDistance;
		cp.fadeNow = timeAccum;
		cp.fadeInTimeRcp = fadeInTimeRcp;

		const float collisionDist = std::max(0.0f, settings.CollisionDistance);
		cp.invisibleFadeCull = settings.InvisibleFadeCull;
		cp.simpleShadingPixelSize = std::max(0.0f, settings.SimpleShadingPixelSize);
		cp.collisionDistSq = collisionDist * collisionDist;
		cp.midLODPixelSize = settings.MidLODPixelSize;
		cp.farLODPixelSize = settings.EnableMidLOD ? std::min(settings.FarLODPixelSize, settings.MidLODPixelSize) : settings.FarLODPixelSize;

		cp.meshLODBandPx = std::max(0.0f, settings.MeshLODBandPixels);
		cp.hiZEnabled = hiZ.IsValid() ? 1.0f : 0.0f;
		cp.hiZSizeX = (float)hiZ.GetWidth();
		cp.hiZSizeY = (float)hiZ.GetHeight();

		cp.hiZTexelPixels = hiZ.GetTexelPixels();
		cp.hiZMipCount = (float)hiZ.GetMipCount();
		cp.occlusionBias = std::max(0.0f, settings.OcclusionBias);
		cp.costBiasStartDist = std::max(0.0f, settings.CostBiasStartDistance);

		cullParamsCB->Update(cp);
	}

	uint32_t visibleBuckets = 0;
	sliceTableCPU.clear();

	// Measures the CPU time spent frustum culling bucket slices
	globals::profiler->BeginPass("GrassOptimizations::SliceCull");
	for (auto& [key, b] : bucketStore.buckets) {
		b.ResetCullState();
		if (!b.totalInstances || !b.instanceSRV)
			continue;

		if (!b.coarseValid)
			bucketStore.UpdateCoarseBounds(b);

		CullBucketSlices(b, frustumSoA, camPosV);

		if (!b.cullVisible)
			continue;

		for (uint32_t tier = 0; tier < (uint32_t)GrassMeshLibrary::LODTier::kCount; ++tier)
			b.lodBins[tier].active = bucketStore.EnsureLODBin(b, (GrassMeshLibrary::LODTier)tier, device);
		++visibleBuckets;
	}
	globals::profiler->EndPass();

	// Measures the slice table upload, the per-bucket constants, and the cull dispatch per visible bucket.
	globals::profiler->BeginPass("GrassOptimizations::InstanceCull");
	UploadCullState(device, ctx, visibleBuckets);
	globals::profiler->EndPass();
=======
	CullGrass(cam);
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
}

GrassOptimizations::GrassPassSetup GrassOptimizations::SetupGrassPass(RE::BSShader* shader, RE::BSRenderPass* pass)
{
	if (auto* bucket = loaded ? bucketStore.FindBucket(pass->geometry) : nullptr) {
		if (!HasCurrentCull() || !bucket->optimizedReady || !bucket->sliceTableCount)
			return GrassPassSetup::kCovered;

<<<<<<< HEAD
	const auto continuesRun = [&cellOf](const BucketSlice& slice, const GrassBucket::SliceRun& run, uint64_t cell) {
		return slice.bufferOffset != UINT32_MAX && slice.count != 0 &&
		       slice.bufferOffset == run.firstSliceOffset + run.instanceCount &&
		       cellOf(slice.origin) == cell;
	};

	b.sliceRuns.clear();
	const uint32_t sliceCount = (uint32_t)b.slices.size();

	for (uint32_t first = 0; first < sliceCount;) {
		if (b.slices[first].bufferOffset == UINT32_MAX || b.slices[first].count == 0) {
			++first;
			continue;
		}

		GrassBucket::SliceRun run;
		run.firstSliceOffset = b.slices[first].bufferOffset;
		run.instanceCount = b.slices[first].count;
		__m128 lo = _mm_load_ps(b.sliceBounds[first].lo);
		__m128 hi = _mm_load_ps(b.sliceBounds[first].hi);
		const uint64_t cell = cellOf(b.slices[first].origin);

		uint32_t next = first + 1;
		for (; next < sliceCount && continuesRun(b.slices[next], run, cell); ++next) {
			lo = _mm_min_ps(lo, _mm_load_ps(b.sliceBounds[next].lo));
			hi = _mm_max_ps(hi, _mm_load_ps(b.sliceBounds[next].hi));
			run.instanceCount += b.slices[next].count;
		}

		_mm_store_ps(run.bounds.lo, lo);
		_mm_store_ps(run.bounds.hi, hi);
		b.sliceRuns.push_back(run);
		first = next;
||||||| 12b261724
	const auto continuesRun = [&cellOf](const BucketSlice& slice, const GrassBucket::SliceRun& run, uint64_t cell) {
		return slice.bufferOffset != UINT32_MAX && slice.count != 0 &&
		       slice.bufferOffset == run.firstSliceOffset + run.instanceCount &&
		       cellOf(slice.origin) == cell;
	};

	b.sliceRuns.clear();
	const uint32_t sliceCount = (uint32_t)b.slices.size();

	for (uint32_t first = 0; first < sliceCount;) {
		if (b.slices[first].bufferOffset == UINT32_MAX || b.slices[first].count == 0)
		{
			++first;
			continue;
		}

		GrassBucket::SliceRun run;
		run.firstSliceOffset = b.slices[first].bufferOffset;
		run.instanceCount = b.slices[first].count;
		__m128 lo = _mm_load_ps(b.sliceBounds[first].lo);
		__m128 hi = _mm_load_ps(b.sliceBounds[first].hi);
		const uint64_t cell = cellOf(b.slices[first].origin);

		uint32_t next = first + 1;
		for (; next < sliceCount && continuesRun(b.slices[next], run, cell); ++next) {
			lo = _mm_min_ps(lo, _mm_load_ps(b.sliceBounds[next].lo));
			hi = _mm_max_ps(hi, _mm_load_ps(b.sliceBounds[next].hi));
			run.instanceCount += b.slices[next].count;
		}

		_mm_store_ps(run.bounds.lo, lo);
		_mm_store_ps(run.bounds.hi, hi);
		b.sliceRuns.push_back(run);
		first = next;
=======
		SetupOptimizedPass(*bucket);
		return GrassPassSetup::kOptimized;
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
	}

	if (loaded && HasCurrentCull() && pass->geometry->GetType().get() == RE::BSGeometry::Type::kMultiStreamInstanceTriShape) {
		const auto* snapshot = bucketStore.GetCullSnapshot();
		if (const auto* native = snapshot ? snapshot->Find(pass->geometry) : nullptr; native && bucketStore.IsShapeOptimized(*native, globals::game::graphicsState->frameCount))
			return GrassPassSetup::kCovered;
	}

	// Native coverage owns its material setup because optimized passes bind their own material state.
	const auto* shaderProperty = pass->shaderProperty;
	if (loaded && shaderProperty && shaderProperty->material)
		Hooks::BSGrassShader_SetupMaterial::func(shader, static_cast<const RE::BSLightingShaderMaterialBase*>(shaderProperty->material));
	return GrassPassSetup::kNative;
}

<<<<<<< HEAD
bool GrassOptimizations::AnyFrustumVisible(const FrustumSoA* frustums, uint32_t frustumCount, __m128 lo, __m128 hi)
{
	for (uint32_t i = 0; i < frustumCount; ++i) {
		if (AabbVisible(frustums[i], lo, hi))
			return true;
	}
	return false;
}

void GrassOptimizations::CullBucketSlices(GrassBucket& b, const FrustumSoA* frustumSoAs, uint32_t frustumCount, __m128 camPosV)
||||||| 12b261724
void GrassOptimizations::CullBucketSlices(GrassBucket& b, const FrustumSoA& frustumSoA, __m128 camPosV)
=======
void GrassOptimizations::SetupOptimizedPass(const GrassBucket& b)
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
{
<<<<<<< HEAD
	b.sliceTableOffset = (uint32_t)sliceTableCPU.size();
	b.sliceTableCount = 0;
	b.visibleInstances = 0;
	b.cullVisible = false;
	for (GrassBucket::LODBin& bin : b.lodBins)
		bin.active = false;

	if (b.sliceBounds.size() != b.slices.size())
		return;

	const __m128 bucketLo = _mm_setr_ps(b.coarseMin.x, b.coarseMin.y, b.coarseMin.z, 0.0f);
	const __m128 bucketHi = _mm_setr_ps(b.coarseMax.x, b.coarseMax.y, b.coarseMax.z, 0.0f);
	if (!AnyFrustumVisible(frustumSoAs, frustumCount, bucketLo, bucketHi))
		return;

	if (!b.clustersValid)
		MergeSlicesIntoRuns(b);

	const __m128 pad = _mm_set1_ps(b.modelRadius + 64.0f);
	for (const GrassBucket::SliceRun& run : b.sliceRuns) {
		const __m128 lo = _mm_sub_ps(_mm_load_ps(run.bounds.lo), pad);
		const __m128 hi = _mm_add_ps(_mm_load_ps(run.bounds.hi), pad);

		const __m128 beyond = _mm_max_ps(_mm_max_ps(_mm_sub_ps(lo, camPosV), _mm_sub_ps(camPosV, hi)), _mm_setzero_ps());
		auto distanceSq = _mm_cvtss_f32(_mm_dp_ps(beyond, beyond, 0x71));

		const bool withinRenderDistance = distanceSq <= maxDistSq;
		if (!withinRenderDistance || !AnyFrustumVisible(frustumSoAs, frustumCount, lo, hi))
			continue;

		sliceTableCPU.emplace_back(run.firstSliceOffset, b.visibleInstances);
		++b.sliceTableCount;
		b.visibleInstances += run.instanceCount;
	}

	b.cullVisible = b.sliceTableCount != 0;
	if (!b.cullVisible)
		sliceTableCPU.resize(b.sliceTableOffset);
}

void GrassOptimizations::UploadCullState(ID3D11Device* device, ID3D11DeviceContext* ctx, uint32_t visibleBuckets)
{
	// One map fills every visible bucket's slot — replaces a Map/Unmap per bucket.
	bool cullStateUploaded = false;
	if (visibleBuckets && EnsureCullBucketCapacity(visibleBuckets, device)) {
		D3D11_MAPPED_SUBRESOURCE m{};
		if (SUCCEEDED(ctx->Map(cullBucketCB->CB(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
			auto* bytes = static_cast<uint8_t*>(m.pData);
			uint32_t slot = 0;
			for (auto& [key, b] : bucketStore.buckets) {
				if (!b.cullVisible)
					continue;
				b.cullSlot = slot;
				auto* cb = reinterpret_cast<CullBucketCB*>(bytes + (size_t)slot * kSlotBytes);
				cb->instanceCount = b.visibleInstances;
				cb->sliceTableOffset = b.sliceTableOffset;
				cb->sliceCount = b.sliceTableCount;
				cb->wavePeriod = b.wavePeriod;
				cb->timeBase = timeBase;
				cb->prevTimeBase = prevTimeBase;
				cb->boundCenter[0] = b.boundCenter.x;
				cb->boundCenter[1] = b.boundCenter.y;
				cb->boundCenter[2] = b.boundCenter.z;
				cb->modelRadius = b.modelRadius;
				cb->distScale = b.distScale;
				cb->minPixelScale = b.minPixelScale;
				cb->isComplex = b.isComplex ? 1.0f : 0.0f;
				cb->midLODEnabled = b.lodBins[(size_t)GrassMeshLibrary::LODTier::kMiddle].active ? 1.0f : 0.0f;
				cb->farLODEnabled = b.lodBins[(size_t)GrassMeshLibrary::LODTier::kFar].active ? 1.0f : 0.0f;
				cb->outputCapacityPerEye = b.capacityInstances;
				++slot;
			}
			ctx->Unmap(cullBucketCB->CB(), 0);
			cullStateUploaded = true;
		}
	}

	// If the cull state failed to upload, skip all buckets to prevent the CS from running using garbage or out-of-date data.
	if (visibleBuckets && !cullStateUploaded) {
		for (auto& [key, b] : bucketStore.buckets)
			b.cullVisible = false;
	}

	// These fields must be updated before culling, regardless of SetupGeometry hook order.
	if (globals::features::grassCollision.loaded) {
		globals::features::grassCollision.Update();
		globals::features::grassCollision.BindDeformationResources(true);
	}
	if (globals::features::wind.loaded)
		globals::features::wind.UpdateGrassWindSpring(true);
	globals::state->UpdatePermutationBuffer();
	ID3D11Buffer* deformationBuffers[] = {
		globals::state->permutationCB->CB(), globals::state->sharedDataCB->CB(), globals::state->featureDataCB->CB()
	};
	ctx->CSSetConstantBuffers(4, ARRAYSIZE(deformationBuffers), deformationBuffers);

	ID3D11Buffer* paramsCB = cullParamsCB->CB();
	ctx->CSSetConstantBuffers(0, 1, &paramsCB);
	ID3D11Buffer* frameBuffers[1]{ *globals::game::perFrame.get() };
	ctx->CSSetConstantBuffers(12, 1, frameBuffers);

	bool sliceTableUploaded = sliceTableCPU.empty();
	if (!sliceTableCPU.empty()) {
		if (sliceTableCPU.size() > sliceTableCapacity) {
			sliceTable.reset();
			sliceTableCapacity = 0;

			uint32_t cap = 256;
			while (cap < sliceTableCPU.size())
				cap *= 2;

			D3D11_BUFFER_DESC bd{};
			bd.ByteWidth = cap * 2 * sizeof(uint32_t);
			bd.Usage = D3D11_USAGE_DYNAMIC;
			bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
			bd.StructureByteStride = 2 * sizeof(uint32_t);
			try {
				sliceTable = std::make_unique<Buffer>(bd, nullptr, "GrassOptimizations::SliceTable");
				D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
				sv.Format = DXGI_FORMAT_UNKNOWN;
				sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
				sv.Buffer.NumElements = cap;
				sliceTable->CreateSRV(sv);
				sliceTableCapacity = cap;
			} catch (...) {
				logger::error("[GRASS OPTIMIZATIONS] slice table create failed elements={}", cap);
				sliceTable.reset();
			}
		}

		if (sliceTable && sliceTable->srv) {
			D3D11_MAPPED_SUBRESOURCE m{};
			if (SUCCEEDED(ctx->Map(sliceTable->resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
				std::memcpy(m.pData, sliceTableCPU.data(), sliceTableCPU.size() * 2 * sizeof(uint32_t));
				ctx->Unmap(sliceTable->resource.get(), 0);
				sliceTableUploaded = true;
			}
		}
	}

	if (!sliceTableUploaded) {
		for (auto& [key, b] : bucketStore.buckets)
			b.cullVisible = false;
	}

	ctx->CSSetShader(cullCS.get(), nullptr, 0);

	for (auto& [key, b] : bucketStore.buckets)
		if (b.cullVisible)
			CullBucket(b, ctx);

	ID3D11UnorderedAccessView* nullUAVs[4 + 2 * (size_t)GrassMeshLibrary::LODTier::kCount] = {};
	ctx->CSSetUnorderedAccessViews(0, (UINT)std::size(nullUAVs), nullUAVs, nullptr);
	ID3D11ShaderResourceView* nullSRVs[4] = {};
	ctx->CSSetShaderResources(0, 4, nullSRVs);
	ID3D11ShaderResourceView* nullDeformationSRVs[11] = {};
	ctx->CSSetShaderResources(100, ARRAYSIZE(nullDeformationSRVs), nullDeformationSRVs);
	// Shared b5/b6 must stay bound for subsequent compute passes.
	ID3D11Buffer* nullDeformationBuffers[2] = {};
	ctx->CSSetConstantBuffers(3, ARRAYSIZE(nullDeformationBuffers), nullDeformationBuffers);
	ID3D11SamplerState* nullDeformationSamplers[2] = {};
	ctx->CSSetSamplers(14, ARRAYSIZE(nullDeformationSamplers), nullDeformationSamplers);
	ctx->CSSetShader(nullptr, nullptr, 0);
}

void GrassOptimizations::BuildFrustumSoA(FrustumSoA& out, const RE::NiFrustumPlanes& f)
{
	static constexpr RE::NiFrustumPlanes::ActivePlane kBits[RE::NiFrustumPlanes::Planes::kTotal] = {
		RE::NiFrustumPlanes::ActivePlane::kNear, RE::NiFrustumPlanes::ActivePlane::kFar,
		RE::NiFrustumPlanes::ActivePlane::kLeft, RE::NiFrustumPlanes::ActivePlane::kRight,
		RE::NiFrustumPlanes::ActivePlane::kTop, RE::NiFrustumPlanes::ActivePlane::kBottom
	};

	// Pad unused and inactive slots with an always-pass plane (zero normal, constant -1), keeping the per-slice test branch-free.
	alignas(16) float nx[8], ny[8], nz[8], d[8];
	for (uint32_t i = 0; i < 8; ++i) {
		nx[i] = ny[i] = nz[i] = 0.0f;
		d[i] = -1.0f;
	}

	for (uint32_t i = 0; i < 6; ++i) {
		if (!f.activePlanes.any(kBits[i]))
			continue;
		const auto& pl = f.cullingPlanes[i];
		nx[i] = pl.normal.x;
		ny[i] = pl.normal.y;
		nz[i] = pl.normal.z;
		d[i] = pl.constant;
	}

	for (uint32_t g = 0; g < 2; ++g) {
		out.nx[g] = _mm_load_ps(nx + g * 4);
		out.ny[g] = _mm_load_ps(ny + g * 4);
		out.nz[g] = _mm_load_ps(nz + g * 4);
		out.d[g] = _mm_load_ps(d + g * 4);
	}
}

bool GrassOptimizations::AabbVisible(const FrustumSoA& f, __m128 lo, __m128 hi)
{
	const __m128 lx = _mm_shuffle_ps(lo, lo, _MM_SHUFFLE(0, 0, 0, 0));
	const __m128 ly = _mm_shuffle_ps(lo, lo, _MM_SHUFFLE(1, 1, 1, 1));
	const __m128 lz = _mm_shuffle_ps(lo, lo, _MM_SHUFFLE(2, 2, 2, 2));
	const __m128 hx = _mm_shuffle_ps(hi, hi, _MM_SHUFFLE(0, 0, 0, 0));
	const __m128 hy = _mm_shuffle_ps(hi, hi, _MM_SHUFFLE(1, 1, 1, 1));
	const __m128 hz = _mm_shuffle_ps(hi, hi, _MM_SHUFFLE(2, 2, 2, 2));

	for (uint32_t g = 0; g < 2; ++g) {
		// Positive vertex: the box corner furthest along each plane normal.
		// blendv keys off the normal's sign bit.
		const __m128 px = _mm_blendv_ps(hx, lx, f.nx[g]);
		const __m128 py = _mm_blendv_ps(hy, ly, f.ny[g]);
		const __m128 pz = _mm_blendv_ps(hz, lz, f.nz[g]);

		const __m128 dot = _mm_add_ps(
			_mm_add_ps(_mm_mul_ps(f.nx[g], px), _mm_mul_ps(f.ny[g], py)),
			_mm_mul_ps(f.nz[g], pz));

		// dot(n, p) - constant < 0 → outside
		if (_mm_movemask_ps(_mm_cmplt_ps(_mm_sub_ps(dot, f.d[g]), _mm_setzero_ps())))
			return false;
	}
	return true;
}

void GrassOptimizations::SetupResources()
{
	cullParamsCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<CullParamsCB>(), "GrassOptimizations::CullParamsCB");
	// VR maps both eyes' slots in one map so its per-eye draws rebind by offset; see kEyeSlotBytes.
	eyeIndexCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc((globals::game::isVR ? 2u : 1u) * kEyeSlotBytes), "GrassOptimizations::EyeIndexCB");
	hiZ.SetupResources();
	bucketStore.SetupResources();

	if (FAILED(globals::d3d::context->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&ctx1))) || !ctx1) {
		logger::error("[GRASS OPTIMIZATIONS] ID3D11DeviceContext1 unavailable — feature disabled");
		ctx1 = nullptr;
	}
}

GrassOptimizations::Hooks::CodePatch GrassOptimizations::Hooks::drawLoopPatch;
GrassOptimizations::Hooks::CodePatch GrassOptimizations::Hooks::fadeBufferPatch;

void GrassOptimizations::ApplyActive(bool a_active)
{
	if (a_active == active)
		return;
	if (!Hooks::SetEnginePatches(a_active)) {
		// Left mismatched, QueueEnabledSync would retry and log every frame.
		settings.Enabled = active;
		return;
	}
	active = a_active;
	globals::shaderCache->Clear(RE::BSShader::Type::Grass);
}

void GrassOptimizations::QueueEnabledSync()
{
	if (!loaded || settings.Enabled == active || transitionQueued)
		return;
	if (auto* task = SKSE::GetTaskInterface()) {
		transitionQueued = true;
		task->AddTask([this]() {
			transitionQueued = false;
			if (loaded)
				ApplyActive(settings.Enabled);
		});
	}
}

void GrassOptimizations::OnRuntimeDisabled()
{
	ApplyActive(false);
}

void GrassOptimizations::ClearShaderCache()
{
	cullCS.Reset();
	layoutSignature.Reset();
	inputLayouts.clear();
	hiZ.ClearShaderCache();
	bucketStore.ClearShaderCache();
}

// VR needs one input layout per vertex descriptor: SV_InstanceID excludes StartInstanceLocation, so
// eye 1's draw needs the compacted stream's TEXCOORD4-7 instance attrs at the right per-vertex offsets
// for that descriptor (a descriptor-agnostic layout silently drops them, leaving VR grass invisible).
ID3D11InputLayout* GrassOptimizations::GetOptimizedInputLayout(uint64_t a_descVal)
{
	if (auto it = inputLayouts.find(a_descVal); it != inputLayouts.end())
		return it->second.get();

	ID3DBlob* signature = layoutSignature.Get(L"Data\\Shaders\\GrassOptimizations\\GrassInstanceSignatureVS.hlsl", {}, "vs_5_0");
	if (!signature)
		return nullptr;

	uint64_t storage = a_descVal;
	auto& vdesc = *reinterpret_cast<RE::BSGraphics::VertexDesc*>(&storage);
	std::array<D3D11_INPUT_ELEMENT_DESC, 8> elements{};
	uint32_t count = 0;
	elements[count++] = { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 };
	if (vdesc.HasFlag(RE::BSGraphics::Vertex::VF_UV))
		elements[count++] = { "TEXCOORD", 0, DXGI_FORMAT_R16G16_FLOAT, 0, vdesc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_TEXCOORD0), D3D11_INPUT_PER_VERTEX_DATA, 0 };
	if (vdesc.HasFlag(RE::BSGraphics::Vertex::VF_NORMAL))
		elements[count++] = { "NORMAL", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, vdesc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_NORMAL), D3D11_INPUT_PER_VERTEX_DATA, 0 };
	if (vdesc.HasFlag(RE::BSGraphics::Vertex::VF_COLORS))
		elements[count++] = { "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, vdesc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_COLOR), D3D11_INPUT_PER_VERTEX_DATA, 0 };
	for (uint32_t i = 0; i < 4; ++i)
		elements[count++] = { "TEXCOORD", 4 + i, DXGI_FORMAT_R16G16B16A16_FLOAT, 1, i * 8, D3D11_INPUT_PER_INSTANCE_DATA, 1 };

	winrt::com_ptr<ID3D11InputLayout> layout;
	const HRESULT hr = globals::d3d::device->CreateInputLayout(elements.data(), count,
		signature->GetBufferPointer(), signature->GetBufferSize(), layout.put());
	if (FAILED(hr) || !layout) {
		logger::error("[GRASS OPTIMIZATIONS] input layout creation failed for desc {:X} hr={:08X}", a_descVal, (uint32_t)hr);
		return nullptr;
	}
	Util::SetResourceName(layout.get(), "GrassOptimizations::InputLayout");
	auto* raw = layout.get();
	inputLayouts.emplace(a_descVal, std::move(layout));
	return raw;
}

ID3D11ComputeShader* GrassOptimizations::GetCullCS()
{
	std::vector<std::pair<const char*, const char*>> defines;
	if (globals::features::grassCollision.loaded)
		defines.emplace_back("GRASS_COLLISION", "1");
	return cullCS.Get(L"Data\\Shaders\\GrassOptimizations\\GrassCullingCS.hlsl", defines,
		"cs_5_0", "main", "GrassOptimizations::CullCS");
}

static void WriteArgsUint32(ID3D11DeviceContext* ctx, ID3D11Buffer* buf, uint32_t byteOffset, uint32_t value)
{
	const D3D11_BOX box{ byteOffset, 0, 0, byteOffset + static_cast<uint32_t>(sizeof(uint32_t)), 1, 1 };
	ctx->UpdateSubresource(buf, 0, &box, &value, 0, 0);
}

static void ResetInstanceCount(ID3D11DeviceContext* ctx, ID3D11Buffer* buf)
{
	WriteArgsUint32(ctx, buf, InstanceCountOffsetForEye(0), 0);
	if (globals::game::isVR)
		WriteArgsUint32(ctx, buf, InstanceCountOffsetForEye(1), 0);
}

// One 4-byte slot per (eye, tier) in the shared LOD counter buffer; matches GrassCullingCS.hlsl's
// kLODCounterEyeStride/MiddleLODCountOffset/FarLODCountOffset.
static constexpr uint32_t kLODCounterEyeStride = 8;
static uint32_t LODCounterOffsetForEye(uint32_t eye, size_t tier)
{
	return eye * kLODCounterEyeStride + (uint32_t)(tier * sizeof(uint32_t));
}

void GrassOptimizations::CullBucket(GrassBucket& b, ID3D11DeviceContext* ctx)
{
	CS_GPU_PASS("GrassOptimizations::CullBucket");
	static_assert(
		(size_t)GrassMeshLibrary::LODTier::kMiddle == 0 &&
			(size_t)GrassMeshLibrary::LODTier::kFar == 1 &&
			(size_t)GrassMeshLibrary::LODTier::kCount == 2,
		"GrassCullingCS LOD counter offsets must match LODTier");

	if (b.cullSlot == UINT32_MAX)
		return;

	// Reset only the instance-count dword(s): the args UAV spans CPU-set fields (StartInstanceLocation
	// on VR) that must survive every frame, so a wholesale ClearUnorderedAccessView would corrupt them.
	ResetInstanceCount(ctx, b.argsBuf);
	const UINT zeros[4] = { 0, 0, 0, 0 };
	ctx->ClearUnorderedAccessViewUint(b.lodCounterUAV, zeros);

	// Main outputs, two outputs per LOD tier, then the shared LOD counter, matching u0-u7 in GrassCullingCS.
	ID3D11UnorderedAccessView* uavs[4 + 2 * (size_t)GrassMeshLibrary::LODTier::kCount] = { b.compactedUAV, b.extrasUAV, b.argsUAV };
	for (size_t tier = 0; tier < (size_t)GrassMeshLibrary::LODTier::kCount; ++tier) {
		const GrassBucket::LODBin& bin = b.lodBins[tier];
		uavs[3 + tier * 2 + 0] = bin.active ? bin.compactedUAV : nullptr;
		uavs[3 + tier * 2 + 1] = bin.active ? bin.extrasUAV : nullptr;
	}
	uavs[std::size(uavs) - 1] = b.lodCounterUAV;
	ctx->CSSetUnorderedAccessViews(0, (UINT)std::size(uavs), uavs, nullptr);

	ID3D11ShaderResourceView* sliceTableSRV = sliceTable ? sliceTable->srv.get() : nullptr;
	ID3D11ShaderResourceView* srvs[4] = { b.instanceSRV, b.originSRV,
		hiZ.GetSRV(), sliceTableSRV };
	ctx->CSSetShaderResources(0, 4, srvs);

||||||| 12b261724
	b.sliceTableOffset = (uint32_t)sliceTableCPU.size();
	b.sliceTableCount = 0;
	b.visibleInstances = 0;
	b.cullVisible = false;
	for (GrassBucket::LODBin& bin : b.lodBins)
		bin.active = false;

	if (b.sliceBounds.size() != b.slices.size())
		return;

	const __m128 bucketLo = _mm_setr_ps(b.coarseMin.x, b.coarseMin.y, b.coarseMin.z, 0.0f);
	const __m128 bucketHi = _mm_setr_ps(b.coarseMax.x, b.coarseMax.y, b.coarseMax.z, 0.0f);
	if (!AabbVisible(frustumSoA, bucketLo, bucketHi))
		return;

	if (!b.clustersValid)
		MergeSlicesIntoRuns(b);

	const __m128 pad = _mm_set1_ps(b.modelRadius + 64.0f);
	for (const GrassBucket::SliceRun& run : b.sliceRuns) {
		const __m128 lo = _mm_sub_ps(_mm_load_ps(run.bounds.lo), pad);
		const __m128 hi = _mm_add_ps(_mm_load_ps(run.bounds.hi), pad);

		const __m128 beyond = _mm_max_ps(_mm_max_ps(_mm_sub_ps(lo, camPosV), _mm_sub_ps(camPosV, hi)), _mm_setzero_ps());
		auto distanceSq = _mm_cvtss_f32(_mm_dp_ps(beyond, beyond, 0x71));

		const bool withinRenderDistance = distanceSq <= maxDistSq;
		if (!withinRenderDistance || !AabbVisible(frustumSoA, lo, hi))
			continue;

		sliceTableCPU.emplace_back(run.firstSliceOffset, b.visibleInstances);
		++b.sliceTableCount;
		b.visibleInstances += run.instanceCount;
	}

	b.cullVisible = b.sliceTableCount != 0;
	if (!b.cullVisible)
		sliceTableCPU.resize(b.sliceTableOffset);
}

void GrassOptimizations::UploadCullState(ID3D11Device* device, ID3D11DeviceContext* ctx, uint32_t visibleBuckets)
{
	// One map fills every visible bucket's slot — replaces a Map/Unmap per bucket.
	bool cullStateUploaded = false;
	if (visibleBuckets && EnsureCullBucketCapacity(visibleBuckets, device)) {
		D3D11_MAPPED_SUBRESOURCE m{};
		if (SUCCEEDED(ctx->Map(cullBucketCB->CB(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
			auto* bytes = static_cast<uint8_t*>(m.pData);
			uint32_t slot = 0;
			for (auto& [key, b] : bucketStore.buckets) {
				if (!b.cullVisible)
					continue;
				b.cullSlot = slot;
				auto* cb = reinterpret_cast<CullBucketCB*>(bytes + (size_t)slot * kSlotBytes);
				cb->instanceCount = b.visibleInstances;
				cb->sliceTableOffset = b.sliceTableOffset;
				cb->sliceCount = b.sliceTableCount;
				cb->wavePeriod = b.wavePeriod;
				cb->timeBase = timeBase;
				cb->prevTimeBase = prevTimeBase;
				cb->boundCenter[0] = b.boundCenter.x;
				cb->boundCenter[1] = b.boundCenter.y;
				cb->boundCenter[2] = b.boundCenter.z;
				cb->modelRadius = b.modelRadius;
				cb->distScale = b.distScale;
				cb->minPixelScale = b.minPixelScale;
				cb->isComplex = b.isComplex ? 1.0f : 0.0f;
				cb->midLODEnabled = b.lodBins[(size_t)GrassMeshLibrary::LODTier::kMiddle].active ? 1.0f : 0.0f;
				cb->farLODEnabled = b.lodBins[(size_t)GrassMeshLibrary::LODTier::kFar].active ? 1.0f : 0.0f;
				++slot;
			}
			ctx->Unmap(cullBucketCB->CB(), 0);
			cullStateUploaded = true;
		}
	}

	// If the cull state failed to upload, skip all buckets to prevent the CS from running using garbage or out-of-date data.
	if (visibleBuckets && !cullStateUploaded) {
		for (auto& [key, b] : bucketStore.buckets)
			b.cullVisible = false;
	}

	ID3D11Buffer* paramsCB = cullParamsCB->CB();
	ctx->CSSetConstantBuffers(0, 1, &paramsCB);
	ID3D11Buffer* frameBuffers[1]{ *globals::game::perFrame.get() };
	ctx->CSSetConstantBuffers(12, 1, frameBuffers);

	bool sliceTableUploaded = sliceTableCPU.empty();
	if (!sliceTableCPU.empty()) {
		if (sliceTableCPU.size() > sliceTableCapacity) {
			sliceTable.reset();
			sliceTableCapacity = 0;

			uint32_t cap = 256;
			while (cap < sliceTableCPU.size())
				cap *= 2;

			D3D11_BUFFER_DESC bd{};
			bd.ByteWidth = cap * 2 * sizeof(uint32_t);
			bd.Usage = D3D11_USAGE_DYNAMIC;
			bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
			bd.StructureByteStride = 2 * sizeof(uint32_t);
			try {
				sliceTable = std::make_unique<Buffer>(bd, nullptr, "GrassOptimizations::SliceTable");
				D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
				sv.Format = DXGI_FORMAT_UNKNOWN;
				sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
				sv.Buffer.NumElements = cap;
				sliceTable->CreateSRV(sv);
				sliceTableCapacity = cap;
			} catch (...) {
				logger::error("[GRASS OPTIMIZATIONS] slice table create failed elements={}", cap);
				sliceTable.reset();
			}
		}

		if (sliceTable && sliceTable->srv) {
			D3D11_MAPPED_SUBRESOURCE m{};
			if (SUCCEEDED(ctx->Map(sliceTable->resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
				std::memcpy(m.pData, sliceTableCPU.data(), sliceTableCPU.size() * 2 * sizeof(uint32_t));
				ctx->Unmap(sliceTable->resource.get(), 0);
				sliceTableUploaded = true;
			}
		}
	}

	if (!sliceTableUploaded) {
		for (auto& [key, b] : bucketStore.buckets)
			b.cullVisible = false;
	}

	ctx->CSSetShader(cullCS, nullptr, 0);

	for (auto& [key, b] : bucketStore.buckets)
		if (b.cullVisible)
			CullBucket(b, ctx);

	ID3D11UnorderedAccessView* nullUAVs[4 + 2 * (size_t)GrassMeshLibrary::LODTier::kCount] = {};
	ctx->CSSetUnorderedAccessViews(0, (UINT)std::size(nullUAVs), nullUAVs, nullptr);
	ID3D11ShaderResourceView* nullSRVs[4] = {};
	ctx->CSSetShaderResources(0, 4, nullSRVs);
	ctx->CSSetShader(nullptr, nullptr, 0);
}

void GrassOptimizations::BuildFrustumSoA(FrustumSoA& out, const RE::NiFrustumPlanes& f)
{
	static constexpr RE::NiFrustumPlanes::ActivePlane kBits[RE::NiFrustumPlanes::Planes::kTotal] = {
		RE::NiFrustumPlanes::ActivePlane::kNear, RE::NiFrustumPlanes::ActivePlane::kFar,
		RE::NiFrustumPlanes::ActivePlane::kLeft, RE::NiFrustumPlanes::ActivePlane::kRight,
		RE::NiFrustumPlanes::ActivePlane::kTop, RE::NiFrustumPlanes::ActivePlane::kBottom
	};

	// Pad unused and inactive slots with an always-pass plane (zero normal, constant -1), keeping the per-slice test branch-free.
	alignas(16) float nx[8], ny[8], nz[8], d[8];
	for (uint32_t i = 0; i < 8; ++i) {
		nx[i] = ny[i] = nz[i] = 0.0f;
		d[i] = -1.0f;
	}

	for (uint32_t i = 0; i < 6; ++i) {
		if (!f.activePlanes.any(kBits[i]))
			continue;
		const auto& pl = f.cullingPlanes[i];
		nx[i] = pl.normal.x;
		ny[i] = pl.normal.y;
		nz[i] = pl.normal.z;
		d[i] = pl.constant;
	}

	for (uint32_t g = 0; g < 2; ++g) {
		out.nx[g] = _mm_load_ps(nx + g * 4);
		out.ny[g] = _mm_load_ps(ny + g * 4);
		out.nz[g] = _mm_load_ps(nz + g * 4);
		out.d[g] = _mm_load_ps(d + g * 4);
	}
}

bool GrassOptimizations::AabbVisible(const FrustumSoA& f, __m128 lo, __m128 hi)
{
	const __m128 lx = _mm_shuffle_ps(lo, lo, _MM_SHUFFLE(0, 0, 0, 0));
	const __m128 ly = _mm_shuffle_ps(lo, lo, _MM_SHUFFLE(1, 1, 1, 1));
	const __m128 lz = _mm_shuffle_ps(lo, lo, _MM_SHUFFLE(2, 2, 2, 2));
	const __m128 hx = _mm_shuffle_ps(hi, hi, _MM_SHUFFLE(0, 0, 0, 0));
	const __m128 hy = _mm_shuffle_ps(hi, hi, _MM_SHUFFLE(1, 1, 1, 1));
	const __m128 hz = _mm_shuffle_ps(hi, hi, _MM_SHUFFLE(2, 2, 2, 2));

	for (uint32_t g = 0; g < 2; ++g) {
		// Positive vertex: the box corner furthest along each plane normal.
		// blendv keys off the normal's sign bit.
		const __m128 px = _mm_blendv_ps(hx, lx, f.nx[g]);
		const __m128 py = _mm_blendv_ps(hy, ly, f.ny[g]);
		const __m128 pz = _mm_blendv_ps(hz, lz, f.nz[g]);

		const __m128 dot = _mm_add_ps(
			_mm_add_ps(_mm_mul_ps(f.nx[g], px), _mm_mul_ps(f.ny[g], py)),
			_mm_mul_ps(f.nz[g], pz));

		// dot(n, p) - constant < 0 → outside
		if (_mm_movemask_ps(_mm_cmplt_ps(_mm_sub_ps(dot, f.d[g]), _mm_setzero_ps())))
			return false;
	}
	return true;
}

void GrassOptimizations::SetupResources()
{
	cullParamsCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<CullParamsCB>(), "GrassOptimizations::CullParamsCB");
	hiZ.SetupResources();
	bucketStore.SetupResources();

	if (FAILED(globals::d3d::context->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&ctx1))) || !ctx1) {
		logger::error("[GRASS OPTIMIZATIONS] ID3D11DeviceContext1 unavailable — feature disabled");
		ctx1 = nullptr;
	}
}

void GrassOptimizations::ClearShaderCache()
{
	auto release = [](ID3D11ComputeShader*& shader) {
		if (shader)
			shader->Release();
		shader = nullptr;
	};
	release(cullCS);
	cullCSFailed = false;
	hiZ.ClearShaderCache();
	bucketStore.ClearShaderCache();
}

ID3D11ComputeShader* GrassOptimizations::GetCullCS()
{
	if (!cullCS && !cullCSFailed) {
		cullCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\GrassOptimizations\\GrassCullingCS.hlsl", {}, "cs_5_0"));
		if (!cullCS) {
			cullCSFailed = true;
			logger::error("[GRASS OPTIMIZATIONS] cull CS load failed — feature disabled");
		}
	}
	return cullCS;
}

void GrassOptimizations::CullBucket(GrassBucket& b, ID3D11DeviceContext* ctx)
{
	static_assert(
		(size_t)GrassMeshLibrary::LODTier::kMiddle == 0 &&
		(size_t)GrassMeshLibrary::LODTier::kFar == 1 &&
		(size_t)GrassMeshLibrary::LODTier::kCount == 2,
		"GrassCullingCS LOD counter offsets must match LODTier");

	if (b.cullSlot == UINT32_MAX)
		return;

	// Clearing the args view allows the instance count to be directly reset to zero for the draw.
	const UINT zeros[4] = { 0, 0, 0, 0 };
	ctx->ClearUnorderedAccessViewUint(b.argsUAV, zeros);
	ctx->ClearUnorderedAccessViewUint(b.lodCounterUAV, zeros);

	// Main outputs, two outputs per LOD tier, then the shared LOD counter, matching u0-u7 in GrassCullingCS.
	ID3D11UnorderedAccessView* uavs[4 + 2 * (size_t)GrassMeshLibrary::LODTier::kCount] = { b.compactedUAV, b.extrasUAV, b.argsUAV };
	for (size_t tier = 0; tier < (size_t)GrassMeshLibrary::LODTier::kCount; ++tier) {
		const GrassBucket::LODBin& bin = b.lodBins[tier];
		uavs[3 + tier * 2 + 0] = bin.active ? bin.compactedUAV : nullptr;
		uavs[3 + tier * 2 + 1] = bin.active ? bin.extrasUAV : nullptr;
	}
	uavs[std::size(uavs) - 1] = b.lodCounterUAV;
	ctx->CSSetUnorderedAccessViews(0, (UINT)std::size(uavs), uavs, nullptr);

	ID3D11ShaderResourceView* sliceTableSRV = sliceTable ? sliceTable->srv.get() : nullptr;
	ID3D11ShaderResourceView* srvs[4] = { b.instanceSRV, b.originSRV,
		hiZ.GetSRV(), sliceTableSRV };
	ctx->CSSetShaderResources(0, 4, srvs);

=======
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
	ID3D11Buffer* bucketCB = cullBucketCB->CB();
	UINT windowFirst = (b.cullSlot * kSlotBytes + kSlotMaterialOffset) / 16;
	UINT geometryNum = 32;

<<<<<<< HEAD
	// Skipping the dispatch keeps the instance count at zero for the draw. The VR shader evaluates
	// both eyes per thread while the flat shader receives the same single Z slice as before.
	if (b.visibleInstances && b.sliceTableCount && sliceTableSRV)
		ctx->Dispatch((b.visibleInstances + 63) / 64, 1, 1);

	// The LOD counter UAV must be unbound before its values can be copied into the draw arguments.
	ID3D11UnorderedAccessView* nullUAVs[std::size(uavs)] = {};
	ctx->CSSetUnorderedAccessViews(0, (UINT)std::size(nullUAVs), nullUAVs, nullptr);
	const uint32_t eyeCount = globals::game::isVR ? 2u : 1u;
	for (size_t tier = 0; tier < (size_t)GrassMeshLibrary::LODTier::kCount; ++tier) {
		const GrassBucket::LODBin& bin = b.lodBins[tier];
		if (!bin.active || !bin.argsBuf)
			continue;
		for (uint32_t eye = 0; eye < eyeCount; ++eye) {
			const UINT countOffset = LODCounterOffsetForEye(eye, tier);
			const D3D11_BOX countBox{ countOffset, 0, 0, static_cast<UINT>(countOffset + sizeof(uint32_t)), 1, 1 };
			ctx->CopySubresourceRegion(bin.argsBuf, 0, InstanceCountOffsetForEye(eye), 0, 0, b.lodCounterBuf, 0, &countBox);
		}
||||||| 12b261724
	// Skipping the dispatch keeps the instance count at zero for the draw.
	if (b.visibleInstances && b.sliceTableCount && sliceTableSRV)
		ctx->Dispatch((b.visibleInstances + 63) / 64, 1, 1);

	// The LOD counter UAV must be unbound before its values can be copied into the draw arguments.
	ID3D11UnorderedAccessView* nullUAVs[std::size(uavs)] = {};
	ctx->CSSetUnorderedAccessViews(0, (UINT)std::size(nullUAVs), nullUAVs, nullptr);
	for (size_t tier = 0; tier < (size_t)GrassMeshLibrary::LODTier::kCount; ++tier) {
		const GrassBucket::LODBin& bin = b.lodBins[tier];
		if (!bin.active || !bin.argsBuf)
			continue;
		const UINT countOffset = (UINT)(tier * sizeof(uint32_t));
		const D3D11_BOX countBox{ countOffset, 0, 0, static_cast<UINT>(countOffset + sizeof(uint32_t)), 1, 1 };
		ctx->CopySubresourceRegion(bin.argsBuf, 0, instanceCountOffset, 0, 0, b.lodCounterBuf, 0, &countBox);
=======
	ctx1->VSSetConstantBuffers1(2, 1, &bucketCB, &windowFirst, &geometryNum);
	ctx1->PSSetConstantBuffers1(2, 1, &bucketCB, &windowFirst, &geometryNum);
	// BSGrassShader_SetupGeometry sets this texture's address and filter modes once the hook chain returns.
	globals::game::shadowState->SetPSTexture(0, b.asset.diffuseTexture->rendererTexture);
	// Canopy heights come from the bucket constants, so only the lighting mode is shared with native grass.
	globals::state->SetGrassFlags(b.asset.key.draw.sphereNormal, b.asset.key.draw.pbrMaterial);
	if (b.asset.key.draw.pbrMaterial) {
		TruePBR::SetGrassPBRTextures(static_cast<const BSLightingShaderMaterialPBR*>(b.asset.material.get()));
		UINT materialNum = 16;
		ctx1->PSSetConstantBuffers1(1, 1, &bucketCB, &windowFirst, &materialNum);
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
	}
}

void GrassOptimizations::DrawBucket(GrassBucket& b)
{
	auto* ctx = globals::d3d::context;
	const uint64_t descVal = b.asset.key.descVal;
	auto* meshVB = b.asset.vertexBuffer.get();
	auto* indexB = b.asset.indexBuffer.get();
	const UINT meshStride = VertexStrideFromDesc(descVal);
	if (!meshVB || !indexB || !meshStride)
		return;

	auto& shadowState = globals::game::shadowState->GetRuntimeData();
	static REL::Relocation<void (*)(uint32_t)> SetDirtyStates{ REL::RelocationID(75580, 77386) };
	if (shadowState.topology != D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST) {
		shadowState.topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		shadowState.stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_PRIMITIVE_TOPO);
	}

	if (shadowState.vertexDesc != descVal) {
		shadowState.vertexDesc = descVal;
		shadowState.stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_VERTEX_DESC);
	}

	// Batch material texture bindings instead of letting SetDirtyStates bind each slot separately.
	constexpr uint32_t kMaterialTextureBits = 0x7F;
	const uint32_t materialTextures = shadowState.PSResourceModifiedBits & kMaterialTextureBits;
	shadowState.PSResourceModifiedBits &= ~kMaterialTextureBits;
	SetDirtyStates(0);
	if (materialTextures) {
		const uint32_t firstTexture = std::countr_zero(materialTextures);
		const uint32_t textureCount = std::bit_width(materialTextures) - firstTexture;
		ctx->PSSetShaderResources(firstTexture, textureCount, reinterpret_cast<ID3D11ShaderResourceView* const*>(&shadowState.PSTexture[firstTexture]));
	}

	// Bind complexity and canopy heights for all tiers; the vertex shader selects each record's tier.
	ID3D11Buffer* bucketCB = cullBucketCB->CB();
	UINT bucketFirst = b.cullSlot * (kSlotBytes / 16);
	UINT bucketNum = 16;
	ctx1->VSSetConstantBuffers1(13, 1, &bucketCB, &bucketFirst, &bucketNum);

	for (uint32_t tier = 0; tier < kTierCount; ++tier) {
		ID3D11Buffer* vertexBuffer = meshVB;
		ID3D11Buffer* indexBuffer = indexB;
		UINT stride = meshStride;

		if (tier) {
			const auto* lod = b.lodMeshes[tier - 1];
			if (!lod)
				continue;

			vertexBuffer = lod->vertexBuffer;
			indexBuffer = lod->indexBuffer;
			stride = lod->meshStride;

			if (shadowState.vertexDesc != lod->descVal) {
				shadowState.vertexDesc = lod->descVal;
				shadowState.stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_VERTEX_DESC);
				SetDirtyStates(0);
			}
		}

		// A zero-stride instance stream satisfies the input layout while the shader reads cull records.
		ID3D11Buffer* vbs[2] = { vertexBuffer, vertexBuffer };
		const UINT strides[2] = { stride, 0 };
		const UINT offsets[2] = { 0, 0 };
		ID3D11ShaderResourceView* instanceViews[4] = { b.output->compactedSRVs[tier].get(), b.source.instances->srv.get(), b.groups->srv.get(), b.complexSRV };

		ctx->IASetIndexBuffer(indexBuffer, DXGI_FORMAT_R16_UINT, 0);
		ctx->IASetVertexBuffers(0, 2, vbs, strides, offsets);
		ctx->VSSetShaderResources(2, 4, instanceViews);

		ctx->DrawIndexedInstancedIndirect(bucketStore.arguments->resource.get(), (b.cullSlot * kTierCount + tier) * kArgsStride);
	}
}

void GrassOptimizations::OnGrassMeshDestroyed(RE::BSMultiStreamInstanceTriShape* shape)
{
	bucketStore.StageRemoval(shape);
}

std::uint32_t GrassOptimizations::Hooks::AddGroup::thunk(RE::BSMultiStreamInstanceTriShape* shape, std::uint32_t count, std::uint16_t* instanceData, std::uint32_t instanceShorts, float boundPadding)
{
<<<<<<< HEAD
	auto prop = This->GetGeometryRuntimeData().shaderProperty;
	if (globals::features::grassOptimizations.active && prop && prop->GetRTTI() == globals::rtti::BSGrassShaderPropertyRTTI.get()) {
		auto& self = globals::features::grassOptimizations;

		// Only queue one representative shape per frame for each bucket to skip redundant setup.
		if (!self.bucketStore.ClaimQueueSlot(This, globals::game::graphicsState->GetFrameCount()))
			return;

		// Skips redundant and costly frustum checks since they are now handled by the coarse slice cull and CS.
		auto& shape = *This;
		process->AppendVirtual(shape, alphaGroupIndex);
		return;
	}

	func(This, process, alphaGroupIndex);
||||||| 12b261724
	auto prop = This->GetGeometryRuntimeData().shaderProperty;
	if (prop && prop->GetRTTI() == globals::rtti::BSGrassShaderPropertyRTTI.get()) {
		auto& self = globals::features::grassOptimizations;

		// Only queue one representative shape per frame for each bucket to skip redundant setup.
		if (!self.bucketStore.ClaimQueueSlot(This, globals::game::graphicsState->frameCount))
			return;

		// Skips redundant and costly frustum checks since they are now handled by the coarse slice cull and CS.
		auto& shape = *This;
		process->AppendVirtual(shape, alphaGroupIndex);
		return;
	}

	func(This, process, alphaGroupIndex);
=======
	const auto index = func(shape, count, instanceData, instanceShorts, boundPadding);
	globals::features::grassOptimizations.bucketStore.CaptureEngineGroup(shape, index);
	return index;
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
}

void GrassOptimizations::Hooks::DoneAddingInstances::thunk(RE::BSMultiStreamInstanceTriShape* shape, RE::BSTArray<std::uint32_t>& groupIndices)
{
	AttachQueuedGroups::pendingCapture.store(true, std::memory_order_release);
	func(shape, groupIndices);
	AttachQueuedGroups::pendingCapture.store(true, std::memory_order_release);
}

void GrassOptimizations::Hooks::RemoveGroup::thunk(RE::BSMultiStreamInstanceTriShape* shape, std::uint32_t index)
{
	globals::features::grassOptimizations.bucketStore.RemoveEngineGroup(shape, index, func.get());
}

std::uint32_t GrassOptimizations::Hooks::AddGroupGIDBuffer::thunk(RE::BSMultiStreamInstanceTriShape* a1, RE::BSMultiStreamInstanceTriShape::GroupHeader* a2, std::uint16_t* a3)
{
	const auto index = func(a1, a2, a3);
	globals::features::grassOptimizations.bucketStore.CaptureEngineGroup(a1, index);
	return index;
}

void GrassOptimizations::Hooks::AddQueuedGroupGIDBuffer::thunk(RE::BSMultiStreamInstanceTriShape* shape, RE::BSMultiStreamInstanceTriShape::GroupHeader* header, std::uint16_t* instanceData, RE::BSTArray<std::uint32_t>& groupIndices)
{
	AttachQueuedGroups::pendingCapture.store(true, std::memory_order_release);
	func(shape, header, instanceData, groupIndices);
	AttachQueuedGroups::pendingCapture.store(true, std::memory_order_release);
}

void GrassOptimizations::Hooks::AddQueuedGroupGIDFile::thunk(RE::BSMultiStreamInstanceTriShape* shape, RE::BSStream* stream, RE::BSTArray<std::uint32_t>& groupIndices)
{
	AttachQueuedGroups::pendingCapture.store(true, std::memory_order_release);
	func(shape, stream, groupIndices);
	AttachQueuedGroups::pendingCapture.store(true, std::memory_order_release);
}

void GrassOptimizations::Hooks::AttachQueuedGroups::thunk()
{
	const bool pending = pendingCapture.exchange(false, std::memory_order_acq_rel);
	func();

	// Keep work queued during attachment pending for the next batch as well.
	if (pending || pendingCapture.load(std::memory_order_acquire))
		globals::features::grassOptimizations.bucketStore.CaptureQueuedGroups();
}

void GrassOptimizations::Hooks::AddGroupGIDFile::thunk(RE::BSMultiStreamInstanceTriShape* a1, RE::BSStream* a2)
{
	// The file loader does not return a group index, so identify the first changed slot.
	static thread_local std::vector<void*> groupsBefore;
	const auto& groups = a1->GetMultiStreamTrishapeRuntimeData().instanceGroups;
	groupsBefore.assign(groups.begin(), groups.end());

	func(a1, a2);

	uint32_t index = 0;
	while (index < groups.size() && (!groups[index] || (index < groupsBefore.size() && groups[index] == groupsBefore[index])))
		++index;

	if (index < groups.size())
		globals::features::grassOptimizations.bucketStore.CaptureEngineGroup(a1, index);
}

RE::BSMultiStreamInstanceTriShape* GrassOptimizations::Hooks::LoadGrassType::thunk(RE::BGSGrassManager* grassManager, RE::GrassParam* a_param, uint32_t CellXDivided, uint32_t CellYDivided, uint64_t* typeKey, RE::BSFixedString* modelPath)
{
	auto* shape = func(grassManager, a_param, CellXDivided, CellYDivided, typeKey, modelPath);

	if (shape && modelPath)
		globals::features::grassOptimizations.bucketStore.RegisterModel(shape, modelPath->c_str());

	return shape;
}

void GrassOptimizations::Hooks::NiNode_OnVisible::thunk(RE::NiNode* This, RE::NiCullingProcess* process, std::int32_t alphaGroupIndex)
{
	auto& self = globals::features::grassOptimizations;
	if (This != grassRoot.load(std::memory_order_relaxed) || !self.loaded)
		return func(This, process, alphaGroupIndex);

	// Match vanilla gate
	if (std::bit_cast<std::uint32_t>(This->worldBound.radius) == 0 && !This->GetFlags().any(RE::NiAVObject::Flag::kAlwaysDraw))
		return;

	// Queue optimized buckets for the main view, or for any view after two frames without a main-view visit.
	const auto frame = globals::game::graphicsState->frameCount;
	const bool mainView = process->camera && process->camera == RE::Main::WorldRootCamera();
	if (mainView)
		mainViewVisitFrame.store(frame, std::memory_order_relaxed);

	const bool queueOptimized = mainView || frame - mainViewVisitFrame.load(std::memory_order_relaxed) > 2;

	const auto* snapshot = self.bucketStore.GetCullSnapshot();
	const auto cull = [&](RE::NiAVObject* child) {
		const auto* native = snapshot ? snapshot->Find(child) : nullptr;
		if (!native || !native->direct)
			child->Cull(process, alphaGroupIndex);
		else if (queueOptimized)
			process->AppendVirtual(*static_cast<RE::BSGeometry*>(child), alphaGroupIndex);
	};

	// Visit direct shapes before shapes in child nodes, matching the engine's flattened grass order.
	auto& rootChildren = This->GetChildren();
	const std::span children(rootChildren.begin(), rootChildren.free_idx());
	for (const auto& child : children) {
		if (child && !child->AsNode())
			cull(child.get());
	}

	for (const auto& child : children) {
		if (auto* childNode = child ? child->AsNode() : nullptr) {
			auto& nodeChildren = childNode->GetChildren();
			for (const auto& shape : std::span(nodeChildren.begin(), nodeChildren.free_idx())) {
				if (shape)
					cull(shape.get());
			}
		}
	}

	if (snapshot && queueOptimized) {
		for (const auto& proxy : snapshot->proxies)
			process->AppendVirtual(*proxy, alphaGroupIndex);
	}
}

void GrassOptimizations::Hooks::BSGrassShader_SetupGeometry::thunk(RE::BSShader* This, RE::BSRenderPass* a2, std::uint32_t flags)
{
	auto& self = globals::features::grassOptimizations;

<<<<<<< HEAD
	const auto frame = globals::game::graphicsState->GetFrameCount();
||||||| 12b261724
	const auto frame = globals::game::graphicsState->frameCount;
=======
	// Update from a main-view pass, so occlusion and fades see the main camera's PerFrame data.
	const auto frame = globals::game::graphicsState->frameCount;
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
	if (self.lastFrame != frame) {
<<<<<<< HEAD
		self.QueueEnabledSync();
		if (self.active) {
			self.UpdateGrass();
		} else if (self.bucketStore.HasPending()) {
			// Captures keep staging while inactive so re-enabling needs no cell reload.
			std::scoped_lock blk(self.bucketStore.bucketMutex);
			CS_GPU_PASS("GrassOptimizations::ApplyPending");
			self.bucketStore.ApplyPending(globals::d3d::device, globals::d3d::context);
		}
		self.lastFrame = frame;
||||||| 12b261724
		self.UpdateGrass();
		self.lastFrame = frame;
=======
		const auto* cam = RE::Main::WorldRootCamera();
		if (!cam || IsMainCameraPerFrame(cam->world.translate)) {
			self.UpdateGrass(cam);
			self.lastFrame = frame;
		}
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
	}

	func(This, a2, flags);

	if (self.active) {
		auto* shadowState = globals::game::shadowState;
		shadowState->SetPSTextureAddressMode(0, RE::BSGraphics::TextureAddressMode::kClampSClampT);
		shadowState->SetPSTextureFilterMode(0, RE::BSGraphics::TextureFilterMode::kAnisotropic);
	}
}

void GrassOptimizations::Hooks::BSGrassShader_SetupMaterial::thunk(RE::BSShader* This, const RE::BSLightingShaderMaterialBase* material)
{
	if (!globals::features::grassOptimizations.loaded)
		func(This, material);
}

void GrassOptimizations::Hooks::DrawVanilla(RE::BSMultiStreamInstanceTriShape* geometry)
{
	auto& self = globals::features::grassOptimizations;
	const auto* snapshot = self.loaded ? self.bucketStore.GetCullSnapshot() : nullptr;
	const auto* native = snapshot ? snapshot->Find(geometry) : nullptr;
	const bool current = native && self.HasCurrentCull();
	if (current && self.bucketStore.IsShapeOptimized(*native, globals::game::graphicsState->frameCount))
		return;

	const auto* shape = current && native->registration->registered.load(std::memory_order_acquire) ? native->registration.get() : nullptr;

<<<<<<< HEAD
thread_local RE::BSMultiStreamInstanceTriShape::GroupHeader tl_lastFileGroupHeader{};
thread_local std::vector<uint16_t> tl_lastFileInstanceData;
thread_local bool tl_haveFileGroup = false;

void GrassOptimizations::Hooks::ReadGroupHeaderStreamTraits::thunk(RE::BSStreamHeader* streamHeader, RE::BSMultiStreamInstanceTriShape::GroupHeader* groupHeader, uint32_t size)
{
	func(streamHeader, groupHeader, size);
	std::memcpy(&tl_lastFileGroupHeader, groupHeader, std::min<uint32_t>(size, sizeof(tl_lastFileGroupHeader)));
}

void GrassOptimizations::Hooks::ReadInstanceGroupStreamTraits::thunk(RE::BSStreamHeader* streamHeader, uint16_t* instanceData, uint32_t size)
{
	func(streamHeader, instanceData, size);
	tl_lastFileInstanceData.resize((size + sizeof(uint16_t) - 1) / sizeof(uint16_t));
	std::memcpy(tl_lastFileInstanceData.data(), instanceData, size);
	tl_haveFileGroup = true;
}

void GrassOptimizations::Hooks::AddGroupQueuedGIDFile::thunk(RE::BSMultiStreamInstanceTriShape* a1, RE::BSStream* a2, RE::BSTArray<std::uint32_t>& a3)
{
	tl_haveFileGroup = false;
	func(a1, a2, a3);

	if (tl_haveFileGroup) {
		globals::features::grassOptimizations.bucketStore.CaptureGIDGroup(a1, &tl_lastFileGroupHeader,
			tl_lastFileInstanceData.data(), tl_lastFileInstanceData.size() * sizeof(uint16_t));
		tl_haveFileGroup = false;
	}
}

void GrassOptimizations::Hooks::AddGroupGIDFile::thunk(RE::BSMultiStreamInstanceTriShape* a1, RE::BSStream* a2)
{
	tl_haveFileGroup = false;
	func(a1, a2);

	if (tl_haveFileGroup) {
		globals::features::grassOptimizations.bucketStore.CaptureGIDGroup(a1, &tl_lastFileGroupHeader,
			tl_lastFileInstanceData.data(), tl_lastFileInstanceData.size() * sizeof(uint16_t));
		tl_haveFileGroup = false;
	}
}

void GrassOptimizations::OnGrassMeshLoaded(RE::BSMultiStreamInstanceTriShape* shape, const char* modelPath)
{
	if (shape && modelPath)
		bucketStore.meshLibrary.RecordModelPath(shape, modelPath);
}

static void UploadEyeIndexCB(GrassOptimizations& self, ID3D11DeviceContext* ctx, uint32_t capacityPerEye)
{
	D3D11_MAPPED_SUBRESOURCE m{};
	if (FAILED(ctx->Map(self.eyeIndexCB->CB(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
		return;
	auto* bytes = static_cast<uint8_t*>(m.pData);
	const uint32_t slotCount = globals::game::isVR ? 2u : 1u;
	for (uint32_t eye = 0; eye < slotCount; ++eye) {
		auto* cb = reinterpret_cast<GrassOptimizations::EyeIndexCB*>(bytes + (size_t)eye * GrassOptimizations::kEyeSlotBytes);
		*cb = { eye, eye * capacityPerEye, {} };
	}
	ctx->Unmap(self.eyeIndexCB->CB(), 0);
}

static void BindEyeIndexCB(GrassOptimizations& self, uint32_t eyeIndex)
{
	ID3D11Buffer* cb = self.eyeIndexCB->CB();
	UINT first = eyeIndex * (GrassOptimizations::kEyeSlotBytes / 16);
	UINT num = GrassOptimizations::kEyeSlotBytes / 16;
	// b7 matches GrassOptimizationsEyeCB in RunGrass.hlsl (free there: cb7 only exists on the vanilla path).
	self.ctx1->VSSetConstantBuffers1(7, 1, &cb, &first, &num);
}

static void WriteIndirectArgs(ID3D11DeviceContext* ctx, ID3D11Buffer* argsBuf, uint32_t indexCount, uint32_t capacity)
{
	WriteArgsUint32(ctx, argsBuf, argsByteOffset, indexCount);
	if (globals::game::isVR) {
		WriteArgsUint32(ctx, argsBuf, ArgsByteOffsetForEye(1), indexCount);
		WriteArgsUint32(ctx, argsBuf, StartInstanceLocationOffsetForEye(1), capacity);
	}
}

static void DrawGrassIndirect(GrassOptimizations& self, ID3D11DeviceContext* ctx, ID3D11Buffer* argsBuf, uint64_t descVal, uint32_t capacity)
{
	if (globals::game::isVR) {
		if (ID3D11InputLayout* layout = self.GetOptimizedInputLayout(descVal))
			ctx->IASetInputLayout(layout);
	}
	UploadEyeIndexCB(self, ctx, capacity);
	BindEyeIndexCB(self, 0);
	ctx->DrawIndexedInstancedIndirect(argsBuf, argsByteOffset);
	if (globals::game::isVR) {
		BindEyeIndexCB(self, 1);
		ctx->DrawIndexedInstancedIndirect(argsBuf, ArgsByteOffsetForEye(1));
	}
}

void VanillaDrawInstanceTriShape(RE::BSMultiStreamInstanceTriShape* geometry)
{
||||||| 12b261724
thread_local RE::BSMultiStreamInstanceTriShape::GroupHeader tl_lastFileGroupHeader{};
thread_local std::vector<uint16_t> tl_lastFileInstanceData;
thread_local bool tl_haveFileGroup = false;

void GrassOptimizations::Hooks::ReadGroupHeaderStreamTraits::thunk(RE::BSStreamHeader* streamHeader, RE::BSMultiStreamInstanceTriShape::GroupHeader* groupHeader, uint32_t size)
{
	func(streamHeader, groupHeader, size);
	std::memcpy(&tl_lastFileGroupHeader, groupHeader, std::min<uint32_t>(size, sizeof(tl_lastFileGroupHeader)));
}

void GrassOptimizations::Hooks::ReadInstanceGroupStreamTraits::thunk(RE::BSStreamHeader* streamHeader, uint16_t* instanceData, uint32_t size)
{
	func(streamHeader, instanceData, size);
	tl_lastFileInstanceData.resize((size + sizeof(uint16_t) - 1) / sizeof(uint16_t));
	std::memcpy(tl_lastFileInstanceData.data(), instanceData, size);
	tl_haveFileGroup = true;
}

void GrassOptimizations::Hooks::AddGroupQueuedGIDFile::thunk(RE::BSMultiStreamInstanceTriShape* a1, RE::BSStream* a2, RE::BSTArray<std::uint32_t>& a3)
{
	tl_haveFileGroup = false;
	func(a1, a2, a3);

	if (tl_haveFileGroup) {
		globals::features::grassOptimizations.bucketStore.CaptureGIDGroup(a1, &tl_lastFileGroupHeader,
			tl_lastFileInstanceData.data(), tl_lastFileInstanceData.size() * sizeof(uint16_t));
		tl_haveFileGroup = false;
	}
}

void GrassOptimizations::Hooks::AddGroupGIDFile::thunk(RE::BSMultiStreamInstanceTriShape* a1, RE::BSStream* a2)
{
	tl_haveFileGroup = false;
	func(a1, a2);

	if (tl_haveFileGroup) {
		globals::features::grassOptimizations.bucketStore.CaptureGIDGroup(a1, &tl_lastFileGroupHeader,
			tl_lastFileInstanceData.data(), tl_lastFileInstanceData.size() * sizeof(uint16_t));
		tl_haveFileGroup = false;
	}
}

RE::BSMultiStreamInstanceTriShape* GrassOptimizations::Hooks::LoadGrassType::thunk(RE::BGSGrassManager* grassManager, RE::GrassParam* a_param, uint32_t CellXDivided, uint32_t CellYDivided, uint64_t* typeKey, RE::BSFixedString* modelPath)
{
	auto* shape = func(grassManager, a_param, CellXDivided, CellYDivided, typeKey, modelPath);

	if (shape && modelPath)
		globals::features::grassOptimizations.bucketStore.meshLibrary.RecordModelPath(shape, modelPath->c_str());

	return shape;
}

void VanillaDrawInstanceTriShape(RE::BSMultiStreamInstanceTriShape* geometry)
{
=======
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
	auto* ctx = globals::d3d::context;
	auto& groups = geometry->GetMultiStreamTrishapeRuntimeData().instanceGroups;

	const bool direct = native && native->direct;

	if (auto& cb = globals::features::grassOptimizations.vanillaDrawCB) {
		ID3D11Buffer* marker = cb->CB();
		ctx->VSSetConstantBuffers(13, 1, &marker);
	}

	for (uint32_t i = 0; i < groups.size(); ++i) {
		auto* curInstanceGroup = groups[i];
		if (!curInstanceGroup || (!direct && !curInstanceGroup->isVisible))
			continue;

<<<<<<< HEAD
		uint32_t indexCount = 0;
		uint32_t* indexCountPtr = &indexCount;
		auto buffer = reinterpret_cast<ID3D11Buffer**>(globals::game::renderer->GetID3D11Resource(1, &indexCountPtr, 7));
		if (buffer && indexCountPtr) {
			*indexCountPtr = i;
			if (*buffer)
				ctx->Unmap(*buffer, 0);
			ctx->VSSetConstantBuffers(7u, 1u, buffer);
		}
||||||| 12b261724
		uint32_t indexCount = 0;
		uint32_t* indexCountPtr = &indexCount;
		static REL::Relocation<ID3D11Buffer** (*)(RE::BSGraphics::Renderer*, uint64_t, uint32_t**, uint32_t)> MapDynamicBuffer{ REL::RelocationID(75561, 77362) };
		auto buffer = MapDynamicBuffer(globals::game::renderer, 1, &indexCountPtr, 7);
		if (buffer && indexCountPtr) {
			*indexCountPtr = i;
			if (*buffer)
				ctx->Unmap(*buffer, 0);
			ctx->VSSetConstantBuffers(7u, 1u, buffer);
		}
=======
		if (shape && self.bucketStore.IsGroupOptimized(*shape, i))
			continue;
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb

		globals::game::renderer->DrawIndexedInstancedTriangles(geometry->GetGeometryRuntimeData().rendererData, 0, geometry->GetTrishapeRuntimeData().triangleCount, curInstanceGroup->instanceCount, geometry->GetGeometryRuntimeData().vertexDesc, curInstanceGroup->vertexBuffer);
	}
}

void GrassOptimizations::Hooks::DrawInstanceTriShape::thunk(RE::BSRenderPass*, RE::BSMultiStreamInstanceTriShape* geometry)
{
	auto& self = globals::features::grassOptimizations;
<<<<<<< HEAD
	auto* ctx = globals::d3d::context;

	auto shaderProperty = geometry->GetGeometryRuntimeData().shaderProperty;
	if (!shaderProperty || shaderProperty->GetRTTI() != globals::rtti::BSGrassShaderPropertyRTTI.get()) {
		VanillaDrawInstanceTriShape(geometry);
		return;
	}
	RE::NiSourceTexture* diffuseTexture = shaderProperty->GetBaseTexture();
	if (!diffuseTexture) {
		VanillaDrawInstanceTriShape(geometry);
		return;
	}

	const uint64_t descVal = *reinterpret_cast<uint64_t*>(&geometry->GetGeometryRuntimeData().vertexDesc);
	const uint32_t frame = globals::game::graphicsState->GetFrameCount();

	GrassBucket* b = nullptr;
	{
		std::scoped_lock lk(self.bucketStore.bucketMutex);

		const uint32_t meshId = self.bucketStore.meshLibrary.ResolveMeshId(geometry);
		const uint32_t triCount = meshId ? 0u : (uint32_t)geometry->GetTrishapeRuntimeData().triangleCount;
		auto* material = static_cast<RE::BSGrassShaderProperty*>(shaderProperty.get())->material;
		auto it = self.bucketStore.buckets.find({ meshId, material, meshId ? nullptr : diffuseTexture, triCount, meshId ? 0u : descVal });
		if (it == self.bucketStore.buckets.end() || !it->second.totalInstances || !it->second.instanceBuf) {
			VanillaDrawInstanceTriShape(geometry);
			return;
		}
		b = &it->second;

		// Since draws are dispatch per shape, ensure each bucket is only drawn once per frame per technique.
		uint32_t descriptor = 0;
		if (globals::game::currentPixelShader && *globals::game::currentPixelShader)
			descriptor = (*globals::game::currentPixelShader)->id;
		const uint64_t passKey = (static_cast<uint64_t>(pass->passEnum) << 32) | descriptor;

		if (b->drawnFrame == frame && b->drawnPassKey == passKey)
			return;
		b->drawnFrame = frame;
		b->drawnPassKey = passKey;
	}

	CS_GPU_PASS("GrassOptimizations::Draw");

	if (!b->cullVisible) {
		return;
	}

	auto* rendererData = geometry->GetGeometryRuntimeData().rendererData;
	if (!rendererData)
		return;
	auto* meshVB = Util::AsReal(rendererData->vertexBuffer);
	auto* indexB = Util::AsReal(rendererData->indexBuffer);
	if (!meshVB || !indexB)
		return;

	if (!b->argsIndexCountWritten) {
		const uint32_t indexCount = 3u * geometry->GetTrishapeRuntimeData().triangleCount;
		WriteIndirectArgs(ctx, b->argsBuf, indexCount, b->capacityInstances);
		b->argsIndexCountWritten = true;
	}

	// Replicate vanilla state setup
	auto& shadowState = globals::game::shadowState->GetRuntimeData();
	if (shadowState.vertexDesc != descVal) {
		shadowState.vertexDesc = descVal;
		shadowState.stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_VERTEX_DESC);
	}
	if (shadowState.topology != REX::W32::D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST) {
		shadowState.topology = REX::W32::D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		shadowState.stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_PRIMITIVE_TOPO);
	}
	RE::BSGraphics::SetDirtyStates(false);

	ctx->IASetIndexBuffer(indexB, DXGI_FORMAT_R16_UINT, 0);

	ID3D11Buffer* vbs[2] = { meshVB, nullptr };
	// Stream 0 is the mesh's own vertex buffer, so its stride comes from that mesh's descriptor; only
	// stream 1, the compacted instance records, is fixed at kGrassStride.
	UINT strides[2] = { VertexStrideFromDesc(descVal), kGrassStride };
	UINT offsets[2] = { 0, 0 };
	if (!strides[0])
		return;

	vbs[1] = b->compactedBuf;
	ctx->IASetVertexBuffers(0, 2, vbs, strides, offsets);
	ctx->VSSetShaderResources(2, 1, &b->extrasSRV);
	DrawGrassIndirect(self, ctx, b->argsBuf, descVal, b->capacityInstances);

	std::array<const GrassMeshLibrary::LODMesh*, (size_t)GrassMeshLibrary::LODTier::kCount> lodMeshes{};
	{
		std::scoped_lock lk(self.bucketStore.bucketMutex);
		for (uint32_t tier = 0; tier < (uint32_t)GrassMeshLibrary::LODTier::kCount; ++tier)
			lodMeshes[tier] = self.bucketStore.meshLibrary.GetLODMesh(b->meshId, (GrassMeshLibrary::LODTier)tier);
	}

	for (uint32_t tier = 0; tier < (uint32_t)GrassMeshLibrary::LODTier::kCount; ++tier) {
		GrassBucket::LODBin& bin = b->lodBins[tier];
		if (!bin.active)
			continue;

		const GrassMeshLibrary::LODMesh* lod = lodMeshes[tier];
		if (!lod || !lod->vertexBuffer || !lod->indexBuffer)
			continue;

		if (!bin.argsIndexCountWritten) {
			WriteIndirectArgs(ctx, bin.argsBuf, lod->indexCount, bin.capacityInstances);
			bin.argsIndexCountWritten = true;
		}

		if (shadowState.vertexDesc != lod->descVal) {
			shadowState.vertexDesc = lod->descVal;
			shadowState.stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_VERTEX_DESC);
			RE::BSGraphics::SetDirtyStates(false);
		}

		ctx->IASetIndexBuffer(lod->indexBuffer, DXGI_FORMAT_R16_UINT, 0);

		vbs[0] = lod->vertexBuffer;
		vbs[1] = bin.compactedBuf;
		strides[0] = lod->meshStride;
		ctx->IASetVertexBuffers(0, 2, vbs, strides, offsets);
		ctx->VSSetShaderResources(2, 1, &bin.extrasSRV);
		DrawGrassIndirect(self, ctx, bin.argsBuf, lod->descVal, bin.capacityInstances);
||||||| 12b261724
	auto* ctx = globals::d3d::context;

	auto shaderProperty = geometry->GetGeometryRuntimeData().shaderProperty;
	if (!shaderProperty || shaderProperty->GetRTTI() != globals::rtti::BSGrassShaderPropertyRTTI.get()) {
		VanillaDrawInstanceTriShape(geometry);
		return;
	}
	RE::NiSourceTexture* diffuseTexture = shaderProperty->GetBaseTexture();
	if (!diffuseTexture) {
		VanillaDrawInstanceTriShape(geometry);
		return;
	}

	const uint64_t descVal = *reinterpret_cast<uint64_t*>(&geometry->GetGeometryRuntimeData().vertexDesc);
	const uint32_t frame = globals::game::graphicsState->frameCount;

	GrassBucket* b = nullptr;
	{
		std::scoped_lock lk(self.bucketStore.bucketMutex);

		const uint32_t meshId = self.bucketStore.meshLibrary.ResolveMeshId(geometry);
		const uint32_t triCount = meshId ? 0u : (uint32_t)geometry->GetTrishapeRuntimeData().triangleCount;
		auto* material = static_cast<RE::BSGrassShaderProperty*>(shaderProperty.get())->material;
		auto it = self.bucketStore.buckets.find({ meshId, material, meshId ? nullptr : diffuseTexture, triCount, meshId ? 0u : descVal });
		if (it == self.bucketStore.buckets.end() || !it->second.totalInstances || !it->second.instanceBuf) {
			VanillaDrawInstanceTriShape(geometry);
			return;
		}
		b = &it->second;

		// Since draws are dispatch per shape, ensure each bucket is only drawn once per frame per technique.
		uint32_t descriptor = 0;
		if (globals::game::currentPixelShader && *globals::game::currentPixelShader)
			descriptor = (*globals::game::currentPixelShader)->id;
		const uint64_t passKey = (static_cast<uint64_t>(pass->passEnum) << 32) | descriptor;

		if (b->drawnFrame == frame && b->drawnPassKey == passKey)
			return;
		b->drawnFrame = frame;
		b->drawnPassKey = passKey;
	}

	if (!b->cullVisible) {
		return;
	}

	auto* rendererData = geometry->GetGeometryRuntimeData().rendererData;
	if (!rendererData)
		return;
	auto* meshVB = reinterpret_cast<ID3D11Buffer*>(rendererData->vertexBuffer);
	auto* indexB = reinterpret_cast<ID3D11Buffer*>(rendererData->indexBuffer);
	if (!meshVB || !indexB)
		return;

	if (!b->argsIndexCountWritten) {
		const uint32_t indexCount = 3u * geometry->GetTrishapeRuntimeData().triangleCount;
		const D3D11_BOX argBox{ argsByteOffset, 0, 0, argsByteOffset + sizeof(uint32_t), 1, 1 };
		ctx->UpdateSubresource(b->argsBuf, 0, &argBox, &indexCount, 0, 0);
		b->argsIndexCountWritten = true;
	}

	// Replicate vanilla state setup
	auto& shadowState = globals::game::shadowState->GetRuntimeData();
	if (shadowState.vertexDesc != descVal) {
		shadowState.vertexDesc = descVal;
		shadowState.stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_VERTEX_DESC);
	}
	if (shadowState.topology != D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST) {
		shadowState.topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		shadowState.stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_PRIMITIVE_TOPO);
	}
	static REL::Relocation<void (*)(uint32_t)> SetDirtyStates{ REL::RelocationID(75580, 77386) };
	SetDirtyStates(0);

	ctx->IASetIndexBuffer(indexB, DXGI_FORMAT_R16_UINT, 0);

	ID3D11Buffer* vbs[2] = { meshVB, nullptr };
	UINT strides[2] = { VertexStrideFromDesc(descVal), kGrassStride };
	UINT offsets[2] = { 0, 0 };
	if (!strides[0])
		return;

	vbs[1] = b->compactedBuf;
	ctx->IASetVertexBuffers(0, 2, vbs, strides, offsets);
	ctx->VSSetShaderResources(2, 1, &b->extrasSRV);
	ctx->DrawIndexedInstancedIndirect(b->argsBuf, argsByteOffset);

	std::array<const GrassMeshLibrary::LODMesh*, (size_t)GrassMeshLibrary::LODTier::kCount> lodMeshes{};
	{
		std::scoped_lock lk(self.bucketStore.bucketMutex);
		for (uint32_t tier = 0; tier < (uint32_t)GrassMeshLibrary::LODTier::kCount; ++tier)
			lodMeshes[tier] = self.bucketStore.meshLibrary.GetLODMesh(b->meshId, (GrassMeshLibrary::LODTier)tier);
	}

	for (uint32_t tier = 0; tier < (uint32_t)GrassMeshLibrary::LODTier::kCount; ++tier) {
		GrassBucket::LODBin& bin = b->lodBins[tier];
		if (!bin.active)
			continue;

		const GrassMeshLibrary::LODMesh* lod = lodMeshes[tier];
		if (!lod || !lod->vertexBuffer || !lod->indexBuffer)
			continue;

		if (!bin.argsIndexCountWritten) {
			const D3D11_BOX argBox{ argsByteOffset, 0, 0, argsByteOffset + sizeof(uint32_t), 1, 1 };
			ctx->UpdateSubresource(bin.argsBuf, 0, &argBox, &lod->indexCount, 0, 0);
			bin.argsIndexCountWritten = true;
		}

		if (shadowState.vertexDesc != lod->descVal) {
			shadowState.vertexDesc = lod->descVal;
			shadowState.stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_VERTEX_DESC);
			SetDirtyStates(0);
		}

		ctx->IASetIndexBuffer(lod->indexBuffer, DXGI_FORMAT_R16_UINT, 0);

		vbs[0] = lod->vertexBuffer;
		vbs[1] = bin.compactedBuf;
		strides[0] = lod->meshStride;
		ctx->IASetVertexBuffers(0, 2, vbs, strides, offsets);
		ctx->VSSetShaderResources(2, 1, &bin.extrasSRV);
		ctx->DrawIndexedInstancedIndirect(bin.argsBuf, argsByteOffset);
=======
	if (auto* bucket = self.loaded ? self.bucketStore.FindBucket(geometry) : nullptr) {
		if (self.HasCurrentCull() && bucket->optimizedReady && bucket->sliceTableCount)
			self.DrawBucket(*bucket);
	} else {
		DrawVanilla(geometry);
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
	}
}

void GrassOptimizations::Hooks::Install()
{
	auto& trampoline = SKSE::GetTrampoline();

	stl::write_vfunc<0x0, BSMultiStreamInstanceTriShape_dtor>(RE::VTABLE_BSMultiStreamInstanceTriShape[0]);
	stl::write_vfunc<0x34, NiNode_OnVisible>(RE::VTABLE_NiNode[0]);
	stl::write_vfunc<0x3A, DoneAddingInstances>(RE::VTABLE_BSMultiStreamInstanceTriShape[0]);
	stl::write_vfunc<0x3C, AddGroup>(RE::VTABLE_BSMultiStreamInstanceTriShape[0]);
	stl::write_vfunc<0x3D, RemoveGroup>(RE::VTABLE_BSMultiStreamInstanceTriShape[0]);

	stl::write_vfunc<0x6, BSGrassShader_SetupGeometry>(RE::VTABLE_BSGrassShader[0]);
	stl::write_vfunc<0x4, BSGrassShader_SetupMaterial>(RE::VTABLE_BSGrassShader[0]);

	// Queued cache groups receive their native slots when the shared attachment queue is drained.
	stl::write_thunk_call<AddQueuedGroupGIDBuffer>(REL::RelocationID(15205, 15373).address() + Util::VersionedRelocation::Select(0x7FF, 0x756, 0x768));
	stl::write_thunk_call<AddQueuedGroupGIDFile>(REL::RelocationID(15206, 15374).address() + REL::Relocate(0x394, 0x384));
	stl::detour_thunk<AttachQueuedGroups>(REL::RelocationID(74603, 76335));

	stl::write_thunk_call<AddGroupGIDBuffer>(REL::RelocationID(15205, 15373).address() + Util::VersionedRelocation::Select(0x806, 0x75D, 0x76F));
	stl::write_thunk_call<AddGroupGIDFile>(REL::RelocationID(15206, 15374).address() + REL::Relocate(0x39B, 0x38B));

	// Record each grass type's source .nif path alongside its shape.
	stl::write_thunk_call<LoadGrassType>(REL::RelocationID(15204, 15372).address() + Util::VersionedRelocation::Select(0x2F5, 0x2F5, 0x305));
	stl::write_thunk_call<LoadGrassType>(REL::RelocationID(15205, 15373).address() + Util::VersionedRelocation::Select(0x62B, 0x597, 0x590));
	stl::write_thunk_call<LoadGrassType>(REL::RelocationID(15206, 15374).address() + REL::Relocate(0x25C, 0x25C));

	std::uint8_t patch[] = { 0x4C, 0x89, 0xF2 };  // mov rdx, r14
	REL::safe_write(REL::RelocationID(100847, 107637).address() + REL::Relocate(0x660, 0x648), patch, sizeof(patch));
	stl::write_thunk_call<DrawInstanceTriShape>(REL::RelocationID(100847, 107637).address() + REL::Relocate(0x663, 0x64B));
	trampoline.write_branch<5>(REL::RelocationID(100847, 107637).address() + REL::Relocate(0x668, 0x650), REL::RelocationID(100847, 107637).address() + REL::Relocate(0x759, 0x73A));

	// Skip mapping the vanilla dynamic fade buffer.
	if (REL::Module::IsAE()) {
		trampoline.write_branch<5>(
			REL::RelocationID(99996, 106685).address() + Util::VersionedRelocation::Select(0x595, 0x595, 0x6A2),
			REL::RelocationID(99996, 106685).address() + Util::VersionedRelocation::Select(0x6C6, 0x6C6, 0x7D6));
	} else {
		REL::safe_write(REL::RelocationID(99996, 106685).address() + 0x54D, REL::NOP5);
	}

	logger::info("[GRASS OPTIMIZATIONS] Installed hooks");
}

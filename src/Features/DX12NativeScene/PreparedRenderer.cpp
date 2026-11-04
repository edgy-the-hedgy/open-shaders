#include "DX12NativeScene.h"
#include "Features/RTDirectLighting/RTDirectLighting.h"
#include "Features/RedstoneGI/RedstoneGI.h"
#include "Features/LightLimitFix.h"
#include "Deferred.h"

#include "Globals.h"
#include "GpuPass.h"
#include "State.h"
#include "Util.h"
#include "Utils/Form.h"

std::optional<DX12NativeScene::PublishedScene> DX12NativeScene::GetPublishedScene() const
{
	if (!loaded || !ready || !settings.UsePreparedScene ||
		!preparedActive || transitionOpen || globals::game::isVR || globals::state->isMapMenuOpen)
		return std::nullopt;
	auto cell = preparedScene.GetCell();
	auto accelerationScene = preparedAcceleration.GetScene();
	if (!cell || !accelerationScene || accelerationScene->scope.scene != preparedRevision)
		return std::nullopt;
	const auto* current = globals::game::player ? globals::game::player->GetParentCell() : nullptr;
	if (!current || current->GetFormID() != preparedCellID)
		return std::nullopt;
	return PublishedScene{ std::move(cell), std::move(accelerationScene), preparedLightReferences, preparedRoomReferences, preparedCellID, preparedRevision };
}

void DX12NativeScene::RequestPreparedScene()
{
	if (!settings.UsePreparedScene || !settings.UsePreparedMaterials) {
		if (!settings.UsePreparedScene) ready = false;
		settings.UsePreparedScene = settings.UsePreparedMaterials = true;
		resetRequested = true;
	}
}

void DX12NativeScene::ResetPreparedScene()
{
	++preparedRevision;
	preparedCellID = 0;
	preparedSubmitted = preparedActive = false;
	preparedTraceFrame.reset();
	preparedLightReferences.clear();
	preparedRoomReferences.fill(0);
	preparedScene.Reset();
	preparedAcceleration.Reset();
	preparedTrace.Reset(true);
	nativeShadowTrace.Reset(true);
}

void DX12NativeScene::AdvancePreparedScene(uint32_t, uint64_t a_budgetBytes)
{
	CS_GPU_PASS("DX12NativeScene::PreparedSceneProgress");
	auto* cell = globals::game::player ? globals::game::player->GetParentCell() : nullptr;
	const auto cellID = cell ? cell->GetFormID() : 0;
	const bool active = ready && !transitionOpen && !globals::game::isVR && !globals::state->isMapMenuOpen && cell && cell->IsInteriorCell();
	if (resetRequested || cellID != preparedCellID || active != preparedActive) {
		ResetPreparedScene();
		preparedCellID = cellID;
		preparedActive = active;
		resetRequested = false;
	}
	if (!active) return;
	if (preparedSubmitted) return;
	if (preparedScene.Requested() && !preparedScene.GetCell()) return;
	const auto backend = Feature::FindGraphicsBackend();
	if (!backend || !backend->commandQueue) return;
	if (!preparedScene.Requested()) {
		const auto* source = cell->GetFile(0);
		if (!source) return;
		const auto local = cell->GetLocalFormID();
		const auto key = std::format("{:06X}:{}", local, source->GetFilename());
		const auto directory = std::filesystem::path("Data/SKSE/Plugins/CommunityShaders/RedstoneScene/cells") /
			source->GetFilename() / std::format("{:06X}", local);
		preparedScene.Request(*backend, directory, key, preparedRevision, a_budgetBytes,
			settings.UsePreparedMaterials ? uint64_t(settings.MaterialTextureBudgetMB) * 1024 * 1024 : 0);
	}
	if (!preparedSubmitted) {
		if (auto prepared = preparedScene.GetCell()) {
			auto resolve = [](const json& object, const char* field) {
				const auto entry = object.find(field);
				if (entry == object.end() || !entry->is_string()) return 0u;
				const auto& key = entry->get_ref<const std::string&>();
				const auto separator = key.find(':');
				return separator == std::string::npos ? 0u : Util::SpidToFormId("0x" + key.substr(0, separator) + "~" + key.substr(separator + 1));
			};
			for (const auto& definition : prepared->lightDefinitions) {
				std::array<uint32_t, 2> identity{};
				const auto referenceID = resolve(definition, "referenceKey");
				const auto baseline = definition.find("baseline");
				const auto baseID = baseline != definition.end() && baseline->is_object() ? resolve(*baseline, "baseKey") : 0u;
				if (auto* reference = RE::TESForm::LookupByID<RE::TESObjectREFR>(referenceID)) {
					const auto* base = reference->GetObjectReference();
					if (base && baseID && base->GetFormID() == baseID && base->GetFormType() == RE::FormType::Light && reference->GetParentCell() == cell)
						identity = { referenceID, baseID };
				}
				preparedLightReferences.push_back(identity);
			}
			for (uint32_t room = 0; room < prepared->roomDefinitions.size(); ++room)
				preparedRoomReferences[room] = resolve(prepared->roomDefinitions[room], "referenceKey");
			preparedAcceleration.Start(*backend, prepared->snapshot, prepared->geometries, a_budgetBytes);
			preparedSubmitted = true;
		}
	}
	preparedAcceleration.Advance();
}

bool DX12NativeScene::WantsOpaqueRoomMetadata() const
{
	return ready && settings.UsePreparedScene && preparedActive && !transitionOpen && !globals::game::isVR && !globals::state->isMapMenuOpen;
}

void DX12NativeScene::OnBeforeDeferredLightingComposite(ID3D11ShaderResourceView* diffuse, ID3D11ShaderResourceView* specular,
	const std::array<uint32_t, 2>& size)
{
	const auto& hybrid = globals::features::redstoneGI;
	if (((hybrid.loaded && hybrid.settings.NativeHybrid) || (globals::features::rtDirectLighting.loaded && globals::features::rtDirectLighting.WantsComposite())) && !transitionOpen && !globals::game::isVR &&
		!globals::state->isMapMenuOpen && !(globals::state->permutationData.ExtraShaderDescriptor &
			static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections)))
		if (const auto graphics=Feature::FindGraphicsBackend()) {
            nativeHybrid.Retire();
            nativeComposition.CaptureSource(*graphics,diffuse,specular,size,globals::game::graphicsState->GetFrameCount(),
                uint64_t(settings.SceneCompositionBudgetMB)*1024u*1024u);
        }
}

void DX12NativeScene::OnDeferredLightingComplete(ID3D11UnorderedAccessView* target, const std::array<uint32_t, 2>& size)
{
    nativeHybrid.Retire();
    const auto frame=globals::game::graphicsState->GetFrameCount();
    const auto& hybrid=globals::features::redstoneGI;
    if (!hybrid.loaded || !hybrid.settings.NativeHybrid) nativeHybrid.Reset();
    std::optional<DX12Native::NativeSceneOutput> consumed;
	const bool shadowRequested = settings.ShadowPreview || (settings.ShadowStrength > 0 && settings.ShadowBlend > 0);
	shadowLayerStatus = !settings.NativeShadowMask ? "RT mask producer disabled" : !shadowRequested ?
		"Image layer bypassed: enable preview or raise both strength and blend" : "Waiting for main-world composition";
    if (!transitionOpen && !globals::game::isVR && !globals::state->isMapMenuOpen && target &&
        target==Util::AsReal(globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN].UAV) &&
        !(globals::state->permutationData.ExtraShaderDescriptor & static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections))) {
        if (const auto graphics=Feature::FindGraphicsBackend()) {
            if (hybrid.loaded && hybrid.settings.NativeHybrid)
                if (const auto input=nativeComposition.GetSceneInput(frame,size)) nativeHybrid.Dispatch(*graphics,size,frame,hybrid,*input);
			std::optional<DX12Native::NativeShadowLayer> shadow;
			if (settings.NativeShadowMask && shadowRequested) {
				const auto mask = GetNativeShadowMask(frame);
				if (!mask) {
					shadowLayerStatus = "No current RT mask: " + shadowMaskUnavailable;
				} else if (mask->size != size) {
					shadowLayerStatus = std::format("Mask/composition size mismatch: {}x{} vs {}x{}", mask->size[0], mask->size[1], size[0], size[1]);
				} else {
					shadow = DX12Native::NativeShadowLayer{ mask->gpu,
						{ mask->cameraOrigin.x, mask->cameraOrigin.y, mask->cameraOrigin.z },
						settings.ShadowStrength, settings.ShadowBlend, settings.ShadowRadius, settings.ShadowReceiverTolerance, settings.ShadowPreview };
					shadow->temporal = settings.ShadowTemporal;
					shadow->historyWeight = settings.ShadowHistoryWeight;
					shadow->traceResolution = settings.ShadowTraceResolution;
					shadow->sceneRevision = mask->scene->scope.scene;
					shadow->policy = { std::bit_cast<uint32_t>(settings.ShadowEmitterRadius), std::bit_cast<uint32_t>(settings.ShadowReachMultiplier),
						std::bit_cast<uint32_t>(settings.ShadowFalloffFraction), std::bit_cast<uint32_t>(settings.ShadowFalloffExponent), settings.ShadowLightIndex,
						uint32_t(settings.ShadowStochastic) | (uint32_t(settings.ShadowAllLights) << 1u) | (uint32_t(settings.ShadowDistanceFade) << 2u),
						settings.ShadowTraceResolution, settings.ShadowNormalInfluence };
				}
			}
            consumed=nativeComposition.Present(*graphics,target,size,preparedTrace.GetSceneOutput(frame),
                nativeHybrid.GetSceneOutput(frame),uint64_t(settings.SceneCompositionBudgetMB)*1024u*1024u,shadow);
			if (shadow) {
				const auto& layerFailure = nativeComposition.ShadowFailure();
				const auto& compositionFailure = nativeComposition.Failure();
				shadowLayerStatus = !layerFailure.empty() ? layerFailure : !compositionFailure.empty() ? compositionFailure :
					consumed ? "Shadow layer submitted; GPU pixel coverage not verified" : "No composition submitted: frame slots busy or composition unavailable";
			}
        }
    }
    preparedTrace.ReleaseSceneOutput(frame,consumed ? consumed->ready : nullptr,consumed ? consumed->value : 0);
    nativeHybrid.ReleaseSceneOutput(frame,consumed ? consumed->ready : nullptr,consumed ? consumed->value : 0);
	ReleaseNativeShadowMask(frame,consumed ? consumed->ready : nullptr,consumed ? consumed->value : 0);
	if (settings.NativeShadowMask && shadowRequested && shadowLayerStatus != shadowLayerLoggedStatus &&
		(shadowLayerLoggedStatus.empty() || frame - shadowLayerLogFrame >= 120u)) {
		logger::info("[DX12native-shadowmask] {}", shadowLayerStatus);
		const auto preparation = preparedScene.GetDiagnostics();
		const auto preparationFailure = preparation.value("failure", std::string{});
		if (!preparationFailure.empty()) logger::info("[DX12native-shadowmask] Cache unavailable: {}", preparationFailure);
		shadowLayerLoggedStatus = shadowLayerStatus;
		shadowLayerLogFrame = frame;
	}
}

void DX12NativeScene::OnAfterDeferredLighting(ID3D11UnorderedAccessView* target, const std::array<uint32_t, 2>& size)
{
	if (!settings.PreviewPreparedScene || !settings.UsePreparedScene ||
		(globals::features::rtDirectLighting.loaded && globals::features::rtDirectLighting.WantsComposite())) return;
	if (!target || !ready || !preparedActive || transitionOpen || globals::game::isVR || globals::state->isMapMenuOpen) return;
	if (target != Util::AsReal(globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN].UAV) ||
		(globals::state->permutationData.ExtraShaderDescriptor & static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections))) return;
	const auto frame = globals::game::graphicsState->GetFrameCount();
	if (frame % 8 || preparedTraceFrame == frame) return;
	const auto state = GetPublishedScene();
	if (!state) { preparedTrace.Reset(); return; }
	const uint32_t view = settings.UsePreparedMaterials ? settings.PreparedMaterialView : 0u;
	const bool all = settings.PreviewAllLights && view >= DX12Native::DirectionalTrace::kFirstDirectView;
	if (!all && (!view || view >= DX12Native::DirectionalTrace::kFirstDirectView) && settings.PreviewLight >= state->cell->lights.size()) return;
	const auto backend = Feature::FindGraphicsBackend();
	if (!backend || !preparedTrace.Initialize(*backend)) return;
	// Opaque receiver-room metadata still uses the existing LLF room generation.
	const auto lights = globals::features::lightLimitFix.GetLightView(frame);
	const float3 light = settings.PreviewLight < state->cell->lights.size() ? state->cell->lights[settings.PreviewLight] : float3{};
	const auto identity = settings.PreviewLight < state->lightReferences.size() ? state->lightReferences[settings.PreviewLight] : std::array<uint32_t, 2>{};
	auto* metadata = Util::AsReal(globals::game::renderer->GetRuntimeData().renderTargets[MASKS2].SRV);
	D3D11_TEXTURE2D_DESC desc{};
	if (!Util::GetTexture2DDesc(metadata, desc) || desc.Format != DXGI_FORMAT_R16G16B16A16_UNORM) metadata = nullptr;
	preparedTrace.Dispatch(state->acceleration, size, light, frame, uint64_t(settings.BufferBudgetMB) * 1024 * 1024 / 4, true, lights,
		view, nullptr, identity[0], identity[1], metadata,
		all ? std::span<const std::array<uint32_t, 2>>(state->lightReferences) : std::span<const std::array<uint32_t, 2>>{},
		all, { settings.PreviewDirectStrength, settings.PreviewDiffuseStrength, settings.PreviewSpecularStrength, settings.PreviewEmissionStrength },
		false, 1.0f, false, 0u, state->roomReferences, 1.0f, 2u,
		all ? std::span<const DX12Native::PreparedPointLight>(state->cell->nativeLights) :
			(settings.PreviewLight < state->cell->nativeLights.size() ?
				std::span<const DX12Native::PreparedPointLight>(state->cell->nativeLights).subspan(settings.PreviewLight, 1) :
				std::span<const DX12Native::PreparedPointLight>{}));
	preparedTraceFrame = frame;
}

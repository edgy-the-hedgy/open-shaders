#include "RedstoneShadows.h"
#include "Features/LightLimitFix.h"
#include "Deferred.h"

#include "Globals.h"
#include "GpuPass.h"
#include "State.h"
#include "Util.h"
#include "Utils/Form.h"

void RedstoneShadows::ResetPreparedScene()
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
}

void RedstoneShadows::AdvancePreparedScene(uint32_t, uint64_t a_budgetBytes)
{
	CS_GPU_PASS("RedstoneShadows::PreparedSceneProgress");
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

bool RedstoneShadows::WantsOpaqueRoomMetadata() const
{
	return ready && settings.UsePreparedScene && preparedActive && !transitionOpen && !globals::game::isVR && !globals::state->isMapMenuOpen;
}

int RedstoneShadows::GetIndirectLightingPriority() const
{
	return settings.UseDXRIndirect && !globals::game::isVR ? 200 : -1;
}

void RedstoneShadows::OnAfterDeferredLighting(ID3D11UnorderedAccessView* a_target, const std::array<uint32_t, 2>& a_size)
{
	CS_GPU_PASS("RedstoneShadows::PreparedLighting");
	constexpr uint32_t kPreviewInterval = 8;
	const bool direct = settings.UsePreparedDirectLighting || settings.UseDXRIndirect;
	if (!a_target || !settings.UsePreparedScene || (!direct && !settings.PreviewPreparedScene) ||
		!ready || !preparedActive || transitionOpen || globals::game::isVR || globals::state->isMapMenuOpen) return;
	if (direct && !settings.UsePreparedMaterials) return;
	if (a_target != Util::AsReal(globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN].UAV) ||
		(globals::state->permutationData.ExtraShaderDescriptor & static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections))) return;
	const auto frame = globals::game::graphicsState->GetFrameCount();
	if ((!direct && frame % kPreviewInterval) || preparedTraceFrame == frame) return;
	auto* cell = globals::game::player ? globals::game::player->GetParentCell() : nullptr;
	if (!cell || cell->GetFormID() != preparedCellID) { preparedTrace.Reset(); return; }
	const auto prepared = preparedScene.GetCell();
	const auto completedScene = preparedAcceleration.GetScene();
	if (!prepared || !completedScene || completedScene->scope.scene != preparedRevision) return;
	const auto materialView = direct ? RedstoneShadow::DirectionalTrace::kCombinedDirectView : settings.UsePreparedMaterials ? settings.PreparedMaterialView : 0;
	const bool allLights = settings.UseAllPreparedLights && materialView >= RedstoneShadow::DirectionalTrace::kFirstDirectView;
	if (!allLights && (!materialView || materialView >= RedstoneShadow::DirectionalTrace::kFirstDirectView) && settings.PreparedLight >= prepared->lights.size()) return;
	const auto backend = Feature::FindGraphicsBackend();
	if (!backend || !preparedTrace.Initialize(*backend)) return;
	const auto lights = globals::features::lightLimitFix.GetLightView(frame);
	const auto light = settings.PreparedLight < prepared->lights.size() ? prepared->lights[settings.PreparedLight] : float3{};
	const auto budget = direct ? uint64_t(settings.DirectLightingBudgetMB) * 1024 * 1024 : uint64_t(settings.BufferBudgetMB) * 1024 * 1024 / 4;
	const auto identity = settings.PreparedLight < preparedLightReferences.size() ? preparedLightReferences[settings.PreparedLight] : std::array<uint32_t, 2>{};
	const auto& metadata = globals::game::renderer->GetRuntimeData().renderTargets[MASKS2];
	D3D11_TEXTURE2D_DESC metadataDesc{};
	auto* metadataSrv = Util::AsReal(metadata.SRV);
	if (!Util::GetTexture2DDesc(metadataSrv, metadataDesc) || metadataDesc.Format != DXGI_FORMAT_R16G16B16A16_UNORM) metadataSrv = nullptr;
	preparedTrace.Dispatch(completedScene, a_size, light, frame, budget, true, lights, materialView, direct ? a_target : nullptr,
		identity[0], identity[1], metadataSrv, allLights ? std::span<const std::array<uint32_t, 2>>(preparedLightReferences) : std::span<const std::array<uint32_t, 2>>{}, allLights,
		{ settings.DirectStrength, settings.DiffuseStrength, settings.SpecularStrength, settings.EmissionStrength },
		settings.ConnectIndirectLighting && !settings.UseDXRIndirect, settings.IndirectStrength, settings.UseDirectRayRegeneration, settings.UseDXRIndirect ? settings.IndirectRays : 0u, preparedRoomReferences);
	preparedTraceFrame = frame;
}

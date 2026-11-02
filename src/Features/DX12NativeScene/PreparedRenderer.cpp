#include "DX12NativeScene.h"
#include "Features/RTDirectLighting/RTDirectLighting.h"
#include "Features/LightLimitFix.h"
#include "Deferred.h"

#include "Globals.h"
#include "GpuPass.h"
#include "State.h"
#include "Util.h"
#include "Utils/Form.h"
#include "Utils/FileDigest.h"
#include <BS_thread_pool.hpp>
#include <fstream>
#include <unordered_set>
#include <cctype>

namespace
{
	// Never publish a partially copied package: immutable generations are
	// committed by a single, atomically replaced active.json pointer.
	void CacheAcceptedScene(std::filesystem::path a_source, std::string a_plugin, uint32_t a_local,
		std::string a_cell, uint32_t a_stage, std::string a_layoutDigest, uint32_t a_worldspace)
	{
		static BS::thread_pool<> pool{ 1 };
		pool.detach_task([source = std::move(a_source), plugin = std::move(a_plugin),
			local = a_local, cell = std::move(a_cell), stage = a_stage,
			digest = std::move(a_layoutDigest), worldspace = a_worldspace] {
			try {
				// Paths are formed solely from a resolved plugin filename and a
				// hex local form ID. Reject traversal and arbitrary separators.
				if (plugin.empty() || plugin.find("..") != std::string::npos ||
					!std::ranges::all_of(plugin, [](unsigned char c) {
						return std::isalnum(c) || c == '.' || c == '_' || c == '-';
					})) return;
				const auto base = std::filesystem::path("Data/SKSE/Plugins/CommunityShaders/RedstoneScene/runtime-cache/cells") /
					plugin / std::format("{:06X}", local) / std::format("q{}", stage);
				const auto revision = base / "revisions" / digest;
				const auto manifestPath = source / "cell-layout.json";
				if (std::filesystem::file_size(manifestPath) > 16ull * 1024 * 1024) return;
				std::ifstream manifestFile(manifestPath);
				const auto layout = json::parse(manifestFile);
				if (layout.at("layoutDigest").get<std::string>() != digest ||
					layout.at("cell").get<std::string>() != cell ||
					!layout.at("instances").is_array()) return;
				const auto sourceHash = Util::FileDigest::Sha256FileHex(manifestPath);
				if (!sourceHash) return;
				std::filesystem::create_directories(revision);
				const auto atlas = std::filesystem::path("Data/SKSE/Plugins/CommunityShaders/RedstoneScene/atlas") /
					std::format("q{}", stage);
				std::unordered_set<std::string> seen;
				const auto install = [&](const std::string& folder, const std::string& hash, const std::string& extension) {
					if (hash.size() != 64 || !std::ranges::all_of(hash, [](unsigned char c) {
						return c >= '0' && c <= '9' || c >= 'a' && c <= 'f';
					})) throw std::runtime_error("Invalid cache asset digest");
					const auto fileName = hash + extension;
					if (!seen.insert(folder + "/" + fileName).second) return;
					auto input = source / folder / fileName;
					if (!std::filesystem::exists(input)) input = atlas / folder / fileName;
					const auto actual = Util::FileDigest::Sha256FileHex(input);
					if (!actual || _stricmp(actual->c_str(), hash.c_str()) != 0)
						throw std::runtime_error("Cache source digest mismatch");
					const auto outputDirectory = revision / folder;
					std::filesystem::create_directories(outputDirectory);
					const auto output = outputDirectory / fileName;
					if (const auto prior = Util::FileDigest::Sha256FileHex(output);
						prior && _stricmp(prior->c_str(), hash.c_str()) == 0) return;
					const auto temporary = output.wstring() + L".part";
					std::error_code ignored;
					std::filesystem::remove(temporary, ignored);
					std::filesystem::create_hard_link(input, temporary, ignored);
					if (ignored) std::filesystem::copy_file(input, temporary, std::filesystem::copy_options::overwrite_existing);
					if (!MoveFileExW(temporary.c_str(), output.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
						throw std::runtime_error("Failed to commit cache asset");
				};
				for (const auto& row : layout.at("instances")) {
					const auto geometry = row.at("geometrySha256").get<std::string>();
					if (row.at("geometryFile").get<std::string>() != "geometry/" + geometry + ".rsgm")
						throw std::runtime_error("Cache geometry manifest mismatch");
					install("geometry", geometry, ".rsgm");
					if (row.value("shadingStatus", std::string{}) == "compiled") {
						const auto shading = row.at("shadingSha256").get<std::string>();
						if (row.at("shadingFile").get<std::string>() != "shading/" + shading + ".rsga")
							throw std::runtime_error("Cache shading manifest mismatch");
						install("shading", shading, ".rsga");
					}
				}
				const auto textures = source / "cell-textures.json";
				if (std::filesystem::exists(textures))
					std::filesystem::copy_file(textures, revision / "cell-textures.json", std::filesystem::copy_options::overwrite_existing);
				const auto content = manifestPath;
				const auto stagedManifest = revision / "cell-layout.json.part";
				std::filesystem::copy_file(content, stagedManifest, std::filesystem::copy_options::overwrite_existing);
				if (!MoveFileExW(stagedManifest.c_str(), (revision / "cell-layout.json").c_str(),
					MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
					throw std::runtime_error("Failed to commit scene manifest");
				const json pointer{ { "schemaVersion", 1 }, { "stage", stage }, { "cell", cell },
					{ "worldspaceFormID", worldspace }, { "layoutDigest", digest },
					{ "sourceDirectory", source.string() }, { "sourceSha256", *sourceHash },
					{ "revisionDirectory", revision.string() } };
				const auto stagedPointer = base / "active.json.part";
				{
					std::ofstream out(stagedPointer, std::ios::binary | std::ios::trunc);
					out << pointer.dump(2);
					if (!out) throw std::runtime_error("Failed to write scene cache pointer");
				}
				if (!MoveFileExW(stagedPointer.c_str(), (base / "active.json").c_str(),
					MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
					throw std::runtime_error("Failed to publish scene cache pointer");
			} catch (const std::exception&) {
				// Cache writes are optional and must never interrupt native rendering.
				// A failed write cannot replace active.json with a partial revision.
			}
		});
	}
}

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
	upgradeScene.Reset();
	upgradeAcceleration.Reset();
	publishedStage = upgradeStage = 0;
	upgradeRevision = 0;
	upgradeSubmitted = false;
	preparedPackageRoot.clear();
	publishedPackageDirectory.clear();
	cachedRevision = 0;
	upgradeManifestTime = {};
	upgradeFailure.clear();
	lastUpgradePollFrame = 0;
	preparedTrace.Reset(true);
}

void DX12NativeScene::AdvancePreparedScene(uint32_t a_frame, uint64_t a_budgetBytes)
{
	CS_GPU_PASS("DX12NativeScene::PreparedSceneProgress");
	auto* cell = globals::game::player ? globals::game::player->GetParentCell() : nullptr;
	const auto cellID = cell ? cell->GetFormID() : 0;
	const bool active = ready && !transitionOpen && !globals::game::isVR &&
		!globals::state->isMapMenuOpen && cell && cell->IsInteriorCell();
	if (resetRequested || cellID != preparedCellID || active != preparedActive) {
		ResetPreparedScene();
		preparedCellID = cellID;
		preparedActive = active;
		resetRequested = false;
	}
	if (!active) return;
	preparedAcceleration.Advance();
	upgradeAcceleration.Advance();
	const auto backend = Feature::FindGraphicsBackend();
	if (!backend || !backend->commandQueue) return;
	const auto* source = cell->GetFile(0);
	if (!source) return;
	const auto local = cell->GetLocalFormID();
	const auto key = std::format("{:06X}:{}", local, source->GetFilename());
	const auto root = std::filesystem::path("Data/SKSE/Plugins/CommunityShaders/RedstoneScene/cells") /
		source->GetFilename() / std::format("{:06X}", local);
	if (preparedPackageRoot.empty()) preparedPackageRoot = root;
	const auto materialBudget = settings.UsePreparedMaterials ? uint64_t(settings.MaterialTextureBudgetMB) * 1024 * 1024 : 0;

	const auto references = [&](const DX12Native::PreparedCell& prepared) {
		auto resolve = [](const json& object, const char* field) {
			const auto entry = object.find(field);
			if (entry == object.end() || !entry->is_string()) return 0u;
			const auto& name = entry->get_ref<const std::string&>();
			const auto separator = name.find(':');
			return separator == std::string::npos ? 0u :
				Util::SpidToFormId("0x" + name.substr(0, separator) + "~" + name.substr(separator + 1));
		};
		std::vector<std::array<uint32_t, 2>> lights;
		std::array<uint32_t, 128> rooms{};
		for (const auto& definition : prepared.lightDefinitions) {
			std::array<uint32_t, 2> identity{};
			const auto referenceID = resolve(definition, "referenceKey");
			const auto baseline = definition.find("baseline");
			const auto baseID = baseline != definition.end() && baseline->is_object() ? resolve(*baseline, "baseKey") : 0u;
			if (auto* reference = RE::TESForm::LookupByID<RE::TESObjectREFR>(referenceID)) {
				const auto* base = reference->GetObjectReference();
				if (base && baseID && base->GetFormID() == baseID &&
					base->GetFormType() == RE::FormType::Light && reference->GetParentCell() == cell)
					identity = { referenceID, baseID };
			}
			lights.push_back(identity);
		}
		for (uint32_t room = 0; room < prepared.roomDefinitions.size() && room < rooms.size(); ++room)
			rooms[room] = resolve(prepared.roomDefinitions[room], "referenceKey");
		return std::pair{ std::move(lights), rooms };
	};

	if (!preparedScene.Requested()) {
		const auto q0 = root / "q0";
		std::error_code error;
		const auto directory = std::filesystem::exists(q0 / "cell-layout.json", error) ? q0 : root;
		publishedPackageDirectory = directory;
		preparedScene.Request(*backend, directory, key, preparedRevision, a_budgetBytes, materialBudget, 0);
	}
	if (!preparedSubmitted) {
		if (auto prepared = preparedScene.GetCell()) {
			auto [lights, rooms] = references(*prepared);
			preparedLightReferences = std::move(lights);
			preparedRoomReferences = rooms;
			preparedAcceleration.Start(*backend, prepared->snapshot, prepared->geometries, a_budgetBytes);
			preparedSubmitted = true;
		}
	}
	if (!preparedSubmitted || !preparedAcceleration.GetScene()) return;
	if (cachedRevision != preparedRevision && !publishedPackageDirectory.empty()) {
		if (auto validated = preparedScene.GetCell()) {
			cachedRevision = preparedRevision;
			CacheAcceptedScene(publishedPackageDirectory, std::string(source->GetFilename()), local, key,
				publishedStage, validated->digest, runtimeWorldspaceID.load(std::memory_order_relaxed));
		}
	}

	// Independent candidate: Q0 continues to render while Q1/Q2 file decoding,
	// upload and fenced BLAS/TLAS building take place on the workers.
	if (publishedStage >= 2) return;
	const auto targetStage = publishedStage + 1;
	const auto candidateDirectory = root / std::format("q{}", targetStage);
	const auto manifest = candidateDirectory / "cell-layout.json";
	// Do not query the filesystem on every rendered frame.
	if (!upgradeScene.Requested() && a_frame - lastUpgradePollFrame < 120) return;
	std::error_code error;
	const bool exists = std::filesystem::exists(manifest, error);
	if (error || !exists) {
		if (!upgradeScene.Requested()) lastUpgradePollFrame = a_frame;
		return;
	}
	const auto modified = std::filesystem::last_write_time(manifest, error);
	if (error) return;
	if (upgradeScene.Requested()) {
		if (modified != upgradeManifestTime && !upgradeSubmitted &&
			!upgradeScene.GetDiagnostics().value("loading", false)) {
			upgradeScene.Reset();
			upgradeAcceleration.Reset();
			upgradeFailure.clear();
		}
	}
	if (!upgradeScene.Requested()) {
		upgradeRevision = preparedRevision + 1;
		upgradeStage = targetStage;
		upgradeManifestTime = modified;
		lastUpgradePollFrame = a_frame;
		upgradeScene.Request(*backend, candidateDirectory, key, upgradeRevision,
			a_budgetBytes, materialBudget, upgradeStage);
	}
	auto candidate = upgradeScene.GetCell();
	if (!candidate) {
		const auto diag = upgradeScene.GetDiagnostics();
		if (!diag.value("loading", false)) upgradeFailure = diag.value("failure", std::string{});
		return;
	}
	if (!upgradeSubmitted) {
		upgradeAcceleration.Start(*backend, candidate->snapshot, candidate->geometries, a_budgetBytes);
		upgradeSubmitted = true;
	}
	upgradeAcceleration.Advance();
	if (auto readyUpgrade = upgradeAcceleration.GetScene();
		readyUpgrade && readyUpgrade->scope.scene == upgradeRevision) {
		auto [lights, rooms] = references(*candidate);
		// The published GPU scene is not altered until the new worker reports
		// its fenced, fully built TLAS. Prior shared owners remain valid.
		std::swap(preparedScene, upgradeScene);
		std::swap(preparedAcceleration, upgradeAcceleration);
		preparedLightReferences = std::move(lights);
		preparedRoomReferences = rooms;
		preparedRevision = upgradeRevision;
		publishedStage = upgradeStage;
		publishedPackageDirectory = candidateDirectory;
		upgradeScene.Reset();
		upgradeAcceleration.Reset();
		upgradeSubmitted = false;
		upgradeRevision = 0;
		upgradeFailure.clear();
		upgradeManifestTime = {};
		preparedTrace.Reset(true);
	} else if (upgradeAcceleration.Finished()) {
		upgradeFailure = upgradeAcceleration.GetDiagnostics().value("failure", std::string{});
	}
}

bool DX12NativeScene::WantsOpaqueRoomMetadata() const
{
	return ready && settings.UsePreparedScene && preparedActive && !transitionOpen && !globals::game::isVR && !globals::state->isMapMenuOpen;
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

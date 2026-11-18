#include <catch2/catch_test_macros.hpp>

#include "Features/NativePBR/NativeSnowObservations.h"
#include "Features/NativePBR/NativeSnowPublication.h"
#include "Features/NativePBR/NativeSnowConstantCapture.h"
#include "Features/NativePBR/NativeSnowTextureResidency.h"
#include "Features/DX12NativeScene/LiveLooseTexturePlan.h"
#include <limits>

TEST_CASE("Native snow reads reflected CPU constants without a GPU readback", "[nativepbr][snow]")
{
	std::array<float, 64> data{};
	std::array<uint8_t, 20> offsets{};
	offsets[6] = 40;
	for (size_t index = 0; index < 12; ++index)
		data[40 + index] = float(index + 1);
	const auto projection = NativePBR::ReadSnowShaderConstant<12>(data, offsets, 6);
	REQUIRE(projection);
	CHECK((*projection)[8] == 9.0f);
	CHECK_FALSE(NativePBR::ReadSnowShaderConstant<12>(data, offsets, 5));
	CHECK_FALSE(NativePBR::ReadSnowShaderConstant<12>(std::span<const float>(data).first(50), offsets, 6));
	CHECK_FALSE(NativePBR::ReadSnowShaderConstant<12>(data, offsets, 21));
	data[41] = std::numeric_limits<float>::quiet_NaN();
	CHECK_FALSE(NativePBR::ReadSnowShaderConstant<12>(data, offsets, 6));
}

TEST_CASE("Native snow projection observations separate camera frames from same-frame ambiguity", "[nativepbr][snow]")
{
	using namespace NativePBR;
	NativeSnowObservations observations;
	REQUIRE(observations.Observe("Ref", "Shape", kProjectedUvShaderFlag, kProjectedUvNifFlag));
	ObservedSnowProjection projection;
	projection.frame = 50;
	projection.coordinateSpace = SnowProjectionSpace::Object;
	projection.direction = std::array<float, 3>{ 0.0f, 0.0f, 1.0f };
	projection.projectedUVParams = std::array<float, 4>{ 1.0f, 2.0f, 3.0f, 4.0f };
	REQUIRE(observations.ObserveProjection("Ref", "Shape", projection));
	CHECK_FALSE(observations.ObserveProjection("ref", "shape", projection));
	const auto identity = observations.ExportIfIdentityChanged(0);
	REQUIRE(identity);
	projection.frame = 51;
	CHECK_FALSE(observations.ObserveProjection("Ref", "Shape", projection));
	CHECK_FALSE(observations.ExportIfIdentityChanged(identity->identityRevision));
	projection.projectedUVParams->at(0) = 2.0f;
	REQUIRE(observations.ObserveProjection("Ref", "Shape", projection));
	CHECK_FALSE(observations.ExportIfIdentityChanged(identity->identityRevision));
	const auto snapshot = observations.ExportIfChanged(0);
	REQUIRE(snapshot);
	CHECK_FALSE(snapshot->entries.begin()->second.conflictingPasses);
	CHECK(snapshot->entries.begin()->second.projection->projectedUVParams->at(0) == 2.0f);
	projection.projectedUVParams->at(0) = 3.0f;
	REQUIRE(observations.ObserveProjection("Ref", "Shape", projection));
	const auto ambiguous = observations.ExportIfChanged(snapshot->revision);
	REQUIRE(ambiguous);
	CHECK(ambiguous->entries.begin()->second.conflictingPasses);
}

TEST_CASE("Native snow coverage readiness requires an original projection direction and parameters", "[nativepbr][snow]")
{
	using namespace NativePBR;
	NativeSnowProjection projection;
	CHECK_FALSE(SnowProjectionCoverageReady(projection));
	projection.coordinateSpace = SnowProjectionSpace::Object;
	projection.direction = std::array<float, 3>{ 0.0f, 0.0f, 1.0f };
	projection.textureProjection = std::array<float, 12>{};
	projection.projectedUVParams = std::array<float, 4>{};
	projection.projectedUVParams2 = std::array<float, 4>{};
	CHECK_FALSE(SnowProjectionCoverageReady(projection));
	projection.projectedUVParams3 = std::array<float, 4>{};
	CHECK(SnowProjectionCoverageReady(projection));
	CHECK(ClassifySnowInput(SnowProjected, true, false, true, true) == SnowInputStatus::Observed);
	CHECK(ClassifySnowInput(SnowProjected, true, false, true, false) == SnowInputStatus::UnresolvedProjection);
}

TEST_CASE("Native snow separates NIF and original shader-state authority", "[nativepbr][snow]")
{
	using namespace NativePBR;
	SnowSourceState source;
	CHECK(ClassifySnow(source) == 0);
	source.renderPassDescriptor = 0;
	CHECK(ClassifySnow(source) == 0);
	source.renderPassDescriptor = kSnowShaderFlag;
	CHECK(ClassifySnow(source) == SnowOrdinary);
	source.renderPassDescriptor = kBaseObjectSnowShaderFlag;
	CHECK(ClassifySnow(source) == SnowBaseObject);
	source.renderPassDescriptor = kSnowShaderFlag | kBaseObjectSnowShaderFlag;
	CHECK(ClassifySnow(source) == (SnowOrdinary | SnowBaseObject));
	source.renderPassDescriptor = kProjectedUvShaderFlag;
	CHECK(ClassifySnow(source) == SnowProjected);
	source.nifFlags = kSnowNifFlag;
	CHECK(ClassifySnow(source) == (SnowOrdinary | SnowProjected));
	source.nifFlags = kLandscapeNifFlag;
	CHECK(ClassifySnow(source) == (SnowLandscape | SnowProjected));
	CHECK(SnowPassesAgree(kSnowShaderFlag, kSnowShaderFlag | (1u << 9)));
	CHECK_FALSE(SnowPassesAgree(kSnowShaderFlag, kBaseObjectSnowShaderFlag));
	SnowSourceState observed;
	observed.runtimeNifFlags = kSnowNifFlag;
	const auto consistent = BindSnowSource(kSnowNifFlag, observed);
	CHECK_FALSE(consistent.conflictingPasses);
	CHECK(consistent.nifFlags == kSnowNifFlag);
	const auto stale = BindSnowSource(kProjectedUvNifFlag, observed);
	CHECK(stale.conflictingPasses);
	CHECK(stale.nifFlags == kProjectedUvNifFlag);
}

TEST_CASE("Native snow observations retain conflicts and reset scene generations", "[nativepbr][snow]")
{
	using namespace NativePBR;
	NativeSnowObservations observed;
	REQUIRE(observed.Observe("123:Skyrim.esm", "SnowCliff", kSnowShaderFlag, kSnowNifFlag));
	auto first = observed.ExportIfChanged(0);
	REQUIRE(first);
	CHECK(first->entries.size() == 1);
	CHECK_FALSE(first->entries.begin()->second.conflictingPasses);
	CHECK_FALSE(observed.Observe("123:skyrim.ESM", "snowcliff", kSnowShaderFlag | (1u << 9), kSnowNifFlag));
	CHECK_FALSE(observed.ExportIfChanged(first->revision));
	REQUIRE(observed.Observe("123:Skyrim.esm", "SnowCliff", kProjectedUvShaderFlag, kSnowNifFlag));
	auto second = observed.ExportIfChanged(first->revision);
	REQUIRE(second);
	CHECK(second->entries.begin()->second.conflictingPasses);
	CHECK_FALSE(first->entries.begin()->second.conflictingPasses);
	observed.Reset();
	auto after = observed.ExportIfChanged(second->revision);
	REQUIRE(after);
	CHECK(after->entries.empty());
	CHECK(after->epoch == second->epoch + 1);
}

TEST_CASE("Native snow texture observations accept verified paths without replacing conflicting sources", "[nativepbr][snow]")
{
	using namespace NativePBR;
	NativeSnowObservations observations;
	REQUIRE(observations.Observe("Ref", "Shape", kProjectedUvShaderFlag, kProjectedUvNifFlag));
	std::array<ObservedSnowTexture, kSnowProjectedTextureCount> textures;
	textures[0] = { "textures/snow/diffuse.dds", true };
	textures[1] = { "textures/snow/normal.dds", false };
	ObservedSnowSampler sampler;
	sampler.filter = 21;
	sampler.addressU = 1;
	textures[0].sampler = sampler;
	CHECK(observations.BeginProjectedTextureProbe("ref", "shape", 100));
	CHECK_FALSE(observations.BeginProjectedTextureProbe("Ref", "Shape", 101));
	CHECK(observations.BeginProjectedTextureProbe("Ref", "Shape", 280));
	REQUIRE(observations.ObserveProjectedTextures("Ref", "Shape", textures));
	auto snapshot = observations.ExportIfChanged(0);
	REQUIRE(snapshot);
	CHECK(snapshot->entries.begin()->second.projectedTextures[0].canonicalPath == "textures/snow/diffuse.dds");
	CHECK(snapshot->entries.begin()->second.projectedTextures[0].sampler->filter == 21);
	CHECK_FALSE(snapshot->entries.begin()->second.conflictingPasses);
	textures[0].canonicalPath = "textures/snow/other.dds";
	REQUIRE(observations.ObserveProjectedTextures("Ref", "Shape", textures));
	auto conflicting = observations.ExportIfChanged(snapshot->revision);
	REQUIRE(conflicting);
	CHECK(conflicting->entries.begin()->second.conflictingPasses);
	CHECK(conflicting->entries.begin()->second.projectedTextures[0].canonicalPath == "textures/snow/diffuse.dds");
	CHECK(conflicting->entries.begin()->second.projectedTextures[0].sampler->addressU == 1);
}

TEST_CASE("Native snow publications pin scene identity and separate unresolved projection", "[nativepbr][snow]")
{
	using namespace NativePBR;
	auto scene = std::make_shared<DX12Native::TopLevel>();
	auto prepared = std::make_shared<PreparedSceneMaterials>();
	prepared->geometry = scene;
	prepared->cellID = 10;
	prepared->sceneRevision = 20;
	prepared->materialSerial = 30;
	prepared->instanceEpoch = 40;
	for (uint32_t index = 0; index < 4; ++index) {
		DX12Native::Instance instance;
		instance.identity = index + 100;
		scene->instances.push_back(instance);
		PreparedSceneMaterialBinding binding;
		binding.instanceIndex = index;
		binding.geometryIdentity = instance.identity;
		auto recipe = std::make_shared<Recipe>();
		recipe->identity = 5;
		recipe->revision = 7;
		recipe->parameters.textureGeneration = 9;
		binding.material.recipe = recipe;
		binding.material.status = MaterialLookupStatus::Available;
		prepared->instances.push_back(binding);
	}
	prepared->instances[0].material.instanceProperties.snow.nifFlags = kSnowNifFlag;
	prepared->instances[1].material.instanceProperties.snow.renderPassDescriptor = kProjectedUvShaderFlag;
	prepared->instances[1].material.instanceProperties.snow.runtimeNifFlags = kProjectedUvNifFlag;
	prepared->instances[2].material.instanceProperties.snow.nifFlags = kLandscapeNifFlag;
	prepared->instances[3].material.instanceProperties.snow.nifFlags = kSnowNifFlag;
	prepared->instances[3].material.instanceProperties.snow.conflictingPasses = true;
	const auto published = PrepareNativeSnowPublication(prepared);
	REQUIRE(published);
	CHECK(published->version == kNativeSnowPublicationVersion);
	CHECK(published->sceneRevision == 20);
	CHECK(published->instances.size() == 4);
	CHECK(published->ordinary == 2);
	CHECK(published->projected == 1);
	CHECK(published->landscape == 1);
	CHECK(published->incompleteProjection == 1);
	CHECK(published->ambiguous == 1);
	CHECK(published->instances[0].status == SnowInputStatus::NifOnly);
	CHECK(published->instances[1].status == SnowInputStatus::UnresolvedProjection);
	CHECK_FALSE(published->instances[1].projection.direction);
	CHECK(published->instances[1].projection.textures[0].textureSlot == 3);
	CHECK(published->instances[1].projection.textures[3].samplerSlot == 11);
	CHECK(published->instances[2].status == SnowInputStatus::LandscapeMetadataOnly);
	CHECK_FALSE(published->instances[2].landscape.layers[0]);
	CHECK(published->instances[3].status == SnowInputStatus::AmbiguousInstance);
	CHECK(published->instances[0].materialRevision == 7);
	CHECK(published->instances[0].textureGeneration == 9);
	CHECK(published->prepared == prepared);
	CHECK(published->Matches(*prepared));
	auto newerGeneration = std::make_shared<PreparedSceneMaterials>(*prepared);
	newerGeneration->materialSerial++;
	CHECK_FALSE(published->Matches(*newerGeneration));
	auto mismatched = std::make_shared<PreparedSceneMaterials>(*prepared);
	mismatched->instances[0].geometryIdentity = 999;
	CHECK_FALSE(PrepareNativeSnowPublication(mismatched));
	CHECK(published->instances[0].geometryIdentity == 100);
}

TEST_CASE("Native snow DDS lookup uses existing page descriptors without allocating copies", "[nativepbr][snow]")
{
	using namespace NativePBR;
	SnowTextureReference texture;
	texture.canonicalPath = "textures/snow/surface.dds";
	texture.srgb = true;
	const std::unordered_map<std::string, uint64_t> sources{ { "textures/snow/surface.dds", 123 } };
	const std::unordered_map<uint64_t, std::array<uint32_t, 2>> logical{ { 123, { 5, 9 } } };
	const auto color = FindSnowTextureLocation(texture, sources, logical, 6);
	REQUIRE(color);
	CHECK(color->at(0) == 5 + kGpuTexturePoolCapacity);
	CHECK(color->at(1) == 9);
	texture.srgb = false;
	const auto linear = FindSnowTextureLocation(texture, sources, logical, 6);
	REQUIRE(linear);
	CHECK(linear->at(0) == 5);
	texture.srgb.reset();
	CHECK_FALSE(FindSnowTextureLocation(texture, sources, logical, 6));
	texture.srgb = true;
	CHECK_FALSE(FindSnowTextureLocation(texture, sources, logical, 5));
	texture.canonicalPath = "textures/snow/missing.dds";
	CHECK_FALSE(FindSnowTextureLocation(texture, sources, logical, 6));
}

TEST_CASE("Native snow residency rejects wrong material generations and retains unresolved roles", "[nativepbr][snow]")
{
	using namespace NativePBR;
	auto recipes = std::make_shared<RecipeGeneration>();
	auto materials = std::make_shared<MaterialPublication>();
	materials->recipes = recipes;
	auto prepared = std::make_shared<PreparedSceneMaterials>();
	prepared->materialPublication = materials;
	auto publication = std::make_shared<NativeSnowPublication>();
	publication->prepared = prepared;
	NativeSnowInstance projected;
	projected.classification = SnowProjected;
	projected.projection.textures[0].canonicalPath = "textures/snow/diffuse.dds";
	projected.projection.textures[0].srgb = true;
	projected.projection.textures[0].status = SnowTextureStatus::IdentifiedPath;
	publication->instances.push_back(projected);
	auto gpu = std::make_shared<GpuMaterialResources>();
	gpu->generation = std::make_shared<RecipeGeneration>();
	CHECK(BindNativeSnowTextureResidency(publication, gpu) == publication);
	gpu->generation = recipes;
	const auto bound = BindNativeSnowTextureResidency(publication, gpu);
	REQUIRE(bound);
	CHECK(bound != publication);
	CHECK(bound->gpuMaterials == gpu);
	CHECK(bound->instances[0].projection.textures[0].status == SnowTextureStatus::Unavailable);
	CHECK(bound->projectedTexturesResolved == 0);
	CHECK(bound->projectedTexturesUnresolved == 4);
	CHECK(bound->textureSetsUnavailable == 1);
	CHECK(publication->instances[0].projection.textures[0].status == SnowTextureStatus::IdentifiedPath);
	CHECK_FALSE(GpuMaterialRetirement::Completed(10, 11));
	CHECK(GpuMaterialRetirement::Completed(11, 11));
}

TEST_CASE("Native snow keeps projected observations when a material recipe is unavailable", "[nativepbr][snow]")
{
	using namespace NativePBR;
	NativeSnowObservations observations;
	REQUIRE(observations.Observe("123:Skyrim.esm", "Cliff", kProjectedUvShaderFlag, kProjectedUvNifFlag));
	std::array<ObservedSnowTexture, kSnowProjectedTextureCount> textures;
	textures[0] = { "textures/snow/diffuse.dds", true };
	REQUIRE(observations.ObserveProjectedTextures("123:Skyrim.esm", "Cliff", textures));
	auto scene = std::make_shared<DX12Native::TopLevel>();
	DX12Native::Instance instance;
	instance.identity = 77;
	instance.preparedIdentity.referenceKey = "123:Skyrim.esm";
	instance.preparedIdentity.shapeName = "Cliff";
	scene->instances.push_back(instance);
	auto prepared = std::make_shared<PreparedSceneMaterials>();
	prepared->geometry = scene;
	prepared->instances.push_back({ 0, 77, { MaterialLookupStatus::MissingInstance } });
	const auto published = PrepareNativeSnowPublication(prepared, observations.ExportIfChanged(0));
	REQUIRE(published);
	REQUIRE(published->instances.size() == 1);
	CHECK(published->instances[0].classification == SnowProjected);
	CHECK(published->instances[0].status == SnowInputStatus::MissingMaterial);
	CHECK(published->instances[0].projection.textures[0].status == SnowTextureStatus::IdentifiedPath);
	CHECK(published->instances[0].projection.textures[0].canonicalPath == "textures/snow/diffuse.dds");
	CHECK(published->projectedTexturesUnresolved == 4);
}

TEST_CASE("Native snow DDS requests share the existing bounded worker plan", "[nativepbr][snow]")
{
	DX12Native::LiveLooseTexturePlan plan;
	plan.paths = { "textures/material/base.dds" };
	DX12Native::AppendNativeTextureRequests(plan,
		{ "textures/snow/normal.dds", "textures/material/base.dds", "textures/snow/noise.dds",
			"../escape.dds", "textures/snow/normal.dds" },
		{ "textures/snow/noise.dds" }, {}, 2);
	REQUIRE(plan.paths.size() == 2);
	CHECK(plan.paths[1] == "textures/snow/normal.dds");
	CHECK(plan.alreadyPresent == 1);
	CHECK(plan.invalid == 1);
	CHECK(plan.deferred == 0);
	DX12Native::AppendNativeTextureRequests(plan,
		{ "textures/snow/detail.dds" }, {}, {}, 2);
	CHECK(plan.deferred == 1);
}

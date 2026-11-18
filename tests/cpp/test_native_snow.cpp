#include <catch2/catch_test_macros.hpp>

#include "Features/NativePBR/NativeSnowObservations.h"
#include "Features/NativePBR/NativeSnowPublication.h"

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

#include "MaterialCatalog.h"

#include "Materials/PBRRecordConfig.h"

const PBRRecords::TextureSetData* NativePBR::MaterialCatalogSnapshot::FindTextureSet(std::string_view a_editorId) const
{
	auto it = textureSets.find(std::string(a_editorId));
	return it == textureSets.end() ? nullptr : &it->second;
}

const PBRRecords::MaterialObjectData* NativePBR::MaterialCatalogSnapshot::FindMaterialObject(std::string_view a_editorId) const
{
	auto it = materialObjects.find(std::string(a_editorId));
	return it == materialObjects.end() ? nullptr : &it->second;
}

void NativePBR::MaterialCatalog::Load(bool a_verbose)
{
	auto catalog = std::make_shared<MaterialCatalogSnapshot>();
	PBRRecords::ReadConfigs("Data\\PBRTextureSets", [catalog](const std::string& a_editorId, const json& a_config) {
		try {
			catalog->textureSets.insert_or_assign(a_editorId, a_config.get<PBRRecords::TextureSetData>());
			return true;
		} catch (const std::exception& e) {
			++catalog->rejectedTextureSets;
			logger::warn("[NativePBR] texture set {} rejected: {}", a_editorId, e.what());
			return false;
		}
	}, a_verbose, "NativePBR");

	PBRRecords::ReadConfigs("Data\\PBRMaterialObjects", [catalog](const std::string& a_editorId, const json& a_config) {
		try {
			catalog->materialObjects.insert_or_assign(a_editorId, a_config.get<PBRRecords::MaterialObjectData>());
			return true;
		} catch (const std::exception& e) {
			++catalog->rejectedMaterialObjects;
			logger::warn("[NativePBR] material object {} rejected: {}", a_editorId, e.what());
			return false;
		}
	}, a_verbose, "NativePBR");

	published.store(std::move(catalog), std::memory_order_release);
}

std::shared_ptr<const NativePBR::MaterialCatalogSnapshot> NativePBR::MaterialCatalog::GetSnapshot() const
{
	return published.load(std::memory_order_acquire);
}

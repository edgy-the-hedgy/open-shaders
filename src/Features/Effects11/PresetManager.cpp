#include "PresetManager.h"

<<<<<<< HEAD
#include <Windows.h>
#include <fstream>

#include "Utils/FileSystem.h"

namespace
{
	struct KnownEffect
	{
		const char* flagName;
		const char* fileName;
	};

	// The optional ENB effects Effects11 actually loads (see EffectManager.h); enbeffect.fx
	// is unconditional and has no matching ini flag, so it's excluded here.
	constexpr KnownEffect kKnownEffects[] = {
		{ "EnableBloom", "enbbloom.fx" },
		{ "EnableLens", "enblens.fx" },
		{ "EnableAdaptation", "enbadaptation.fx" },
		{ "EnablePostPassShader", "enbeffectpostpass.fx" },
	};

	bool ReadIniBool(const std::string& a_iniPath, const char* a_key)
	{
		char buffer[16];
		GetPrivateProfileStringA("EFFECT", a_key, "false", buffer, sizeof(buffer), a_iniPath.c_str());
		return _stricmp(buffer, "true") == 0;
	}

	// Reproduces enbeffect.fx's leading comment block verbatim (minus pure border
	// decoration), handling both "// line" and "/* ... */" block style. Empty if the
	// file has no such block.
	std::string ScrapeHeaderComment(const std::filesystem::path& a_enbeffectPath)
	{
		std::ifstream file(a_enbeffectPath);
		if (!file)
			return {};

		std::string result;
		std::string line;
		bool insideBlockComment = false;
		int contentLines = 0;
		for (int i = 0; i < 40 && contentLines < 10 && std::getline(file, line); ++i) {
			const auto firstNonSpace = line.find_first_not_of(" \t\r");
			const bool blank = firstNonSpace == std::string::npos;

			if (!insideBlockComment) {
				if (blank)
					continue;  // gap between comment paragraphs -- keep scanning
				const bool isLineComment = line.compare(firstNonSpace, 2, "//") == 0;
				const bool blockOpens = line.compare(firstNonSpace, 2, "/*") == 0;
				if (!isLineComment && !blockOpens)
					break;  // a real (non-comment) code line ends the header block
				if (blockOpens)
					insideBlockComment = true;
			} else if (line.find("*/") != std::string::npos) {
				insideBlockComment = false;
			}

			std::string content = blank ? std::string{} : line.substr(firstNonSpace);

			// Strip only a leading/trailing comment marker -- never scan-and-remove "//"
			// mid-line, or a "http://" inside the text gets mangled.
			if (content.rfind("//", 0) == 0 || content.rfind("/*", 0) == 0)
				content.erase(0, 2);
			else if (!content.empty() && content.front() == '*')
				content.erase(0, 1);

			auto trailEnd = content.find_last_not_of(" \t\r");
			while (trailEnd != std::string::npos && trailEnd >= 1 &&
				   ((content[trailEnd - 1] == '*' && content[trailEnd] == '/') ||
					   (content[trailEnd - 1] == '/' && content[trailEnd] == '/'))) {
				content.erase(trailEnd - 1);
				trailEnd = content.find_last_not_of(" \t\r");
			}

			auto start = content.find_first_not_of(" \t");
			if (start == std::string::npos)
				continue;
			auto end = content.find_last_not_of(" \t\r");
			content = content.substr(start, end - start + 1);

			if (content.find_first_not_of("+-=*/ \t") == std::string::npos)
				continue;  // pure border decoration, not content

			result += (result.empty() ? "" : "\n") + content;
			++contentLines;
		}
		return result;
	}

	void PopulateSummary(PresetLocation& a_location)
	{
		const auto enbseriesDir = a_location.root / "enbseries";
		const auto iniPathStr = (a_location.root / "enbseries.ini").string();

		for (const auto& known : kKnownEffects) {
			a_location.effectStatus.push_back({ known.flagName,
				ReadIniBool(iniPathStr, known.flagName),
				Util::PathHelpers::SafeExists(enbseriesDir / known.fileName) });
		}

		a_location.headerComment = ScrapeHeaderComment(enbseriesDir / "enbeffect.fx");
	}
}

||||||| 12b261724
=======
#include "EffectManager.h"
#include "Presets/UnifiedPresetCatalog.h"
#include "SettingManager.h"
#include "Utils/FileSystem.h"

#include <algorithm>
#include <format>

>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
PresetManager& PresetManager::GetSingleton()
{
	static PresetManager instance;
	return instance;
}

<<<<<<< HEAD
void PresetManager::Rescan()
||||||| 12b261724
bool PresetManager::UseDataFolder() const
=======
std::string PresetManager::FormatPresetIdForLog(const std::string& id)
{
	return id.empty() ? "Legacy" : id;
}

std::string PresetManager::MakeUnifiedPackPresetId(const std::string& packId)
{
	return std::string(kUnifiedPackIdPrefix) + packId;
}

bool PresetManager::UseDataFolder() const
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
{
<<<<<<< HEAD
	discoveredLocations.clear();

	// GetDataPath() resolves against the game exe's own path (falling back to CWD only
	// if that fails), unlike a bare absolute("Data") which trusts CWD outright -- those
	// can differ depending on how the game was launched.
	const auto dataRoot = Util::PathHelpers::GetDataPath();
	if (Util::PathHelpers::SafeExists(dataRoot / "enbseries.ini") && Util::PathHelpers::SafeIsDirectory(dataRoot / "enbseries")) {
		discoveredLocations.push_back({ PresetLocationKind::DataRoot, dataRoot, "Data", true });
	}

	const auto gameRoot = dataRoot.parent_path();
	if (Util::PathHelpers::SafeExists(gameRoot / "enbseries.ini") && Util::PathHelpers::SafeIsDirectory(gameRoot / "enbseries")) {
		discoveredLocations.push_back({ PresetLocationKind::GameRoot, gameRoot, "Game root", true });
	}

	for (const auto& childRoot : Util::PathHelpers::SafeChildDirectories(dataRoot)) {
		if (Util::PathHelpers::SafeExists(childRoot / "enbseries.ini") && Util::PathHelpers::SafeIsDirectory(childRoot / "enbseries")) {
			discoveredLocations.push_back({ PresetLocationKind::DataSubfolder,
				childRoot,
				"Data\\" + childRoot.filename().string(),
				true });
		}
	}

	// Cheap (one ini read + a handful of stat() calls per location): safe to do inline
	// here since Rescan() only runs at Initialize() or an explicit UI/devbench action,
	// never per-frame.
	for (auto& location : discoveredLocations)
		PopulateSummary(location);

	// A rescan can drop the active preset (deleted from disk); GetENBSeriesPath() must
	// stop pointing at it once it's no longer in discoveredLocations.
	if (!activeRoot.empty() && !GetActiveLocation())
		activeRoot.clear();
}

const std::vector<PresetLocation>& PresetManager::GetDiscoveredLocations() const
{
	return discoveredLocations;
}

void PresetManager::SetActiveLocation(const std::filesystem::path& root)
{
	activeRoot = root;
}

const PresetLocation* PresetManager::GetActiveLocation() const
{
	for (const auto& loc : discoveredLocations) {
		if (loc.root == activeRoot)
			return &loc;
	}
	return nullptr;
}

std::string PresetManager::ToRelativeKey(const std::filesystem::path& a_root) const
{
	// SafeRelative falls back to returning a_root unchanged on failure; a_root is always
	// absolute here, so an absolute result means resolution failed -- treat that as no key
	// rather than persisting/matching against a path that was never actually relative.
	auto relative = Util::PathHelpers::SafeRelative(a_root, Util::PathHelpers::GetDataPath().parent_path());
	if (relative.is_absolute())
		return {};
	return relative.string();
}

const PresetLocation* PresetManager::FindByRelativeKey(const std::string& a_relativeKey) const
{
	for (const auto& loc : discoveredLocations) {
		if (ToRelativeKey(loc.root) == a_relativeKey)
			return &loc;
	}
	return nullptr;
||||||| 12b261724
	return std::filesystem::exists("Data\\enbseries.ini") && std::filesystem::is_directory("Data\\enbseries");
=======
	return IsValidEnbSeriesLayout(
		std::filesystem::path("Data") / kEnbSeriesIniName,
		std::filesystem::path("Data") / kEnbSeriesDirName);
}

bool PresetManager::IsValidEnbSeriesLayout(const std::filesystem::path& iniPath, const std::filesystem::path& seriesDir)
{
	return std::filesystem::exists(iniPath) && std::filesystem::is_directory(seriesDir);
}

bool PresetManager::HasLegacyInstall() const
{
	if (UseDataFolder())
		return true;
	return IsValidEnbSeriesLayout(kEnbSeriesIniName, kEnbSeriesDirName);
}

std::filesystem::path PresetManager::GetLegacyENBSeriesPath() const
{
	if (UseDataFolder())
		return std::filesystem::absolute(std::filesystem::path("Data") / kEnbSeriesDirName);
	return std::filesystem::absolute(kEnbSeriesDirName);
}

std::filesystem::path PresetManager::GetLegacyENBSeriesIniPath() const
{
	if (UseDataFolder())
		return std::filesystem::absolute(std::filesystem::path("Data") / kEnbSeriesIniName);
	return std::filesystem::absolute(kEnbSeriesIniName);
}

bool PresetManager::ValidateLibraryPreset(const std::filesystem::path& presetRoot, std::string& outReason)
{
	const auto iniPath = presetRoot / kEnbSeriesIniName;
	const auto seriesPath = presetRoot / kEnbSeriesDirName;

	if (!std::filesystem::exists(iniPath)) {
		outReason = std::format("Missing {}", kEnbSeriesIniName);
		return false;
	}
	if (!std::filesystem::is_directory(seriesPath)) {
		outReason = std::format("Missing {} folder", kEnbSeriesDirName);
		return false;
	}
	if (!std::filesystem::exists(seriesPath / kRequiredEffectFile)) {
		outReason = std::format("Missing required {}", kRequiredEffectFile);
		return false;
	}
	return true;
}

const PresetManager::PresetInfo* PresetManager::FindPreset(const std::string& id) const
{
	for (const auto& preset : presets) {
		if (preset.id == id)
			return &preset;
	}
	return nullptr;
}

void PresetManager::EnsureDefaultSelection()
{
	if (const auto* current = FindPreset(activePresetId); current && current->valid)
		return;
	SelectDefaultPreset();
}

void PresetManager::SelectDefaultPreset()
{
	for (const auto& preset : presets) {
		if (!preset.isLegacy && preset.valid) {
			activePresetId = preset.id;
			return;
		}
	}

	activePresetId = kLegacyPresetId;
}

void PresetManager::ScanLibraryDirectory(const std::filesystem::path& relativePath, bool unifiedPacks, std::vector<PresetInfo>& out)
{
	for (const auto& folder : Util::PathHelpers::ListCommunityShaderEntries(relativePath, true)) {
		const auto packName = Util::PathHelpers::ToUtf8(folder.filename());
		auto libraryRoot = folder;
		auto displayName = packName;

		if (unifiedPacks) {
			const auto manifest = UnifiedPresetCatalog::ReadPackManifest(folder);
			libraryRoot = UnifiedPresetCatalog::ResolveEffects11Root(folder, manifest);
			if (libraryRoot.empty())
				continue;
			if (const auto name = manifest.find(PackManifest::kNameKey); name != manifest.end() && name->is_string())
				displayName = name->get<std::string>();
		}

		PresetInfo info;
		info.id = unifiedPacks ? MakeUnifiedPackPresetId(packName) : packName;
		info.displayName = std::move(displayName);
		info.rootPath = libraryRoot;
		info.isLegacy = false;
		info.isUnifiedPack = unifiedPacks;
		info.valid = ValidateLibraryPreset(libraryRoot, info.invalidReason);
		out.push_back(std::move(info));
	}
}

void PresetManager::DiscoverPresets()
{
	presets = ScanPresets(true);
	discovered = true;
	EnsureDefaultSelection();

	logger::info("[Effects11] Discovered {} preset(s); active='{}'",
		presets.size(), FormatPresetIdForLog(activePresetId));
}

std::vector<PresetManager::PresetInfo> PresetManager::ScanPresets(bool includeUnifiedPacks) const
{
	Util::FileHelpers::EnsureDirectoryExists(Util::PathHelpers::GetEffects11PresetsRealPath());
	Util::FileHelpers::EnsureDirectoryExists(Util::PathHelpers::GetUnifiedPresetsRealPath());
	std::vector<PresetInfo> found;

	PresetInfo legacy;
	legacy.id = kLegacyPresetId;
	legacy.displayName = "Legacy (game root / Data)";
	legacy.isLegacy = true;
	legacy.valid = HasLegacyInstall();
	if (!legacy.valid)
		legacy.invalidReason = std::format("No {} + {} folder at game root or Data", kEnbSeriesIniName, kEnbSeriesDirName);
	else
		legacy.rootPath = GetLegacyENBSeriesIniPath().parent_path();
	found.push_back(std::move(legacy));

	ScanLibraryDirectory(Util::PathHelpers::kEffects11PresetsSubdir, false, found);
	if (includeUnifiedPacks)
		ScanLibraryDirectory(Util::PathHelpers::kUnifiedPresetsSubdir, true, found);

	// Keep Legacy first; sort library entries by display name.
	std::sort(found.begin() + 1, found.end(), [](const PresetInfo& a, const PresetInfo& b) {
		return a.displayName < b.displayName;
	});
	return found;
}

std::filesystem::path PresetManager::GetActiveLibraryRoot() const
{
	if (IsLegacyActive())
		return {};

	if (const auto* preset = FindPreset(activePresetId); preset && preset->valid && !preset->isLegacy)
		return preset->rootPath;

	return {};
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
}

std::filesystem::path PresetManager::GetENBSeriesPath() const
{
<<<<<<< HEAD
	if (activeRoot.empty())
		return {};
	return activeRoot / "enbseries";
||||||| 12b261724
	if (UseDataFolder())
		return std::filesystem::absolute("Data\\enbseries");
	return std::filesystem::absolute("enbseries");
=======
	if (const auto libraryRoot = GetActiveLibraryRoot(); !libraryRoot.empty())
		return std::filesystem::absolute(libraryRoot / kEnbSeriesDirName);
	return GetLegacyENBSeriesPath();
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
}

std::filesystem::path PresetManager::GetENBSeriesIniPath() const
{
<<<<<<< HEAD
	if (activeRoot.empty())
		return {};
	return activeRoot / "enbseries.ini";
||||||| 12b261724
	if (UseDataFolder())
		return std::filesystem::absolute("Data\\enbseries.ini");
	return std::filesystem::absolute("enbseries.ini");
=======
	if (const auto libraryRoot = GetActiveLibraryRoot(); !libraryRoot.empty())
		return std::filesystem::absolute(libraryRoot / kEnbSeriesIniName);
	return GetLegacyENBSeriesIniPath();
}

bool PresetManager::SetActivePreset(const std::string& id)
{
	if (!discovered)
		DiscoverPresets();

	const auto* preset = FindPreset(id);
	if (!preset) {
		logger::warn("[Effects11] Unknown preset id '{}'", FormatPresetIdForLog(id));
		return false;
	}
	if (!preset->valid) {
		logger::warn("[Effects11] Cannot activate invalid preset '{}': {}", preset->displayName, preset->invalidReason);
		return false;
	}

	activePresetId = id;
	logger::info("[Effects11] Active preset set to '{}'", FormatPresetIdForLog(id));
	return true;
}

void PresetManager::ReloadActive()
{
	// SettingManager::Load() reinitializes WeatherManager via ReloadAllWeatherSettings().
	// EffectManager::Apply() unloads/recompiles ENB FX only, not the CS shader cache.
	SettingManager::GetSingleton().Load();
	EffectManager::GetSingleton().Apply();
}

bool PresetManager::SwitchPreset(const std::string& id, bool saveCurrent)
{
	if (!discovered)
		DiscoverPresets();

	if (id == activePresetId) {
		if (const auto* current = FindPreset(activePresetId); current && current->valid)
			return true;
	}

	const auto* target = FindPreset(id);
	if (!target || !target->valid)
		return false;

	auto& settingManager = SettingManager::GetSingleton();
	auto& effectManager = EffectManager::GetSingleton();

	if (saveCurrent) {
		if (const auto* current = FindPreset(activePresetId); current && current->valid) {
			settingManager.Save();
			effectManager.Save();
		}
	}

	if (!SetActivePreset(id))
		return false;

	ReloadActive();
	return true;
}

std::filesystem::path PresetManager::GetActivePresetRoot() const
{
	return GetENBSeriesIniPath().parent_path();
}

bool PresetManager::CanExportActivePreset() const
{
	std::string reason;
	return ValidateLibraryPreset(GetActivePresetRoot(), reason);
}

bool PresetManager::ExportActivePresetTo(const std::filesystem::path& destRoot, bool saveCurrent)
{
	if (destRoot.empty()) {
		logger::error("[Effects11] Export failed: destination root is empty");
		return false;
	}

	if (!discovered)
		DiscoverPresets();

	const auto* current = FindPreset(activePresetId);
	if (!current || !current->valid) {
		logger::error("[Effects11] Export failed: active preset '{}' is missing or invalid",
			FormatPresetIdForLog(activePresetId));
		return false;
	}

	if (saveCurrent) {
		SettingManager::GetSingleton().Save();
		EffectManager::GetSingleton().Save();
	}

	const auto sourceRoot = GetActivePresetRoot();
	std::string sourceReason;
	if (!ValidateLibraryPreset(sourceRoot, sourceReason)) {
		logger::error("[Effects11] Export failed: active preset root '{}' is invalid ({})",
			Util::PathHelpers::ToUtf8(sourceRoot), sourceReason);
		return false;
	}

	std::error_code ec;
	if (std::filesystem::equivalent(sourceRoot, destRoot, ec)) {
		logger::info("[Effects11] Export skipped copy; '{}' is already the active preset root", Util::PathHelpers::ToUtf8(destRoot));
		return true;
	}

	Util::FileHelpers::EnsureDirectoryExists(destRoot);

	const auto srcIni = sourceRoot / kEnbSeriesIniName;
	const auto srcSeries = sourceRoot / kEnbSeriesDirName;
	const auto destIni = destRoot / kEnbSeriesIniName;
	const auto destSeries = destRoot / kEnbSeriesDirName;

	std::filesystem::remove(destIni, ec);
	std::filesystem::remove_all(destSeries, ec);

	std::filesystem::copy_file(srcIni, destIni, std::filesystem::copy_options::overwrite_existing, ec);
	if (ec) {
		logger::error("[Effects11] Export failed copying '{}': {}", Util::PathHelpers::ToUtf8(srcIni), ec.message());
		return false;
	}

	std::filesystem::copy(srcSeries, destSeries,
		std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, ec);
	if (ec) {
		logger::error("[Effects11] Export failed copying '{}': {}", Util::PathHelpers::ToUtf8(srcSeries), ec.message());
		return false;
	}

	// Optional companion file some ENB packs ship next to enbseries.ini.
	const auto srcLocal = sourceRoot / "enblocal.ini";
	if (std::filesystem::is_regular_file(srcLocal, ec)) {
		std::filesystem::copy_file(srcLocal, destRoot / "enblocal.ini",
			std::filesystem::copy_options::overwrite_existing, ec);
		if (ec)
			logger::warn("[Effects11] Export could not copy enblocal.ini: {}", ec.message());
	}

	std::string destReason;
	if (!ValidateLibraryPreset(destRoot, destReason)) {
		logger::error("[Effects11] Export wrote an invalid layout at '{}': {}", Util::PathHelpers::ToUtf8(destRoot), destReason);
		return false;
	}

	logger::info("[Effects11] Exported active preset '{}' to '{}'",
		FormatPresetIdForLog(activePresetId), Util::PathHelpers::ToUtf8(destRoot));
	return true;
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
}

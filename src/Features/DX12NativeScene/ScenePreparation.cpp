#include "ScenePreparation.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    using InitializeHost = int32_t(__cdecl*)(const wchar_t*, const void*, void**);
    using GetDelegate = int32_t(__cdecl*)(void*, int32_t, void**);
    using CloseHost = int32_t(__cdecl*)(void*);
    using LoadAssembly = int32_t(__cdecl*)(const wchar_t*, const wchar_t*, const wchar_t*, const wchar_t*, void*, void**);
    using PrepareEntry = int32_t(__cdecl*)(void*, int32_t);

    struct Request
    {
        const wchar_t* dataDirectory;
        const wchar_t* pluginsFile;
        const wchar_t* cacheDirectory;
        wchar_t* errorBuffer;
        int32_t errorCapacity;
        int32_t quality;
        int32_t force;
    };
    static_assert(sizeof(Request) == 48);

    std::filesystem::path GameDirectory()
    {
        std::array<wchar_t, 32768> buffer{};
        const auto count = GetModuleFileNameW(nullptr, buffer.data(), DWORD(buffer.size()));
        if (!count || count == buffer.size()) throw std::runtime_error("Game executable path unavailable");
        return std::filesystem::path(std::wstring_view(buffer.data(), count)).parent_path();
    }

    std::filesystem::path FindHostFxr()
    {
        std::vector<std::filesystem::path> roots;
        for (const auto* variable : { L"DOTNET_ROOT", L"ProgramFiles" }) {
            std::array<wchar_t, 32768> value{};
            const auto count = GetEnvironmentVariableW(variable, value.data(), DWORD(value.size()));
            if (count && count < value.size()) {
                auto root = std::filesystem::path(std::wstring_view(value.data(), count));
                roots.push_back(std::wstring_view(variable) == L"ProgramFiles" ? root / L"dotnet" : root);
            }
        }
        std::filesystem::path selected;
        std::wstring selectedVersion;
        for (const auto& root : roots) {
            std::error_code error;
            const auto directory = root / L"host" / L"fxr";
            if (!std::filesystem::is_directory(directory, error)) continue;
            for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
                if (!entry.is_directory(error)) continue;
                const auto version = entry.path().filename().wstring();
                if (!version.starts_with(L"10.")) continue;
                const auto candidate = entry.path() / L"hostfxr.dll";
                if (std::filesystem::is_regular_file(candidate, error) && version > selectedVersion) {
                    selectedVersion = version;
                    selected = candidate;
                }
            }
        }
        if (selected.empty()) throw std::runtime_error(".NET hostfxr unavailable; install the .NET 10 x64 runtime");
        return selected;
    }

    int32_t PrepareGuestRoom(uint32_t quality, bool force, std::string& failure)
    {
        const auto game = GameDirectory();
        const auto data = (game / L"Data").wstring();
        const auto assemblyDirectory = game / L"Data" / L"SKSE" / L"Plugins" / L"CommunityShaders" / L"NativeScenePrep";
        const auto assembly = (assemblyDirectory / L"RedstoneScenePrep.dll").wstring();
        const auto runtimeConfig = (assemblyDirectory / L"RedstoneScenePrep.runtimeconfig.json").wstring();
        if (!std::filesystem::exists(assembly) || !std::filesystem::exists(runtimeConfig))
            throw std::runtime_error("NativeScenePrep managed runtime payload missing from installed plugin");
        const auto cache = (game / L"Data" / L"SKSE" / L"Plugins" / L"CommunityShaders" /
            L"RedstoneScene" / L"cells" / L"Unslaad.esm" / L"04FD2B").wstring();
        std::array<wchar_t, 32768> appData{};
        const auto localSize = GetEnvironmentVariableW(L"LOCALAPPDATA", appData.data(), DWORD(appData.size()));
        if (!localSize || localSize >= appData.size()) throw std::runtime_error("LOCALAPPDATA unavailable");
        const auto plugins = (std::filesystem::path(std::wstring_view(appData.data(), localSize)) /
            L"Skyrim Special Edition" / L"Plugins.txt").wstring();

        const auto hostPath = FindHostFxr();
        const auto module = LoadLibraryW(hostPath.c_str());
        if (!module) throw std::runtime_error("Could not load hostfxr");
        const auto initialize = reinterpret_cast<InitializeHost>(GetProcAddress(module, "hostfxr_initialize_for_runtime_config"));
        const auto getDelegate = reinterpret_cast<GetDelegate>(GetProcAddress(module, "hostfxr_get_runtime_delegate"));
        const auto close = reinterpret_cast<CloseHost>(GetProcAddress(module, "hostfxr_close"));
        if (!initialize || !getDelegate || !close) {
            FreeLibrary(module);
            throw std::runtime_error("hostfxr API unavailable");
        }
        void* context = nullptr;
        void* loader = nullptr;
        const auto initialized = initialize(runtimeConfig.c_str(), nullptr, &context);
        if (initialized < 0 || !context) {
            FreeLibrary(module);
            throw std::runtime_error("Could not initialize the .NET 10 scene preparation runtime");
        }
        const auto delegateResult = getDelegate(context, 3, &loader);
        close(context);
        if (delegateResult < 0 || !loader) {
            FreeLibrary(module);
            throw std::runtime_error("Could not acquire .NET assembly loader");
        }
        void* entry = nullptr;
        const auto load = reinterpret_cast<LoadAssembly>(loader);
        const auto loadResult = load(assembly.c_str(), L"RuntimePreparationBridge, RedstoneScenePrep",
            L"Prepare", reinterpret_cast<const wchar_t*>(static_cast<intptr_t>(-1)), nullptr, &entry);
        if (loadResult < 0 || !entry) {
            FreeLibrary(module);
            throw std::runtime_error("Could not resolve managed preparation entry point");
        }
        std::array<wchar_t, 1024> details{};
        Request request{ data.c_str(), plugins.c_str(), cache.c_str(), details.data(),
            int32_t(details.size()), int32_t(quality), force ? 1 : 0 };
        const auto result = reinterpret_cast<PrepareEntry>(entry)(&request, sizeof(request));
        FreeLibrary(module);
        if (result < 0) {
            failure = std::filesystem::path(details.data()).string();
            if (failure.empty()) failure = "Managed scene preparation failed";
        }
        return result;
    }
}

void NativeScenePreparation::Advance(bool inGuestRoom, bool autoEnabled, uint32_t quality)
{
    auto owned = state;
    bool rebuild = false;
    {
        std::lock_guard lock(owned->mutex);
        if (!inGuestRoom) {
            owned->wasInRoom = false;
            return;
        }
        if (!autoEnabled && !owned->pendingRebuild) return;
        if (owned->status.busy) return;
        if (owned->wasInRoom && owned->status.quality == quality && !owned->pendingRebuild) return;
        rebuild = owned->pendingRebuild;
        owned->pendingRebuild = false;
        owned->wasInRoom = true;
        owned->status.busy = true;
        owned->status.attempted = false;
        owned->status.quality = quality;
        owned->status.failure.clear();
    }
    try {
        static BS::thread_pool<> pool{ 1 };
        pool.detach_task([owned, quality, rebuild] {
            std::string failure;
            int32_t result = -1;
            try { result = PrepareGuestRoom(quality, rebuild, failure); }
            catch (const std::exception& error) { failure = error.what(); }
            std::lock_guard lock(owned->mutex);
            owned->status.busy = false;
            owned->status.attempted = true;
            owned->status.succeeded = result >= 0;
            owned->status.failure = std::move(failure);
            if (result == 1) ++owned->status.generation;
        });
    } catch (const std::exception& error) {
        std::lock_guard lock(owned->mutex);
        owned->status.busy = false;
        owned->status.attempted = true;
        owned->status.succeeded = false;
        owned->status.failure = error.what();
    }
}

void NativeScenePreparation::RequestRebuild()
{
    std::lock_guard lock(state->mutex);
    state->pendingRebuild = true;
}

NativeScenePreparation::Status NativeScenePreparation::GetStatus() const
{
    std::lock_guard lock(state->mutex);
    return state->status;
}

bool NativeScenePreparation::CanLoad() const
{
    std::lock_guard lock(state->mutex);
    return state->status.attempted && !state->status.busy;
}

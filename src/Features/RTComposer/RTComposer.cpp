#include "RTComposer.h"

#include "Features/DX12NativeScene/DX12NativeScene.h"
#include "Features/RedstoneGI/RedstoneGI.h"
#include "Features/RTDirectLighting/RTDirectLighting.h"
#include "Features/RTIndirectLighting/RTIndirectLighting.h"
#include "Globals.h"
#include "Utils/DevBenchUx.h"
#include "Utils/RayRegenerationSettings.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace
{
    constexpr std::array kDirectKeys{ "UseDirectRayRegeneration", "DirectRRResolution", "RRMaterialIsolation", "RRGlossyRoughness" };

    void CopyMissing(json& destination, const json& source, const char* field)
    {
        if (source.is_object() && source.contains(field) && !destination.contains(field))
            destination[field] = source[field];
    }
}

std::pair<std::string, std::vector<std::string>> RTComposer::GetFeatureSummary()
{
    return { "Central Ray Regeneration controls and diagnostics for the existing direct, indirect and Hybrid pipelines.", {} };
}

void RTComposer::MigrateLegacySettings(json& root)
{
    if (!root.is_object()) return;
    auto& composer = root["RTComposer"];
    if (!composer.is_object()) composer = json::object();

    json oldRR = json::object();
    if (auto up = root.find("Upscaling"); up != root.end() && up->is_object()) {
        if (auto rr = up->find("FSRRayRegeneration"); rr != up->end() && rr->is_object()) oldRR = *rr;
    }
    const json* oldGI = nullptr;
    if (auto gi = root.find("RedstoneGI"); gi != root.end() && gi->is_object()) oldGI = &*gi;
    else if (auto up = root.find("Upscaling"); up != root.end() && up->is_object()) {
        if (auto legacyGI = up->find("redstoneGI"); legacyGI != up->end() && legacyGI->is_object()) oldGI = &*legacyGI;
    }
    if (oldGI) {
        if (!oldRR.contains("resolutionMode") && oldGI->contains("ResolutionMode"))
            oldRR["resolutionMode"] = (*oldGI)["ResolutionMode"];
        if (!oldRR.contains("checkerboard") && oldGI->contains("RRCheckerboard"))
            oldRR["checkerboard"] = (*oldGI)["RRCheckerboard"];
        if (!oldRR.contains("enabled") && oldGI->contains("RREnabled"))
            oldRR["enabled"] = (*oldGI)["RREnabled"];
        if (!oldRR.contains("configuration") && oldGI->contains("RRConfiguration"))
            oldRR["configuration"] = (*oldGI)["RRConfiguration"];
    }
    if (!oldRR.empty()) {
        auto& target = composer["FSRRayRegeneration"];
        if (!target.is_object()) target = json::object();
        for (const auto& [key, v] : oldRR.items()) if (!target.contains(key)) target[key] = v;
    }
    json oldDirect = json::object();
    if (auto older = root.find("RedstoneShadows"); older != root.end() && older->is_object())
        for (const char* key : kDirectKeys) CopyMissing(oldDirect, *older, key);
    if (auto direct = root.find("RTDirectLighting"); direct != root.end() && direct->is_object())
        for (const char* key : kDirectKeys) if (direct->contains(key)) oldDirect[key] = (*direct)[key];
    if (!oldDirect.empty()) {
        auto& target = composer["DirectRR"];
        if (!target.is_object()) target = json::object();
        for (const auto& [key, v] : oldDirect.items()) if (!target.contains(key)) target[key] = v;
    }
}

void RTComposer::MigrateChangedLegacySettings(const json& before, json& after)
{
    if (!after.is_object() || !before.is_object()) return;
    // Only migrate keys introduced or changed by the later, higher-priority layer.
    json sparse = json::object();
    auto addChanged = [&](const char* owner, const char* nested, const char* key) {
        auto nowOwner = after.find(owner);
        if (nowOwner == after.end() || !nowOwner->is_object()) return;
        const json* now = &*nowOwner;
        const json* old = nullptr;
        if (auto prev = before.find(owner); prev != before.end() && prev->is_object()) old = &*prev;
        if (nested) {
            auto nowNested = now->find(nested);
            if (nowNested == now->end() || !nowNested->is_object()) return;
            now = &*nowNested;
            if (old) {
                auto oldNested = old->find(nested);
                old = oldNested != old->end() && oldNested->is_object() ? &*oldNested : nullptr;
            }
        }
        auto current = now->find(key);
        if (current == now->end()) return;
        if (old && old->contains(key) && (*old)[key] == *current) return;
        if (nested) sparse[owner][nested][key] = *current;
        else sparse[owner][key] = *current;
    };
    for (const char* key : { "enabled", "checkerboard", "resolutionMode", "configuration" })
        addChanged("Upscaling", "FSRRayRegeneration", key);
    for (const char* key : kDirectKeys) {
        addChanged("RTDirectLighting", nullptr, key);
        addChanged("RedstoneShadows", nullptr, key);
    }
    for (const char* key : { "ResolutionMode", "RRCheckerboard", "RREnabled", "RRConfiguration" })
        addChanged("RedstoneGI", nullptr, key);
    if (!sparse.empty()) {
        MigrateLegacySettings(sparse);
        if (sparse.contains("RTComposer")) after["RTComposer"].merge_patch(sparse["RTComposer"]);
    }
}

void RTComposer::ImportLegacySettings(json& root)
{
    MigrateLegacySettings(root);
}

void RTComposer::SaveSettings(json& value)
{
    const auto policy = RayRegeneration::GetSettings();
    const auto& direct = globals::features::rtDirectLighting.settings;
    value = {
        { "FSRRayRegeneration", {
            { "enabled", policy.enabled },
            { "checkerboard", policy.checkerboard },
            { "resolutionMode", policy.resolutionMode },
            { "configuration", RayRegeneration::GetConfiguration().overrides }
        } },
        { "DirectRR", {
            { "UseDirectRayRegeneration", direct.UseDirectRayRegeneration },
            { "DirectRRResolution", direct.DirectRRResolution },
            { "RRMaterialIsolation", direct.RRMaterialIsolation },
            { "RRGlossyRoughness", direct.RRGlossyRoughness }
        } },
        { "Composition", {
            { "enabled", composition.enabled },
            { "mode", static_cast<uint32_t>(composition.mode) },
            { "shadowInfluence", composition.shadowInfluence },
            { "rtDirectContribution", composition.rtDirectContribution },
            { "shadowThreshold", composition.shadowThreshold }
        } }
    };
}

void RTComposer::LoadSettings(json& value)
{
    if (!value.is_object()) return;
    // Each setting is optional: old profiles retain the disabled legacy path.
    if (auto it = value.find("Composition"); it != value.end() && it->is_object()) {
        CompositionSettings next{};
        next.enabled = it->value("enabled", false);
        const auto mode = it->value("mode", 0u);
        next.mode = mode == 1u ? CompositorMode::HybridShadowed : CompositorMode::ShadowOnly;
        auto bounded = [&](const char* name, float fallback) {
            const float x = it->value(name, fallback);
            return std::isfinite(x) ? std::clamp(x, 0.0f, 1.0f) : fallback;
        };
        next.shadowInfluence = bounded("shadowInfluence", 1.0f);
        next.rtDirectContribution = bounded("rtDirectContribution", 0.0f);
        next.shadowThreshold = bounded("shadowThreshold", 0.25f);
        composition = next;
    } else composition = {};
    if (auto it = value.find("FSRRayRegeneration"); it != value.end() && it->is_object()) {
        const auto prior = RayRegeneration::GetSettings();
        RayRegeneration::Configure(it->value("resolutionMode", prior.resolutionMode),
            it->value("checkerboard", prior.checkerboard), it->value("enabled", prior.enabled));
        if (auto cfg = it->find("configuration"); cfg != it->end() && cfg->is_object())
            globals::features::redstoneGI.GetDenoiser().SetConfiguration(*cfg);
    }
    if (auto it = value.find("DirectRR"); it != value.end() && it->is_object()) {
        auto& direct = globals::features::rtDirectLighting.settings;
        const auto beforeResolution = direct.DirectRRResolution;
        const auto beforeIsolation = direct.RRMaterialIsolation;
        const auto beforeRoughness = direct.RRGlossyRoughness;
        direct.UseDirectRayRegeneration = it->value("UseDirectRayRegeneration", direct.UseDirectRayRegeneration);
        direct.DirectRRResolution = std::min(it->value("DirectRRResolution", direct.DirectRRResolution), 3u);
        direct.RRMaterialIsolation = it->value("RRMaterialIsolation", direct.RRMaterialIsolation);
        const auto roughness = it->value("RRGlossyRoughness", direct.RRGlossyRoughness);
        direct.RRGlossyRoughness = std::isfinite(roughness) ? std::clamp(roughness, 0.04f, 1.0f) : 0.2f;
        if (beforeResolution != direct.DirectRRResolution || beforeIsolation != direct.RRMaterialIsolation ||
            beforeRoughness != direct.RRGlossyRoughness)
            globals::features::dx12NativeScene.Tracer().Reset();
    }
}

void RTComposer::RestoreDefaultSettings()
{
    composition = {};
    RayRegeneration::Configure(1u, true, true);
    globals::features::redstoneGI.GetDenoiser().SetConfiguration(json::object());
    auto& direct = globals::features::rtDirectLighting.settings;
    direct.UseDirectRayRegeneration = true;
    direct.DirectRRResolution = 3u;
    direct.RRMaterialIsolation = false;
    direct.RRGlossyRoughness = 0.2f;
}

void RTComposer::DrawSettings()
{
    if (globals::game::isVR) {
        ImGui::TextUnformatted("FSR Ray Regeneration is unavailable in VR.");
        return;
    }
    ImGui::PushID("RTComposer");
    ImGui::SeparatorText("Compositor");
    ImGui::Checkbox("Enable RT shadow composition", &composition.enabled);
    int compositorMode = static_cast<int>(composition.mode);
    if (ImGui::Combo("Compositor mode", &compositorMode, "Shadow Only\0Hybrid-Shadowed\0"))
        composition.mode = compositorMode == 1 ? CompositorMode::HybridShadowed : CompositorMode::ShadowOnly;
    ImGui::SliderFloat("Shadow influence", &composition.shadowInfluence, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("RT direct contribution", &composition.rtDirectContribution, 0.0f, 1.0f, "%.2f");
    if (composition.mode == CompositorMode::HybridShadowed) {
        ImGui::SliderFloat("GI skip visibility threshold", &composition.shadowThreshold, 0.0f, 1.0f, "%.2f");
        ImGui::TextWrapped("DX12 HybridGI is not yet available. This mode is stored but does not change the active renderer.");
    } else {
        ImGui::TextWrapped("Shadow Only preserves existing DX11 SSGI and masks the final raster image. Invalid visibility remains neutral.");
    }
    ImGui::TextWrapped("Compositor controls are opt-in. Existing lighting and RR behavior remains unchanged while disabled.");
    ImGui::SeparatorText("FSR Ray Regeneration");
    const auto old = RayRegeneration::GetSettings();
    bool enabled = old.enabled, checkerboard = old.checkerboard;
    int resolution = static_cast<int>(old.resolutionMode);
    bool changed = ImGui::Checkbox("Enable FSR Ray Regeneration", &enabled);
    changed |= ImGui::Combo("Reconstruction resolution", &resolution, "Full resolution\0Half resolution\0Quarter resolution\0");
    changed |= ImGui::Checkbox("Checkerboard RR inputs", &checkerboard);
    if (changed) RayRegeneration::Configure(uint32_t(resolution), checkerboard, enabled);
    ImGui::TextWrapped("Shared RR policy. Independent Direct, RT-exclusive and Hybrid policies remain deferred.");
    ImGui::SeparatorText("Direct Lighting RR");
    auto& direct = globals::features::rtDirectLighting.settings;
    int directResolution = int(direct.DirectRRResolution);
    if (ImGui::Combo("Direct RR resolution", &directResolution, "Full\0Half\0Quarter\0Use shared RR setting (legacy)\0")) {
        direct.DirectRRResolution = uint32_t(directResolution);
        globals::features::dx12NativeScene.Tracer().Reset();
    }
    globals::features::rtDirectLighting.DrawRayRegenerationSettings();
    ImGui::SeparatorText("RR Signals and Diagnostics");
    globals::features::rtIndirectLighting.DrawUnifiedDebugSettings();
    if (!globals::features::rtIndirectLighting.UsesRTExclusive()) {
        ImGui::SeparatorText("Hybrid GI / AO Ray Regeneration");
        globals::features::redstoneGI.DrawRayRegenerationSettings();
    }
    ImGui::PopID();
}

void RTComposer::RegisterUxActions()
{
    auto& registry = Util::DevBenchUx::Registry::GetSingleton();
    registry.RegisterCommand(GetShortName(), "setComposition",
        "Set independent shadow compositor mode and influence; Hybrid-Shadowed remains gated until DX12 GI is ready.",
        +[](Feature* self, const json& args) {
            auto& policy = static_cast<RTComposer*>(self)->composition;
            if (args.contains("enabled")) policy.enabled = args.at("enabled").get<bool>();
            if (args.contains("mode")) {
                const auto mode = args.at("mode").get<uint32_t>();
                if (mode > 1u) throw std::invalid_argument("Unknown RT compositor mode");
                policy.mode = mode == 1u ? CompositorMode::HybridShadowed : CompositorMode::ShadowOnly;
            }
            auto setUnit = [&](const char* key, float& target) {
                if (!args.contains(key)) return;
                const auto value = args.at(key).get<float>();
                if (!std::isfinite(value)) throw std::invalid_argument("Composition value must be finite");
                target = std::clamp(value, 0.0f, 1.0f);
            };
            setUnit("shadowInfluence", policy.shadowInfluence);
            setUnit("rtDirectContribution", policy.rtDirectContribution);
            setUnit("shadowThreshold", policy.shadowThreshold);
        });
    registry.RegisterCommand(GetShortName(), "setRayRegeneration",
        "Set shared RR controls; saved under RTComposer.FSRRayRegeneration.",
        +[](Feature*, const json& args) {
            const auto prior = RayRegeneration::GetSettings();
            RayRegeneration::Configure(args.value("resolutionMode", prior.resolutionMode),
                args.value("checkerboard", prior.checkerboard), args.value("enabled", prior.enabled));
        });
}

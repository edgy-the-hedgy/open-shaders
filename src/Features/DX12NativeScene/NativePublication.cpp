#include "NativePublication.h"
#include "Globals.h"
#include "Deferred.h"
#include "State.h"
#include "GpuPass.h"
#include "Util.h"

namespace DX12Native
{
    bool NativePublication::Publish(const GraphicsBackend& backend, ID3D11UnorderedAccessView* target,
        ID3D11ShaderResourceView* first, ID3D11ShaderResourceView* second,
        std::array<uint32_t, 2> size, bool direct, float blend, ID3D11Buffer* camera, bool legacyIndirect, float indirectStrength)
    {
        CS_GPU_PASS("DX12NativeScene::PublishNativeLighting");
        if (!target || !first) return false;
        if (!shader) shader.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(
            L"Data\\Shaders\\RTDirectLighting\\publishNative.cs.hlsl", {}, "cs_5_0")));
        if (!shader) return false;
        struct Settings { std::array<uint32_t, 2> size; uint32_t direct; float blend; uint32_t legacy; float strength; uint32_t padding[2]{}; };
        if (!settings) settings = std::make_unique<ConstantBuffer>(ConstantBufferDesc<Settings>(), "DX12NativeScene::PublicationSettings");
        settings->Update(Settings{ size, uint32_t(direct), blend, uint32_t(legacyIndirect), indirectStrength });
        auto* context = backend.d3d11Context.get();
        winrt::com_ptr<ID3D11ComputeShader> previousShader;
        std::array<winrt::com_ptr<ID3D11ShaderResourceView>, 3> previousReads;
        winrt::com_ptr<ID3D11UnorderedAccessView> previousTarget;
        std::array<winrt::com_ptr<ID3D11Buffer>, 4> previousBuffers;
        constexpr std::array slots{ 0u, 5u, 6u, 12u };
        context->CSGetShader(previousShader.put(), nullptr, nullptr);
        for (uint32_t i = 0; i < 3; ++i) context->CSGetShaderResources(i, 1, previousReads[i].put());
        for (uint32_t i = 0; i < slots.size(); ++i) context->CSGetConstantBuffers(slots[i], 1, previousBuffers[i].put());
        context->CSGetUnorderedAccessViews(0, 1, previousTarget.put());
        globals::state->BindSharedDataCS(context);
        const auto& targets = globals::game::renderer->GetRuntimeData().renderTargets;
        ID3D11ShaderResourceView* reads[]{ first, second, Util::AsReal(targets[NORMALROUGHNESS].SRV) };
        auto* cb = settings->CB();
        context->CSSetConstantBuffers(0, 1, &cb);
        if (camera) context->CSSetConstantBuffers(12, 1, &camera);
        context->CSSetShaderResources(0, 3, reads);
        context->CSSetUnorderedAccessViews(0, 1, &target, nullptr);
        context->CSSetShader(shader.get(), nullptr, 0);
        context->Dispatch((size[0] + 7u) / 8u, (size[1] + 7u) / 8u, 1);
        ID3D11UnorderedAccessView* clear = nullptr;
        context->CSSetUnorderedAccessViews(0, 1, &clear, nullptr);
        for (uint32_t i = 0; i < slots.size(); ++i) {
            cb = previousBuffers[i].get(); context->CSSetConstantBuffers(slots[i], 1, &cb);
        }
        for (uint32_t i = 0; i < 3; ++i) {
            auto* read = previousReads[i].get(); context->CSSetShaderResources(i, 1, &read);
        }
        target = previousTarget.get(); context->CSSetUnorderedAccessViews(0, 1, &target, nullptr);
        context->CSSetShader(previousShader.get(), nullptr, 0);
        return true;
    }
}

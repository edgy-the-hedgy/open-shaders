#pragma once
#include "GraphicsBackend.h"
#include "Buffer.h"

namespace DX12Native
{
    /** @brief Publishes native lighting through the existing DX11 scene boundary. */
    class NativePublication
    {
    public:
        /** @brief Records scene writes only for valid native output pixels. */
        bool Publish(const GraphicsBackend& backend, ID3D11UnorderedAccessView* target,
            ID3D11ShaderResourceView* first, ID3D11ShaderResourceView* second,
            std::array<uint32_t, 2> size, bool direct, float blend,
            ID3D11Buffer* camera = nullptr, bool legacyIndirect = false, float indirectStrength = 1.0f);
    private:
        winrt::com_ptr<ID3D11ComputeShader> shader;
        std::unique_ptr<ConstantBuffer> settings;
    };
}

#pragma once
#include "Features/DX12/DX12NativeEnvironment.h"
#include "Features/DX12/VideoMemoryBudget.h"
#include <array>
#include <memory>

namespace NativeCubemaps
{
    // Compatibility transport only: DX12 produces and filters every mip.
    // DX11 receives a conventional cube by copying a fenced, packed strip.
    class ForwardEnvironment
    {
    public:
        ~ForwardEnvironment();
        void Update(const GraphicsBackend&,std::shared_ptr<const DX12Native::EnvironmentFrame>,const DX12Native::EnvironmentStamp&);
        void Bind(const GraphicsBackend&,const DX12Native::EnvironmentStamp&);
        void Unbind();
        void Reset();
        json Diagnostics() const;
    private:
        struct Slot;
        void Poll();
        GraphicsBackend owner;
        DX12Native::VideoMemoryBudget budget;
        SharedFence produced,consumed;
        std::array<std::shared_ptr<Slot>,3> slots;
        std::shared_ptr<Slot> current,pending;
        winrt::com_ptr<ID3D11DeviceContext4> boundContext;
        std::array<winrt::com_ptr<ID3D11ShaderResourceView>,2> prior,boundViews;
        bool bound=false;
        uint64_t publications=0,deferred=0;
        std::string failure;
    };
}

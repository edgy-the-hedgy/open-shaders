#pragma once
#include <d3d11.h>
#include "DX12MaterialFrameContract.h"
#include <atomic>
#include <cstdint>

namespace DX12Native::TextureRevision
{
	// No shared, tiled, CPU-writable or GPU-output resources may use the
	// revision cache. Deferred command lists invalidate the entire epoch.
	inline constexpr GUID kWriteRevision{0x5a21b4d7,0x370e,0x46f2,{0xb1,0xa8,0x61,0xd3,0x07,0x90,0x42,0xec}};
	inline std::atomic<bool> enabled{false};
	inline std::atomic<uint64_t> epoch{1}, serial{1};
	struct Stamp {
		uint64_t epoch=0, revision=0;
		constexpr bool operator==(const Stamp& other) const { return MaterialFrameContract::ContentRevisionMatches(epoch,revision,other.epoch,other.revision); }
		constexpr explicit operator bool() const { return epoch!=0; }
	};
	inline bool Eligible(const D3D11_TEXTURE2D_DESC& d)
	{
		return d.Usage==D3D11_USAGE_DEFAULT && d.BindFlags==D3D11_BIND_SHADER_RESOURCE &&
			d.CPUAccessFlags==0 && d.MiscFlags==0 && d.ArraySize==1 && d.SampleDesc.Count==1;
	}
	inline void InvalidateAll() { epoch.fetch_add(1,std::memory_order_acq_rel); }
	inline void MarkWrite(ID3D11Resource* resource)
	{
		if (!enabled.load(std::memory_order_acquire) || !resource) return;
		D3D11_RESOURCE_DIMENSION type{};
		resource->GetType(&type);
		if (type!=D3D11_RESOURCE_DIMENSION_TEXTURE2D) return;
		D3D11_TEXTURE2D_DESC desc{};
		static_cast<ID3D11Texture2D*>(resource)->GetDesc(&desc);
		if (!Eligible(desc)) return;
		const auto next=serial.fetch_add(1,std::memory_order_acq_rel);
		if (FAILED(resource->SetPrivateData(kWriteRevision,sizeof(next),&next))) InvalidateAll();
	}
	inline void MarkView(ID3D11View* view)
	{
		if (!view) return;
		ID3D11Resource* resource=nullptr;
		view->GetResource(&resource);
		if (resource) { MarkWrite(resource); resource->Release(); }
	}
	inline Stamp Read(ID3D11Resource* resource,const D3D11_TEXTURE2D_DESC& desc)
	{
		if (!resource || !Eligible(desc) || !enabled.load(std::memory_order_acquire)) return {};
		const auto generation=epoch.load(std::memory_order_acquire);
		uint64_t version=0;
		UINT size=sizeof(version);
		const auto hr=resource->GetPrivateData(kWriteRevision,&size,&version);
		if (hr!=DXGI_ERROR_NOT_FOUND && (FAILED(hr) || size!=sizeof(version))) return {};
		if (generation!=epoch.load(std::memory_order_acquire)) return {};
		return {generation,version};
	}
}

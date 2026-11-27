#pragma once

#include "DX12EnvironmentSource.h"
#include "GraphicsBackend.h"
#include "GraphicsInterop.h"
#include "VideoMemoryBudget.h"

#include <array>
#include <memory>
#include <string>

namespace DX12Native
{
	/** @brief Immutable copy of one completed environment revision. */
	struct EnvironmentCube
	{
		std::unique_ptr<WrappedResource> staging;
        winrt::com_ptr<ID3D12Resource> resource;
        winrt::com_ptr<ID3D12CommandAllocator> allocator;
        winrt::com_ptr<ID3D12GraphicsCommandList> commands;
        winrt::com_ptr<ID3D12Fence> sourceFence;
        uint64_t sourceValue = 0;
		EnvironmentStamp stamp{};
		uint64_t revision = 0, allocationBytes = 0;
		winrt::com_ptr<ID3D12Fence> readyFence;
		uint64_t readyValue = 0;
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
		uint32_t width = 0, mipCount = 0;
		bool IsReadyFor(const EnvironmentStamp& requested) const;
		bool Retired() const;
	};

	/** @brief One environment publication, independent of material or display frame lifetime. */
	struct EnvironmentFrame
	{
		EnvironmentStamp stamp{};
		uint64_t revision = 0;
		// Input admission, not proof that any particular texel matched native evidence.
		bool nativeRadianceInput = false, rasterCompatibilityInput = false;
		std::shared_ptr<const EnvironmentCube> base, reflection, sky;
		winrt::com_ptr<ID3D12Fence> producerFence;
		uint64_t producerValue = 0;
	};

	/** @brief Captures changed DX11 cubemap revisions into bounded fenced DX12 resources. */
	class NativeEnvironment
	{
	public:
		~NativeEnvironment();
		void Capture(const GraphicsBackend& backend, const EnvironmentStamp& requested,
			const EnvironmentSource& base, const EnvironmentSource& reflection, const EnvironmentSource& sky,
			bool allowMissingBase = false);
		void Poll(bool releaseUnused = false);
		void Reset();
		std::shared_ptr<const EnvironmentFrame> Get() const { return current; }
		uint64_t AllocationBytes() const;
		uint64_t Publications() const { return publications; }
		const std::string& Failure() const { return failure; }

	private:
		struct CubePool
		{
			std::array<std::shared_ptr<EnvironmentCube>, 3> slots;
			std::shared_ptr<EnvironmentCube> current;
		};
		std::shared_ptr<const EnvironmentCube> CaptureCube(CubePool& pool, const EnvironmentSource& source,
			const EnvironmentStamp& requested, const char* name);
		GraphicsBackend owner;
		SharedFence producer;
		winrt::com_ptr<ID3D12Fence> copyFence;
		uint64_t nextCopyValue = 0;
		VideoMemoryBudget videoBudget;
		std::array<CubePool, 3> pools;
		std::shared_ptr<const EnvironmentFrame> current;
		uint64_t publications = 0;
		std::string failure;
	};
}

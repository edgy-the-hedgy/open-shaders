#pragma once

#include "GraphicsInterop.h"
#include "GraphicsMemoryBudget.h"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

/** @brief Copies a raster scene to DX12 without native lighting or geometry inputs. */
class NativeComposite
{
public:
	enum class Transfer : uint8_t { Unknown, Linear, LegacyScene };

	struct Frame
	{
		winrt::com_ptr<ID3D12Resource> color;
		winrt::com_ptr<ID3D12Fence> producerFence;
		uint64_t producerValue = 0;
		uint32_t frame = 0, width = 0, height = 0;
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
		D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
		Transfer transfer = Transfer::Unknown;
		bool valid = false;

		/** @brief Checks the frame metadata required by a render-size consumer. */
		bool Matches(uint32_t a_frame, uint32_t a_width, uint32_t a_height, DXGI_FORMAT a_format) const
		{
			return valid && frame == a_frame && width == a_width && height == a_height &&
				format == a_format && state == D3D12_RESOURCE_STATE_COMMON;
		}

		/** @brief Returns true when the GPU has completed this frame's copy. */
		bool IsComplete() const
		{
			if (!valid || !producerFence || !producerValue)
				return false;
			const auto value = producerFence->GetCompletedValue();
			return value != UINT64_MAX && value >= producerValue;
		}
	};

	/** @brief Publishes one DX12-owned color frame using GPU-only synchronization. */
	std::shared_ptr<const Frame> Capture(ID3D11Device5* a_device11, ID3D11DeviceContext4* a_context11,
		ID3D12Device* a_device12, ID3D12CommandQueue* a_queue, ID3D11Texture2D* a_source,
		uint32_t a_frame, uint32_t a_width, uint32_t a_height, Transfer a_transfer);
	/** @brief Prevents slot reuse until a downstream GPU fence has completed. */
	bool Retain(const std::shared_ptr<const Frame>& a_frame, ID3D12Fence* a_fence, uint64_t a_value);
	/** @brief Reports the latest producer status without exposing mutable internal state. */
	std::string Status() const;
	uint64_t CompletedFrames() const { return completedFrames.load(); }
	~NativeComposite();

private:
	struct Slot
	{
		std::shared_ptr<Frame> frame;
		std::unique_ptr<WrappedResource> shared;
		winrt::com_ptr<ID3D12CommandAllocator> allocator;
		winrt::com_ptr<ID3D12GraphicsCommandList> commands;
		SharedFence fence;
		std::vector<std::pair<winrt::com_ptr<ID3D12Fence>, uint64_t>> consumers;
		ID3D12Device* device = nullptr;
		ID3D11Device* device11 = nullptr;
		D3D11_TEXTURE2D_DESC sourceDesc{};
		uint64_t frameBytes = 0;
		bool countedCompleted = false;
		bool untracked = false;
	};

	mutable std::mutex mutex;
	std::array<std::unique_ptr<Slot>, 4> slots;
	GraphicsMemoryBudget videoBudget;
	std::string status = "Not captured";
	std::atomic<uint64_t> completedFrames{ 0 };
};

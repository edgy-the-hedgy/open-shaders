#pragma once

#include "GraphicsBackend.h"
#include "Scene.h"
#include "GraphicsInterop.h"
#include <directx/d3dx12.h>

namespace RedstoneShadow
{
	/** @brief Owns native DX12 geometry exported entirely on the GPU. */
	struct Geometry
	{
		std::shared_ptr<const Mesh> source;
		winrt::com_ptr<ID3D12Resource> buffer;
		uint64_t allocationBytes = 0;
		uint64_t indexOffset = 0;
		uint64_t validationOffset = 0;
		bool prepared = false;
	};

	/** @brief Converts owned D3D11 geometry into persistent DX12 buffers through existing texture interop. */
	class GeometryBridge
	{
	public:
		/** @brief Retains the existing backend and creates independent producer/completion fence timelines. */
		bool Initialize(const GraphicsBackend& a_backend);
		/** @brief Preserves bounded in-flight objects if teardown precedes GPU completion. */
		~GeometryBridge();
		/** @brief Invalidates cached exports while retaining in-flight objects until their fence completes. */
		void Reset();
		/** @brief Allows an explicit shader reload to retry export without retiring in-flight resources. */
		void ReloadShader() { exportShader = nullptr; failed = false; failure.clear(); }
		/** @brief Polls retirement and submits at most one bounded mesh export without CPU waits. */
		void Advance(const Snapshot* a_snapshot, uint64_t a_budgetBytes);
		/** @brief Reports completed DX12 exports independently of acceleration structures and tracing. */
		json GetDiagnostics() const;
		/** @brief Retains a copy-complete DX12 mesh for independently fenced consumers. */
		std::shared_ptr<const Geometry> Find(const Mesh* a_mesh) const;
		/** @brief Reads render-owned export progress to avoid rebuilding preparation on unchanged polls. */
		uint64_t Progress() const { return completed; }
		/** @brief Exposes latched transport failure without allocating diagnostics. */
		bool Failed() const { return failed; }
		/** @brief Reports export deferrals for bounded batch abandonment. */
		uint64_t BudgetDeferrals() const { return budgetDeferrals; }

	private:
		static constexpr uint32_t kTextureWidth = 1024;
		static constexpr uint32_t kMaximumTextureHeight = 8192;
		struct Transfer
		{
			std::unique_ptr<WrappedResource> texture;
			std::shared_ptr<Geometry> geometry;
			winrt::com_ptr<ID3D11Buffer> settings;
			winrt::com_ptr<ID3D11ShaderResourceView> vertices, indices;
			winrt::com_ptr<ID3D12CommandAllocator> allocator;
			winrt::com_ptr<ID3D12GraphicsCommandList> commands;
			uint64_t fence = 0;
			uint64_t generation = 0;
			uint64_t sharedBytes = 0;
			bool submitted = false;
		};
		void Submit(Transfer& a_transfer, const std::shared_ptr<const Mesh>& a_mesh, uint64_t a_budgetBytes);
		uint64_t ResidentBytes() const;
		GraphicsBackend backend;
		SharedFence producer;
		winrt::com_ptr<ID3D12Fence> completion;
		winrt::com_ptr<ID3D11ComputeShader> exportShader;
		std::array<Transfer, 2> transfers;
		std::unordered_map<const Mesh*, std::shared_ptr<const Geometry>> geometries;
		std::unordered_set<const Mesh*> currentMeshes;
		uint64_t snapshotGeneration = UINT64_MAX, snapshotSweep = UINT64_MAX, exportCursor = 0;
		uint64_t generation = 0, submitted = 0, completed = 0, busyFrames = 0, budgetDeferrals = 0;
		uint64_t nextFence = 0;
		bool failed = false;
		std::string failure;
		std::string lastDeferral;
		uint64_t lastRequiredBytes = 0, lastBudgetBytes = 0;
	};
}

#pragma once

#include "GeometryBridge.h"

namespace RedstoneShadow
{
	/** @brief Retains a range-specific BLAS and its immutable native input geometry. */
	struct BottomLevel
	{
		std::shared_ptr<const Geometry> geometry;
		winrt::com_ptr<ID3D12Resource> result;
		uint32_t firstIndex = 0, indexCount = 0;
		uint64_t bytes = 0;
	};

	/** @brief Owns a completed TLAS with matching instance records and BLAS lifetimes. */
	struct TopLevel
	{
		winrt::com_ptr<ID3D12Resource> result;
		std::vector<std::shared_ptr<const BottomLevel>> bottoms;
		std::vector<Instance> instances;
		uint64_t generation = 0, sweep = 0, bytes = 0, blasRevision = 0;
		uint32_t omittedInstances = 0;
	};

	/** @brief Builds bounded acceleration structures independently of engine object traversal. */
	class AccelerationScene
	{
	public:
		/** @brief Uses the existing device and queue for DXR 1.1 builds. */
		bool Initialize(const GraphicsBackend& a_backend);
		/** @brief Preserves pending build resources on teardown without a CPU fence wait. */
		~AccelerationScene();
		/** @brief Invalidates publication and retires pending builds by generation. */
		void Reset();
		/** @brief Builds at most one missing BLAS and one TLAS per frame, with two in-flight jobs. */
		void Advance(const Snapshot* a_snapshot, const GeometryBridge& a_bridge, uint64_t a_budgetBytes);
		/** @brief Retains the latest completed TLAS for a separately fenced ray consumer. */
		std::shared_ptr<const TopLevel> GetScene() const { return published; }
		/** @brief Reports build submission/completion separately from traversal and coverage. */
		json GetDiagnostics() const;

	private:
		static constexpr uint32_t kMaximumInstances = 8192;
		struct RangeKey
		{
			const Mesh* mesh = nullptr;
			uint32_t first = 0, count = 0;
			bool operator==(const RangeKey&) const = default;
		};
		struct RangeHash
		{
			size_t operator()(const RangeKey& a_key) const
			{
				return std::hash<const Mesh*>{}(a_key.mesh) ^ std::rotl(size_t(a_key.first), 1) ^ std::rotl(size_t(a_key.count), 2);
			}
		};
		struct Build
		{
			winrt::com_ptr<ID3D12CommandAllocator> allocator;
			winrt::com_ptr<ID3D12GraphicsCommandList4> commands;
			winrt::com_ptr<ID3D12Resource> scratch, instanceUpload;
			std::shared_ptr<BottomLevel> bottom;
			std::shared_ptr<TopLevel> top;
			uint64_t fence = 0, generation = 0, transientBytes = 0;
			bool submitted = false;
		};
		winrt::com_ptr<ID3D12Resource> Buffer(uint64_t a_bytes, D3D12_HEAP_TYPE a_heap, D3D12_RESOURCE_STATES a_state, const wchar_t* a_name);
		uint64_t AllocationBytes(uint64_t a_bytes, D3D12_HEAP_TYPE a_heap) const;
		uint64_t ResidentBytes() const;
		void Begin(Build& a_build);
		void Submit(Build& a_build);
		bool BuildBottom(Build& a_build, const Instance& a_instance, std::shared_ptr<const Geometry> a_geometry, uint64_t a_budgetBytes);
		bool BuildTop(Build& a_build, const Snapshot& a_snapshot, uint64_t a_budgetBytes);
		void Retire();
		GraphicsBackend backend;
		winrt::com_ptr<ID3D12Device5> device;
		winrt::com_ptr<ID3D12Fence> completion;
		std::array<Build, 2> builds;
		std::unordered_map<RangeKey, std::shared_ptr<const BottomLevel>, RangeHash> bottoms;
		std::shared_ptr<const TopLevel> published;
		uint64_t generation = 0, nextFence = 0, bottomSubmissions = 0, topSubmissions = 0, bottomCompletions = 0, topCompletions = 0;
		uint64_t budgetDeferrals = 0, busyFrames = 0;
		size_t buildCursor = 0;
		std::string failure;
	};
}

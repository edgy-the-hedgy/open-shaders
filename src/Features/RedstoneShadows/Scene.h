#pragma once

#include "Features/LightLimitFix/CasterObservation.h"
#include <d3d11.h>
#include <winrt/base.h>
#include <memory>
#include <bit>
#include <unordered_map>
#include <unordered_set>

namespace RedstoneShadow
{
	inline constexpr uint8_t kOpaqueInstanceMask = 1;
	inline constexpr uint8_t kMaterialPendingInstanceMask = 2;
	/** @brief Preserves offline material work classes without rediscovering them during traversal. */
	enum class OfflineMaterialFlags : uint32_t
	{
		None = 0,
		AlphaCoverage = 1,
		VertexAlpha = 2,
		AnimatedMaterial = 4,
		Unknown = 0x80000000u
	};
	/** @brief Budgets engine observations and owned GPU geometry independently. */
	struct SceneLimits
	{
		uint32_t maxInstances = 8192;
		uint64_t bufferBytes = 128ull * 1024 * 1024;
	};

	/** @brief Owns copied GPU bytes without retaining engine geometry. */
	struct Mesh
	{
		winrt::com_ptr<ID3D11Buffer> vertices, indices;
		uint32_t stride = 0;
		uint32_t vertexCount = 0;
		uint64_t descriptor = 0;
		uint64_t bytes = 0;
	};

	/** @brief Captures material coverage without a live Skyrim material pointer. */
	struct Material
	{
		winrt::com_ptr<ID3D11ShaderResourceView> alphaTexture;
		float4 uvTransform{ 1, 1, 0, 0 };
		float alphaCutoff = 0;
		float alpha = 1;
		uint16_t alphaFlags = 0;
		int32_t textureClampMode = 0;
		bool twoSided = false;
		bool alphaTest = false;
		bool opaqueCandidate = true;
		OfflineMaterialFlags offlineMaterialFlags = OfflineMaterialFlags::None;
	};

	/** @brief Records a rigid instance separately from retained mesh bytes. */
	struct Instance
	{
		uint64_t identity = 0;
		std::shared_ptr<const Mesh> mesh;
		Material material;
		DirectX::XMFLOAT3X4 world{};
		float4 bound{};
		uint32_t firstIndex = 0;
		uint32_t indexCount = 0;
		uint64_t observationFrame = 0;
	};

	/** @brief Publishes completed GPU copies; acquisition coverage is distinct from traversal coverage. */
	struct Snapshot
	{
		uint64_t generation = 0;
		uint64_t sweep = 0;
		CasterObservation::Scope scope;
		uint64_t firstFrame = 0, lastFrame = 0;
		std::vector<Instance> instances;
		std::unordered_map<std::string, uint32_t> omissions;
		uint32_t visitedNodes = 0;
		bool truncated = false;
		bool offline = false;
	};

	/** @brief Consumes owned SCM batches at the existing render-thread handoff. */
	class Scene
	{
	public:
		/** @brief Creates the nonblocking copy query. */
		bool Initialize();
		/** @brief Consumes one queued batch only when the prior batch is released. */
		void Advance(const SceneLimits& a_limits, uint64_t a_frame);
		/** @brief Polls copy completion without flushing or waiting. */
		bool RetireCopies();
		/** @brief Invalidates publication while preserving pending copy ownership. */
		void Reset();
		/** @brief Releases the current batch after consumer processing finishes. */
		void ReleaseSnapshot() { published.reset(); }
		/** @brief Borrows the current completed snapshot. */
		const Snapshot* GetSnapshot() const { return published.get(); }
		/** @brief Borrows observed scalar records while copies are pending or complete. */
		const Snapshot* GetDebugSnapshot() const { return published ? published.get() : collecting.get(); }
		/** @brief Retains owned immutable records for asynchronous preparation. */
		std::shared_ptr<const Snapshot> RetainSnapshot() const { return published; }
		/** @brief Returns observed-subset validity and copy costs. */
		json GetDiagnostics() const;
		/** @brief Returns a latched copy/allocation failure without building diagnostics. */
		bool Failed() const { return !failure.empty(); }
	private:
		struct Cached
		{
			winrt::com_ptr<ID3D11Buffer> vertices, indices;
			std::shared_ptr<const Mesh> mesh;
			uint64_t batch = 0;
		};
		std::shared_ptr<const Mesh> Acquire(const CasterObservation::Record& a_record, const SceneLimits& a_limits);
		uint64_t ResidentBytes() const;
		std::vector<Cached> meshes;
		std::shared_ptr<Snapshot> collecting;
		std::shared_ptr<const Snapshot> published;
		winrt::com_ptr<ID3D11Query> completion;
		bool copiesPending = false, discardPending = false;
		uint64_t copiedBytes = 0, reusedMeshes = 0, busyFrames = 0;
		uint64_t batchCopiedBytes = 0;
		uint64_t queuedEpochDiscards = 0, copyEpochDiscards = 0, resetEpochDiscards = 0;
		std::string failure;
	};
}

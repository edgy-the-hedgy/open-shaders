#pragma once

#include <d3d11.h>
#include <winrt/base.h>
#include <memory>
#include <bit>
#include <unordered_map>
#include <unordered_set>

namespace RedstoneShadow
{
	/** @brief Budgets engine observations and owned GPU geometry independently. */
	struct SceneLimits
	{
		uint32_t nodesPerFrame = 2048;
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
		uint64_t firstFrame = 0, lastFrame = 0;
		std::vector<Instance> instances;
		std::unordered_map<std::string, uint32_t> omissions;
		uint32_t visitedNodes = 0;
		bool truncated = false;
	};

	/** @brief Acquires bounded off-screen scene records on the engine render thread. */
	class Scene
	{
	public:
		/** @brief Initializes the nonblocking GPU-copy completion query. */
		bool Initialize();
		/** @brief Advances one bounded observation batch and polls pending copies without flushing. */
		void Advance(const SceneLimits& a_limits, uint64_t a_frame);
		/** @brief Retires completed copies without starting new acquisition, including while disabled. */
		bool RetireCopies();
		/** @brief Invalidates scene identity and releases engine references on the engine thread. */
		void Reset();
		/** @brief Returns acquisition, memory and coverage evidence without a GPU readback. */
		json GetDiagnostics() const;
		/** @brief Borrows the copy-complete snapshot until the next engine-thread scene mutation. */
		const Snapshot* GetSnapshot() const { return published.get(); }

	private:
		static constexpr uint32_t kMaximumNodes = 65536;
		static constexpr uint32_t kMaximumGridLength = 32;
		static constexpr uint64_t kMaximumCopyBytesPerFrame = 16ull * 1024 * 1024;
		struct Identity
		{
			RE::NiPointer<RE::BSGeometry> object;
			uint64_t serial = 0, sweep = 0;
		};
		struct CachedMesh
		{
			winrt::com_ptr<ID3D11Buffer> sourceVertices, sourceIndices;
			std::shared_ptr<const Mesh> owned;
			uint64_t sweep = 0;
		};
		struct MeshKey
		{
			uintptr_t vertices = 0, indices = 0;
			uint64_t descriptor = 0;
			uint64_t revision = 0;
			bool operator==(const MeshKey&) const = default;
		};
		struct MeshKeyHash
		{
			size_t operator()(const MeshKey& a_key) const
			{
				return std::hash<uintptr_t>{}(a_key.vertices) ^ std::rotl(std::hash<uintptr_t>{}(a_key.indices), 1) ^
					std::hash<uint64_t>{}(a_key.descriptor) ^ std::rotl(std::hash<uint64_t>{}(a_key.revision), 2);
			}
		};
		struct PendingNode
		{
			RE::NiPointer<RE::NiAVObject> object;
			uint32_t child = 0;
		};
		void Begin(uint64_t a_frame);
		void Enqueue(RE::NiAVObject* a_object);
		void Observe(RE::BSGeometry* a_geometry, const SceneLimits& a_limits, uint64_t a_frame);
		std::shared_ptr<const Mesh> AcquireMesh(RE::BSGeometry* a_geometry, const SceneLimits& a_limits, uint64_t a_frame);
		uint64_t ResidentBytes() const;
		void Complete();
		std::vector<PendingNode> pending;
		std::unordered_map<RE::NiAVObject*, RE::NiPointer<RE::NiAVObject>> visited;
		std::unordered_map<RE::BSGeometry*, Identity> identities;
		std::unordered_map<MeshKey, CachedMesh, MeshKeyHash> meshes;
		std::unique_ptr<Snapshot> collecting;
		std::unique_ptr<const Snapshot> published;
		winrt::com_ptr<ID3D11Query> completion;
		bool copiesPending = false, discardPending = false, retryObservation = false;
		uint64_t generation = 1, sweep = 0, nextIdentity = 1;
		uint64_t copiedBytes = 0, reusedMeshes = 0, busyFrames = 0;
		uint64_t residentBytes = 0;
		uint64_t frameCopiedBytes = 0;
		double acquisitionMs = 0;
		std::string failure;
	};
}

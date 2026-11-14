#pragma once

#include "GpuMaterialAbi.h"
#include "Features/DX12NativeScene/MaterialResources.h"
#include "Features/DX12NativeScene/RuntimeTextureProgress.h"

#include <atomic>
#include <mutex>
#include <optional>
#include <unordered_set>

namespace NativePBR
{
	/** @brief A record index refers only to this immutable material generation. */
	struct GpuMaterialLookup
	{
		uint32_t index = 0, residentRoles = 0, recipeIssues = 0;
		uint64_t revision = 0;
	};

	/** @brief Reusable screen/ray material records and DDS residency, without scene geometry. */
	struct GpuMaterialResources
	{
		static constexpr uint32_t kTextureDescriptorCount = 2 * DX12Native::kMaximumTexturePools;
		static_assert(kTextureDescriptorCount == 512);
		std::shared_ptr<const RecipeGeneration> generation;
		std::shared_ptr<const DX12Native::MaterialResources> residency;
		winrt::com_ptr<ID3D12Device> device;
		winrt::com_ptr<ID3D12Resource> records;
		winrt::com_ptr<ID3D12DescriptorHeap> textureDescriptors;
		std::vector<GpuMaterialLookup> entries;
		std::unordered_map<uint64_t, uint32_t> indices;
		uint64_t tableBytes = 0;
		uint32_t fullyResident = 0, partiallyResident = 0, featureLimited = 0;

		/** @brief Resolve an identity for either a visible draw or an independently bound TLAS hit. */
		std::optional<GpuMaterialLookup> Find(uint64_t a_identity, uint64_t a_revision = 0) const;
		/** @brief Prepare immutable material records and shared texture pages on a worker. */
		static std::shared_ptr<const GpuMaterialResources> Create(const GraphicsBackend& a_backend,
			std::shared_ptr<const RecipeGeneration> a_generation, uint64_t a_budgetLimit,
			uint64_t a_batchLimit, bool a_importArchives,
			std::shared_ptr<const GpuMaterialResources> a_previous = {},
			std::shared_ptr<const DX12Native::MaterialResources> a_sharedResidency = {},
			std::unordered_set<std::string> a_attemptedPaths = {});
	};

	/** @brief Owns background generation preparation and publishes after upload completion. */
	class GpuMaterialWorker
	{
	public:
		void Request(const GraphicsBackend& a_backend, std::shared_ptr<const RecipeGeneration> a_generation,
			uint64_t a_budgetLimit = 0, uint64_t a_batchLimit = 16ull * 1024 * 1024,
			bool a_importArchives = true, std::shared_ptr<const DX12Native::MaterialResources> a_sharedResidency = {});
		void Advance();
		void Reset();
		std::shared_ptr<const GpuMaterialResources> Get() const { return published; }
		const std::string& Failure() const { return failure; }
		uint32_t StreamBatches() const { return textureProgress.Batches(); }
		uint32_t StreamSuccessful() const { return textureProgress.Successful(); }
		uint32_t StreamFailed() const { return textureProgress.Failed(); }
		bool UploadPending() const;

	private:
		struct State;
		std::shared_ptr<State> pending;
		std::shared_ptr<const RecipeGeneration> desired;
		std::shared_ptr<const DX12Native::MaterialResources> initialResidency;
		std::shared_ptr<const GpuMaterialResources> published;
		DX12Native::RuntimeTextureProgress textureProgress;
		GraphicsBackend backend;
		uint64_t requestRevision = 0, budgetLimit = 0, batchLimit = 0, nextBatchAtMs = 0;
		bool importArchives = true, attempted = false;
		DX12Native::MaterialUploadRetry retry;
		std::string failure;
	};

	/** @brief Keeps generations alive until screen/ray consumer fences finish. */
	class GpuMaterialRetirement
	{
	public:
		void RetainUntil(std::shared_ptr<const GpuMaterialResources> a_generation,
			winrt::com_ptr<ID3D12Fence> a_consumerFence, uint64_t a_value);
		void Collect();

	private:
		struct Pending
		{
			std::shared_ptr<const GpuMaterialResources> generation;
			winrt::com_ptr<ID3D12Fence> fence;
			uint64_t value;
		};
		std::mutex mutex;
		std::vector<Pending> pending;
	};
}

#pragma once

#include "Features/NativePBR/GpuMaterialResources.h"
#include "Features/NativePBR/MaterialSidecarContract.h"

#include <array>
#include <functional>
#include <memory>

/** @brief DX12-only consumer of a producer-owned opaque surface frame. */
class NativePBRSurfacePass
{
public:
	static constexpr uint32_t kSurfaceInputs = 8;
	static constexpr uint32_t kSurfaceOutputs = 5;
	static constexpr uint32_t kMaterialPages = NativePBR::GpuMaterialResources::kTextureDescriptorCount;
	static constexpr uint32_t kProvisionalSurfaceAbiVersion = 2;

	struct InputFrame
	{
		std::array<winrt::com_ptr<ID3D12Resource>, kSurfaceInputs> textures;
		std::array<D3D12_RESOURCE_STATES, kSurfaceInputs> states{};
		std::shared_ptr<const std::vector<NativePBR::SidecarDraw>> draws;
		winrt::com_ptr<ID3D12Fence> readyFence;
		std::function<bool(ID3D12Fence*, uint64_t)> retainThrough;
		uint64_t readyValue = 0;
		uint32_t frame = 0, width = 0, height = 0;
		uint32_t abiVersion = 0;
		bool nativeOwnedDraws = false;
	};

	struct Slot
	{
		std::array<winrt::com_ptr<ID3D12Resource>, kSurfaceOutputs> outputs;
		winrt::com_ptr<ID3D12DescriptorHeap> descriptors;
		winrt::com_ptr<ID3D12Resource> drawBindings;
		winrt::com_ptr<ID3D12RootSignature> root;
		winrt::com_ptr<ID3D12PipelineState> pipeline;
		std::shared_ptr<const InputFrame> input;
		std::shared_ptr<const NativePBR::GpuMaterialResources> materials;
		uint64_t bytes = 0;
		uint32_t width = 0, height = 0, drawCapacity = 0;
	};

	/** @brief Initializes the SM6.5 pipeline without requiring a surface producer. */
	bool Initialize(ID3D12Device* a_device);
	/** @brief Records and binds a fully fenced material evaluation, without changing final scene color. */
	bool Record(ID3D12Device* a_device, ID3D12GraphicsCommandList* a_commands, Slot& a_slot,
		std::shared_ptr<const InputFrame> a_input, std::shared_ptr<const NativePBR::GpuMaterialResources> a_materials,
		uint32_t a_frame, uint32_t a_width, uint32_t a_height, uint64_t a_additionalBudget);
	/** @brief Reports why an attempted material dispatch was deferred. */
	const std::string& Status() const { return status; }
	uint64_t Submissions() const { return submissions; }
	uint64_t MissingMaterials() const { return missingMaterials; }
	uint64_t MissingRevisions() const { return missingRevisions; }
	uint64_t MissingTextures() const { return missingTextures; }
	uint64_t UnsupportedRecipes() const { return unsupportedRecipes; }
	uint64_t UnsupportedInputs() const { return unsupportedInputs; }

private:
	winrt::com_ptr<ID3D12RootSignature> root;
	winrt::com_ptr<ID3D12PipelineState> pipeline;
	ID3D12Device* device = nullptr;
	std::string status = "Waiting for surface input producer";
	uint64_t submissions = 0, missingMaterials = 0, missingRevisions = 0, missingTextures = 0, unsupportedRecipes = 0, unsupportedInputs = 0;
};

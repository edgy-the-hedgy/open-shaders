#pragma once

#include "NativeMaterialEvaluation.h"

namespace NativePBR
{
	// One residency owner per serialized scene worker. Only completed
	// publications enter the reusable cache; submitted uploads remain with
	// the build that owns their GPU fence.
	class MaterialResidency
	{
	public:
		using Evaluation = DX12Native::NativeMaterialEvaluation;
		std::shared_ptr<const Evaluation> Prepare(ID3D12Device* device,
			const DX12Native::PreparedMaterialFrame& prepared,
			const DX12Native::SceneInputs& scene, std::vector<Evaluation::Upload>& uploads) const
		{
			return Evaluation::Prepare(device, prepared, scene, completedTextures, uploads);
		}

		void PublishCompleted(const std::shared_ptr<const Evaluation>& evaluation)
		{
			if (evaluation)
				completedTextures = evaluation->textures;
			else
				Reset();
		}

		void Reset() { completedTextures.clear(); }

	private:
		Evaluation::TextureCache completedTextures;
	};
}

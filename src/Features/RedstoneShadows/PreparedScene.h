#pragma once

#include "AccelerationScene.h"
#include <atomic>
#include <filesystem>

namespace RedstoneShadow
{
	/** @brief Owns offline geometry and baseline lights independently of engine pointers. */
	struct PreparedCell
	{
		std::shared_ptr<const Snapshot> snapshot;
		AccelerationScene::GeometryMap geometries;
		std::vector<float3> lights;
		std::string digest;
		uint64_t bytes = 0;
		uint32_t maskedInstances = 0;
	};

	/** @brief Loads one bounded package on an owned worker without D3D11 capture. */
	class PreparedScene
	{
	public:
		/** @brief Starts one file/decode/upload task using copied identity and backend data. */
		void Request(const GraphicsBackend& a_backend, std::filesystem::path a_directory, std::string a_cell, uint64_t a_epoch, uint64_t a_budget);
		/** @brief Cancels publication without joining the loader. */
		void Reset();
		/** @brief Reports whether this revision already attempted a package load. */
		bool Requested() const { return requested; }
		/** @brief Retains only a current-revision fully loaded package. */
		std::shared_ptr<const PreparedCell> GetCell() const;
		/** @brief Reports loader state independently of traversal and runtime validity. */
		json GetDiagnostics() const;
	private:
		struct State;
		std::shared_ptr<State> state;
		uint64_t revision = 1;
		bool requested = false;
	};
}

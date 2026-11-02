#pragma once

#include "Scene.h"
#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <array>

namespace RE { class TESObjectCELL; }

namespace DX12Native
{
	// Native Q0 assembly from verified atlas geometry and live static placements.
	// Ref enumeration stays on the existing render-thread feature callback;
	// the worker touches only copied scalars, files and JSON. No NIF parsing.
	class RuntimeGeometryGenerator
	{
	public:
		RuntimeGeometryGenerator();
		void Advance(RE::TESObjectCELL* a_cell, const std::filesystem::path& a_directory,
			const std::string& a_cellKey, uint32_t a_worldspaceID, uint32_t a_frame, uint64_t a_budgetBytes);
		void Reset();
		json GetDiagnostics() const;

	private:
		struct Placement
		{
			std::string model;
			uint32_t referenceID = 0, baseID = 0;
			std::array<float, 12> transform{};
		};
		struct State;
		std::shared_ptr<State> state;
		std::atomic<uint32_t> cellID{ 0 };
		uint32_t lastPollFrame = 0;
		uint64_t generation = 0;
	};
}

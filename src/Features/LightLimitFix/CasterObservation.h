#pragma once

#include <atomic>
#include <array>
#include <d3d11.h>
#include <winrt/base.h>

namespace CasterObservation
{
	inline constexpr uint32_t kCapacity = 256;
	inline constexpr uint32_t kQueueCapacity = 2;
	inline constexpr uint32_t kMaximumObservations = kCapacity * 4;
	/** @brief Identifies one observed local accumulation without queued engine pointers. */
	struct Scope
	{
		uint64_t scene = 0, light = 0, batch = 0;
		uint32_t frame = 0, slot = 0, policy = 0;
		bool operator==(const Scope&) const = default;
	};
	/** @brief Owns immutable buffers and scalar rigid geometry captured during native accumulation. */
	struct Record
	{
		winrt::com_ptr<ID3D11Buffer> vertices, indices;
		DirectX::XMFLOAT3X4 world{};
		float4 bound{};
		uint64_t descriptor = 0;
		uint32_t first = 0, count = 0;
		bool twoSided = false;
	};
	/** @brief Fixed-capacity observed subset; completeness never implies production coverage. */
	struct Batch
	{
		Scope scope;
		std::array<Record, kCapacity> records;
		uint32_t count = 0, observations = 0, omissions = 0, duplicates = 0;
		bool incomplete = false, overflow = false;
	};
	/** @brief Refreshes the render consumer lease or disables observation without changing scheduling. */
	void SetEnabled(bool a_enabled);
	/** @brief Enables temporary diagnostic counters without changing caster admission. */
	void SetDiagnosticsEnabled(bool a_enabled);
	/** @brief Selects one rotating native slot once per scheduler frame. */
	void Select(uint32_t a_frame, uint32_t a_first, uint32_t a_count);
	/** @brief Starts only the selected slot's first actual accumulation attempt. */
	bool Begin(uint32_t a_frame, uint32_t a_slot, uint64_t a_lightGeneration, uint32_t a_policy);
	/** @brief Captures synchronous append data; performs no immediate-context commands. */
	void Observe(RE::BSGeometry& a_geometry);
	/** @brief Marks a reset-guard skip as incomplete. */
	void Incomplete();
	/** @brief Publishes one completed attempt into the bounded FIFO. */
	void End();
	/** @brief Invalidates queued scope generations without waiting for producers. */
	void Invalidate();
	/** @brief Reads the current invalidation epoch without inspecting engine objects. */
	uint64_t Epoch();
	/** @brief Borrows the oldest ready slot exclusively until Release on the render thread. */
	Batch* Acquire();
	/** @brief Releases a consumed queue slot without a producer wait. */
	void Release(Batch* a_batch);
	/** @brief Allocates a monotonic light lifetime identity at existing slot assignment. */
	uint64_t NewLightGeneration();
	/** @brief Reports scalar selection/queue evidence; no engine objects are read. */
	json Diagnostics();
}

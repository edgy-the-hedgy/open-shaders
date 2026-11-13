#include "GraphicsMemoryBudget.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("DX12 frame allocations reserve adapter headroom", "[DX12]")
{
	constexpr uint64_t mib = 1024ull * 1024;
	CHECK(GraphicsMemoryBudget::Evaluate(1000 * mib, 600 * mib, 20 * mib) == 320 * mib);
	CHECK(GraphicsMemoryBudget::Evaluate(1000 * mib, 950 * mib, 20 * mib) == 20 * mib);
	CHECK(GraphicsMemoryBudget::Evaluate(1000 * mib, 1200 * mib, 20 * mib) == 20 * mib);
	CHECK(GraphicsMemoryBudget::Evaluate(0, 0, 20 * mib) == 0);
}

TEST_CASE("DX12 frame budget handles tracked-byte overflow", "[DX12]")
{
	CHECK(GraphicsMemoryBudget::Evaluate(1000, 0, UINT64_MAX - 100) == UINT64_MAX);
}

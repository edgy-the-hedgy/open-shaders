#include "Features/NativeComposite/NativeCompositePass.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("Native Composite identifies compatible render frames", "[DX12]")
{
	NativeComposite::Frame frame{};
	frame.frame = 42;
	frame.width = 1600;
	frame.height = 900;
	frame.format = DXGI_FORMAT_R16G16B16A16_FLOAT;

	CHECK_FALSE(frame.Matches(42, 1600, 900, DXGI_FORMAT_R16G16B16A16_FLOAT));
	frame.valid = true;
	CHECK(frame.Matches(42, 1600, 900, DXGI_FORMAT_R16G16B16A16_FLOAT));
	CHECK_FALSE(frame.Matches(41, 1600, 900, DXGI_FORMAT_R16G16B16A16_FLOAT));
	CHECK_FALSE(frame.Matches(42, 1920, 900, DXGI_FORMAT_R16G16B16A16_FLOAT));
	CHECK_FALSE(frame.Matches(42, 1600, 1080, DXGI_FORMAT_R16G16B16A16_FLOAT));
	CHECK_FALSE(frame.Matches(42, 1600, 900, DXGI_FORMAT_R8G8B8A8_UNORM));
	frame.state = D3D12_RESOURCE_STATE_COPY_DEST;
	CHECK_FALSE(frame.Matches(42, 1600, 900, DXGI_FORMAT_R16G16B16A16_FLOAT));
}

#pragma once

#include "Globals.h"
#include "DX12MaterialFrameContract.h"

namespace DX12Native
{
	/** @brief Main-view camera and raster metadata, frozen for an engine/plugin frame pair. */
	struct NativeWorldFrame
	{
		globals::FrameBuffer camera{};
		uint32_t engineFrame = 0, pluginFrame = 0;
		uintptr_t cameraIdentity = 0;
		uint32_t width = 0, height = 0, displayWidth = 0, displayHeight = 0;
		MaterialFrameContract::PixelJitter jitter{}, previousJitter{};
		float4 previousDynamicResolution1{}, previousDynamicResolution2{};
		float cameraNear = 0, cameraFar = 0, cameraFov = 0;
		bool reverseZ = false, resetHistory = true;
		bool Matches(uint32_t engine, uint32_t plugin) const
		{
			return cameraIdentity && engineFrame == engine && pluginFrame == plugin &&
				width && height && displayWidth && displayHeight && jitter.valid;
		}
	};
}

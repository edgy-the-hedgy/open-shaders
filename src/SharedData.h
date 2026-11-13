#pragma once

#include "Utils/ColorSpace.h"
#include <array>
#include "Features/Wind/TransientWindImpulse.h"
#include "Features/Wind/WindField.h"

struct alignas(16) SharedDataCB
{
	float4 WaterData[25];
	float4 DirLightDirection;
	float4 DirLightColor;
	float4 SunDirection;
	float4 SunColor;
	float4 MasserDirection;
	float4 MasserColor;
	float4 SecundaDirection;
	float4 SecundaColor;
	float4 CameraData;
	float4 BufferDim;
	float Timer;
	uint FrameCount;
	uint FrameCountAlwaysActive;
	uint InInterior;
	uint HasDirectionalShadows;
	uint InMapMenu;
	uint HideSky;
	float MipBias;
	float WaterSystemHeight;  // TES::GetWaterHeight in camera-relative Z; -NI_INFINITY when no water body found
	uint PostWaterComposite;
	uint ResetHistory;
	uint TonemapOwner;
	float4 AmbientSHR;
	float4 AmbientSHG;
	float4 AmbientSHB;
	float4 VRFoveationData0;          // x=center coverage scale, y=feather, z=horizontal scale, w=SSR raymarch mode (0 off, 1 feathered, 2 hard cutoff)
	float4 VRFoveationCenterOffsets;  // xy=left eye center offset, zw=right eye center offset
	float4 HDRData;                   // xyz + menu scene encoding in w — see HDRDisplay::GetSharedDataHDR
	float RefractionScale;            // ISRefraction.hlsl heat-shimmer multiplier; 1.0 = unmodified vanilla strength
	float3 pad1;
	WindField::WindTuning WindFieldTuning;
	float4 WindFieldAmbient;  // xyz: selected mean weather velocity, w: accumulated gust travel
	float4 WindFieldPreviousAmbient;
	WindField::Field WindFieldCurrent;
	WindField::Field WindFieldPrevious;
	WindField::Field WindFieldTransition;
	WindField::Field WindFieldPreviousTransition;
	float4 WindFieldTransitionData;  // x/y: current/previous blend, z/w: reserved
	float4 WindFieldSpringDebug;     // xy: field minimum, z: field size, w: maximum tilt radians
	std::array<uint32_t, 4> WindFieldActiveCounts;
	std::array<WindField::TransientWindSource, WindField::kTransientImpulseCapacity> WindFieldTransientImpulses;
	std::array<WindField::TransientWindSource, WindField::kTransientImpulseCapacity> WindFieldPreviousTransientImpulses;
};
STATIC_ASSERT_ALIGNAS_16(SharedDataCB);
// Each float4 cbuffer field must start on a 16-byte boundary to match the HLSL SharedData
// layout — a stray scalar inserted above would silently shift these and corrupt shader reads.
static_assert(offsetof(SharedDataCB, VRFoveationData0) % 16 == 0);
static_assert(offsetof(SharedDataCB, VRFoveationCenterOffsets) % 16 == 0);
static_assert(offsetof(SharedDataCB, HDRData) % 16 == 0);
static_assert(offsetof(SharedDataCB, WindFieldTuning) % 16 == 0);
static_assert(offsetof(SharedDataCB, WindFieldAmbient) % 16 == 0);
static_assert(offsetof(SharedDataCB, WindFieldPreviousAmbient) % 16 == 0);
static_assert(offsetof(SharedDataCB, WindFieldCurrent) % 16 == 0);
static_assert(offsetof(SharedDataCB, WindFieldPrevious) % 16 == 0);
static_assert(offsetof(SharedDataCB, WindFieldTransition) % 16 == 0);
static_assert(offsetof(SharedDataCB, WindFieldPreviousTransition) % 16 == 0);
static_assert(offsetof(SharedDataCB, WindFieldTransitionData) % 16 == 0);
static_assert(offsetof(SharedDataCB, WindFieldSpringDebug) % 16 == 0);
static_assert(offsetof(SharedDataCB, WindFieldActiveCounts) % 16 == 0);
static_assert(offsetof(SharedDataCB, WindFieldTransientImpulses) % 16 == 0);
static_assert(offsetof(SharedDataCB, WindFieldPreviousTransientImpulses) % 16 == 0);
static_assert(sizeof(SharedDataCB) <= D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * 16);

struct SharedLighting
{
	const RE::NiLight* directionalLight = nullptr;
	Util::ColorSpace::LightColor directional;
	Util::ColorSpace::LightColor sun;
	Util::ColorSpace::LightColor masser;
	Util::ColorSpace::LightColor secunda;
};

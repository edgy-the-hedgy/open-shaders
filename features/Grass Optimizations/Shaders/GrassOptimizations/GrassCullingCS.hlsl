<<<<<<< HEAD
#include "Common/FrameBuffer.hlsli"
#include "Common/GrassWindResponse.hlsli"
||||||| 12b261724
#include "Common/FrameBuffer.hlsli"
=======
#include "Common/GrassInstance.hlsli"
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
#include "Common/Math.hlsli"
#include "Common/Random.hlsli"
#include "Common/ReverseZ.hlsli"

#ifdef GRASS_COLLISION
#	include "GrassCollision/GrassCollisionField.hlsli"
#endif

cbuffer CullParams : register(b0)
{
	// [0..5] = eye 0's planes; [6..11] = eye 1's. VR planes use camera-relative coordinates.
	float4 FrustumPlanes[12];

<<<<<<< HEAD
	uint EyeCount;
	float3 _padEye;
||||||| 12b261724
    float MinPixelSize;
    float FullDetailPixelSize;
    float LODMinKeep;
    float LODFadeBand;
=======
	// PerFrame uses the light's camera during shadow passes, so retain the main camera's projection here.
	float4 CameraViewProj[4];
	float4 CameraPos;
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb

	float MinPixelSize;
	float FullDetailPixelSize;
	float LODMinKeep;
	float LODFadeBand;

	float MeshCostBias;
	float ProjScale;
	float MaxDistSq;
	float EdgeFadeStart;

	float AlphaParam1;
	float AlphaParam2;
	float FadeNow;
	float FadeInTimeRcp;

<<<<<<< HEAD
	float InvisibleFadeCull;
	float SimpleShadingPixelSize;
	float HiZProjScale;
	float MidLODPixelSize;
||||||| 12b261724
    float MeshLODBandPx;
    float HiZEnabled;
    float2 HiZSize;
=======
	float InvisibleFadeCull;
	float SimpleShadingPixelSize;
	float CollisionDistSq;
	float MidLODPixelSize;
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb

	float MeshLODBandPx;
	float HiZEnabled;
	float2 HiZSize;

<<<<<<< HEAD
	float HiZTexelPixels;
	float HiZMipCount;
	float OcclusionBias;
	float CostBiasStartDist;

	float FarLODPixelSize;
	float3 _pad0;
||||||| 12b261724
    float FarLODPixelSize;
    float3 _pad0;
=======
	float ProjectionWLength;
	float HiZMipCount;
	float OcclusionBias;
	float CostBiasStartDist;

	float FarLODPixelSize;
	float DensityDistanceRcp;
	float2 _pad0;
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
};

cbuffer CullBucket : register(b1)
{
<<<<<<< HEAD
	uint InstanceCount;
	float WavePeriod;
	float TimeBase;
	float PrevTimeBase;
	float3 BoundCenter;
	float ModelRadius;
	float DistScale;
	float MinPixelScale;
	float IsComplex;
	float MidLODEnabled;
	// The dispatch covers only these slices' combined instance count.
	uint SliceTableOffset;
	uint SliceCount;
	float FarLODEnabled;
	// Per-eye slot capacity of the output buffers below; eye 1's survivors land at this offset.
	uint OutputCapacityPerEye;
||||||| 12b261724
    uint InstanceCount;
    float WavePeriod;
    float TimeBase;
    float PrevTimeBase;
    float3 BoundCenter;
    float ModelRadius;
    float DistScale;
    float MinPixelScale;
    float IsComplex;
    float MidLODEnabled;
    // The dispatch covers the instances referenced by this slice range.
    uint SliceTableOffset;
    uint SliceCount;
    float FarLODEnabled;
    float _pad2;
=======
	uint GroupCount;
	// SliceTable maps dispatched groups to visible source runs.
	uint SliceTableOffset;
	uint SliceCount;
	// Split large buckets across rows to stay within the 65,535-group dispatch limit.
	uint DispatchWidth;

	// Each LOD tier occupies TierStride records in Compacted.
	uint TierStride;
	uint OutputOffset;
	uint ArgsOffset;
	uint _bucketPad0;

	float BoundExtent;
	float ModelRadius;
	float DistScale;
	float MinPixelScale;

	float MidLODEnabled;
	float FarLODEnabled;
	float ComplexThreshold;
	float MidLODMeshCost;

	float WavePeriod;
	float TimeBase;
	float PrevTimeBase;
	float FarLODMeshCost;
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
};

ByteAddressBuffer Instances : register(t0);
<<<<<<< HEAD
StructuredBuffer<float4> Origins : register(t1);
// Max-depth reduction of the scene depth copy (see GrassHiZCS.hlsl).
||||||| 12b261724
StructuredBuffer<float4> Origins : register(t1);
// Scene-depth max pyramid; see GrassHiZCS.hlsl.
=======
// Source groups contain instance ranges and cell origins.
StructuredBuffer<GrassInstance::CullGroup> Groups : register(t1);
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
Texture2D<float> HiZ : register(t2);
<<<<<<< HEAD
// Maps a compacted thread index back to a real instance: .x = the slice's first instance, .y = the instance total of every visible slice before it.
||||||| 12b261724
// .x is the first source instance; .y is the compacted start index.
=======
// Each entry stores the run's first source group in .x and its first dispatch-group index in .y.
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
StructuredBuffer<uint2> SliceTable : register(t3);

// RunGrass resolves each output record against its source instance and group.
RWByteAddressBuffer Compacted : register(u0);
// Each tier has DrawIndexedInstanced arguments with its instance count in the second DWORD.
RWByteAddressBuffer Args : register(u1);

// Tiers select the full, middle and far meshes.
static const uint TierCount = 3;
static const uint ArgsStride = 20;
static const uint TierCulled = TierCount;

<<<<<<< HEAD
RWByteAddressBuffer FarLODCompacted : register(u5);
RWStructuredBuffer<float4> FarLODExtras : register(u6);
// Mid and Far LOD survivor counts share one UAV so the shader stays within D3D11's eight-UAV limit.
// Packed as 4 bytes per (eye, tier): eye 1's slots sit kLODCounterEyeStride bytes past eye 0's.
RWByteAddressBuffer LODCounters : register(u7);

static const uint kLODCounterEyeStride = 8;
static const uint MiddleLODCountOffset = 0;
static const uint FarLODCountOffset = 4;
||||||| 12b261724
RWByteAddressBuffer FarLODCompacted : register(u5);
RWStructuredBuffer<float4> FarLODExtras : register(u6);
// Both LOD counts share one UAV so the shader stays within D3D11's eight-UAV limit.
RWByteAddressBuffer LODCounters : register(u7);

static const uint MiddleLODCountOffset = 0;
static const uint FarLODCountOffset = 4;
=======
// Aggregate survivors within each group to issue one global atomic per occupied tier.
groupshared uint GroupTierCount[TierCount];
groupshared uint GroupTierBase[TierCount];
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb

// Uses a uint hash to generate a random float between [0, 1)
float RandFloat(uint bits)
{
	const uint mantissaMask = 0x007FFFFFu;
	const uint one = 0x3F800000u;

	bits &= mantissaMask;
	bits |= one;

	return asfloat(bits) - 1.0;
}

<<<<<<< HEAD
bool CullEye(uint eyeIndex, float3 world, float4 og, uint4 raw0, uint4 raw1, uint2 rand,
	float sizeVariance, out float fade, out float flags, out uint tier)
||||||| 12b261724
// Precompute the per-instance wind term used by the vertex shader.
float WindScalar(float basis, float timer)
=======
float4 ProjectDirection(float3 v)
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
{
<<<<<<< HEAD
	fade = 0.0;
	flags = 0.0;
	tier = 0u;
	const float3 dv = world - FrameBuffer::CameraPosAdjust[eyeIndex].xyz;
	const float distSq = dot(dv, dv);

	const float dist = sqrt(distSq);

	// Mesh complexity only biases culling past CostBiasStartDist, ramping to full over the distance past that
	const float costRamp = saturate((dist - CostBiasStartDist) / max(CostBiasStartDist, 1e-4));
	const float effCostBias = MeshCostBias * costRamp;

	const float dScale = lerp(1.0, DistScale, effCostBias);
	const float effMaxDistSq = MaxDistSq * dScale * dScale;
	if (distSq > effMaxDistSq)
		return false;

	const float instanceRadius = ModelRadius * (1.0 + max(sizeVariance, 0.0));
#if defined(VR)
	// The VR planes are camera-relative and conservatively test the full instance bound.
	const float frustumRadius = instanceRadius + length(BoundCenter) * (1.0 + max(sizeVariance, 0.0));
#endif
	[unroll] for (uint p = 0; p < 6; ++p)
	{
		const float4 plane = FrustumPlanes[eyeIndex * 6 + p];
#if defined(VR)
		if (dot(plane.xyz, dv) - plane.w < -frustumRadius)
			return false;
#else
		if (dot(plane.xyz, world) - plane.w < 0.0)
			return false;
#endif
	}

	const float projPx = (instanceRadius / dist) * ProjScale;
	const float pxScale = lerp(1.0, MinPixelScale, effCostBias);
	const float effMinPx = MinPixelSize * pxScale;
	if (projPx < effMinPx)
		return false;

	if (HiZEnabled > 0.5) {
		// ModelRadius bounds about BoundCenter, not the instance root, so the sphere has to be there to be accurately occluded.
		const float3 rot0 = float3(f16tof32(raw0.z & 0xFFFF), f16tof32(raw0.z >> 16), f16tof32(raw0.w & 0xFFFF));
		const float3 rot1 = float3(f16tof32(raw1.x & 0xFFFF), f16tof32(raw1.x >> 16), f16tof32(raw1.y & 0xFFFF));
		const float3 rot2 = float3(f16tof32(raw1.z & 0xFFFF), f16tof32(raw0.w >> 16), f16tof32(raw1.y >> 16));

		const float3 msCentre = BoundCenter * (1.0 + max(sizeVariance, 0.0));
		const float3 dvC = dv + float3(dot(rot0, msCentre), dot(rot1, msCentre), dot(rot2, msCentre));

		const float occRadius = instanceRadius + length(BoundCenter) * abs(sizeVariance);
		const float distC = max(length(dvC), 1e-4);
		const float projPxOcc = (occRadius / distC) * HiZProjScale;

		const float4 clipC = mul(FrameBuffer::CameraViewProj[eyeIndex], float4(dvC, 1.0));
		if (clipC.w > 0.0) {
			float2 uv = (clipC.xy / clipC.w) * float2(0.5, -0.5) + 0.5;
			// Same UV split as Stereo::ConvertToStereoUV -- keep in sync if that changes.
			if (EyeCount > 1)
				uv.x = (uv.x + (float)eyeIndex) * 0.5;
			const float2 tc = uv * HiZSize;
			const float rT = projPxOcc / HiZTexelPixels;  // occlusion radius expressed in level-0 texels

			// The level where the instance spans ~2 texels, so the 3x3 below covers it exactly.
			const float wantLevel = ceil(log2(max(2.0 * rT, 1.0)));

			// A level too fine to cover the instance would underestimate the max and cull visible grass.
			[branch] if (wantLevel <= HiZMipCount - 1.0)
			{
				const int level = (int)wantLevel;
				const float scale = exp2((float)level);
				const float2 tcL = tc / scale;
				const float rTL = rT / scale;
				const int2 dimL = max(int2(ceil(HiZSize / scale)), int2(1, 1));
				// Clamp the footprint to the active eye's half so taps near the stereo seam don't sample the other eye.
				const int eyeHalf = (uint)dimL.x / 2u;
				const int xMin = (EyeCount > 1 && eyeHalf > 0) ? (int(eyeIndex) * eyeHalf) : 0;
				const int xMax = (EyeCount > 1 && eyeHalf > 0) ? (xMin + eyeHalf - 1) : (dimL.x - 1);

				const int2 t0 = int2(floor(tcL - rTL));
				const int2 t1 = int2(floor(tcL + rTL));
				// An instance hides only once even its nearest point is behind the occluder. A camera inside
				// the sphere collapses dvC, putting nearZ below any tile depth so the test never fires.
				const float3 dvNear = dvC * (max(distC - occRadius, 0.0) / distC);
				const float4 clipN = mul(FrameBuffer::CameraViewProj[eyeIndex], float4(dvNear, 1.0));
				const float nearZ = clipN.z / max(clipN.w, 1e-4);

				float tileMax = FrameBuffer::NearPlaneDepth();
				[unroll] for (int y = 0; y < 3; ++y)
				{
					[unroll] for (int x = 0; x < 3; ++x)
					{
						if (t0.x + x <= t1.x && t0.y + y <= t1.y) {
							const int2 t = clamp(t0 + int2(x, y), int2(xMin, 0), int2(xMax, dimL.y - 1));
							tileMax = FrameBuffer::FartherDepth(tileMax, HiZ.Load(int3(t, level)));
						}
					}
				}

				// Behind the farthest occluder of every covering tile means hidden. The tolerance absorbs
				// projection and depth error that would otherwise drop instances only marginally behind it.
				const float occluderDepth = FrameBuffer::FartherDepth(tileMax + OcclusionBias, tileMax - OcclusionBias);
				if (FrameBuffer::IsNearerDepth(occluderDepth, nearZ))
					return false;
			}
		}
	}

	float lodFade = 1.0;
	const float effFullPx = FullDetailPixelSize * pxScale;
	if (projPx < effFullPx) {
		const float t = saturate((effFullPx - projPx) / max(effFullPx - effMinPx, 1e-4));
		const float keep = lerp(1.0, LODMinKeep, t);
		const float h = RandFloat(rand.x);
		if (h > keep + LODFadeBand)
			return false;
		lodFade = saturate((keep + LODFadeBand - h) / LODFadeBand);
	}

	const float maxDist = sqrt(effMaxDistSq);
	const float edgeStart = maxDist * EdgeFadeStart;
	const float edgeFade = saturate((maxDist - dist) / max(maxDist - edgeStart, 1e-4));

	const float4 clip = mul(FrameBuffer::CameraViewProj[eyeIndex], float4(dv, 1.0));
	const float distFade = 1.0 - saturate((length(FrameBuffer::ToStandardClip(clip, FrameBuffer::IsReverseProjection(eyeIndex))) - AlphaParam1) / AlphaParam2);
	const float spawnFade = saturate((FadeNow - og.w) * FadeInTimeRcp);

	fade = distFade * spawnFade * lodFade * edgeFade;
	if (fade <= InvisibleFadeCull)
		return false;

	flags = (SimpleShadingPixelSize > 0.0 && projPx < SimpleShadingPixelSize) ? 2.0 : 0.0;

	// Both thresholds use the same hash so an instance crosses middle then far in order.
	const float h = RandFloat(rand.y);
	const float halfBand = MeshLODBandPx * 0.5;
	const float bandRcp = 1.0 / max(MeshLODBandPx, 1e-4);
	if (MidLODEnabled > 0.5 && h < saturate((MidLODPixelSize + halfBand - projPx) * bandRcp))
		tier = 1u;
	if (FarLODEnabled > 0.5 && h < saturate((FarLODPixelSize + halfBand - projPx) * bandRcp))
		tier = 2u;

	return true;
||||||| 12b261724
    const float a = 0.4 * (basis + timer);
    float sa, ca;
    sincos(a, sa, ca);
    const float t3 = 0.2 * cos(Math::PI * ca);
    const float t1 = sin(Math::PI * sa);
    const float t2 = sin(Math::TAU * sa);
    return (t1 + t2) * 0.3 + t3;
=======
	return float4(dot(CameraViewProj[0].xyz, v), dot(CameraViewProj[1].xyz, v), dot(CameraViewProj[2].xyz, v), dot(CameraViewProj[3].xyz, v));
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
}

<<<<<<< HEAD
void StoreSurvivor(uint eyeIndex, uint tier, uint4 raw0, uint4 raw1, float4 e0, float4 e1,
	float4 currentResponse, float4 previousResponse, float4 currentCollision, float4 previousCollision)
||||||| 12b261724
[numthreads(64, 1, 1)] void main(uint3 tid : SV_DispatchThreadID)
=======
// Return the LOD tier or TierCulled and populate the output record for surviving instances.
uint CullInstance(uint idx, GrassInstance::CullGroup group, out float2 wind, out float fade, out uint flags)
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
{
<<<<<<< HEAD
	// Scaled by 4 so it clears the far-shading flag already packed into e1.w.
	const float4 e1Tier = float4(e1.xyz, e1.w + 4.0 * (float)tier);
||||||| 12b261724
    const uint compactIdx = tid.x;
    if (compactIdx >= InstanceCount || SliceCount == 0)
        return;
=======
	wind = 0.0;
	fade = 0.0;
	flags = 0;
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb

<<<<<<< HEAD
	// eyeSlotBase must match the StartInstanceLocation baked into eye 1's args block: SV_InstanceID
	// excludes it, so the SRV index needs it added explicitly.
	static const uint kArgsBlockStride = 32;
	const uint eyeByteOffset = eyeIndex * kArgsBlockStride;
	const uint eyeSlotBase = eyeIndex * OutputCapacityPerEye;
	const uint lodCounterEyeOffset = eyeIndex * kLODCounterEyeStride;
||||||| 12b261724
    // Find the source slice containing this compacted index.
    uint lo = 0;
    uint hi = SliceCount - 1;
    [loop] while (lo < hi)
    {
        const uint mid = (lo + hi + 1) >> 1;
        if (SliceTable[SliceTableOffset + mid].y <= compactIdx)
            lo = mid;
        else
            hi = mid - 1;
    }
    const uint2 slice = SliceTable[SliceTableOffset + lo];
=======
	float4 data1, data2, data3, data4;
	GrassInstance::Decode(Instances.Load4(idx * GrassInstance::Stride), data1, data2, data3, data4);
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb

<<<<<<< HEAD
	uint slot;
	if (tier == 2) {
		LODCounters.InterlockedAdd(lodCounterEyeOffset + FarLODCountOffset, 1, slot);
		slot += eyeSlotBase;
		FarLODCompacted.Store4(slot * 32, raw0);
		FarLODCompacted.Store4(slot * 32 + 16, raw1);
		FarLODExtras[slot * 6 + 0] = e0;
		FarLODExtras[slot * 6 + 1] = e1Tier;
		FarLODExtras[slot * 6 + 2] = currentResponse;
		FarLODExtras[slot * 6 + 3] = previousResponse;
		FarLODExtras[slot * 6 + 4] = currentCollision;
		FarLODExtras[slot * 6 + 5] = previousCollision;
	} else if (tier == 1) {
		LODCounters.InterlockedAdd(lodCounterEyeOffset + MiddleLODCountOffset, 1, slot);
		slot += eyeSlotBase;
		MidLODCompacted.Store4(slot * 32, raw0);
		MidLODCompacted.Store4(slot * 32 + 16, raw1);
		MidLODExtras[slot * 6 + 0] = e0;
		MidLODExtras[slot * 6 + 1] = e1Tier;
		MidLODExtras[slot * 6 + 2] = currentResponse;
		MidLODExtras[slot * 6 + 3] = previousResponse;
		MidLODExtras[slot * 6 + 4] = currentCollision;
		MidLODExtras[slot * 6 + 5] = previousCollision;
	} else {
		Counter.InterlockedAdd(eyeByteOffset, 1, slot);
		slot += eyeSlotBase;
		Compacted.Store4(slot * 32, raw0);
		Compacted.Store4(slot * 32 + 16, raw1);
		Extras[slot * 6 + 0] = e0;
		Extras[slot * 6 + 1] = e1;
		Extras[slot * 6 + 2] = currentResponse;
		Extras[slot * 6 + 3] = previousResponse;
		Extras[slot * 6 + 4] = currentCollision;
		Extras[slot * 6 + 5] = previousCollision;
	}
}

[numthreads(64, 1, 1)] void main(uint3 tid : SV_DispatchThreadID) {
	const uint compactIdx = tid.x;
	if (compactIdx >= InstanceCount || SliceCount == 0)
		return;

	// Find the source slice containing this compacted index.
	uint lo = 0;
	uint hi = SliceCount - 1;
	[loop] while (lo < hi)
	{
		const uint mid = (lo + hi + 1) >> 1;
		if (SliceTable[SliceTableOffset + mid].y <= compactIdx)
			lo = mid;
		else
			hi = mid - 1;
	}
	const uint2 slice = SliceTable[SliceTableOffset + lo];

	// Use the source index to keep dither decisions stable across slice changes.
	const uint idx = slice.x + (compactIdx - slice.y);

	// Generate independent density and LOD dither values with one hash.
	const uint2 rand = Random::pcg2d(uint2(idx, 0u));

	const uint base = idx * 32;
	const uint4 raw0 = Instances.Load4(base);
	const uint4 raw1 = Instances.Load4(base + 16);

	const float2 localXY = float2(f16tof32(raw0.x & 0xFFFF), f16tof32(raw0.x >> 16));
	const float localZ = f16tof32(raw0.y & 0xFFFF);

	const float4 og = Origins[idx];
	const float3 world = float3(localXY, localZ) + og.xyz;

	const float sizeVariance = f16tof32(raw1.z >> 16);
	const float4 e0 = float4(og.xyz, IsComplex);

	float fade0, flags0;
	uint tier0;
	const bool visible0 = CullEye(0u, world, og, raw0, raw1, rand, sizeVariance, fade0, flags0, tier0);
#if defined(VR)
	float fade1 = 0.0, flags1 = 0.0;
	uint tier1 = 0u;
	const bool visible1 = CullEye(1u, world, og, raw0, raw1, rand, sizeVariance, fade1, flags1, tier1);

	if (!visible0 && !visible1)
		return;
#else
	if (!visible0)
		return;
#endif

	// Both eyes share the wind and collision response while retaining independent visibility and draw records.
	float4 currentResponse, previousResponse;
	float2 flutter;
	GrassWindResponse::Sample(localXY, world.xy, world.xy, Math::IdentityMatrix, Math::IdentityMatrix,
		TimeBase * WavePeriod, PrevTimeBase * WavePeriod, currentResponse, previousResponse, flutter);
	float4 currentCollision = 0.0;
	float4 previousCollision = 0.0;
#ifdef GRASS_COLLISION
	const float3 currentCollisionRoot = world - FrameBuffer::CameraPosAdjust[0].xyz;
	const float3 previousRoot = world - FrameBuffer::CameraPreviousPosAdjust[0].xyz;
	currentCollision = GrassCollision::SampleCurrentDeformation(currentCollisionRoot.xy);
	previousCollision = GrassCollision::SamplePreviousDeformation(previousRoot.xy);
	currentCollision.w = smoothstep(GrassCollision::FADE_DISTANCE, 0.0, length(currentCollisionRoot));
	previousCollision.w = smoothstep(GrassCollision::FADE_DISTANCE, 0.0, length(previousRoot));
#endif

	if (visible0)
		StoreSurvivor(0u, tier0, raw0, raw1, e0, float4(flutter, fade0, flags0),
			currentResponse, previousResponse, currentCollision, previousCollision);
#if defined(VR)
	if (visible1)
		StoreSurvivor(1u, tier1, raw0, raw1, e0, float4(flutter, fade1, flags1),
			currentResponse, previousResponse, currentCollision, previousCollision);
#endif
||||||| 12b261724
    // Use the source index to keep dither decisions stable across slice changes.
    const uint idx = slice.x + (compactIdx - slice.y);

    // Generate independent density and LOD dither values with one hash.
    const uint2 rand = Random::pcg2d(uint2(idx, 0u));

    const uint base = idx * 32;
    const uint4 raw0 = Instances.Load4(base);
    const uint4 raw1 = Instances.Load4(base + 16);

    const float2 localXY = float2(f16tof32(raw0.x & 0xFFFF), f16tof32(raw0.x >> 16));
    const float localZ = f16tof32(raw0.y & 0xFFFF);

    const float4 og = Origins[idx];
    const float3 world = float3(localXY, localZ) + og.xyz;

    const float3 dv = world - FrameBuffer::CameraPosAdjust.xyz;
    const float distSq = dot(dv, dv);

    const float dist = sqrt(distSq);

    // Ramp the mesh-cost bias in beyond CostBiasStartDist.
    const float costRamp = saturate((dist - CostBiasStartDist) / max(CostBiasStartDist, 1e-4));
    const float effCostBias = MeshCostBias * costRamp;

    const float dScale = lerp(1.0, DistScale, effCostBias);
    const float effMaxDistSq = MaxDistSq * dScale * dScale;
    if (distSq > effMaxDistSq)
        return;

	[unroll]
    for (uint p = 0; p < 6; ++p)
    {
        if (dot(FrustumPlanes[p].xyz, world) - FrustumPlanes[p].w < 0.0)
            return;
    }

    // Conservatively approximate the vertex shader's ScaleMask-based size variation.
    const float sizeVariance = f16tof32(raw1.z >> 16);
    const float instanceRadius = ModelRadius * (1.0 + max(sizeVariance, 0.0));

    const float projPx = (instanceRadius / dist) * ProjScale;
    const float pxScale = lerp(1.0, MinPixelScale, effCostBias);
    const float effMinPx = MinPixelSize * pxScale;
    if (projPx < effMinPx)
        return;

    if (HiZEnabled > 0.5)
    {
        // Transform the model-space bounding-sphere center into instance space.
        const float3 rot0 = float3(f16tof32(raw0.z & 0xFFFF), f16tof32(raw0.z >> 16), f16tof32(raw0.w & 0xFFFF));
        const float3 rot1 = float3(f16tof32(raw1.x & 0xFFFF), f16tof32(raw1.x >> 16), f16tof32(raw1.y & 0xFFFF));
        const float3 rot2 = float3(f16tof32(raw1.z & 0xFFFF), f16tof32(raw0.w >> 16), f16tof32(raw1.y >> 16));

        const float3 msCentre = BoundCenter * (1.0 + max(sizeVariance, 0.0));
        const float3 dvC = dv + float3(dot(rot0, msCentre), dot(rot1, msCentre), dot(rot2, msCentre));

        const float occRadius = instanceRadius + length(BoundCenter) * abs(sizeVariance);
        const float distC = max(length(dvC), 1e-4);
        const float projPxOcc = (occRadius / distC) * ProjScale;

        const float4 clipC = mul(FrameBuffer::CameraViewProj, float4(dvC, 1.0));
        if (clipC.w > 0.0)
        {
            const float2 uv = (clipC.xy / clipC.w) * float2(0.5, -0.5) + 0.5;
            const float2 tc = uv * HiZSize;
            // Projected radius in level-0 Hi-Z texels.
            const float rT = projPxOcc / HiZTexelPixels;

            // Select a mip coarse enough for the 3x3 footprint to cover the sphere.
            const float wantLevel = ceil(log2(max(2.0 * rT, 1.0)));

            // Skip occlusion when no available mip can cover the sphere safely.
            [branch] if (wantLevel <= HiZMipCount - 1.0)
            {
            const int level = (int)wantLevel;
            const float scale = exp2((float)level);
            const float2 tcL = tc / scale;
            const float rTL = rT / scale;
            const int2 dimL = max(int2(ceil(HiZSize / scale)), int2(1, 1));

            const int2 t0 = int2(floor(tcL - rTL));
            const int2 t1 = int2(floor(tcL + rTL));

            // Test the sphere's nearest point; a camera inside it yields a non-occluded depth.
            const float3 dvNear = dvC * (max(distC - occRadius, 0.0) / distC);
            const float4 clipN = mul(FrameBuffer::CameraViewProj, float4(dvNear, 1.0));
            const float nearZ = clipN.z / max(clipN.w, 1e-4);

            float tileMax = 0.0;
			[unroll] for (int y = 0; y < 3; ++y)
            {
				[unroll] for (int x = 0; x < 3; ++x)
                {
                    if (t0.x + x <= t1.x && t0.y + y <= t1.y)
                    {
                        const int2 t = clamp(t0 + int2(x, y), int2(0, 0), dimL - 1);
                        tileMax = max(tileMax, HiZ.Load(int3(t, level)));
                    }
                }
            }

            // Cull only when the sphere is behind every sampled tile, allowing for depth error.
            if (nearZ > tileMax + OcclusionBias)
                return;
            }
        }
    }

    float lodFade = 1.0;
    const float effFullPx = FullDetailPixelSize * pxScale;
    if (projPx < effFullPx)
    {
        const float t = saturate((effFullPx - projPx) / max(effFullPx - effMinPx, 1e-4));
        const float keep = lerp(1.0, LODMinKeep, t);
        const float h = RandFloat(rand.x);
        if (h > keep + LODFadeBand)
            return;
        lodFade = saturate((keep + LODFadeBand - h) / LODFadeBand);
    }

    const float maxDist = sqrt(effMaxDistSq);
    const float edgeStart = maxDist * EdgeFadeStart;
    const float edgeFade = saturate((maxDist - dist) / max(maxDist - edgeStart, 1e-4));

    const float4 clip = mul(FrameBuffer::CameraViewProj, float4(dv, 1.0));
    const float distFade = 1.0 - saturate((length(clip.xyz) - AlphaParam1) / AlphaParam2);
    const float spawnFade = saturate((FadeNow - og.w) * FadeInTimeRcp);
    
    const float fade = distFade * spawnFade * lodFade * edgeFade;
    if (fade <= InvisibleFadeCull)
        return;

    const float basis = (localXY.x + localXY.y) * -0.0078125;

    const float4 e0 = float4(og.xyz, IsComplex);
    
    const float collisionFlag = (distSq < CollisionDistSq) ? 1.0 : 0.0;
    const float farFlag = (SimpleShadingPixelSize > 0.0 && projPx < SimpleShadingPixelSize) ? 2.0 : 0.0;

    const float4 e1 = float4(WindScalar(basis, TimeBase * WavePeriod), WindScalar(basis, PrevTimeBase * WavePeriod), fade, collisionFlag + farFlag);

    // Dither both LOD transitions with the same value to keep tier changes gradual and ordered.
    const float h = RandFloat(rand.y);
    const float halfBand = MeshLODBandPx * 0.5;
    const float bandRcp = 1.0 / max(MeshLODBandPx, 1e-4);

    uint tier = 0;
    if (MidLODEnabled > 0.5 && h < saturate((MidLODPixelSize + halfBand - projPx) * bandRcp))
        tier = 1;
    if (FarLODEnabled > 0.5 && h < saturate((FarLODPixelSize + halfBand - projPx) * bandRcp))
        tier = 2;

    // Store the tier above the two low flag bits in e1.w.
    const float4 e1Tier = float4(e1.xyz, e1.w + 4.0 * (float)tier);

    uint slot;
    if (tier == 2)
    {
        LODCounters.InterlockedAdd(FarLODCountOffset, 1, slot);
        FarLODCompacted.Store4(slot * 32, raw0);
        FarLODCompacted.Store4(slot * 32 + 16, raw1);
        FarLODExtras[slot * 2 + 0] = e0;
        FarLODExtras[slot * 2 + 1] = e1Tier;
    }
    else if (tier == 1)
    {
        LODCounters.InterlockedAdd(MiddleLODCountOffset, 1, slot);
        MidLODCompacted.Store4(slot * 32, raw0);
        MidLODCompacted.Store4(slot * 32 + 16, raw1);
        MidLODExtras[slot * 2 + 0] = e0;
        MidLODExtras[slot * 2 + 1] = e1Tier;
    }
    else
    {
        Counter.InterlockedAdd(0, 1, slot);
        Compacted.Store4(slot * 32, raw0);
        Compacted.Store4(slot * 32 + 16, raw1);
        Extras[slot * 2 + 0] = e0;
        Extras[slot * 2 + 1] = e1;
    }
=======
	const float2 localXY = data1.xy;
	const float localZ = data1.z;

	const float3 world = float3(localXY, localZ) + group.Origin;

	const float3 dv = world - CameraPos.xyz;
	const float distSq = dot(dv, dv);
	if (distSq > MaxDistSq)
		return TierCulled;

	const float dist = sqrt(distSq);

	// Bounds include the mesh's offset from its root, scale variation and deformation padding.
	const float sizeVariance = data4.y;
	const float boundsRadius = BoundExtent * max(1.0, abs(1.0 + sizeVariance)) + 128.0;
	[unroll] for (uint p = 0; p < 6; ++p)
	{
		if (dot(FrustumPlanes[p].xyz, world) - FrustumPlanes[p].w < -boundsRadius)
			return TierCulled;
	}

	// Conservatively approximate the vertex shader's ScaleMask-based size variation.
	const float instanceRadius = ModelRadius * (1.0 + max(sizeVariance, 0.0));

	// Use camera depth so pixel sizing stays consistent across the view.
	const float clipW = dot(CameraViewProj[3].xyz, dv) + CameraViewProj[3].w;
	const float projPx = (instanceRadius * ProjectionWLength / max(clipW, 1e-4)) * ProjScale;

	// Hash world positions so compaction preserves density and LOD choices.
	const uint2 rand = Random::pcg2d(asuint(world.xy) ^ asuint(world.z));

	// Choose the mesh before applying its distance and density penalties.
	const float h = RandFloat(rand.y);
	const float halfBand = MeshLODBandPx * 0.5;
	const float bandRcp = 1.0 / max(MeshLODBandPx, 1e-4);

	uint tier = 0;
	if (MidLODEnabled > 0.5 && h < saturate((MidLODPixelSize + halfBand - projPx) * bandRcp))
		tier = 1;
	if (FarLODEnabled > 0.5 && h < saturate((FarLODPixelSize + halfBand - projPx) * bandRcp))
		tier = 2;

	const float meshCost = tier == 1 ? MidLODMeshCost : (tier == 2 ? FarLODMeshCost : MinPixelScale);
	const float costRamp = saturate((dist - CostBiasStartDist) / max(CostBiasStartDist, 1e-4));
	const float effCostBias = MeshCostBias * costRamp;
	const float dScale = lerp(1.0, tier == 0 ? DistScale : rcp(meshCost), effCostBias);
	const float effMaxDistSq = MaxDistSq * dScale * dScale;
	if (distSq > effMaxDistSq)
		return TierCulled;

	const float pxScale = lerp(1.0, meshCost, effCostBias);
	const float effMinPx = MinPixelSize * pxScale;
	if (projPx < effMinPx)
		return TierCulled;

	float lodFade = 1.0;
	const float effFullPx = FullDetailPixelSize * pxScale;
	if (projPx < effFullPx) {
		const float distanceRamp = saturate(dist * DensityDistanceRcp);
		const float t = saturate((effFullPx - projPx) / max(effFullPx - effMinPx, 1e-4)) * distanceRamp;
		const float keep = lerp(1.0, LODMinKeep, t);
		const float densityRandom = RandFloat(rand.x);
		if (densityRandom > keep + LODFadeBand)
			return TierCulled;
		lodFade = saturate((keep + LODFadeBand - densityRandom) / LODFadeBand);
	}

	const float maxDist = sqrt(effMaxDistSq);
	const float edgeStart = maxDist * EdgeFadeStart;
	const float edgeFade = saturate((maxDist - dist) / max(maxDist - edgeStart, 1e-4));

	const float4 clipOffset = float4(CameraViewProj[0].w, CameraViewProj[1].w, CameraViewProj[2].w, CameraViewProj[3].w);
	const float4 clip = ProjectDirection(dv) + clipOffset;
	const float distFade = 1.0 - saturate((length(clip.xyz) - AlphaParam1) / AlphaParam2);
	const float spawnFade = saturate((FadeNow - group.FadeStart) * FadeInTimeRcp);

	fade = distFade * spawnFade * lodFade * edgeFade;
	if (fade <= InvisibleFadeCull)
		return TierCulled;

	// Apply density and fade rejection before sampling depth.
	if (HiZEnabled > 0.5) {
		const float occRadius = boundsRadius;
		const float4 clipC = clip;
		const float minW = clipC.w - occRadius * ProjectionWLength;

		// A sphere crossing the camera plane has no finite projected bounds.
		[branch] if (minW > 1e-4)
		{
			const float3 ndc = clipC.xyz / clipC.w;
			// Bound the NDC change by the sphere radius times the projection gradient, divided by the minimum positive W.
			const float gradientX = length(CameraViewProj[0].xyz - ndc.x * CameraViewProj[3].xyz);
			const float gradientY = length(CameraViewProj[1].xyz - ndc.y * CameraViewProj[3].xyz);
			const float gradientZ = length(CameraViewProj[2].xyz - ndc.z * CameraViewProj[3].xyz);

			const float3 extent = (occRadius / minW) * float3(gradientX, gradientY, gradientZ);
			const float3 ndcLo = ndc - extent - 1e-5;
			const float3 ndcHi = ndc + extent + 1e-5;
			const float2 tcLo = saturate(float2(ndcLo.x, -ndcHi.y) * 0.5 + 0.5) * HiZSize;
			const float2 tcHi = saturate(float2(ndcHi.x, -ndcLo.y) * 0.5 + 0.5) * HiZSize;

			float wantLevel = ceil(log2(max(max(tcHi.x - tcLo.x, tcHi.y - tcLo.y), 1.0)));
			float scale = exp2(wantLevel);
			int2 t0 = int2(floor(tcLo / scale));
			int2 t1 = int2(floor(tcHi / scale));

			[branch] if (any(t1 - t0 > 1))
			{
				wantLevel += 1.0;
				scale *= 2.0;
				t0 = int2(floor(tcLo / scale));
				t1 = int2(floor(tcHi / scale));
			}

			// Near-plane intersections and footprints beyond the available pyramid remain visible.
			[branch] if (ndcLo.z > 0.0 && wantLevel <= HiZMipCount - 1.0)
			{
				const int level = (int)wantLevel;
				const int2 dimL = max(int2(ceil(HiZSize / scale)), int2(1, 1));
				const int2 lo = clamp(t0, int2(0, 0), dimL - 1);
				const int2 hi = clamp(t1, int2(0, 0), dimL - 1);
				const float tileMax = max(max(HiZ.Load(int3(lo, level)), HiZ.Load(int3(hi.x, lo.y, level))),
					max(HiZ.Load(int3(lo.x, hi.y, level)), HiZ.Load(int3(hi, level))));

				if (ndcLo.z > tileMax + OcclusionBias)
					return TierCulled;
			}
		}
	}

	flags = tier << GrassInstance::FlagTierShift;
	if (distSq < CollisionDistSq)
		flags |= GrassInstance::FlagCollision;
	if (SimpleShadingPixelSize > 0.0 && projPx < SimpleShadingPixelSize)
		flags |= GrassInstance::FlagSimpleShading;

	wind = float2(GrassInstance::WindScalar(localXY, TimeBase * WavePeriod), GrassInstance::WindScalar(localXY, PrevTimeBase * WavePeriod));

	return tier;
}

[numthreads(64, 1, 1)] void main(uint3 groupId : SV_GroupID, uint threadIdx : SV_GroupIndex) {
	if (threadIdx < TierCount)
		GroupTierCount[threadIdx] = 0;

	GroupMemoryBarrierWithGroupSync();

	uint tier = TierCulled;
	uint groupIndex = 0;
	float2 wind = 0.0;
	float fade = 0.0;
	uint flags = 0;

	const uint groupIdx = groupId.y * DispatchWidth + groupId.x;
	if (groupIdx < GroupCount) {
		// Every thread in the group follows the same search for its visible source run.
		uint lo = 0;
		uint hi = SliceCount - 1;
		[loop] while (lo < hi)
		{
			const uint mid = (lo + hi + 1) >> 1;
			if (SliceTable[SliceTableOffset + mid].y <= groupIdx)
				lo = mid;
			else
				hi = mid - 1;
		}

		const uint2 run = SliceTable[SliceTableOffset + lo];
		groupIndex = run.x + (groupIdx - run.y);
		const GrassInstance::CullGroup group = Groups[groupIndex];

		if (threadIdx < group.Count)
			tier = CullInstance(group.SourceStart + threadIdx, group, wind, fade, flags);
	}

	uint localSlot = 0;
	if (tier != TierCulled)
		InterlockedAdd(GroupTierCount[tier], 1, localSlot);

	GroupMemoryBarrierWithGroupSync();

	if (threadIdx < TierCount && GroupTierCount[threadIdx] != 0) {
		uint base;
		Args.InterlockedAdd(ArgsOffset + threadIdx * ArgsStride + 4, GroupTierCount[threadIdx], base);
		GroupTierBase[threadIdx] = base;
	}

	GroupMemoryBarrierWithGroupSync();

	if (tier == TierCulled)
		return;

	const uint slot = OutputOffset + tier * TierStride + GroupTierBase[tier] + localSlot;
	const uint sourceIndex = Groups[groupIndex].SourceStart + threadIdx;
	const uint4 record = uint4(sourceIndex, GrassInstance::PackRecord(groupIndex, fade, flags), asuint(wind));
	Compacted.Store4(slot * GrassInstance::RecordStride, record);
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
}

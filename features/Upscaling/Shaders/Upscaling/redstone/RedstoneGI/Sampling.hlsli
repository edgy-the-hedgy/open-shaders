#ifndef REDSTONE_SAMPLING
#define REDSTONE_SAMPLING
struct Surface
{
	uint2 pixel;
	float2 uv;
	float depth;
	float3 position;
	float3 normal;
	float3 view;
	float2 noise;
	float screenRadius;
	float minimumSample;
	float adaptiveScale;
};
struct Slice
{
	float2 omega;
	float3 axis;
	float normalAngle;
	float projectedNormalLength;
	float azimuthWeight;
	float logRadius;
};
struct SurfaceSample
{
	float2 uv;
	float3 position;
	float3 direction;
	float distance;
	float2 giAngles;
	float radianceMip;
};
uint CoverageMask(float2 range)
{
	uint first = min(32u, (uint)round(saturate(min(range.x, range.y)) * 32.0));
	uint last = min(32u, (uint)round(saturate(max(range.x, range.y)) * 32.0));
	if (first >= last || first == 32u) return 0;
	uint count = last - first;
	return count == 32u ? 0xffffffffu : ((1u << count) - 1u) << first;
}
float DiffuseArcPrimitive(float angle, float2 normalSinCos)
{
	float sine, cosine;
	sincos(angle, sine, cosine);
	return 0.5 * (normalSinCos.y * sine * sine + normalSinCos.x * (angle - sine * cosine));
}
float DiffuseCoverage(uint bits, Slice slice)
{
	const float sectorAngle = Math::PI / 32.0;
	float2 normalSinCos;
	sincos(slice.normalAngle, normalSinCos.x, normalSinCos.y);
	float weight = 0;
	while (bits) {
		uint first = firstbitlow(bits);
		uint count = firstbitlow(~(bits >> first));
		count = min(count, 32u - first);
		float lower = slice.normalAngle - (float(first + count) * sectorAngle - Math::HALF_PI);
		float upper = slice.normalAngle - (float(first) * sectorAngle - Math::HALF_PI);
		float lowerIntegral = DiffuseArcPrimitive(lower, normalSinCos);
		float upperIntegral = DiffuseArcPrimitive(upper, normalSinCos);
		weight += lower < 0 && upper > 0 ? abs(lowerIntegral) + abs(upperIntegral) : abs(upperIntegral - lowerIntegral);
		bits = count == 32u ? 0u : bits & ~(((1u << count) - 1u) << first);
	}
	return weight * slice.projectedNormalLength * slice.azimuthWeight;
}
Surface BuildSurface(uint2 pixel, float depth, float3 normal)
{
	Surface result;
	result.pixel = pixel;
	result.uv = (pixel + 0.5) * RCP_OUT_FRAME_DIM;
	result.depth = depth;
	result.position = ScreenToViewPosition(result.uv, depth);
	result.normal = normal;
	result.view = normalize(-result.position);
	uint2 noisePixel = pixel % 128u + uint2(0, (FrameIndex % 64u) * 128u);
	result.noise = srcNoise.Load(int3(noisePixel, 0));
	float viewPixelWidth = depth * NDCToViewMul.x * RCP_OUT_FRAME_DIM.x;
	result.screenRadius = max(MinScreenRadius, EffectRadius / max(viewPixelWidth, 1e-6));
	result.minimumSample = 1.3 / result.screenRadius;
	result.adaptiveScale = 1;
#ifdef ADAPTIVE_SAMPLING
	const int2 offsets[4] = { int2(-1,0), int2(1,0), int2(0,-1), int2(0,1) };
	float depthVariance = 0;
	float normalVariance = 0;
	float2 frameScale = FrameDim * RcpTexDim;
	[unroll] for (uint i = 0; i < 4; ++i) {
	 int2 neighbour = clamp(int2(pixel) + offsets[i], 0, int2(OUT_FRAME_DIM) - 1);
	 float d = READ_DEPTH(srcWorkingDepth, neighbour);
	 float2 uv = (neighbour + 0.5) * RCP_OUT_FRAME_DIM;
	 float3 n = GBuffer::DecodeNormal(FULLRES_LOAD(srcNormalRoughness, neighbour, uv * frameScale, samplerLinearClamp).xy);
	 depthVariance += abs(d - depth) / max(depth, 1.0) * 0.25;
	 normalVariance += (1.0 - saturate(dot(normal, n))) * 0.25;
	}
	float variance = saturate(depthVariance * 6.0 + normalVariance * 2.0);
	result.adaptiveScale = min(lerp(1.0, 0.45, GetDepthFade(depth)), lerp(0.55, 1.0, variance));
#endif
	return result;
}
uint AdaptiveSteps(Surface surface, uint maximum)
{
	return max(1u, min(maximum, (uint)round(maximum * surface.adaptiveScale)));
}
Slice BuildSlice(Surface surface, uint index, uint count)
{
	Slice result;
	float phi = Math::PI * (index + surface.noise.x) / count;
	float2 direction;
	sincos(phi, direction.y, direction.x);
	float2 screenDirection = float2(direction.x, -direction.y);
	result.omega = screenDirection * surface.screenRadius;
	result.logRadius = 0.5 * log2(max(dot(result.omega, result.omega), 1e-6));
	float3 viewPixelX = ScreenToViewPosition(surface.uv + float2(RCP_OUT_FRAME_DIM.x, 0), surface.depth) - surface.position;
	float3 viewPixelY = ScreenToViewPosition(surface.uv - float2(0, RCP_OUT_FRAME_DIM.y), surface.depth) - surface.position;
	float3 viewDirection = viewPixelX * direction.x + viewPixelY * direction.y;
	float3 ortho = viewDirection - dot(viewDirection, surface.view) * surface.view;
	result.azimuthWeight = abs(dot(cross(viewPixelX, viewPixelY), surface.view)) / max(dot(ortho, ortho), 1e-12);
	result.axis = normalize(cross(ortho, surface.view));
	float3 projected = surface.normal - result.axis * dot(surface.normal, result.axis);
	result.projectedNormalLength = length(projected);
	float cosine = saturate(dot(projected, surface.view) * rsqrt(max(dot(projected, projected), 1e-6)));
	result.normalAngle = sign(dot(ortho, projected)) * acos(cosine);
	return result;
}
float SampleOffset(Surface surface, uint slice, uint step, uint count)
{
	float noise = frac(surface.noise.y + slice * 0.6180339887498948482);
	float fraction = (step + noise) / count;
	return fraction * fraction + surface.minimumSample;
}
bool LoadSurfaceSample(Surface surface, Slice slice, int side, float offset, out SurfaceSample surfaceSample)
{
	surfaceSample = (SurfaceSample)0;
	float2 pixel = surface.pixel + 0.5 + slice.omega * offset * side;
	surfaceSample.uv = pixel * RCP_OUT_FRAME_DIM;
	if (any(surfaceSample.uv < 0) || any(surfaceSample.uv >= 1)) return false;
	float mip = clamp(log2(max(offset, 1e-6)) + slice.logRadius - 3.3, 0, 4);
	float2 frameScale = FrameDim * RcpTexDim;
	float depth = srcWorkingDepth.SampleLevel(samplerPointClamp, surfaceSample.uv * frameScale, max(mip, RES_MIP));
	if (!isfinite(depth) || depth <= 0) return false;
	surfaceSample.position = ScreenToViewPosition(surfaceSample.uv, depth);
	float3 delta = surfaceSample.position - surface.position;
	surfaceSample.distance = length(delta);
	if (surfaceSample.distance <= 1e-6) return false;
	surfaceSample.direction = delta / surfaceSample.distance;
	float angle = acos(clamp(dot(surfaceSample.direction, surface.view), -1, 1));
	float backGI = acos(clamp(dot(normalize(delta - surface.view * 300.0), surface.view), -1, 1));
	surfaceSample.giAngles = -side * (side == -1 ? float2(angle, backGI) : float2(backGI, angle));
	surfaceSample.radianceMip = min(4.0, max(mip, RES_MIP + 1.0));
	return true;
}
float3 SampleEmission(SurfaceSample surfaceSample)
{
	float2 frameScale = FrameDim * RcpTexDim;
	float3 normal = GBuffer::DecodeNormal(srcNormal.SampleLevel(samplerPointClamp, surfaceSample.uv * OUT_FRAME_SCALE, surfaceSample.radianceMip));
	if (dot(surfaceSample.position, normal) > 0) normal = -normal;
	float facing = max(0, -dot(normal, surfaceSample.direction));
	float3 radiance = srcRadiance.SampleLevel(samplerPointClamp, surfaceSample.uv * OUT_FRAME_SCALE, surfaceSample.radianceMip);
	return all(isfinite(radiance)) ? max(0, radiance) * facing : 0;
}
float DistanceCompensation(float distance)
{
	return max(0, 1.0 + GIDistanceCompensation * smoothstep(0, GICompensationMaxDist, distance));
}
#endif

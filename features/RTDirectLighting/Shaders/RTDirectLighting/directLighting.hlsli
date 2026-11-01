#include "directContract.hlsli"

static const float ReceiverRelativeTolerance = 1e-5;
static const float MaximumHalfRadiance = 65504;

// Independent integer hash streams for visibility sampling (no noise texture).
uint DirectVisibilityHash(uint value)
{
	value ^= value >> 16u;
	value *= 0x7feb352du;
	value ^= value >> 15u;
	value *= 0x846ca68bu;
	return value ^ (value >> 16u);
}
uint BakedRoomCode(uint instance, float3 position);

bool EvaluateIndirectLighting(EvaluatedSourceMaterial material, float3 position, float3 viewDirection, float3 geometricNormal,
	float footprint, uint2 pixel, out float4 diffuse, out float4 specular);

int LightRoomAdmission(float receiverCode, uint lightIndex)
{
	uint snapshotBase = lightIndex * LightSnapshotTexels;
	uint flags = asuint(LightSnapshot.Load(int3(snapshotBase + 5, 0, 0)).x);
	return (flags & POINT_LIGHT_FLAG_PORTAL_STRICT) == 0 ? 1 : ReceiverRoom::Admission(receiverCode, asuint(LightSnapshot.Load(int3(snapshotBase + 4, 0, 0))));
}

bool TraceDirectVisibility(float3 origin, float3 direction, float distance, out bool visible)
{
	visible = false;
	RayDesc ray;
	ray.Origin = origin;
	ray.Direction = direction;
	ray.TMin = MinimumDistance;
	ray.TMax = distance - MinimumDistance;
	if (ray.TMax <= ray.TMin) return false;
	RayQuery<RAY_FLAG_FORCE_NON_OPAQUE | RAY_FLAG_CULL_BACK_FACING_TRIANGLES> query;
	query.TraceRayInline(Scene, RAY_FLAG_NONE, 3, ray);
	bool valid = true;
	while (query.Proceed()) {
		if (query.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE) continue;
		uint instance = query.CandidateInstanceID();
		if (instance >= InstanceCount) { valid = false; continue; }
		if (GeometryBuffers[NonUniformResourceIndex(instance)].Load(ValidationOffsets[instance]) != 0) { valid = false; continue; }
		if (AdmitCandidate(instance, query.CandidatePrimitiveIndex(), query.CandidateTriangleBarycentrics())) query.CommitNonOpaqueTriangleHit();
	}
	visible = query.CommittedStatus() == COMMITTED_NOTHING;
	return valid;
}

// Single stochastic emitter sample shared by the independent mask pass and the
// legacy direct path (when no valid mask cache is available). Never scales radiance.
bool TracePreparedPointVisibility(uint lightIndex, float3 position, float3 lightPosition,
    float sourceRadius, float3 l, float lightDistance, uint2 pixel, uint sampleIndex, out bool visible)
{
    float3 target = lightPosition;
    if (DirectStochastic.x >= 0.5f && DirectStochastic.y > 0.0f) {
        const float emitterRadius = min(sourceRadius * DirectStochastic.y, lightDistance * 0.5f);
        if (emitterRadius > 0.0f) {
            const uint seed = DirectVisibilityHash(pixel.x * 0x9e3779b9u ^
                pixel.y * 0x85ebca6bu ^ FrameIndex * 0xc2b2ae35u ^
                lightIndex * 0x27d4eb2fu ^ sampleIndex * 0x165667b1u);
            const uint r0 = DirectVisibilityHash(seed ^ 0x68bc21ebu);
            const uint r1 = DirectVisibilityHash(seed ^ 0x02e5be93u);
            const float2 u = (float2(r0 >> 8u, r1 >> 8u) + 0.5f) / 16777216.0f;
            const float angle = u.y * 6.28318530718f;
            const float spread = emitterRadius * sqrt(u.x);
            const float3 tangent = normalize(cross(abs(l.z) < 0.9f ?
                float3(0, 0, 1) : float3(0, 1, 0), l));
            const float3 bitangent = cross(l, tangent);
            target += spread * (cos(angle) * tangent + sin(angle) * bitangent);
        }
    }
    // Offset toward the light instead of the surface normal; the ray terminates
    // at the sampled emitter point, not behind it.
    float3 origin = position + l * MinimumDistance;
    float3 shadowVector = target - origin;

    return TraceDirectVisibility(origin, normalize(shadowVector), length(shadowVector), visible);
}

// Normal mode 0 preserves the shipped flat response. Modes 1 and 2 reuse
// True PBR GGX/Schlick/Smith and Fresnel-weighted diffuse. Ray bias stays unchanged.
bool EvaluatePointLight(
    uint lightIndex,
    EvaluatedSourceMaterial material,
    float3 position,
    float3 viewDirection,
    float3 geometricNormal,
    float receiverCode,
    uint2 pixel,
    uint sampleIndex,
    out float3 diffuse,
    out float3 specular,
    out float4 unavailable)
{
    diffuse = 0;
    specular = 0;
    unavailable = 0;

    uint snapshotBase = lightIndex * LightSnapshotTexels;

    float4 source =
        LightSnapshot.Load(int3(snapshotBase + 6, 0, 0));

    float4 radiance =
        LightSnapshot.Load(int3(snapshotBase + 7, 0, 0));

    if (source.w != 1 || radiance.w != 1)
    {
        unavailable = UnavailableLight(
            LightSnapshot.Load(int3(snapshotBase + 5, 0, 0)));
        return false;
    }

    float3 lightPosition =
        LightSnapshot.Load(int3(snapshotBase + 2, 0, 0)).xyz
        + source.xyz;

    float3 lightVector = lightPosition - position;
    float lightDistance = length(lightVector);

    if (!isfinite(lightDistance) ||
        lightDistance <= MinimumDistance)
    {
        unavailable = Unavailable(InvalidSurface);
        return false;
    }

    float4 falloff =
        LightSnapshot.Load(int3(snapshotBase + 1, 0, 0));

    const float radius = min(max(falloff.x * PointLightFalloff.x, MinimumDistance), MaximumDistance);
    if (lightDistance >= radius)
        return true;

    float3 l = lightVector / lightDistance;

    // Room restrictions remain intact.
    int admission = LightRoomAdmission(receiverCode, lightIndex);

    if (admission < 0)
    {
        unavailable = UnavailableRoom(receiverCode);
        return false;
    }

    if (admission == 0)
        return true;

    if (lightDistance >= MaximumDistance)
    {
        unavailable = Unavailable(InvalidSurface);
        return false;
    }

    // Independent temporary inverse-square law: I / (d^2 + r0^2),
    // with a smooth finite-distance cutoff. No LLF attenuation flag.
    const float sourceRadius = max(falloff.w, MinimumDistance);
    const float fadeWidth = max(radius * PointLightFalloff.y, MinimumDistance);
    const float fadeT = saturate((radius - lightDistance) / fadeWidth);
    const float smoothCutoff = fadeT * fadeT * (3.0 - 2.0 * fadeT);
    const float cutoff = pow(smoothCutoff, PointLightFalloff.z);
    const float attenuation = cutoff / max(lightDistance * lightDistance +
        sourceRadius * sourceRadius, 1e-4);

    if (attenuation <= 0)
        return true;

    // Use the shadow prepass's per-light visibility when validated. Invalid
    // geometry retains the existing direct-light fallback instead of treating
    // a missing bit as darkness.
    bool visible = false;
    const uint bit = 1u << lightIndex;
    const uint2 cached = EnableVisibilityCache != 0u ? DirectVisibilityBits[pixel] : uint2(0u, 0u);
    if ((cached.y & bit) != 0u) {
        visible = (cached.x & bit) != 0u;
    } else if (!TracePreparedPointVisibility(lightIndex, position, lightPosition, sourceRadius, l,
        lightDistance, pixel, sampleIndex, visible)) {
        unavailable = Unavailable(InvalidSurface);
        return false;
    }
    if (!visible) return true;

    const float3 incidentRadiance = radiance.rgb * attenuation;
    if ((DirectNormalMode & 3u) == 0u) {
        diffuse = (1.0 - material.metallic) * material.baseColor *
            BRDF::Diffuse_Lambert() * incidentRadiance;
        specular = 0;
        return true;
    }

    const float3 n = normalize((DirectNormalMode & 3u) == 1u ? geometricNormal : material.normal);
    const float3 v = normalize(viewDirection);
    if (!all(isfinite(n)) || !all(isfinite(v))) {
        unavailable = Unavailable(InvalidSurface);
        return false;
    }
    const float3 halfVector = l + v;
    const float3 h = dot(halfVector, halfVector) > 1e-8 ? normalize(halfVector) : n;
    const float NdotL = clamp(dot(n, l), EPSILON_DOT_CLAMP, 1.0);
    const float NdotV = saturate(abs(dot(n, v)) + EPSILON_DOT_CLAMP);
    const float NdotH = saturate(dot(n, h));
    const float VdotH = saturate(dot(v, h));
    float3 fresnel;
    const float3 microfacet = PBR::SpecularMicrofacet(
        material.roughness, material.f0, NdotL, NdotV, NdotH, VdotH, fresnel);
    const float3 weightedIrradiance = incidentRadiance * NdotL;
    diffuse = (1.0 - material.metallic) * material.baseColor *
        (1.0 - fresnel) * BRDF::Diffuse_Lambert() * weightedIrradiance;
    specular = microfacet * weightedIrradiance;

    return true;
}

// Shadow visibility is a separate pass, independent of both direct radiance
// strength and the final raster/RT blend. Its normal/room/attenuation tests
// match the direct producer; unsupported areas remain neutral, never dark.
void EvaluateShadowMaskForSurface(uint instance, uint primitive, float2 barycentric, float3x4 objectToWorld,
    float3x4 worldToObject, float3 direction, float distance, float4 receiver, float footprint, uint2 pixel)
{
    MaterialRecord record = Materials[Surfaces[instance].material.x];
    if ((record.flags.x & 3u) != 3u || record.flags.w != 0u ||
        abs(distance - length(receiver.xyz)) > max(5.0f, distance * ReceiverRelativeTolerance)) return;
    EvaluatedSourceMaterial material;
    TriangleSurface triangle;
    if (!LoadSourceMaterial(instance, primitive, barycentric, objectToWorld, worldToObject, footprint, material) ||
        !LoadTriangle(instance, primitive, barycentric, triangle)) return;
    const float3 edge0 = mul((float3x3)objectToWorld, triangle.position1 - triangle.position0);
    const float3 edge1 = mul((float3x3)objectToWorld, triangle.position2 - triangle.position0);
    const float3 geometryNormal = normalize(cross(edge0, edge1));
    const uint normalMode = DirectNormalMode & 3u;
    const float3 surfaceNormal = normalize(normalMode == 1u ? geometryNormal : material.normal);
    if (!all(isfinite(geometryNormal)) || (normalMode != 0u && !all(isfinite(surfaceNormal)))) return;

    const uint count = Padding.z != 0u ? Padding.z : 1u;
    if (count > MaximumDirectLights) return;
    const float3 position = CameraOrigin + direction * distance;
    uint2 visibility = uint2(0u, 0u);
    float weightSum = 0.0f;
    float visibleWeight = 0.0f;
    bool valid = true;
    uint eligible = 0u;
    [loop] for (uint lightIndex = 0u; lightIndex < count; ++lightIndex) {
        const uint base = lightIndex * LightSnapshotTexels;
        const float4 source = LightSnapshot.Load(int3(base + 6u, 0, 0));
        const float4 radiance = LightSnapshot.Load(int3(base + 7u, 0, 0));
        if (source.w != 1.0f || radiance.w != 1.0f) continue;
        ++eligible;
        const uint bit = 1u << lightIndex; // 32 lights, including light 31.
        const float3 lightPosition = LightSnapshot.Load(int3(base + 2u, 0, 0)).xyz + source.xyz;
        const float3 delta = lightPosition - position;
        const float dist = length(delta);
        if (!isfinite(dist) || dist <= MinimumDistance) { valid = false; continue; }
        const float4 falloff = LightSnapshot.Load(int3(base + 1u, 0, 0));
        const float radius = min(max(falloff.x * PointLightFalloff.x, MinimumDistance), MaximumDistance);
        if (dist >= radius) { visibility |= uint2(bit, bit); continue; }
        const int admission = LightRoomAdmission(receiver.w, lightIndex);
        if (admission < 0) { valid = false; continue; }
        if (admission == 0) { visibility |= uint2(bit, bit); continue; }
        if (dist >= MaximumDistance) { valid = false; continue; }
        const float sourceRadius = max(falloff.w, MinimumDistance);
        const float fadeWidth = max(radius * PointLightFalloff.y, MinimumDistance);
        const float fadeT = saturate((radius - dist) / fadeWidth);
        const float smoothCutoff = fadeT * fadeT * (3.0f - 2.0f * fadeT);
        const float attenuation = pow(smoothCutoff, PointLightFalloff.z) /
            max(dist * dist + sourceRadius * sourceRadius, 1e-4f);
        if (attenuation <= 0.0f) { visibility |= uint2(bit, bit); continue; }
        const float3 l = delta / dist;
        const float facing = normalMode == 0u ? 1.0f : max(0.0f, dot(surfaceNormal, l));
        // Light contribution estimate is independent of DirectStrength and
        // Diffuse/SpecularStrength. Non-contributing lights cannot darken another.
        const float weight = dot(max(radiance.rgb, 0.0f), float3(0.2126f, 0.7152f, 0.0722f)) * attenuation * facing;
        bool visible;
        if (!TracePreparedPointVisibility(lightIndex, position, lightPosition, sourceRadius, l,
            dist, pixel, 0u, visible)) { valid = false; continue; }
        visibility.y |= bit;
        if (visible) visibility.x |= bit;
        if (isfinite(weight) && weight > 0.0f) {
            weightSum += weight;
            if (visible) visibleWeight += weight;
        } else if (!isfinite(weight)) valid = false;
    }
    DirectVisibilityBits[pixel] = visibility;
    if (valid && eligible > 0u && isfinite(weightSum) && weightSum > 1e-6f)
        DirectShadowMask[pixel] = float2(saturate(visibleWeight / weightSum), 1.0f);
    // Otherwise retain neutral (1,0) set by the pass for this pixel.
}

float4 EvaluateDirectLighting(uint instance, uint primitive, float2 barycentric, float3x4 objectToWorld,
	float3x4 worldToObject, float3 direction, float distance, float4 receiver, float footprint, uint mode, uint2 pixel)
{
	MaterialRecord record = Materials[Surfaces[instance].material.x];
	if ((record.flags.x & 3) != 3 || record.flags.w != 0) return Unavailable(UnsupportedMaterial);
	if (abs(distance - length(receiver.xyz)) > max(5.0f, distance * ReceiverRelativeTolerance)) return Unavailable(MismatchedDepth);
	EvaluatedSourceMaterial material;
	if (!LoadSourceMaterial(instance, primitive, barycentric, objectToWorld, worldToObject, footprint, material)) return Unavailable(InvalidSurface);
	TriangleSurface surface;
	if (!LoadTriangle(instance, primitive, barycentric, surface)) return Unavailable(InvalidSurface);
	float3 edge0 = mul((float3x3)objectToWorld, surface.position1 - surface.position0);
	float3 edge1 = mul((float3x3)objectToWorld, surface.position2 - surface.position0);
	float3 geometricNormal = normalize(cross(edge0, edge1));
	if (!all(isfinite(geometricNormal))) return Unavailable(InvalidSurface);
	uint lightCount = Padding.z != 0 ? Padding.z : 1;
	if (lightCount > MaximumDirectLights) return Unavailable(InvalidSurface);
	float3 position = CameraOrigin + direction * distance;
	float3 diffuseSum = 0, specularSum = 0;
	uint eligibleLightCount = 0;
	[loop] for (uint lightIndex = 0; lightIndex < lightCount; ++lightIndex) {
		const uint base = lightIndex * LightSnapshotTexels;
		// Unsupported baseline lights are not surface failures. Preserve the
		// raster fallback only if the entire submitted light set is unsupported.
		if (LightSnapshot.Load(int3(base + 6, 0, 0)).w != 1.0 ||
			LightSnapshot.Load(int3(base + 7, 0, 0)).w != 1.0) continue;
		++eligibleLightCount;
		float3 diffuse, specular;
		float4 unavailable;
		if (!EvaluatePointLight(lightIndex, material, position, -direction, geometricNormal, receiver.w, pixel, 0u, diffuse, specular, unavailable)) return unavailable;
		diffuseSum += diffuse;
		specularSum += specular;
	}
	if (eligibleLightCount == 0) return Unavailable(MissingLight);
	diffuseSum *= LightingStrengths.x * LightingStrengths.y;
	specularSum *= LightingStrengths.x * LightingStrengths.z;
	float3 result = mode == DirectDiffuseView ? diffuseSum : mode == DirectSpecularView ? specularSum : diffuseSum + specularSum + material.emission * LightingStrengths.w;
	if (!all(isfinite(result)) || any(result < 0)) return Unavailable(InvalidSurface);
	if (ExportDirectSignals != 0) {
		float3 emission = material.emission * LightingStrengths.w;
		if (any(diffuseSum > MaximumHalfRadiance) || any(specularSum > MaximumHalfRadiance) || any(emission > MaximumHalfRadiance)) return Unavailable(InvalidSurface);
		// The SDK expects demodulated first-bounce radiance. Albedo remains
		// separately available for RR and is applied once, after reconstruction.
		const float3 diffuseAlbedo = saturate((1.0 - material.metallic) * material.baseColor);
		const float3 shadingNormal = (DirectNormalMode & 3u) == 1u ? geometricNormal : material.normal;
		const float NdotV = saturate(abs(dot(normalize(shadingNormal), normalize(-direction))));
		const float2 envBRDF = BRDF::EnvBRDF(material.roughness, NdotV);
		const float3 specularAlbedo = saturate(material.f0 * envBRDF.x + envBRDF.y);
		const float3 unmodulatedDiffuse = min(diffuseSum / max(diffuseAlbedo, float3(1e-3, 1e-3, 1e-3)), MaximumHalfRadiance);
		const float3 unmodulatedSpecular = min(specularSum / max(specularAlbedo, float3(1e-3, 1e-3, 1e-3)), MaximumHalfRadiance);
		float3 normalOct = shadingNormal / max(abs(shadingNormal.x) + abs(shadingNormal.y) + abs(shadingNormal.z), 1e-6);
		float2 oct = normalOct.xy;
		if (normalOct.z < 0) oct = (1 - abs(oct.yx)) * float2(oct.x >= 0 ? 1 : -1, oct.y >= 0 ? 1 : -1);
		// RR ignores these alpha lanes, so raw emission can survive composition without subtracting rounded radiance.
		DirectDiffuse[pixel] = float4(unmodulatedDiffuse, emission.r);
		DirectSpecular[pixel] = float4(unmodulatedSpecular, emission.g);
		uint surfaceClass = 0;
		if ((DirectNormalMode & 0x80000000u) != 0u) {
			const float threshold = float((DirectNormalMode >> 8u) & 0xffffu) / 65535.0f;
			surfaceClass = material.metallic >= 0.8f ? 2u : material.roughness < threshold ? 1u : 0u;
		}
		DirectNormalRoughness[pixel] = float4(oct * 0.5 + 0.5, material.roughness, float(surfaceClass));
		DirectSpecularAlbedo[pixel] = float4(specularAlbedo, emission.b);
		DirectDiffuseAlbedo[pixel] = float4(diffuseAlbedo, 1);

	}
	return float4(result, DirectRadianceTag);
}

// Independent RT-exclusive indirect shader pass. It shares the current native
// scene/receiver resources but never writes or composites direct radiance.
void EvaluateIndirectForSurface(uint instance, uint primitive, float2 barycentric, float3x4 objectToWorld,
    float3x4 worldToObject, float3 direction, float distance, float4 receiver, float footprint, uint2 pixel)
{
    if (IndirectRayCount == 0u) return;
    MaterialRecord record = Materials[Surfaces[instance].material.x];
    if ((record.flags.x & 3) != 3 || record.flags.w != 0) return;
    if (abs(distance - length(receiver.xyz)) > max(5.0, distance * ReceiverRelativeTolerance)) return;
    EvaluatedSourceMaterial material;
    if (!LoadSourceMaterial(instance, primitive, barycentric, objectToWorld, worldToObject, footprint, material)) return;
    TriangleSurface surface;
    if (!LoadTriangle(instance, primitive, barycentric, surface)) return;
    const float3 edge0 = mul((float3x3)objectToWorld, surface.position1 - surface.position0);
    const float3 edge1 = mul((float3x3)objectToWorld, surface.position2 - surface.position0);
    const float3 geometricNormal = normalize(cross(edge0, edge1));
    if (!all(isfinite(geometricNormal))) return;
    const float3 position = CameraOrigin + direction * distance;
    if (BakedRoomCode(instance, position) != uint(receiver.w)) return;
    float4 indirectDiffuse, indirectSpecular;
    if (EvaluateIndirectLighting(material, position, -direction, geometricNormal,
        footprint, pixel, indirectDiffuse, indirectSpecular)) {
        IndirectDiffuse[pixel] = indirectDiffuse;
        IndirectSpecular[pixel] = indirectSpecular;
    }
}

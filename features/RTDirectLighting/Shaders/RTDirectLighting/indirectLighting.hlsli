uint IndirectHash(uint value)
{
	value ^= value >> 16; value *= 0x7feb352d;
	value ^= value >> 15; value *= 0x846ca68b;
	return value ^ (value >> 16);
}

uint BakedRoomCode(uint instance, float3 position)
{
	if (BakedRoomsKnown == 0) return ReceiverRoom::Unknown;
	uint linked = Surfaces[instance].material.w;
	if (linked == 0xffffffff) return ReceiverRoom::Unknown;
	if (linked != 0) return linked <= BakedRoomCount ? uint(LightSnapshot.Load(int3(linked - 1, 1, 0)).x) : ReceiverRoom::Unknown;
	uint roomIndex = 0, matches = 0;
	for (uint room = 0; room < BakedRoomCount; ++room) {
		RoomVolume volume = RoomVolumes[room];
		float4 roomPosition = float4(position, 1);
		float3 local = float3(dot(volume.inverse0, roomPosition), dot(volume.inverse1, roomPosition), dot(volume.inverse2, roomPosition));
		float3 margin = volume.halfExtents.xyz - abs(local);
		float3 tolerance = MinimumDistance * float3(length(volume.inverse0.xyz), length(volume.inverse1.xyz), length(volume.inverse2.xyz));
		if (all(margin >= -tolerance)) {
			if (any(margin <= tolerance)) return ReceiverRoom::Unknown;
			roomIndex = room; ++matches;
		}
	}
	if (matches == 0) return ReceiverRoom::NoRoom;
	return matches == 1 ? uint(LightSnapshot.Load(int3(roomIndex, 1, 0)).x) : ReceiverRoom::Unknown;
}

// DXR two-bounce diffuse GI experiment: set to 0 for A/B comparison.
// This is one additional indirect path segment, NOT an addition to DirectDiffuse.
#ifndef REDSTONE_IL_SECOND_BOUNCE
#define REDSTONE_IL_SECOND_BOUNCE 1
#endif

#if REDSTONE_IL_SECOND_BOUNCE
// One terminal ray at the second indirect surface. Direct DIFFUSE is the
// terminal incident-radiance source; primary direct signals stay unchanged.
bool TraceTerminalDiffuse(float3 origin, float3 direction, float footprint,
    uint selectedLight, uint lightCount, uint2 pixel, uint sampleIndex, out float3 radiance)
{
    radiance = 0;
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = MinimumDistance;
    ray.TMax = MaximumDistance;
    RayQuery<RAY_FLAG_FORCE_NON_OPAQUE | RAY_FLAG_CULL_BACK_FACING_TRIANGLES> query;
    query.TraceRayInline(Scene, RAY_FLAG_NONE, 3, ray);
    while (query.Proceed()) {
        if (query.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE) continue;
        uint instance = query.CandidateInstanceID();
        if (instance >= InstanceCount ||
            GeometryBuffers[NonUniformResourceIndex(instance)].Load(ValidationOffsets[instance]) != 0) return false;
        if (AdmitCandidate(instance, query.CandidatePrimitiveIndex(), query.CandidateTriangleBarycentrics()))
            query.CommitNonOpaqueTriangleHit();
    }
    // The prepared scene has no environment radiance yet.
    if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return true;
    uint instance = query.CommittedInstanceID();
    uint primitive = query.CommittedPrimitiveIndex();
    MaterialRecord record = Materials[Surfaces[instance].material.x];
    if ((record.flags.x & 3) != 3 || record.flags.w != 0) return false;
    EvaluatedSourceMaterial material;
    if (!LoadSourceMaterial(instance, primitive, query.CommittedTriangleBarycentrics(),
        query.CommittedObjectToWorld3x4(), query.CommittedWorldToObject3x4(), footprint, material)) return false;
    TriangleSurface surface;
    if (!LoadTriangle(instance, primitive, query.CommittedTriangleBarycentrics(), surface)) return false;
    float3x3 transform = (float3x3)query.CommittedObjectToWorld3x4();
    float3 geometricNormal = normalize(cross(
        mul(transform, surface.position1 - surface.position0),
        mul(transform, surface.position2 - surface.position0)));
    if (!all(isfinite(geometricNormal))) return false;

    float3 position = origin + direction * query.CommittedRayT();
    uint room = BakedRoomCode(instance, position);
    radiance = material.emission * LightingStrengths.w;

    // Uniformly select one light and divide by its selection PDF (= 1/lightCount).
    // Emission is not multiplied by lightCount.
    float3 directDiffuse, unusedSpecular;
    float4 unavailable;
    if (EvaluatePointLight(selectedLight, material, position, -direction,
        geometricNormal, room, pixel, sampleIndex + 0x1000u, directDiffuse, unusedSpecular, unavailable))
        radiance += directDiffuse * float(lightCount);

    return all(isfinite(radiance)) && all(radiance >= 0);
}

float3 SampleSecondBounceDirection(float3 normal, uint2 pixel, uint sampleIndex, out uint selectedLight, uint lightCount)
{
    uint seed = IndirectHash(pixel.x + pixel.y * Size.x +
        FrameIndex * 0x9e3779b9u + sampleIndex * 0x85ebca6bu + 0xd1b54a35u);
    uint angular = IndirectHash(seed ^ 0x68bc21ebu);
    uint light = IndirectHash(seed ^ 0x02e5be93u);
    selectedLight = light % max(lightCount, 1u);

    float2 random = (float2(angular & 0xffffu, angular >> 16) + 0.5) / 65536.0;
    float radius = sqrt(random.x);
    float phi = random.y * 6.28318530718;
    float3 tangent = normalize(cross(abs(normal.z) < 0.9 ? float3(0, 0, 1) : float3(0, 1, 0), normal));
    float3 bitangent = cross(normal, tangent);
    return normalize(tangent * (radius * cos(phi)) + bitangent * (radius * sin(phi)) +
        normal * sqrt(1.0 - random.x));
}
#endif

// Composer tier transport is isolated from the existing Custom estimator.
// PointLightFalloff.w is a reserved tier ID: 0=legacy, 2..5=transport tiers.
// Transport tiers stay gated until DX12 composition and RR masks are ready.
uint TierTransportMode()
{
    return uint(clamp(PointLightFalloff.w + 0.5, 0.0, 5.0));
}
float TierFirstBounceSurvival(uint tier) { return tier == 5u ? 0.66 : 0.5; }
float TierSpecularProbability(uint tier) { return tier >= 3u ? 0.5 : 0.0; }
float TierRandom(uint seed)
{
    return (float(IndirectHash(seed) & 0x00ffffffu) + 0.5) / 16777216.0;
}
// Per-ray stochastic lobe selection, with exact BRDF*cos/(PDF*lobePDF).
// Specular is full material GGX/Schlick/Smith with NDF directional PDF;
// mirror-specific treatment is deliberately excluded.
bool SampleTierScatter(EvaluatedSourceMaterial material, float3 viewDirection,
    float3 geometricNormal, uint seed, float specularProbability,
    out float3 direction, out float3 throughput, out bool choseSpecular)
{
    direction = 0; throughput = 0; choseSpecular = false;
    const float3 v = normalize(viewDirection);
    float3 n = normalize(material.normal);
    const float3 outward = dot(geometricNormal, v) >= 0.0 ?
        normalize(geometricNormal) : -normalize(geometricNormal);
    if (!all(isfinite(n)) || !all(isfinite(v)) || !all(isfinite(outward))) return false;
    if (dot(n, outward) < 0.0) n = -n;
    const float nv = saturate(dot(n, v));
    if (nv <= 1e-5) return false;
    const float fraction = saturate(specularProbability);
    choseSpecular = fraction > 0.0 && TierRandom(seed ^ 0x76bd83e5u) < fraction;
    const float branchPdf = choseSpecular ? fraction : (1.0 - fraction);
    if (branchPdf <= 0.0) return false;
    const float2 xi = float2(TierRandom(seed ^ 0x61c88647u),
        TierRandom(seed ^ 0xbf58476du));
    const float3 tangent = normalize(cross(
        abs(n.z) < 0.9 ? float3(0, 0, 1) : float3(0, 1, 0), n));
    const float3 bitangent = cross(n, tangent);
    const float phi = 6.28318530718 * xi.y;
    if (choseSpecular) {
        // GGX NDF importance sampling; PDF accounts for H->L Jacobian.
        const float roughness = clamp(material.roughness, 0.04, 1.0);
        const float alpha = roughness * roughness, alpha2 = alpha * alpha;
        const float nh = sqrt(saturate((1.0 - xi.x) /
            max(1.0 + (alpha2 - 1.0) * xi.x, 1e-6)));
        const float sh = sqrt(saturate(1.0 - nh * nh));
        const float3 h = normalize(tangent * (sh * cos(phi)) +
            bitangent * (sh * sin(phi)) + n * nh);
        const float vh = dot(v, h);
        if (vh <= 1e-5) return false;
        direction = normalize(reflect(-v, h));
        const float nl = dot(n, direction), noH = saturate(dot(n, h));
        if (nl <= 1e-5 || dot(outward, direction) <= 0.0) return false;
        const float pdf = BRDF::D_GGX(roughness, noH) * noH / max(4.0 * vh, 1e-6);
        if (!isfinite(pdf) || pdf <= 1e-8) return false;
        float3 fresnel;
        const float3 brdf = PBR::SpecularMicrofacet(roughness, material.f0,
            saturate(nl), max(nv, 1e-4), noH, saturate(vh), fresnel);
        throughput = brdf * (nl / (pdf * branchPdf));
    } else {
        const float radius = sqrt(xi.x);
        direction = normalize(tangent * (radius * cos(phi)) +
            bitangent * (radius * sin(phi)) + n * sqrt(1.0 - xi.x));
        if (dot(outward, direction) <= 0.0) return false;
        // Cosine-weighted PDF cancels Lambert * NdotL.
        const float3 h = normalize(v + direction);
        const float3 fresnel = BRDF::F_Schlick(material.f0, saturate(dot(v, h)));
        throughput = max(0.0, (1.0 - fresnel) *
            (1.0 - material.metallic) * material.baseColor) / branchPdf;
    }
    return all(isfinite(throughput)) && all(throughput >= 0.0);
}

bool SecondaryRadiance(float3 origin, float3 direction, float footprint,
    uint2 pixel, uint sampleIndex, uint transportTier,
    out float3 radiance, out float distance)
{
	radiance = 0; distance = -1;
	RayDesc ray;
	ray.Origin = origin; ray.Direction = direction; ray.TMin = MinimumDistance; ray.TMax = MaximumDistance;
	RayQuery<RAY_FLAG_FORCE_NON_OPAQUE | RAY_FLAG_CULL_BACK_FACING_TRIANGLES> query;
	query.TraceRayInline(Scene, RAY_FLAG_NONE, 3, ray);
	while (query.Proceed()) {
		if (query.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE) continue;
		uint instance = query.CandidateInstanceID();
		if (instance >= InstanceCount || GeometryBuffers[NonUniformResourceIndex(instance)].Load(ValidationOffsets[instance]) != 0) return false;
		if (AdmitCandidate(instance, query.CandidatePrimitiveIndex(), query.CandidateTriangleBarycentrics())) query.CommitNonOpaqueTriangleHit();
	}
	if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return true;
	uint instance = query.CommittedInstanceID(), primitive = query.CommittedPrimitiveIndex();
	MaterialRecord record = Materials[Surfaces[instance].material.x];
	if ((record.flags.x & 3) != 3 || record.flags.w != 0) return false;
	EvaluatedSourceMaterial material;
	if (!LoadSourceMaterial(instance, primitive, query.CommittedTriangleBarycentrics(), query.CommittedObjectToWorld3x4(), query.CommittedWorldToObject3x4(), footprint, material)) return false;
	TriangleSurface surface;
	if (!LoadTriangle(instance, primitive, query.CommittedTriangleBarycentrics(), surface)) return false;
	float3x3 transform = (float3x3)query.CommittedObjectToWorld3x4();
	float3 geometricNormal = normalize(cross(mul(transform, surface.position1 - surface.position0), mul(transform, surface.position2 - surface.position0)));
	if (!all(isfinite(geometricNormal))) return false;
	distance = query.CommittedRayT();
	float3 position = origin + direction * distance;
	uint room = BakedRoomCode(instance, position);
	radiance = material.emission * LightingStrengths.w;
	uint lightCount = Padding.z != 0 ? Padding.z : 1;
	for (uint light = 0; light < lightCount; ++light) {
		float3 diffuse, specular;
		float4 unavailable;
		if (!EvaluatePointLight(light, material, position, -direction, geometricNormal, room, pixel, sampleIndex + 1u, diffuse, specular, unavailable)) return false;
		radiance += diffuse + (transportTier == 2u ? 0.0 : specular);
	}
#if REDSTONE_IL_SECOND_BOUNCE
    // The legacy Custom path retains its previous unconditional second ray.
    // Fixed tiers use explicit survivability without altering direct buffers.
    if (transportTier == 0u) {
    // The second segment is traced only from a valid first indirect hit.
    // Existing first-hit emission and direct lighting are always retained.
    uint selectedLight = 0;
    float3 shadingNormal = normalize(material.normal);
    float3 outward = dot(geometricNormal, -direction) >= 0 ? geometricNormal : -geometricNormal;
    if (dot(shadingNormal, outward) < 0) shadingNormal = -shadingNormal;
    float3 bounceDirection = SampleSecondBounceDirection(shadingNormal, pixel, sampleIndex,
        selectedLight, lightCount);
    if (dot(bounceDirection, outward) > 0) {
        float3 terminal;
        if (TraceTerminalDiffuse(position + outward * MinimumDistance, bounceDirection,
            footprint, selectedLight, lightCount, pixel, sampleIndex, terminal)) {
            // Cosine-weighted Lambert estimator: BRDF*cos / (cos/pi) = albedo.
            // Use a simple F0 diffuse-energy approximation at the intermediate hit.
            float3 throughput = saturate(1.0 - material.f0) *
                (1.0 - material.metallic) * material.baseColor;
            radiance += terminal * throughput;
        }
    }
    } else if (transportTier == 5u) {
        const uint seed = IndirectHash(pixel.x + pixel.y * Size.x +
            FrameIndex * 0x9e3779b9u + sampleIndex * 0x85ebca6bu + 0x9407c2e7u);
        if (TierRandom(seed) < 0.25) {
            float3 bounceDirection, throughput;
            bool specular;
            if (SampleTierScatter(material, -direction, geometricNormal,
                seed ^ 0x342d18a9u, 0.5, bounceDirection, throughput, specular)) {
                const float3 outward = dot(geometricNormal, -direction) >= 0 ?
                    normalize(geometricNormal) : -normalize(geometricNormal);
                const uint lightCount2 = max(lightCount, 1u);
                const uint selectedLight = IndirectHash(seed ^ 0x8a452739u) % lightCount2;
                float3 terminal;
                if (!TraceTerminalDiffuse(position + outward * MinimumDistance,
                    bounceDirection, footprint, selectedLight, lightCount2,
                    pixel, sampleIndex, terminal)) return false;
                // Preserve the first-hit emission/direct term separately.
                // The additional segment is weighted by reciprocal survival.
                radiance += terminal * throughput * (1.0 / 0.25);
            }
        }
    }
#endif
	return all(isfinite(radiance)) && all(radiance >= 0);
}

// Tier I: diffuse-only one indirect bounce at 50% ray survival.
// Tier II: 50% survival, 50/50 per-ray diffuse/GGX selection.
// RT-Enhanced: same 50/50 split at 50% survival.
// FSR-REDSTONE: 66% first survival and a conditional 25% second segment.
// The secondary-hit direct irradiance is transported BACK as indirect,
// never copied into the original receiver's direct lighting buffers.
bool EvaluateTierIndirectLighting(EvaluatedSourceMaterial material, float3 position,
    float3 viewDirection, float3 geometricNormal, float footprint,
    uint2 pixel, uint tier, out float4 diffuse, out float4 specular)
{
    diffuse = float4(0, 0, 0, -2);
    specular = float4(0, 0, 0, -2);
    if (BakedRoomsKnown == 0u) return false;
    const uint count = min(IndirectRayCount, 8u);
    if (count == 0u) return true;
    const float survival = TierFirstBounceSurvival(tier);
    const float specProb = TierSpecularProbability(tier);
    const float3 outward = dot(geometricNormal, viewDirection) >= 0.0 ?
        normalize(geometricNormal) : -normalize(geometricNormal);
    float3 sumDiffuse = 0, sumSpecular = 0;
    float distDiffuse = 0, distSpecular = 0;
    uint attemptsDiffuse = 0, attemptsSpecular = 0, hitsDiffuse = 0, hitsSpecular = 0;
    for (uint sampleIndex = 0u; sampleIndex < count; ++sampleIndex) {
        const uint seed = IndirectHash(pixel.x + pixel.y * Size.x +
            FrameIndex * 0x9e3779b9u + sampleIndex * 0x85ebca6bu);
        // No persistent spatial lobe ownership and no deterministic dimming.
        if (TierRandom(seed ^ 0xd254a349u) >= survival) continue;
        float3 rayDirection, throughput;
        bool choseSpecular;
        const bool scattered = SampleTierScatter(material, viewDirection,
            geometricNormal, seed ^ 0x05d526c9u, specProb,
            rayDirection, throughput, choseSpecular);
        // A rejected GGX hemisphere direction is still a valid zero-valued
        // sample of its selected family. Otherwise sparse RR sees artificial
        // missing samples and learns an incorrect distribution near edges.
        if (choseSpecular) ++attemptsSpecular;
        else ++attemptsDiffuse;
        if (!scattered) continue;
        float3 incoming; float distance;
        if (!SecondaryRadiance(position + outward * MinimumDistance, rayDirection,
            footprint, pixel, sampleIndex, tier, incoming, distance)) return false;
        const float3 contribution = incoming * throughput * (1.0 / survival);
        if (choseSpecular) {
            sumSpecular += contribution;
            if (distance >= 0.0) { distSpecular += distance; ++hitsSpecular; }
        } else {
            sumDiffuse += contribution;
            if (distance >= 0.0) { distDiffuse += distance; ++hitsDiffuse; }
        }
    }
    // Normalize by REQUESTED rays, never by survivors or lobe attempts.
    // -2 indicates no sample for this signal family; -1 = sampled but missed.
    if (attemptsDiffuse != 0u)
        diffuse = float4(sumDiffuse / count,
            hitsDiffuse != 0u ? distDiffuse / hitsDiffuse : -1.0);
    if (attemptsSpecular != 0u)
        specular = float4(sumSpecular / count,
            hitsSpecular != 0u ? distSpecular / hitsSpecular : -1.0);
    if (attemptsDiffuse == 0u && attemptsSpecular == 0u)
        return true; // valid zero contribution, but no RR family sample
    return all(isfinite(diffuse)) && all(isfinite(specular)) &&
        all(diffuse.rgb >= 0.0) && all(specular.rgb >= 0.0) &&
        all(diffuse.rgb <= MaximumHalfRadiance) &&
        all(specular.rgb <= MaximumHalfRadiance);
}

bool EvaluateIndirectLighting(EvaluatedSourceMaterial material, float3 position, float3 viewDirection, float3 geometricNormal,
	float footprint, uint2 pixel, out float4 diffuse, out float4 specular)
{
    const uint tier = TierTransportMode();
    if (tier >= 2u && tier <= 5u)
        return EvaluateTierIndirectLighting(material, position, viewDirection,
            geometricNormal, footprint, pixel, tier, diffuse, specular);
	diffuse = specular = 0;
	if (BakedRoomsKnown == 0) return false;
	float3 tangent = normalize(cross(abs(material.normal.z) < 0.9 ? float3(0, 0, 1) : float3(0, 1, 0), material.normal));
	float3 bitangent = cross(material.normal, tangent);
	float nDotV = saturate(dot(material.normal, viewDirection));
	if (nDotV == 0) return true;
	uint count = min(IndirectRayCount, 8u);
	uint hitCount = 0;
	float3 outward = dot(geometricNormal, viewDirection) >= 0 ? geometricNormal : -geometricNormal;
	for (uint sampleIndex = 0; sampleIndex < count; ++sampleIndex) {
		uint seed = IndirectHash(pixel.x + pixel.y * Size.x + FrameIndex * 0x9e3779b9 + sampleIndex * 0x85ebca6b);
		float2 random = (float2(seed & 0xffff, seed >> 16) + 0.5) / 65536.0;
		float radius = sqrt(random.x), angle = random.y * 6.28318530718;
		float3 l = normalize(tangent * (radius * cos(angle)) + bitangent * (radius * sin(angle)) + material.normal * sqrt(1 - random.x));
		if (dot(outward, l) <= 0) continue;
		float3 incoming; float distance;
		if (!SecondaryRadiance(position + outward * MinimumDistance, l, footprint, pixel, sampleIndex, 0u, incoming, distance)) return false;
		float3 h = normalize(viewDirection + l), fresnel;
		float3 specularWeight = PBR::SpecularMicrofacet(material.roughness, material.f0, saturate(dot(material.normal, l)), max(nDotV, 1e-4), saturate(dot(material.normal, h)), saturate(dot(viewDirection, h)), fresnel);
		float3 diffuseWeight = (1 - fresnel) * (1 - material.metallic) * material.baseColor * BRDF::Diffuse_Lambert();
		diffuse.rgb += incoming * diffuseWeight * 3.14159265359;
		specular.rgb += incoming * specularWeight * 3.14159265359;
		if (distance >= 0) { diffuse.a += distance; specular.a += distance; ++hitCount; }
	}
	diffuse.rgb /= max(count, 1u); specular.rgb /= max(count, 1u);
	diffuse.a = hitCount != 0 ? diffuse.a / hitCount : -1;
	specular.a = diffuse.a;
	return all(isfinite(diffuse)) && all(isfinite(specular)) && all(diffuse.rgb >= 0) && all(specular.rgb >= 0) && all(diffuse <= MaximumHalfRadiance) && all(specular <= MaximumHalfRadiance);
}

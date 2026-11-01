#include "LightLimitFix/Attenuation.hlsli"
#include "directContract.hlsli"

static const float ReceiverRelativeTolerance = 1e-5;
static const float MaximumHalfRadiance = 65504;
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

// Normal mode 0 preserves the shipped flat response. Modes 1 and 2 reuse
// True PBR GGX/Schlick/Smith and Fresnel-weighted diffuse. Ray bias stays unchanged.
bool EvaluatePointLight(
    uint lightIndex,
    EvaluatedSourceMaterial material,
    float3 position,
    float3 viewDirection,
    float3 geometricNormal,
    float receiverCode,
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

    if (lightDistance >= falloff.x)
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

    LightLimitFix::Light light = (LightLimitFix::Light) 0;
    light.radius = falloff.x;
    light.invRadius = falloff.y;
    light.fadeZone = falloff.z;
    light.sizeBias = falloff.w;
    light.lightFlags = asuint(
        LightSnapshot.Load(int3(snapshotBase + 5, 0, 0)).x);

    float attenuation =
        LightLimitFix::GetAttenuation(lightDistance, light);

    if (attenuation <= 0)
        return true;

    // Offset toward light, not along the surface normal.
    float3 origin = position + l * MinimumDistance;
    float3 shadowVector = lightPosition - origin;

    bool visible;

    if (!TraceDirectVisibility(
        origin,
        normalize(shadowVector),
        length(shadowVector),
        visible))
    {
        unavailable = Unavailable(InvalidSurface);
        return false;
    }

    if (!visible)
        return true;


    const float3 incidentRadiance = radiance.rgb * attenuation;
    if (DirectNormalMode == 0u) {
        diffuse = (1.0 - material.metallic) * material.baseColor *
            BRDF::Diffuse_Lambert() * incidentRadiance;
        specular = 0;
        return true;
    }

    const float3 n = normalize(DirectNormalMode == 1u ? geometricNormal : material.normal);
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
	[loop] for (uint lightIndex = 0; lightIndex < lightCount; ++lightIndex) {
		float3 diffuse, specular;
		float4 unavailable;
		if (!EvaluatePointLight(lightIndex, material, position, -direction, geometricNormal, receiver.w, diffuse, specular, unavailable)) return unavailable;
		diffuseSum += diffuse;
		specularSum += specular;
	}
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
		const float3 shadingNormal = DirectNormalMode == 1u ? geometricNormal : material.normal;
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
		DirectNormalRoughness[pixel] = float4(oct * 0.5 + 0.5, material.roughness, 0);
		DirectSpecularAlbedo[pixel] = float4(specularAlbedo, emission.b);
		DirectDiffuseAlbedo[pixel] = float4(diffuseAlbedo, 1);
		if (IndirectRayCount != 0 && BakedRoomCode(instance, position) == uint(receiver.w)) {
			float4 indirectDiffuse, indirectSpecular;
			if (EvaluateIndirectLighting(material, position, -direction, geometricNormal, footprint, pixel, indirectDiffuse, indirectSpecular)) {
				IndirectDiffuse[pixel] = indirectDiffuse;
				IndirectSpecular[pixel] = indirectSpecular;
			}
		}
	}
	return float4(result, DirectRadianceTag);
}

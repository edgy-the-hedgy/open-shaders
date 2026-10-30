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

bool SecondaryRadiance(float3 origin, float3 direction, float footprint, out float3 radiance, out float distance)
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
		if (!EvaluatePointLight(light, material, position, -direction, geometricNormal, room, diffuse, specular, unavailable)) return false;
		radiance += diffuse + specular;
	}
	return all(isfinite(radiance)) && all(radiance >= 0);
}

bool EvaluateIndirectLighting(EvaluatedSourceMaterial material, float3 position, float3 viewDirection, float3 geometricNormal,
	float footprint, uint2 pixel, out float4 diffuse, out float4 specular)
{
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
		if (!SecondaryRadiance(position + outward * MinimumDistance, l, footprint, incoming, distance)) return false;
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

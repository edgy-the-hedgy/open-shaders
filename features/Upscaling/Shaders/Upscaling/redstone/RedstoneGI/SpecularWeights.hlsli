#ifndef REDSTONE_SPECULAR_WEIGHTS
#define REDSTONE_SPECULAR_WEIGHTS
float SpecularDistribution(float roughness, float3 normal, float3 halfVector)
{
	float alpha = roughness * roughness;
	float alpha2 = alpha * alpha;
	float NoH = saturate(dot(normal, halfVector));
	float3 tangent = cross(normal, halfVector);
	float d = max(dot(tangent, tangent) + alpha2 * NoH * NoH, 1e-12);
	return alpha2 / (Math::PI * d * d);
}
float SpecularVisibility(float roughness, float NoV, float NoL)
{
	float alpha = roughness * roughness;
	float denominator = NoL * (NoV * (1.0 - alpha) + alpha) + NoV * (NoL * (1.0 - alpha) + alpha);
	return denominator > 0 ? 0.5 / denominator : 0;
}
float SpecularSectorWeight(Surface surface, float roughness, Slice slice, uint sector)
{
	const float sectorAngle = Math::PI / 32.0;
	float angle = slice.normalAngle - ((sector + 0.5) * sectorAngle - Math::HALF_PI);
	float sine, cosine;
	sincos(angle, sine, cosine);
	float3 tangent = normalize(cross(surface.view, slice.axis));
	float3 direction = surface.view * cosine + tangent * sine;
	float NoL = dot(surface.normal, direction);
	float NoV = dot(surface.normal, surface.view);
	float3 halfVector = surface.view + direction;
	float halfLengthSquared = dot(halfVector, halfVector);
	if (NoL <= 0 || NoV <= 0 || halfLengthSquared <= 1e-8) return 0;
	halfVector *= rsqrt(halfLengthSquared);
	return SpecularDistribution(roughness, surface.normal, halfVector) * SpecularVisibility(roughness, NoV, NoL) * NoL * abs(sine) * slice.azimuthWeight;
}
#endif

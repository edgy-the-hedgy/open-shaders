#ifndef REDSTONE_OCCLUSION_SETTINGS
#define REDSTONE_OCCLUSION_SETTINGS
cbuffer RedstoneOcclusionSettings : register(b2)
{
 uint DiffuseOcclusionEnabled;
 uint SpecularOcclusionEnabled;
 uint DiffuseOcclusionSlices;
 uint SpecularOcclusionSlices;
 uint DiffuseOcclusionSteps;
 uint SpecularOcclusionSteps;
 float DiffuseOcclusionRadius;
 float SpecularOcclusionRadius;
 float DiffuseOcclusionStrength;
 float SpecularOcclusionStrength;
 uint OcclusionUseDenoised;
 uint OcclusionPadding;
};
#endif

#include "Common/NativeShadowMask.hlsli"
bool AdmitShadowCandidate(uint instance,uint primitive,float2 barycentric,inout bool valid)
{
    if (instance>=InstanceCount || GeometryBuffers[NonUniformResourceIndex(instance)].Load(ValidationOffsets[instance])!=0) {
        valid=false; return false;
    }
    SurfaceRecord binding=Surfaces[instance];
    if (binding.material.z!=0) return true;
    MaterialRecord material=Materials[binding.material.x];
    TriangleSurface receiverSurface;
    if ((material.flags.x&256u)==0 || material.flags.w!=0 || (material.flags.x&16u)==0 ||
        !LoadTriangle(instance,primitive,barycentric,receiverSurface)) { valid=false; return false; }
    return AdmitCandidate(instance,primitive,barycentric);
}
void TraceNativeShadow(uint2 pixel)
{
    Shadow[pixel]=float4(1,0,0,0);
    float4 cameraRay=Receivers[pixel];
    if (cameraRay.w!=1 || !all(isfinite(cameraRay))) return;
    RayDesc primary;
    primary.Origin=CameraOrigin; primary.Direction=cameraRay.xyz;
    primary.TMin=MinimumDistance; primary.TMax=MaximumDistance;
    bool geometryValid=true;
    RayQuery<RAY_FLAG_FORCE_NON_OPAQUE> surface;
    surface.TraceRayInline(Scene,RAY_FLAG_NONE,3,primary);
    while (surface.Proceed()) {
        if (surface.CandidateType()!=CANDIDATE_NON_OPAQUE_TRIANGLE) continue;
        if (AdmitShadowCandidate(surface.CandidateInstanceID(),surface.CandidatePrimitiveIndex(),surface.CandidateTriangleBarycentrics(),geometryValid))
            surface.CommitNonOpaqueTriangleHit();
    }
    if (surface.CommittedStatus()!=COMMITTED_TRIANGLE_HIT) return;
    float distance=surface.CommittedRayT();
    if (!NativeShadowValid(true,geometryValid,distance)) return;
    RayDesc visibility;
    visibility.Direction=normalize(LightDirection);
    visibility.Origin=primary.Origin+primary.Direction*distance+visibility.Direction*MinimumDistance;
    visibility.TMin=MinimumDistance; visibility.TMax=MaximumDistance;
    RayQuery<RAY_FLAG_FORCE_NON_OPAQUE> blocker;
    blocker.TraceRayInline(Scene,RAY_FLAG_NONE,3,visibility);
    while (blocker.Proceed()) {
        if (blocker.CandidateType()!=CANDIDATE_NON_OPAQUE_TRIANGLE) continue;
        if (AdmitShadowCandidate(blocker.CandidateInstanceID(),blocker.CandidatePrimitiveIndex(),blocker.CandidateTriangleBarycentrics(),geometryValid))
            blocker.CommitNonOpaqueTriangleHit();
    }
    bool valid=NativeShadowValid(true,geometryValid,distance);
    bool blocked=blocker.CommittedStatus()==COMMITTED_TRIANGLE_HIT;
    Shadow[pixel]=valid ? float4(NativeShadowVisibility(valid,blocked),1,distance,float(surface.CommittedInstanceID()+1u)) : float4(1,0,0,0);
}

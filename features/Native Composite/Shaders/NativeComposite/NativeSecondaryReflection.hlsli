#ifndef NATIVE_SECONDARY_REFLECTION_HLSLI
#define NATIVE_SECONDARY_REFLECTION_HLSLI

// Scene/material buffers and SecondaryLights are the calling pipeline's
// immutable publications. No secondary shader reads reconstructed indirect
// radiance or launches another reflection, AO or GI bounce.
#define NATIVE_SECONDARY_SURFACE
#define NATIVE_MATERIAL_PROJECTION
#include "NativeMaterialSurface.hlsli"
#undef NATIVE_MATERIAL_PROJECTION
#undef NATIVE_SECONDARY_SURFACE
#include "NativeDirectBRDF.hlsli"
#include "NativeHeightVisibility.hlsli"

static const uint SecondaryLightRecordBytes=7u*16u;
static const uint SecondaryMaxLightCount=2048u;

bool SecondaryVisible(float3 world,float3 n,float3 direction,float distance,uint instanceCount,uint characters)
{
    const float bias=0.5;
    RayDesc ray;
    ray.Origin=world+(dot(n,direction)<0.0?-n:n)*bias+direction*bias;
    ray.Direction=direction;
    ray.TMin=bias;
    ray.TMax=distance-bias*3.0;
    if(ray.TMax<=ray.TMin)return true;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
    q.TraceRayInline(Scene,RAY_FLAG_NONE,0xff,ray);
    while(q.Proceed()){
        if(q.CandidateType()==CANDIDATE_NON_OPAQUE_TRIANGLE&&
           AcceptAlpha(q.CandidateInstanceID(),q.CandidatePrimitiveIndex(),q.CandidateTriangleBarycentrics(),instanceCount))
            q.CommitNonOpaqueTriangleHit();
    }
    if(q.CommittedStatus()!=COMMITTED_NOTHING)return false;
    if(characters!=0u){
        RayQuery<RAY_FLAG_FORCE_OPAQUE|RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> c;
        c.TraceRayInline(CharacterScene,RAY_FLAG_NONE,0xff,ray);
        while(c.Proceed()){}
        if(c.CommittedStatus()!=COMMITTED_NOTHING)return false;
    }
    return true;
}

struct SecondaryLight
{
    float3 delta;
    float3 incident;
    float radius;
    float weight;
    uint index;
};

SecondaryLight ReadSecondaryLight(uint index,float3 world,float3 n,NativeCoatSample coat)
{
    SecondaryLight result=(SecondaryLight)0;
    uint offset=index*SecondaryLightRecordBytes;
    float4 light=asfloat(SecondaryLights.Load4(offset));
    float4 color=asfloat(SecondaryLights.Load4(offset+16u));
    float4 params=asfloat(SecondaryLights.Load4(offset+32u));
    float3 delta=light.xyz-world;
    float d2=dot(delta,delta);
    if(!isfinite(d2)||d2<=0.25||d2>=light.w*light.w)return result;
    float distance=sqrt(d2);
    float3 l=delta/distance;
    if(dot(n,l)<=0.0&&!HasNativeThinSurface(coat)&&!HasNativeSubsurface(coat)&&
       !(HasNativeCoat(coat)&&dot(coat.normalRoughness.xyz,l)>0.0))return result;
    float fade=saturate((light.w-distance)*max(params.y,0.0));
    float attenuation=params.z!=0.0?fade*fade*(3.0-2.0*fade)/max(d2+params.x*params.x,1.0):
        1.0-saturate(d2/(light.w*light.w));
    if(params.w!=0.0){
        float4 clip=world.x*asfloat(SecondaryLights.Load4(offset+48u))+
            world.y*asfloat(SecondaryLights.Load4(offset+64u))+
            world.z*asfloat(SecondaryLights.Load4(offset+80u))+asfloat(SecondaryLights.Load4(offset+96u));
        if(!all(isfinite(clip))||clip.w<=0.0)return result;
        float2 xy=clip.xy/clip.w;attenuation*=saturate(1.0-dot(xy,xy));
    }
    result.incident=max(color.rgb,0.0)*attenuation;
    if(!all(isfinite(result.incident))||!any(result.incident>0.0))return (SecondaryLight)0;
    result.delta=delta;result.radius=max(params.x,0.0);result.index=index;
    // Positive importance weight for every admitted light; no omitted
    // contribution from choosing a fixed first-N list.
    result.weight=max(dot(result.incident,float3(0.2125,0.7154,0.0721)),1e-6);
    return result;
}

// Shared reflected-hit lighting. Material evaluation may come from a static
// instance or a separately published posed triangle; no scene IDs are inferred.
bool ShadeNativeSecondarySurface(Sample s,NativeCoatSample coat,NativeParallaxShadowSample height,
    float3x3 worldToObject,bool front,float3 world,float3 rayDirection,float distance,uint2 pixel,
    uint lobe,uint frame,uint quality,uint instanceCount,uint characters,uint lightCount,
    float4 sunDirection,float4 sunRadiance,uint foliage,out float3 radiance)
{
    radiance=0;
    if(lightCount>SecondaryMaxLightCount)return false;
    float3 view=-rayDirection;
    if(s.color.a!=SurfaceEvaluatedBase||!all(isfinite(s.color))||!all(isfinite(s.normal))||
       !all(isfinite(s.f0))||!all(isfinite(s.emission)))return false;
    float3 n=mul(s.normal.xyz,worldToObject);
    if(!all(isfinite(n))||dot(n,n)<1e-8)return false;
    n=normalize(n);
    if(HasNativeThinSurface(coat)){
        float3 face=mul(coat.normalRoughness.xyz,worldToObject);
        if(!all(isfinite(face))||dot(face,face)<1e-8)return false;
        face=normalize(face);coat.normalRoughness.xyz=dot(face,rayDirection)>0.0?-face:face;
        coat.f0Colored.x=front?coat.f0Colored.x:coat.f0Colored.y;
        s.normal.w=clamp(s.normal.w*(front?1.0:coat.f0Colored.z),0.045,1.0);
        if(dot(n,view)<0.0)n=-n;
    }
    if(HasNativeCoat(coat)){
        float3 cn=mul(coat.normalRoughness.xyz,worldToObject);
        if(!all(isfinite(cn))||dot(cn,cn)<1e-8)return false;
        coat.normalRoughness.xyz=normalize(cn);
    }
    if(dot(n,view)<=0.0)return false;
    float roughness=clamp(s.normal.w,0.045,1.0);
    float a=roughness*roughness, a2=a*a;
    float ndv=max(saturate(dot(n,view)),1e-4);
    PBR::FuzzLayer fuzz=ResolveNativeFuzz(coat,roughness,n,view,true);
    float3 diffuse=0,specular=0,transmission=0;
    const float heightNoise=frac(52.9829189*frac(dot(float2(pixel),float2(0.06711056,0.00583715))));
    float3 biasNormal=HasNativeThinSurface(coat)?coat.normalRoughness.xyz:n;
    if(sunDirection.w!=0.0&&all(isfinite(sunDirection.xyz))&&dot(sunDirection.xyz,sunDirection.xyz)>1e-8&&
       all(isfinite(sunRadiance.rgb))&&any(sunRadiance.rgb>0.0)){
        float3 l=normalize(sunDirection.xyz);
        if((dot(n,l)>0.0||HasNativeThinSurface(coat)||HasNativeSubsurface(coat)||
            (HasNativeCoat(coat)&&dot(coat.normalRoughness.xyz,l)>0.0))&&
            SecondaryVisible(world,biasNormal,l,3.402823e38,instanceCount,characters))
            AccumulateBRDF(s.color,s.f0,n,view,l,saturate(dot(n,l)),ndv,a2,max(sunRadiance.rgb,0.0),NativeHeightVisibility(height,NativeBaseDirection(l,coat),distance,heightNoise),coat,fuzz,foliage,diffuse,specular,transmission);
    }

    // Independent weighted reservoirs bound shadow queries to two in Quality
    // and one in Performance, while considering every selected native light.
    uint sampleCount=quality==kShadowQualityPerformance?1u:2u;
    SecondaryLight selected[2];
    selected[0]=(SecondaryLight)0;selected[1]=(SecondaryLight)0;
    float total=0.0;
    uint seed=NativeEmitterHash(pixel.x*0x9e3779b9u^pixel.y*0x85ebca6bu^frame*0xc2b2ae35u^lobe*0x27d4eb2fu);
    [loop]for(uint i=0;i<lightCount;++i){
        SecondaryLight candidate=ReadSecondaryLight(i,world,n,coat);
        if(candidate.weight<=0.0)continue;
        total+=candidate.weight;
        [unroll]for(uint j=0;j<2u;++j){
            if(j<sampleCount&&NativeEmitterRandom(seed^NativeEmitterHash(i),0x68bc21ebu+j*0x02e5be93u)*total<candidate.weight)
                selected[j]=candidate;
        }
    }
    [unroll]for(uint j=0;j<2u;++j){
        if(j>=sampleCount||selected[j].weight<=0.0)continue;
        SecondaryLight light=selected[j];
        float distanceToLight=length(light.delta);
        float3 l=light.delta/distanceToLight;
        float spread=min(light.radius*ShadowEmitterMultiplier(quality),distanceToLight*0.5);
        float3 sampledDelta=light.delta+NativeEmitterOffset(l,spread,pixel,light.index^(lobe<<16u)^j,frame);
        float sampledDistance=length(sampledDelta);
        if(sampledDistance<=1e-5||!SecondaryVisible(world,biasNormal,sampledDelta/sampledDistance,sampledDistance,instanceCount,characters))continue;
        float3 incident=light.incident*(total/(light.weight*float(sampleCount)));
        AccumulateBRDF(s.color,s.f0,n,view,l,saturate(dot(n,l)),ndv,a2,incident,NativeHeightVisibility(height,NativeBaseDirection(l,coat),distance,heightNoise),coat,fuzz,foliage,diffuse,specular,transmission);
    }
    radiance=max(diffuse+specular+transmission+s.emission.rgb,0.0);
    return all(isfinite(radiance))&&all(radiance<=65504.0);
}
bool ShadeNativeSecondary(uint instance,uint primitive,float2 bary,float3x3 worldToObject,float3x3 objectToWorld,
    bool front,float3 world,float3 rayDirection,float distance,float footprint,uint2 pixel,uint lobe,uint frame,uint quality,
    uint instanceCount,uint characters,uint lightCount,float4 sunDirection,float4 sunRadiance,uint foliage,uint materialControls,uint projectionRows,out float3 radiance)
{
    radiance=0;
    if(instance>=instanceCount||lightCount>SecondaryMaxLightCount)return false;
    Binding binding=Bindings[instance];
    // Projected or otherwise incomplete native recipes must retain fallback.
    if(binding.materialIndex==0xffffffffu||(binding.flags&SurfaceMissingBaseTexturesBinding)!=0u)return false;
    uint projectionRow=0xffffffffu;
    if((binding.flags&SurfaceBaseOnlyBinding)!=0u){
        if((binding.flags&SurfaceSecondaryProjectionBinding)!=0u){
            if(projectionRows==0u)return false;
            projectionRow=ProjectionLookup.Load(instance*4u);
            if(projectionRow>=projectionRows)return false;
        }else if((binding.flags&SurfaceSecondaryParallaxBinding)==0u)return false;
    }
    // A conservative ray-cone LOD avoids always sampling mip zero on long,
    // rough reflection rays. Primary raster gradients remain unchanged.
    float lod=clamp(log2(max(1.0,distance*max(footprint,0.001)/16.0)),0.0,8.0);
    bool rawUsed;NativeCoatSample coat;NativeParallaxShadowSample height;
    float3 tangent=normalize(cross(rayDirection,abs(rayDirection.z)<0.99?float3(0,0,1):float3(0,1,0)));
    float3 bitangent=cross(rayDirection,tangent);
    float footprintWorld=max(0.01,distance*max(footprint,0.001)*0.01);
    float noise=NativeEmitterRandom(NativeEmitterHash(pixel.x^NativeEmitterHash(pixel.y)^frame),0x68bc21ebu);
    Sample s=EvaluateMaterial(instanceCount,instance,primitive,float3(1.0-bary.x-bary.y,bary),
        mul(worldToObject,rayDirection*distance),mul(worldToObject,tangent*footprintWorld),mul(worldToObject,bitangent*footprintWorld),1.0,distance,worldToObject,objectToWorld,
        (materialControls&1u)!=0u && (binding.flags&SurfaceParallaxReadyBinding)!=0u,false,(materialControls&2u)!=0u,lod,projectionRow,world,noise,(RawRasterSurface)0,rawUsed,coat,height);
    return ShadeNativeSecondarySurface(s,coat,height,worldToObject,front,world,rayDirection,distance,pixel,
        lobe,frame,quality,instanceCount,characters,lightCount,sunDirection,sunRadiance,foliage,radiance);
}


#if defined(NATIVE_CHARACTER_REFLECTION)
// Actor BLAS primitives address the posed streams; rigid movers use their
// admitted instance ordinal and immutable local-space geometry instead.
bool ShadeNativeCharacterSecondary(uint instance,uint primitive,float2 bary,bool front,
    float3x3 worldToObject,float3x3 objectToWorld,
    float3 world,float3 rayDirection,float distance,float footprint,uint2 pixel,uint lobe,uint frame,uint quality,
    uint instanceCount,uint characters,uint lightCount,float4 sunDirection,float4 sunRadiance,uint foliage,
    uint materialControls,uint vertexCount,uint triangleCount,uint partCount,uint moverCount,out float3 radiance)
{
    radiance=0;
    if(lightCount>SecondaryMaxLightCount)return false;
    const bool actor=partCount!=0u&&instance==0u;
    uint part, positionOffset=0u, indexOffset=0u;
    if(actor){
        if(primitive>=triangleCount||vertexCount==0u)return false;
        part=CharacterIndices.Load(triangleCount*12u+primitive*4u);
        if(part>=partCount)return false;
    }else{
        uint mover=instance-(partCount!=0u?1u:0u);
        if(mover>=moverCount)return false;
        uint4 row=CharacterMoverGeometry.Load4(mover*16u);
        positionOffset=row.x;indexOffset=row.y;vertexCount=row.z;
        if(vertexCount==0u||primitive>=row.w/3u)return false;
        part=partCount+mover;
    }
    Binding binding=CharacterBindings[part];
    if(binding.materialIndex==0xffffffffu||(binding.flags&(SurfaceMissingBaseTexturesBinding|512u))!=0u||
       ((binding.flags&SurfaceBaseOnlyBinding)!=0u&&(binding.flags&SurfaceSecondaryParallaxBinding)==0u))return false;
    MaterialRecord material=CharacterMaterialRecords[binding.materialIndex];
    if(any(material.identity.xy!=binding.identity)||any(material.identity.zw!=binding.revision))return false;
    // Character textures occupy an independent descriptor bank. Only actual
    // descriptor indices are rebased; absent slots and location metadata stay unchanged.
    [unroll]for(uint i=0u;i<4u;++i){
        if(material.textures[i].x<512u)material.textures[i].x+=512u;
        if(material.textures[i].z<512u)material.textures[i].z+=512u;
    }
    uint3 ix;
    [branch]if(actor)ix=CharacterIndices.Load3(primitive*12u);
    else ix=CharacterMoverGeometry.Load3(indexOffset+primitive*12u);
    if(any(ix>=vertexCount))return false;
    NativeMaterialTriangle triangle=(NativeMaterialTriangle)0;
    triangle.hasPositions=true;triangle.hasTangents=true;
    [unroll]for(uint j=0u;j<3u;++j){
        uint offset=positionOffset+ix[j]*72u;
        [branch]if(actor){
            triangle.positions[j]=asfloat(CharacterPositions.Load3(offset));
            triangle.normals[j]=asfloat(CharacterPositions.Load3(offset+12u));
            triangle.uv[j]=asfloat(CharacterPositions.Load2(offset+24u));
            triangle.colors[j]=asfloat(CharacterPositions.Load4(offset+32u));
            triangle.tangents[j]=asfloat(CharacterPositions.Load3(offset+48u));
            triangle.bitangents[j]=asfloat(CharacterPositions.Load3(offset+60u));
        }else{
            triangle.positions[j]=asfloat(CharacterMoverGeometry.Load3(offset));
            triangle.normals[j]=asfloat(CharacterMoverGeometry.Load3(offset+12u));
            triangle.uv[j]=asfloat(CharacterMoverGeometry.Load2(offset+24u));
            triangle.colors[j]=asfloat(CharacterMoverGeometry.Load4(offset+32u));
            triangle.tangents[j]=asfloat(CharacterMoverGeometry.Load3(offset+48u));
            triangle.bitangents[j]=asfloat(CharacterMoverGeometry.Load3(offset+60u));
        }
        if(!all(isfinite(triangle.positions[j]))||!all(isfinite(triangle.normals[j]))||
           dot(triangle.normals[j],triangle.normals[j])<1e-8||!all(isfinite(triangle.uv[j]))||
           !all(isfinite(triangle.colors[j])))return false;
        triangle.hasTangents=triangle.hasTangents&&all(isfinite(triangle.tangents[j]))&&
           all(isfinite(triangle.bitangents[j]))&&dot(triangle.tangents[j],triangle.tangents[j])>1e-8&&
           dot(triangle.bitangents[j],triangle.bitangents[j])>1e-8;
    }
    float lod=clamp(log2(max(1.0,distance*max(footprint,0.001)/16.0)),0.0,8.0);
    float3 tangent=normalize(cross(rayDirection,abs(rayDirection.z)<0.99?float3(0,0,1):float3(0,1,0)));
    float3 bitangent=cross(rayDirection,tangent);
    float footprintWorld=max(0.01,distance*max(footprint,0.001)*0.01);
    float noise=NativeEmitterRandom(NativeEmitterHash(pixel.x^NativeEmitterHash(pixel.y)^frame),0x68bc21ebu);
    bool rawUsed;NativeCoatSample coat;NativeParallaxShadowSample height;
    Sample s=EvaluateMaterialTriangle(binding,material,triangle,0u,float3(1.0-bary.x-bary.y,bary),
        mul(worldToObject,rayDirection*distance),mul(worldToObject,tangent*footprintWorld),
        mul(worldToObject,bitangent*footprintWorld),1.0,distance,worldToObject,objectToWorld,
        (materialControls&1u)!=0u&&(binding.flags&SurfaceParallaxReadyBinding)!=0u,false,
        (materialControls&2u)!=0u,lod,0xffffffffu,world,noise,(RawRasterSurface)0,rawUsed,coat,height);
    return ShadeNativeSecondarySurface(s,coat,height,worldToObject,front,world,rayDirection,distance,pixel,
        lobe,frame,quality,instanceCount,characters,lightCount,sunDirection,sunRadiance,foliage,radiance);
}
#endif

#endif

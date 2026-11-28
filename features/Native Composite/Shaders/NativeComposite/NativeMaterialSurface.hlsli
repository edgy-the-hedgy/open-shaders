#ifndef NATIVE_MATERIAL_SURFACE_HLSLI
#define NATIVE_MATERIAL_SURFACE_HLSLI
#include "NativeSurface.hlsli"
#include "NativeRawRasterSurface.hlsli"
struct Sample { float4 color; float4 normal; float4 f0; float4 emission; };
float3 SrgbToLinear(float3 v)
{
    v = max(v,0);
    return lerp(v/12.92, pow((v+0.055)/1.055,2.4), v>0.04045);
}
#if defined(NATIVE_MATERIAL_PROJECTION)
#include "NativeProjection.hlsli"
#endif
// Extended Materials single-layer displacement convention:
// red-channel height, normalized height bound, centered 0.1*scale UV travel,
// Bounded coarse march and contact refinement. Recover UV gradients from the
// committed triangle and adjacent camera rays without extra scene queries.
bool NativeTriangleDifferential(float3 ray, float3 rayOffset, float rayScale,
    float3 e1, float3 e2, out float2 baryOffset, out float3 offset)
{
    baryOffset=0.0;
    offset=0.0;
    if (!all(isfinite(rayOffset)) || dot(rayOffset,rayOffset)<=0.0) return false;
    float3 face=cross(e1,e2);
    float areaSquared=dot(face,face);
    float3 neighbor=ray+rayOffset;
    float denominator=dot(face,neighbor);
    if (!isfinite(areaSquared) || areaSquared<=0.0 || !isfinite(denominator) ||
        abs(denominator)<=1e-6*sqrt(areaSquared)*length(neighbor)) return false;
    // Intersect the adjacent camera ray with the same triangle plane.
    // The raster-depth ray is rescaled to the actual committed hit distance.
    offset=rayScale*(rayOffset-neighbor*(dot(face,rayOffset)/denominator));
    baryOffset=float2(dot(cross(offset,e2),face),dot(cross(e1,offset),face))/areaSquared;
    return all(isfinite(baryOffset)) && all(isfinite(offset));
}
// Extended Materials' object warp fix: preserve height scale and flatten the
// view-ray slope only at bent, grazing surfaces. Use world-space derivatives
// so instance scale does not change the curvature calculation.
float NativeParallaxFlatten(float3 normal, float3 normalX, float3 normalY,
    float3 positionX, float3 positionY, float3 viewWorld)
{
    float fragmentSize=length(max(abs(positionX),abs(positionY)));
    float3 flat=-cross(positionX,positionY);
    if (!isfinite(fragmentSize) || fragmentSize<=0.0 || !all(isfinite(flat)) ||
        dot(flat,flat)<=0.0 || !all(isfinite(normal)) || !all(isfinite(normalX)) ||
        !all(isfinite(normalY)) || !all(isfinite(viewWorld))) return 0.0;
    float curvature=length(max(abs(normalX-normal),abs(normalY-normal)))/fragmentSize;
    float smoothness=saturate(1.0-dot(normal,normalize(flat)));
    float facing=saturate(dot(normal,viewWorld));
    float bend=saturate(smoothness+curvature);
    return saturate(saturate(1.0-2.0*facing)*bend/max(facing,0.0625));
}
float NativeParallaxMarchMip(uint textureIndex, float2 dx, float2 dy, float viewDistance, bool validGradients, float mipBias, out float shadowMip)
{
    uint width,height,layers,levels;
#if defined(NATIVE_CHARACTER_REFLECTION)
    if (textureIndex >= 512u)
        CharacterTextures[NonUniformResourceIndex(textureIndex-512u)].GetDimensions(0u,width,height,layers,levels);
    else
#endif
        Textures[NonUniformResourceIndex(textureIndex)].GetDimensions(0u,width,height,layers,levels);
    float baseMip=0.0;
    if (validGradients) {
        float2 dxSize=dx*float2(width,height), dySize=dy*float2(width,height);
        // Extended Materials uses the shorter gradient for its height footprint.
        float footprint=min(dot(dxSize,dxSize),dot(dySize,dySize));
        if (isfinite(footprint)) baseMip=max(0.5*log2(max(footprint,1.0)),0.0);
    }
    // Match Extended Materials::ComputeParallaxMarchMip, using the native hit distance.
    baseMip=floor(max(baseMip+mipBias,0.0));
    shadowMip=min(baseMip,float(max(levels,1u)-1u));
    float marchMip=baseMip+1.0+min(2.0,baseMip*0.5);
    float farFloor=lerp(0.0,3.0,saturate((viewDistance-512.0)/1536.0));
    return min(floor(max(marchMip,farFloor)),float(max(levels,1u)-1u));
}
float NativeHeightChannel(float4 value, bool alphaHeight) { return alphaHeight ? value.a : value.r; }
float2 NativeParallaxUV(uint textureIndex, uint mode, float2 coords, float3 viewTS, float scale, float mip, float flattenAmount, float heightMultiplier, bool alphaHeight)
{
    if (!isfinite(scale) || scale<=0.0 || !all(isfinite(viewTS)) || viewTS.z<=0.0) return coords;
    float2 slope=viewTS.xy/max(viewTS.z*0.7+0.3+flattenAmount,0.001);
    float maxHeight=0.1*scale;
    // Preserve bounded work even for extreme authored scales.
    uint steps=(uint(clamp(scale*16.0,4.0,16.0))+2u)&~3u;
    float stepSize=rcp(float(steps));
    float2 travel=slope*maxHeight;
    float previousBound=1.0, previousHeight=1.0;
    float bound=1.0, height=1.0;
    bool hit=false;
    [loop] for (uint k=1u;k<=steps;++k) {
        bound=1.0-float(k)*stepSize;
        float2 uv=coords+travel*(bound-0.5);
        height=saturate(NativeHeightChannel(TexLevel(textureIndex,uv,mode,mip),alphaHeight))*heightMultiplier;
        if (height>=bound) { hit=true; break; }
        previousBound=bound; previousHeight=height;
    }
    if (!hit) return coords;
    float start=previousBound;
    float fineStep=stepSize/float(steps);
    [loop] for (uint j=1u;j<=steps;++j) {
        bound=start-float(j)*fineStep;
        height=saturate(NativeHeightChannel(TexLevel(textureIndex,coords+travel*(bound-0.5),mode,mip),alphaHeight))*heightMultiplier;
        if (height>=bound) break;
        previousBound=bound; previousHeight=height;
    }
    float d0=previousBound-previousHeight, d1=bound-height;
    float contact=abs(d0-d1)>1e-6 ? lerp(previousBound,bound,saturate(d0/(d0-d1))) : bound;
    float2 result=coords+travel*(saturate(contact)-0.5);
    return all(isfinite(result)) ? result : coords;
}
float4 NativeSurfaceTex(uint index, float2 uv, uint mode, RawRasterSurface raw, float mipBias)
{
    if (!raw.valid) {
#if defined(NATIVE_SECONDARY_SURFACE)
        return TexLevel(index,uv,mode,max(mipBias,0.0));
#else
        return Tex(index,uv,mode);
#endif
    }
    float bias=exp2(clamp(mipBias,-8.0,8.0));
    float2 dx=raw.dx*bias,dy=raw.dy*bias;
    uint slot=NonUniformResourceIndex(index);
    if (mode==0u) return Textures[slot].SampleGrad(ClampClamp,float3(uv,0),dx,dy);
    if (mode==1u) return Textures[slot].SampleGrad(ClampWrap,float3(uv,0),dx,dy);
    if (mode==2u) return Textures[slot].SampleGrad(WrapClamp,float3(uv,0),dx,dy);
    return Textures[slot].SampleGrad(WrapWrap,float3(uv,0),dx,dy);
}
// Local-space for static instances; posed world-space for character geometry.
// The caller must supply the triangle and material from the same publication.
struct NativeMaterialTriangle
{
    float3 positions[3];
    float3 normals[3];
    float2 uv[3];
    float4 colors[3];
    float3 tangents[3];
    float3 bitangents[3];
    bool hasPositions;
    bool hasTangents;
};

Sample EvaluateMaterialTriangle(Binding binding, MaterialRecord material, NativeMaterialTriangle triData, uint instance, float3 weights, float3 rayObject, float3 rayOffsetX, float3 rayOffsetY, float rayScale, float viewDistance, float3x3 worldToObject, float3x3 objectToWorld, bool parallaxEnabled, bool warpFixEnabled, bool shadowEnabled, float mipBias, uint projectionRow, float3 worldPosition, float projectionNoise, RawRasterSurface raw, out bool rawUsed, out NativeCoatSample coat, out NativeParallaxShadowSample shadow)
{
    rawUsed = false;
    coat = (NativeCoatSample)0;
    shadow = (NativeParallaxShadowSample)0;
    Sample result = (Sample)0;
    if (binding.materialIndex == 0xffffffffu ||
        any(material.identity.xy != binding.identity) || any(material.identity.zw != binding.revision)) return result;
    // Validate all seven role descriptors before any height, coat or base texture
    // sampling. Optional roles may be absent, but never out of range.
    if (material.textures[0].x >= NativeTextureIndexLimit || material.textures[0].z >= NativeTextureIndexLimit)
        return result;
    [unroll] for (uint role=2u; role<7u; ++role) {
        uint descriptor=material.textures[role/2u][(role%2u)*2u];
        if (descriptor!=0xffffffffu && descriptor>=NativeTextureIndexLimit) return result;
    }
    if (!all(isfinite(material.base)) || !all(isfinite(material.uv[0]))) return result;
    float3 n = 0, t = 0, b = 0, color = 0;
    float vertexAlpha=0.0;
    float2 uv = 0;
    float2 triangleUV[3];
    float3 triangleNormal[3];
    [unroll] for (uint i = 0; i < 3u; ++i) {
        triangleNormal[i]=triData.normals[i];
        n += weights[i]*triangleNormal[i];
        triangleUV[i]=triData.uv[i];
        uv += weights[i]*triangleUV[i];
        color += weights[i]*triData.colors[i].rgb;
        vertexAlpha += weights[i]*triData.colors[i].a;
        if (triData.hasTangents) {
            t += weights[i]*triData.tangents[i];
            b += weights[i]*triData.bitangents[i];
        }
    }
    uv = uv*material.uv[0].zw+material.uv[0].xy;
    // Native material UV transforms remain authoritative. Do not admit a raster
    // input with incompatible coordinates or an object-space normal layout.
    // ABI v6 auxiliary data is not yet interpreted by this evaluator. Preserve the
    // base result, but do not certify an unimplemented technique as complete.
    const bool unsupportedRasterTechnique = raw.valid && raw.auxiliaryKind != 0u;
    raw.valid = raw.valid && all(abs(raw.uv-uv)<=max(0.001,abs(uv)*0.002));
    if (raw.valid) {
        float3 rn=mul(raw.normal,objectToWorld),rt=mul(worldToObject,raw.tangent),rb=mul(worldToObject,raw.bitangent);
        raw.valid=all(isfinite(rn)) && all(isfinite(rt)) && all(isfinite(rb)) &&
            dot(rn,rn)>1e-8 && dot(rt,rt)>1e-8 && dot(rb,rb)>1e-8;
        if (raw.valid) { uv=raw.uv; color=raw.color.rgb; vertexAlpha=raw.color.a; n=rn; t=rt; b=rb; rawUsed=true; }
    }
    if (dot(n,n) < 1e-8) return result;
    n = normalize(n);
    // Legacy 36-byte RSGA and rigid meshes without stored tangents can
    // derive a local basis from the actual triangle and its UV gradients.
    if (!raw.valid && (!triData.hasTangents || dot(t,t) < 1e-8 || dot(b,b) < 1e-8)) {
        if (!triData.hasPositions) return result;
        float3 p0=triData.positions[0];
        float3 e1=triData.positions[1]-p0;
        float3 e2=triData.positions[2]-p0;
        float2 duv1=triangleUV[1]-triangleUV[0], duv2=triangleUV[2]-triangleUV[0];
        float determinant=duv1.x*duv2.y-duv1.y*duv2.x;
        if (!isfinite(determinant) || abs(determinant)<1e-10) {
            // Object-space normal maps do not need a UV-derived basis.
            if ((binding.flags & 512u)==0u) return result;
            t=abs(n.z)<0.99 ? cross(float3(0,0,1),n) : cross(float3(0,1,0),n);
            b=cross(n,t);
        } else {
            t=(e1*duv2.y-e2*duv1.y)/determinant;
            b=(e2*duv1.x-e1*duv2.x)/determinant;
        }
    }
    t -= n*dot(n,t);
    if (dot(t,t) < 1e-8) return result;
    t = normalize(t);
    b = cross(n,t)*(dot(cross(n,t),b)<0.0 ? -1.0 : 1.0);
    float2 originalUV=uv;
    const bool legacyMaterial=(material.controls.y & (1u<<9u))!=0u;
    const bool legacyMask=(material.controls.y & (1u<<8u))!=0u;
    const bool alphaHeight=(material.controls.y & (1u<<10u))!=0u;
    bool complexMask=false;
    if (legacyMask && material.textures[1].x!=0xffffffffu) {
        const float epsilon=4.0/255.0;
        float4 coarse=TexLevel(material.textures[1].x,uv,material.properties.z,15.0);
        bool grayscale=abs(coarse.x-coarse.y)<epsilon && abs(coarse.x-coarse.z)<epsilon && abs(coarse.y-coarse.z)<epsilon;
        bool heightOnly=all(coarse.xyz<epsilon)&&coarse.a>epsilon&&coarse.a<1.0-epsilon;
        complexMask=coarse.a<1.0-epsilon&&(!grayscale||heightOnly);
    }
    bool interlayer=(binding.flags & SurfaceCoatReadyBinding)!=0u && (material.properties.x & 8u)!=0u;
    float3 entryNormal=n;
    float eta=1.0;
    bool interlayerActive=false;
    if (parallaxEnabled && (binding.flags & SurfaceParallaxReadyBinding)!=0u && material.textures[2].x!=0xffffffffu &&
        (binding.flags & 512u)==0u && dot(rayObject,rayObject)>1e-8 && triData.hasPositions &&
        (!alphaHeight || complexMask)) {
        float3 view=normalize(-rayObject);
        if (interlayer) {
            float strength=saturate(material.coat.x);
            if (material.textures[2].z!=0xffffffffu)
                strength*=saturate(NativeSurfaceTex(material.textures[2].z,originalUV,material.properties.z,raw,mipBias).a);
            float3 mapped;
            if ((material.properties.x & 16u)!=0u && material.textures[3].x!=0xffffffffu)
                mapped=NativeSurfaceTex(material.textures[3].x,originalUV,material.properties.z,raw,mipBias).xyz*2.0-1.0;
            else {
                mapped=NativeSurfaceTex(material.textures[0].z,originalUV,material.properties.z,raw,mipBias).xyz*2.0-1.0;
                if ((binding.flags & 256u)!=0u) mapped.z=sqrt(saturate(1.0-dot(mapped.xy,mapped.xy)));
            }
            float3 candidate=t*mapped.x+b*mapped.y+n*mapped.z;
            if (!all(isfinite(candidate)) || dot(candidate,candidate)<1e-8 ||
                !isfinite(strength) || !isfinite(material.coat.z)) return result;
            entryNormal=normalize(candidate);
            float rootF0=sqrt(saturate(material.coat.z));
            eta=lerp(1.0,(1.0-rootF0)/(1.0+rootF0),strength);
            view=NativeRefractDirection(view,entryNormal,eta);
            interlayerActive=true;
        }
        float3 viewTS=float3(dot(t,view),dot(b,view),dot(n,view));
        float3 p0=triData.positions[0];
        float3 e1=triData.positions[1]-p0;
        float3 e2=triData.positions[2]-p0;
        float2 uv1=(triangleUV[1]-triangleUV[0])*material.uv[0].zw;
        float2 uv2=(triangleUV[2]-triangleUV[0])*material.uv[0].zw;
        float2 baryX,baryY;
        float3 positionX,positionY;
        bool validX=NativeTriangleDifferential(rayObject,rayOffsetX,rayScale,e1,e2,baryX,positionX);
        bool validY=NativeTriangleDifferential(rayObject,rayOffsetY,rayScale,e1,e2,baryY,positionY);
        float2 dx=baryX.x*uv1+baryX.y*uv2;
        float2 dy=baryY.x*uv1+baryY.y*uv2;
        float shadowMip;
        if (raw.valid) { dx=raw.dx; dy=raw.dy; }
        float mip=NativeParallaxMarchMip(material.textures[2].x,dx,dy,viewDistance,raw.valid || (validX && validY),mipBias,shadowMip);
        float flattenAmount=0.0;
        if (warpFixEnabled && validX && validY) {
            float3 normal1=triangleNormal[1]-triangleNormal[0];
            float3 normal2=triangleNormal[2]-triangleNormal[0];
            float3 interpolatedNormal=weights.x*triangleNormal[0]+weights.y*triangleNormal[1]+weights.z*triangleNormal[2];
            float3 normalWorld=mul(interpolatedNormal,worldToObject);
            float3 normalX=mul(interpolatedNormal+baryX.x*normal1+baryX.y*normal2,worldToObject);
            float3 normalY=mul(interpolatedNormal+baryY.x*normal1+baryY.y*normal2,worldToObject);
            float3 viewWorld=mul(objectToWorld,-rayObject);
            if (dot(normalWorld,normalWorld)>0.0 && dot(normalX,normalX)>0.0 &&
                dot(normalY,normalY)>0.0 && dot(viewWorld,viewWorld)>0.0)
                flattenAmount=NativeParallaxFlatten(normalize(normalWorld),normalize(normalX),normalize(normalY),
                    mul(objectToWorld,positionX),mul(objectToWorld,positionY),normalize(viewWorld));
        }
        uv=NativeParallaxUV(material.textures[2].x,material.properties.z,uv,viewTS,material.base.z,mip,flattenAmount,interlayerActive ? 0.5 : 1.0,alphaHeight);
        if (shadowEnabled && material.base.z>0.0 && material.base.z<=65504.0 && isfinite(material.base.z) &&
            viewTS.z>0.0 && all(isfinite(uv))) {
            float3 worldTangent=mul(t,worldToObject);
            float3 worldBitangent=mul(b,worldToObject);
            float height=NativeHeightChannel(TexLevel(material.textures[2].x,uv,material.properties.z,shadowMip),alphaHeight);
            if (all(isfinite(worldTangent)) && all(isfinite(worldBitangent)) && isfinite(height) &&
                dot(worldTangent,worldTangent)>0.0 && dot(worldBitangent,worldBitangent)>0.0) {
                shadow.uv=uv;
                // Zero is the invalid record; use descriptor index + 1.
                shadow.textureAndMode=(material.textures[2].x+1u)|((material.properties.z&3u)<<16u);
                shadow.height=saturate(height)*(interlayerActive ? 0.5 : 1.0);
                if (interlayerActive) shadow.textureAndMode|=1u<<18u;
                if (alphaHeight) shadow.textureAndMode|=1u<<19u;
                shadow.tangentScale=float4(normalize(worldTangent),material.base.z);
                shadow.bitangentMip=float4(normalize(worldBitangent),shadowMip);
            }
        }
    }
    float4 baseTexel = NativeSurfaceTex(material.textures[0].x,uv,material.properties.z,raw,mipBias);
    float4 normalTexel = NativeSurfaceTex(material.textures[0].z,uv,material.properties.z,raw,mipBias);
    float4 packed = material.textures[1].x!=0xffffffffu ? NativeSurfaceTex(material.textures[1].x,uv,material.properties.z,raw,mipBias) : float4(1.0,0.0,1.0,0.04);
    if (!all(isfinite(baseTexel)) || !all(isfinite(normalTexel)) || !all(isfinite(packed))) return result;
    float4 legacyPacked=packed;
    if (legacyMaterial) packed=float4(1.0-saturate(normalTexel.a),0.0,1.0,1.0);
    float3 sampled = normalTexel.xyz*2.0-1.0;
    if ((binding.flags & 256u) != 0u) sampled.z = sqrt(saturate(1.0-dot(sampled.xy,sampled.xy)));
    if (dot(sampled,sampled)<1e-8) return result;
    float3 normal = (binding.flags & 512u) != 0u ? normalize(sampled) : normalize(t*sampled.x+b*sampled.y+n*sampled.z);
    float3 base = baseTexel.rgb;
    packed.r*=material.base.y;
    packed.a*=material.base.w;
    float projectionWeight=0.0;
    bool projectionApplied=false;
#if defined(NATIVE_MATERIAL_PROJECTION)
    if (projectionRow!=0xffffffffu && triData.hasPositions && (binding.flags&512u)==0u) {
        float3 p0=triData.positions[0];
        float3 e1=triData.positions[1]-p0;
        float3 e2=triData.positions[2]-p0;
        float2 baryX,baryY;
        float3 dx,dy;
        bool validX=NativeTriangleDifferential(rayObject,rayOffsetX,rayScale,e1,e2,baryX,dx);
        bool validY=NativeTriangleDifferential(rayObject,rayOffsetY,rayScale,e1,e2,baryY,dy);
        if (validX && validY)
            projectionApplied=NativeApplyProjection(projectionRow,instance,worldPosition,mul(objectToWorld,dx),mul(objectToWorld,dy),
                objectToWorld,worldToObject,cross(e1,e2),n,t,b,(binding.flags&1u)!=0u ? saturate(vertexAlpha) : 1.0,
                projectionNoise,material,base,normal,packed,projectionWeight);
    }
#endif
    if ((binding.flags & 1u) != 0u) base *= SrgbToLinear(color);
    if (!all(isfinite(packed))) return result;
    float metallic = saturate(packed.g);
    float roughness = clamp(packed.r,0.045,1.0);
    float3 f0 = lerp(saturate(packed.a).xxx,base,metallic);
    if (legacyMask) {
        const float strength=saturate(legacyPacked.r);
        if (complexMask && legacyPacked.g>4.0/255.0) {
            metallic=saturate(legacyPacked.b);
            roughness=clamp(1.0-legacyPacked.g,0.045,1.0);
            f0=lerp(float3(0.04,0.04,0.04),lerp(float3(1,1,1),base,metallic),strength);
        } else {
            f0=lerp(f0,float3(1,1,1),strength);
        }
    }
    float3 emission = 0;
    if (material.textures[1].z != 0xffffffffu) emission = NativeSurfaceTex(material.textures[1].z,uv,material.properties.z,raw,mipBias).rgb;
    // The packed surface contract includes roughness, metallic and AO, not only RGB.
    // Reject non-finite scalar channels before they can become a valid half record.
    if (!all(isfinite(base)) || !all(isfinite(normal)) || !all(isfinite(f0)) || !all(isfinite(emission)) ||
        !isfinite(roughness) || !isfinite(metallic) || !isfinite(packed.b)) return result;
    #if defined(NATIVE_SECONDARY_SURFACE)
    result.color = float4(base,projectionRow!=0xffffffffu && !projectionApplied ? SurfaceFeatureLimited : SurfaceEvaluatedBase);
#else
    // Preserve primary ownership policy until projected receiver parity is verified.
    result.color = float4(base,projectionRow!=0xffffffffu ? SurfaceFeatureLimited : SurfaceEvaluatedBase);
#endif
    result.normal = float4(normal,roughness);
    result.f0 = float4(f0,metallic);
    result.emission = float4(emission,saturate(packed.b));
    if (unsupportedRasterTechnique) result.color.a = SurfaceFeatureLimited;
    if ((binding.flags & SurfaceSubsurfaceReadyBinding) != 0u) {
        float3 scatterColor = SrgbToLinear(max(material.subsurfaceColor.rgb,0.0));
        float thickness = saturate(material.coat.w);
        if (material.textures[2].z != 0xffffffffu) {
            float4 feature0 = NativeSurfaceTex(material.textures[2].z,uv,material.properties.z,raw,mipBias);
            scatterColor *= SrgbToLinear(max(feature0.rgb,0.0));
            thickness *= saturate(feature0.a);
        }
        if (all(isfinite(scatterColor)) && isfinite(thickness)) coat.subsurface=float4(scatterColor,thickness);
        else result.color.a=SurfaceFeatureLimited;
    }
    if ((binding.flags & SurfaceThinSurfaceReadyBinding) != 0u) {
        if (!triData.hasPositions) { result.color.a=SurfaceFeatureLimited; return result; }
        float3 scatterColor=0.0;
        float thickness=1.0;
        if ((material.properties.x & 1u)!=0u) {
            scatterColor=SrgbToLinear(max(material.subsurfaceColor.rgb,0.0));
            thickness=saturate(material.coat.w);
            if (material.textures[2].z!=0xffffffffu) {
                float4 feature0=NativeSurfaceTex(material.textures[2].z,uv,material.properties.z,raw,mipBias);
                scatterColor*=SrgbToLinear(max(feature0.rgb,0.0));
                thickness*=saturate(feature0.a);
            } else scatterColor*=base*(1.0-metallic);
        }
        float3 p0=triData.positions[0];
        float3 p1=triData.positions[1];
        float3 p2=triData.positions[2];
        float3 face=cross(p1-p0,p2-p0);
        if (!all(isfinite(face)) || dot(face,face)<1e-8 || !all(isfinite(scatterColor)) ||
            !isfinite(thickness) || !isfinite(material.surface.y) || !all(isfinite(material.thinSurface.xyz))) {
            result.color.a=SurfaceFeatureLimited; return result;
        }
        coat.normalRoughness=float4(normalize(face),saturate(material.surface.y));
        coat.f0Colored=float4(saturate(material.thinSurface.xy),max(material.thinSurface.z,0.0),
            (binding.flags & SurfaceFoliageReadyBinding)!=0u ? NativeFoliageTag : NativeThinTag);
        coat.subsurface=float4(scatterColor,thickness);
    }
    if ((binding.flags & SurfaceFuzzReadyBinding) != 0u) {
        float3 fuzzColor=SrgbToLinear(max(material.fuzzColor.rgb,0.0));
        float weight=saturate(material.surface.x);
        float fuzzRoughness=material.surface.z;
        if (material.textures[3].x!=0xffffffffu) {
            float4 feature1=NativeSurfaceTex(material.textures[3].x,uv,material.properties.z,raw,mipBias);
            fuzzColor*=SrgbToLinear(max(feature1.rgb,0.0));
            weight*=saturate(feature1.a);
        }
        if ((material.controls.y & 4u)!=0u) {
            if (material.textures[2].x==0xffffffffu) { result.color.a=SurfaceFeatureLimited; return result; }
            fuzzRoughness=NativeSurfaceTex(material.textures[2].x,uv,material.properties.z,raw,mipBias).g;
        }
        if (!all(isfinite(fuzzColor)) || !isfinite(weight) || !isfinite(fuzzRoughness)) {
            result.color.a=SurfaceFeatureLimited; return result;
        }
        if (HasNativeThinSurface(coat)) {
            coat.f0Colored.w=EncodeNativeThinFuzz(fuzzRoughness,HasNativeFoliage(coat));
        } else {
            coat.normalRoughness=float4(0.0,0.0,0.0,clamp(fuzzRoughness,-1.0,1.0));
            coat.f0Colored.w=NativeFuzzTag;
        }
        coat.colorStrength=float4(saturate(fuzzColor),weight);
    }
    if ((binding.flags & SurfaceCoatReadyBinding) != 0u) {
        // Match the legacy feature0 color/strength and feature1 roughness/normal semantics.
        float3 coatColor = SrgbToLinear(max(material.coatColor.rgb,0.0));
        float strength = saturate(material.coat.x);
        float coatRoughness = material.coat.y;
        float2 coatUV=interlayer ? originalUV : uv;
        float3 coatNormal = interlayerActive ? entryNormal : normal;
        if (material.textures[2].z != 0xffffffffu) {
            float4 feature0 = NativeSurfaceTex(material.textures[2].z,coatUV,material.properties.z,raw,mipBias);
            coatColor *= SrgbToLinear(feature0.rgb);
            strength *= saturate(feature0.a);
        }
        if (material.textures[3].x != 0xffffffffu) {
            float4 feature1 = NativeSurfaceTex(material.textures[3].x,coatUV,material.properties.z,raw,mipBias);
            coatRoughness *= feature1.a;
            if ((material.properties.x & 16u) != 0u) {
                float3 mapped = feature1.xyz * 2.0 - 1.0;
                float3 candidate = t*mapped.x + b*mapped.y + n*mapped.z;
                if (!all(isfinite(candidate)) || dot(candidate,candidate) < 1e-8) { result.color.a=SurfaceFeatureLimited; return result; }
                coatNormal = normalize(candidate);
            }
        }
        if (!all(isfinite(coatColor)) || !isfinite(strength) || !isfinite(coatRoughness) || !isfinite(material.coat.z)) {
            result.color.a=SurfaceFeatureLimited; return result;
        }
        coat.normalRoughness=float4(coatNormal,clamp(coatRoughness,0.045,1.0));
        coat.colorStrength=float4(coatColor,strength);
        coat.f0Colored=float4(saturate(material.coat.z).xxx,(material.properties.x & 4u) != 0u ? 1.0 : 0.0);
        if (interlayerActive) coat.subsurface.w=2.0+eta;
    }
    if (projectionWeight>0.0) {
        coat.colorStrength.w*=1.0-projectionWeight;
        if (HasNativeSubsurface(coat) || HasNativeThinSurface(coat)) coat.subsurface.w=lerp(coat.subsurface.w,1.0,projectionWeight);
    }
    return result;
}

Sample EvaluateMaterial(uint instanceCount, uint instance, uint primitive, float3 weights, float3 rayObject, float3 rayOffsetX, float3 rayOffsetY, float rayScale, float viewDistance, float3x3 worldToObject, float3x3 objectToWorld, bool parallaxEnabled, bool warpFixEnabled, bool shadowEnabled, float mipBias, uint projectionRow, float3 worldPosition, float projectionNoise, RawRasterSurface raw, out bool rawUsed, out NativeCoatSample coat, out NativeParallaxShadowSample shadow)
{
    rawUsed = false;
    coat = (NativeCoatSample)0;
    shadow = (NativeParallaxShadowSample)0;
    if (instance >= instanceCount) return (Sample)0;
    Binding binding = Bindings[instance];
    uint4 offsets = Geometry.Load4(instance*32);
    uint4 counts = Geometry.Load4(instance*32+16);
    if (binding.materialIndex == 0xffffffffu || offsets.z == 0xffffffffu ||
        (offsets.w != 60u && offsets.w != 36u) || counts.y < 3u || primitive >= counts.y / 3u) return (Sample)0;
    uint materialCount, materialStride;
    MaterialRecords.GetDimensions(materialCount,materialStride);
    if (binding.materialIndex >= materialCount) return (Sample)0;
    MaterialRecord material = MaterialRecords[binding.materialIndex];
    if (any(material.identity.xy != binding.identity) || any(material.identity.zw != binding.revision)) return (Sample)0;
    uint3 ix = Geometry.Load3(offsets.y + primitive*12u);
    if (any(ix >= counts.x)) return (Sample)0;
    NativeMaterialTriangle triData = (NativeMaterialTriangle)0;
    triData.hasPositions = counts.z >= 12u;
    triData.hasTangents = offsets.w == 60u;
    [unroll] for (uint i = 0; i < 3u; ++i) {
        uint a = offsets.z + ix[i]*offsets.w;
        triData.normals[i] = asfloat(Geometry.Load3(a));
        triData.uv[i] = asfloat(Geometry.Load2(a+12));
        triData.colors[i] = asfloat(Geometry.Load4(a+20));
        if (triData.hasTangents) {
            triData.tangents[i] = asfloat(Geometry.Load3(a+36));
            triData.bitangents[i] = asfloat(Geometry.Load3(a+48));
        }
        if (triData.hasPositions)
            triData.positions[i] = asfloat(Geometry.Load3(offsets.x+ix[i]*counts.z));
    }
    return EvaluateMaterialTriangle(binding,material,triData,instance,weights,rayObject,rayOffsetX,rayOffsetY,
        rayScale,viewDistance,worldToObject,objectToWorld,parallaxEnabled,warpFixEnabled,shadowEnabled,mipBias,
        projectionRow,worldPosition,projectionNoise,raw,rawUsed,coat,shadow);
}

#endif

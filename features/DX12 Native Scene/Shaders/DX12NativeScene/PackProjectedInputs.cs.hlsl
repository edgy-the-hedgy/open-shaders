// Copy the actual GPU geometry constants, not the engine's expired Map pointer.
ByteAddressBuffer VertexConstants : register(t0);
ByteAddressBuffer PixelConstants : register(t1);
cbuffer PackParameters : register(b0)
{
    uint4 Offsets;
    uint4 Metadata; // TLAS instance, shader descriptor, row ordinal, reserved
    uint4 TextureIndices;
    uint4 ExtraTextureIndices;
};
RWTexture2D<uint4> ProjectionRecords : register(u0);
[numthreads(1,1,1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint row = Metadata.z;
    ProjectionRecords[uint2(0,row)] = Metadata;
    // Terrain keeps its original world transform and LOD projection inputs.
    // Offsets.z/w are optional material constants; UINT_MAX denotes absence.
    if (Metadata.x==0xfffffff9u) {
        ProjectionRecords[uint2(1,row)]=VertexConstants.Load4(Offsets.y);
        [unroll] for (uint i=0u;i<3u;++i)
            ProjectionRecords[uint2(2u+i,row)]=VertexConstants.Load4(Offsets.x+16u*i);
        uint4 lod=0u;
        if (Offsets.z!=0xffffffffu) lod=PixelConstants.Load4(Offsets.z);
        lod.xy=uint2(Metadata.y,ExtraTextureIndices.z);
        ProjectionRecords[uint2(5,row)]=lod;
        ProjectionRecords[uint2(6,row)]=Offsets.w==0xffffffffu ? uint4(0u,0u,0u,0x7fc00000u) : PixelConstants.Load4(Offsets.w);
        ProjectionRecords[uint2(7,row)]=TextureIndices;
        return;
    }
    // PBR grass preserves the actual original b1 material constants.
    if (Metadata.x==0xfffffffau) {
        [unroll] for (uint column=1u;column<=5u;++column)
            ProjectionRecords[uint2(column,row)]=PixelConstants.Load4((column-1u)*16u);
        ProjectionRecords[uint2(6,row)]=ExtraTextureIndices;
        ProjectionRecords[uint2(7,row)]=TextureIndices;
        return;
    }
    // Reserved non-scene row kinds: grass, distant tree, effect and model-space gloss.
    // Offsets carries authored Effect tint rather than constant-buffer offsets.
    if (Metadata.x >= 0xfffffffbu && Metadata.x <= 0xfffffffeu) {
        ProjectionRecords[uint2(1,row)] = Offsets;
        [unroll] for (uint column=2u; column<7u; ++column)
            ProjectionRecords[uint2(column,row)] = 0u;
        ProjectionRecords[uint2(7,row)] = TextureIndices;
        return;
    }
    // Skinned primary receivers carry the posed direction in raster evidence.
    // No unposed VS matrix is needed or valid for secondary correspondence.
    if (Offsets.x==0xffffffffu) {
        ProjectionRecords[uint2(1,row)] = 0u;
        ProjectionRecords[uint2(2,row)] = 0u;
        ProjectionRecords[uint2(3,row)] = 0u;
    } else {
        ProjectionRecords[uint2(1,row)] = VertexConstants.Load4(Offsets.x);
        ProjectionRecords[uint2(2,row)] = VertexConstants.Load4(Offsets.x+16u);
        ProjectionRecords[uint2(3,row)] = VertexConstants.Load4(Offsets.x+32u);
    }
    ProjectionRecords[uint2(4,row)] = PixelConstants.Load4(Offsets.y);
    ProjectionRecords[uint2(5,row)] = PixelConstants.Load4(Offsets.z);
    ProjectionRecords[uint2(6,row)] = PixelConstants.Load4(Offsets.w);
    ProjectionRecords[uint2(7,row)] = TextureIndices;
}

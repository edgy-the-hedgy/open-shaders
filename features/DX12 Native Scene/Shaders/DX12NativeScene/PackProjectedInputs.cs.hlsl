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
    ProjectionRecords[uint2(1,row)] = VertexConstants.Load4(Offsets.x);
    ProjectionRecords[uint2(2,row)] = VertexConstants.Load4(Offsets.x+16u);
    ProjectionRecords[uint2(3,row)] = VertexConstants.Load4(Offsets.x+32u);
    ProjectionRecords[uint2(4,row)] = PixelConstants.Load4(Offsets.y);
    ProjectionRecords[uint2(5,row)] = PixelConstants.Load4(Offsets.z);
    ProjectionRecords[uint2(6,row)] = PixelConstants.Load4(Offsets.w);
    ProjectionRecords[uint2(7,row)] = TextureIndices;
}

#ifndef NATIVE_RASTER_DRAW_IDENTITY_HLSLI
#define NATIVE_RASTER_DRAW_IDENTITY_HLSLI
// Matches RasterDrawRecord (80 bytes). A row is an original draw, not a
// primitive lookup. Zero identity means no certified visible input.
static const uint NativeRasterDrawStride = 80u;
bool DecodeRasterDraw(ByteAddressBuffer table, uint drawCount, uint2 encoded, out uint address, out uint2 sceneIdentity)
{
    address = 0u;
    sceneIdentity = uint2(0xffffffffu, 0xffffffffu);
    if (encoded.x == 0u || encoded.x > drawCount) return false;
    address = (encoded.x - 1u) * NativeRasterDrawStride;
    uint4 identity = table.Load4(address);
    uint4 range = table.Load4(address + 16u);
    uint4 instances = table.Load4(address + 64u);
    if (identity.x != encoded.x || range.x == 0u || instances.x == 0u ||
        encoded.y / range.x >= instances.x) return false;
    if (identity.w != 0xffffffffu && instances.x == 1u &&
        encoded.y <= 0x7fffffffu - range.y)
        sceneIdentity = uint2(identity.w, range.y + encoded.y);
    return true;
}
#endif

using System.Numerics;
using System.Text.Json;

// Transient IPC only. The host owns the session directory and removes it.
// RSGT v2 envelope: little-endian magic, version, metadata length, UTF-8 JSON,
// then RSGM v1 blobs. Placed shapes reference a zero-based blob index.
internal static class TileTransfer
{
    private const uint TileMagic = 0x54475352;
    private const uint GeometryMagic = 0x4d475352;
    private const int MaximumMetadataBytes = 16 * 1024 * 1024;

    internal static void Write(string path, TileResult result, JsonSerializerOptions options, FileMode mode)
    {
        var geometry = new List<(Vector3[] Positions, uint[] Indices, byte[]? Shading, MaterialSource? Material)>();
        var lookup = new Dictionary<(Vector3[], uint[], byte[]?, MaterialSource?), int>();
        var placements = result.Shapes.Select(shape =>
        {
            // Decoded NIF arrays are immutable and shared across placements in a tile.
            var key = (shape.Positions, shape.Indices, shape.Shading, shape.Material);
            if (!lookup.TryGetValue(key, out var index))
            {
                index = geometry.Count;
                lookup.Add(key, index);
                geometry.Add(key);
            }
            return new { shape.Identity, Geometry = index, shape.Transform };
        }).ToArray();
        var metadata = JsonSerializer.SerializeToUtf8Bytes(new
        {
            Version = 2, result.EligibleShapes, result.FailedShapes, result.EnumerationComplete,
            Shapes = placements, GeometryCount = geometry.Count, result.Lights, result.Failure,
            result.FullNative, Materials = result.FullNative ? geometry.Select(blob => blob.Material).ToArray() : null,
            result.RejectionReasons, result.NifSources, result.MissingShapes
        }, options);
        if (metadata.Length > MaximumMetadataBytes)
            throw new InvalidDataException("Companion metadata size exceeded");
        using var stream = new FileStream(path, mode, FileAccess.Write, FileShare.None);
        using var writer = new BinaryWriter(stream);
        writer.Write(TileMagic);
        writer.Write(2u);
        writer.Write((uint)metadata.Length);
        writer.Write(metadata);
        foreach (var blob in geometry)
        {
            writer.Write(GeometryMagic);
            writer.Write(1u);
            writer.Write(checked((uint)blob.Positions.Length));
            writer.Write(checked((uint)blob.Indices.Length));
            foreach (var position in blob.Positions)
            {
                writer.Write(position.X);
                writer.Write(position.Y);
                writer.Write(position.Z);
            }
            foreach (var index in blob.Indices) writer.Write(index);
            if (result.FullNative)
            {
                writer.Write((uint)(blob.Shading?.Length ?? 0));
                if (blob.Shading != null) writer.Write(blob.Shading);
            }
        }
    }
}

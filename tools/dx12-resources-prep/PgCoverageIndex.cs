using System.Text.Json;

internal sealed record PgShapeCoverage(string MaterialId, string Classification, string SnowCoverage);

internal sealed class PgCoverageIndex
{
    private readonly Dictionary<(string Mesh, int BlockIndex), PgShapeCoverage> shapes = new();

    internal static PgCoverageIndex ReadCached(string data, IEnumerable<CompiledNif> assets)
    {
        var index = new PgCoverageIndex();
        var file = Path.Combine(data, "SKSE", "Plugins", "CommunityShaders", "DX12-cache", "pg-references", "index-v2.json");
        if (!File.Exists(file) || new FileInfo(file).Length > 128L * 1024 * 1024) return index;
        var expected = assets.ToDictionary(asset => asset.SourcePath, asset => asset.SourceSha256,
            StringComparer.OrdinalIgnoreCase);
        using var document = JsonDocument.Parse(File.ReadAllBytes(file));
        var root = document.RootElement;
        if (root.GetProperty("kind").GetString() != "dx12-pg-references" ||
            root.GetProperty("schemaVersion").GetInt32() != 2 ||
            root.GetProperty("discoveryScope").GetString() != "pg-patcher-outputs-only")
            throw new InvalidDataException("Cached PG reference index contract mismatch.");
        var materials = root.GetProperty("materials").EnumerateArray().Select(row =>
            (Id: row.GetProperty("id").GetString()!, Classification: row.GetProperty("classification").GetString()!)).ToArray();
        foreach (var mesh in root.GetProperty("meshes").EnumerateArray())
        {
            var path = mesh.GetProperty("path").GetString()!;
            if (!expected.TryGetValue(path, out var sha) ||
                !sha.Equals(mesh.GetProperty("sha256").GetString(), StringComparison.OrdinalIgnoreCase))
                continue;
            foreach (var shape in mesh.GetProperty("shapes").EnumerateArray())
            {
                var materialIndex = shape.GetProperty("materialIndex").GetInt32();
                if (materialIndex < 0 || materialIndex >= materials.Length)
                    throw new InvalidDataException("Cached PG material reference is out of bounds.");
                var record = materials[materialIndex];
                if (!index.shapes.TryAdd((path, shape.GetProperty("blockIndex").GetInt32()),
                    new(record.Id, record.Classification, shape.GetProperty("snowCoverage").GetString()!)))
                    throw new InvalidDataException("Duplicate PG mesh shape identity.");
            }
        }
        return index;
    }

    internal PgShapeCoverage? Find(string meshPath, int blockIndex)
    {
        var key = PgAssets.Normalize(meshPath);
        if (!key.StartsWith("meshes\\", StringComparison.Ordinal)) key = "meshes\\" + key;
        return shapes.GetValueOrDefault((key, blockIndex));
    }
}

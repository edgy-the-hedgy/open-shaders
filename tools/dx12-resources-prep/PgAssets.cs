using System.Security.Cryptography;
using System.Text.Json;
using ICSharpCode.SharpZipLib.Checksum;

internal sealed record PgAsset(string Path, string Status, string? SourcePackage, uint? ExpectedPatchedCrc32,
    uint? ActualCrc32, string? Sha256, string? CachedSource, bool CacheHit);

internal sealed record PgOutputMesh(string Path, uint? ExpectedPatchedCrc32, string? SourcePackage);

internal sealed class PgAssets : IDisposable
{
    private readonly string data;
    private readonly string output;
    private readonly JsonDocument? diff;
    private readonly JsonDocument? deployment;
    private readonly Dictionary<string, JsonElement> patches;
    private readonly Dictionary<string, string> sources;
    private readonly string? pgPackage;
    private readonly Dictionary<string, (string Sha256, string? SourcePackage)>? indexedMeshes;
    public string IndexSourceDigest { get; } = "";

    public PgAssets(string dataDirectory, string outputDirectory, bool pgOutputOnly = false, bool indexOnly = false)
    {
        data = dataDirectory;
        output = outputDirectory;
        if (indexOnly)
        {
            var file = System.IO.Path.Combine(data, "SKSE", "Plugins", "CommunityShaders", "DX12-cache", "pg-references", "index-v2.json");
            if (!File.Exists(file) || new FileInfo(file).Length > 128L * 1024 * 1024)
                throw new InvalidDataException("Prepare the PG Reference Index manually from the DX12 Resources main menu first.");
            using var index = JsonDocument.Parse(File.ReadAllBytes(file));
            var root = index.RootElement;
            if (root.GetProperty("kind").GetString() != "dx12-pg-references" ||
                root.GetProperty("schemaVersion").GetInt32() != 2 ||
                root.GetProperty("discoveryScope").GetString() != "pg-patcher-outputs-only")
                throw new InvalidDataException("Cached PG Reference Index contract mismatch.");
            IndexSourceDigest = root.GetProperty("pgDiffSha256").GetString()!;
            indexedMeshes = root.GetProperty("meshes").EnumerateArray().ToDictionary(
                row => Normalize(row.GetProperty("path").GetString()!),
                row => (row.GetProperty("sha256").GetString()!,
                    row.GetProperty("sourcePackage").ValueKind == JsonValueKind.String ?
                        row.GetProperty("sourcePackage").GetString() : null),
                StringComparer.OrdinalIgnoreCase);
            patches = new(StringComparer.OrdinalIgnoreCase);
            sources = new(StringComparer.OrdinalIgnoreCase);
            return;
        }
        using var diffInput = File.OpenRead(System.IO.Path.Combine(data, "ParallaxGen_Diff.json"));
        diff = JsonDocument.Parse(diffInput);
        using var deploymentInput = File.OpenRead(System.IO.Path.Combine(data, "vortex.deployment.json"));
        deployment = JsonDocument.Parse(deploymentInput);
        patches = diff.RootElement.EnumerateObject().ToDictionary(p => Normalize(p.Name), p => p.Value, StringComparer.OrdinalIgnoreCase);
        sources = new(StringComparer.OrdinalIgnoreCase);
        foreach (var row in deployment.RootElement.GetProperty("files").EnumerateArray())
        {
            var path = Normalize(row.GetProperty("relPath").GetString()!);
            if (!pgOutputOnly || patches.ContainsKey(path) || path == "parallaxgen_diff.json" ||
                path.StartsWith("meshes\\_pgpatcher_dups\\", StringComparison.Ordinal) ||
                (path.StartsWith("textures\\", StringComparison.Ordinal) && IsPgSnowTexture(path)))
                sources[path] = row.GetProperty("source").GetString()!;
        }
        sources.TryGetValue("parallaxgen_diff.json", out pgPackage);
    }

    public static string Normalize(string path)
    {
        var normalized = path.Replace('/', '\\').ToLowerInvariant();
        if (System.IO.Path.IsPathRooted(normalized) || normalized.Split('\\').Any(part => part is ".." or "." or ""))
            throw new InvalidDataException($"Invalid asset path: {path}");
        return normalized;
    }

    public IEnumerable<PgOutputMesh> EnumerateOutputMeshes()
    {
        foreach (var (path, metadata) in patches.OrderBy(entry => entry.Key, StringComparer.OrdinalIgnoreCase))
        {
            if (!path.StartsWith("meshes\\", StringComparison.Ordinal) || !path.EndsWith(".nif", StringComparison.Ordinal))
                continue;
            sources.TryGetValue(path, out var source);
            yield return new(path, metadata.GetProperty("crc32patched").GetUInt32(), source);
        }
        if (pgPackage == null) yield break;
        foreach (var (path, source) in sources.OrderBy(entry => entry.Key, StringComparer.OrdinalIgnoreCase))
        {
            if (!path.StartsWith("meshes\\_pgpatcher_dups\\", StringComparison.Ordinal) ||
                !path.EndsWith(".nif", StringComparison.Ordinal) || patches.ContainsKey(path) ||
                !source.Equals(pgPackage, StringComparison.OrdinalIgnoreCase)) continue;
            yield return new(path, null, source);
        }
    }

    public IEnumerable<string> EnumerateOutputSnowTextures()
    {
        if (pgPackage == null) yield break;
        foreach (var (path, source) in sources.OrderBy(entry => entry.Key, StringComparer.OrdinalIgnoreCase))
        {
            if (path.StartsWith("textures\\", StringComparison.Ordinal) &&
                path.EndsWith(".dds", StringComparison.Ordinal) &&
                IsPgSnowTexture(path) &&
                source.Equals(pgPackage, StringComparison.OrdinalIgnoreCase))
                yield return path;
        }
    }

    private static bool IsPgSnowTexture(string path) =>
        path.Contains("snow", StringComparison.OrdinalIgnoreCase) ||
        path is "textures\\effects\\projecteddiffuse.dds" or "textures\\effects\\projectednormal.dds" or
            "textures\\effects\\projectednoise.dds" or "textures\\effects\\projectednormaldetail.dds";

    public PgAsset Read(string model)
    {
        var path = Normalize(model);
        if (!path.StartsWith("meshes\\", StringComparison.Ordinal)) path = "meshes\\" + path;
        if (indexedMeshes != null)
        {
            if (!indexedMeshes.TryGetValue(path, out var indexed))
                return new(path, "outside-manually-built-pg-index", null, null, null, null, null, false);
            var indexedSource = System.IO.Path.GetFullPath(System.IO.Path.Combine(data, path));
            if (!indexedSource.StartsWith(data + System.IO.Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("Indexed mesh path escaped Data directory.");
            if (!File.Exists(indexedSource))
                return new(path, "pg-index-source-missing", indexed.SourcePackage, null, null, null, null, false);
            if (new FileInfo(indexedSource).Length > 64L * 1024 * 1024)
                return new(path, "pg-index-source-oversized", indexed.SourcePackage, null, null, null, null, false);
            var bytes = File.ReadAllBytes(indexedSource);
            var hash = Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();
            if (!hash.Equals(indexed.Sha256, StringComparison.OrdinalIgnoreCase))
                return new(path, "pg-index-stale-manual-refresh-required", indexed.SourcePackage, null, null, null, null, false);
            return CacheSource(path, bytes, hash, "pg-index-verified", indexed.SourcePackage, null, null);
        }
        sources.TryGetValue(path, out var source);
        var tracked = patches.TryGetValue(path, out var metadata);
        uint? expected = tracked ? metadata.GetProperty("crc32patched").GetUInt32() : null;
        var generated = path.StartsWith("meshes\\_pgpatcher_dups\\", StringComparison.Ordinal);
        var linkedToPg = pgPackage != null && string.Equals(source, pgPackage, StringComparison.OrdinalIgnoreCase);
        if (!tracked && !(generated && linkedToPg))
            return new(path, "outside-pg-output", source, null, null, null, null, false);
        var fullPath = System.IO.Path.GetFullPath(System.IO.Path.Combine(data, path));
        if (!fullPath.StartsWith(data + System.IO.Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("Asset path escaped Data directory.");
        if (!File.Exists(fullPath)) return new(path, "pg-deployed-file-missing", source, expected, null, null, null, false);
        var bytes = File.ReadAllBytes(fullPath);
        var crc = new Crc32();
        crc.Update(bytes);
        var actual = checked((uint)crc.Value);
        if (expected.HasValue && actual != expected)
            return new(path, "pg-patched-crc-mismatch", source, expected, actual, null, null, false);
        var hash = Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();
        return CacheSource(path, bytes, hash, tracked ? "pg-patched-crc-verified" : "pg-generated-deployment-linked",
            source, expected, actual);
    }

    private PgAsset CacheSource(string path, byte[] bytes, string hash, string status,
        string? source, uint? expected, uint? actual)
    {
        var relative = $"source-cache/{hash}.nif";
        var cached = System.IO.Path.Combine(output, relative);
        var hit = File.Exists(cached) && Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(cached))).Equals(hash, StringComparison.OrdinalIgnoreCase);
        if (!hit)
        {
            Directory.CreateDirectory(System.IO.Path.GetDirectoryName(cached)!);
            File.WriteAllBytes(cached + ".tmp", bytes);
            File.Move(cached + ".tmp", cached, true);
        }
        return new(path, status, source, expected, actual, hash, relative, hit);
    }

    public void Dispose()
    {
        diff?.Dispose();
        deployment?.Dispose();
    }
}

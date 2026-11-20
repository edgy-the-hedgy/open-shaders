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
    private readonly JsonDocument diff;
    private readonly JsonDocument deployment;
    private readonly Dictionary<string, JsonElement> patches;
    private readonly Dictionary<string, string> sources;
    private readonly string? pgPackage;

    public PgAssets(string dataDirectory, string outputDirectory, bool pgOutputOnly = false)
    {
        data = dataDirectory;
        output = outputDirectory;
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
        var relative = $"source-cache/{hash}.nif";
        var cached = System.IO.Path.Combine(output, relative);
        var hit = File.Exists(cached) && Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(cached))).Equals(hash, StringComparison.OrdinalIgnoreCase);
        if (!hit)
        {
            Directory.CreateDirectory(System.IO.Path.GetDirectoryName(cached)!);
            File.WriteAllBytes(cached + ".tmp", bytes);
            File.Move(cached + ".tmp", cached, true);
        }
        return new(path, tracked ? "pg-patched-crc-verified" : "pg-generated-deployment-linked", source, expected, actual, hash, relative, hit);
    }

    public void Dispose()
    {
        diff.Dispose();
        deployment.Dispose();
    }
}

using System.Security.Cryptography;
using System.Text.Json;
using Mutagen.Bethesda.Plugins;

internal sealed record SnowAtlasReport(int Ordinary, int Projected, int Landscape,
    int TexturePaths, int CachedTextures, int Pages, int UnresolvedTextures, int UnresolvedProjections,
    long SourceBytes);

internal static class SnowAtlas
{
    private const uint OrdinaryFlag2 = 1u << 28;
    private const uint ProjectedFlag1 = 1u << 23;
    private const uint LandscapeFlag1 = 1u << 14;
    private const int PageSlices = 8;
    private const int MaximumPages = 256;
    private const int MaximumSnowTextures = 4096;

    public static SnowAtlasReport Write(string data, string output, ModKey[] plugins, JsonSerializerOptions options)
    {
        using var layout = JsonDocument.Parse(File.ReadAllText(Path.Combine(output, "cell-layout.json")));
        var root = layout.RootElement;
        var cell = root.GetProperty("cell").GetString()!;
        var digest = root.GetProperty("layoutDigest").GetString()!;
        var snow = new List<(string Reference, string Shape, bool Ordinary, bool Projected,
            bool Landscape, string[] Textures)>();
        foreach (var instance in root.GetProperty("instances").EnumerateArray())
        {
            if (!instance.TryGetProperty("material", out var material) || material.ValueKind != JsonValueKind.Object)
                continue;
            var flags1 = material.GetProperty("shaderFlags1").GetUInt32();
            var flags2 = material.GetProperty("shaderFlags2").GetUInt32();
            var ordinary = (flags2 & OrdinaryFlag2) != 0;
            var projected = (flags1 & ProjectedFlag1) != 0;
            var landscape = (flags1 & LandscapeFlag1) != 0;
            if (!ordinary && !projected && !landscape)
                continue;
            var textures = material.GetProperty("textures").EnumerateArray()
                .Select(value => value.ValueKind == JsonValueKind.String ? value.GetString() ?? "" : "")
                .Select(path => string.IsNullOrWhiteSpace(path) ? "" : Canonical(path))
                .ToArray();
            snow.Add((instance.GetProperty("referenceKey").GetString()!,
                instance.GetProperty("shapeName").GetString()!, ordinary, projected, landscape, textures));
        }

        var paths = snow.SelectMany(item => item.Textures).Where(path => path.Length > 0)
            .Distinct(StringComparer.OrdinalIgnoreCase).Order(StringComparer.Ordinal).ToArray();
        if (paths.Length > MaximumSnowTextures)
            throw new InvalidDataException("Snow atlas exceeds bounded texture reference capacity.");
        TextureSources.WriteSnow(data, output, plugins, options,
            [paths.Cast<string?>().ToArray()], cell, digest);
        using var sources = JsonDocument.Parse(File.ReadAllText(Path.Combine(output, "snow-textures.json")));
        var sourceRoot = sources.RootElement;
        var entries = sourceRoot.GetProperty("textures").EnumerateArray().ToArray();
        var sourceByPath = entries.ToDictionary(item => item.GetProperty("path").GetString()!.Replace('\\', '/'),
            StringComparer.OrdinalIgnoreCase);
        var cached = entries.Where(item => item.GetProperty("status").GetString() == "cached")
            .OrderBy(item => item.GetProperty("path").GetString(), StringComparer.Ordinal).ToArray();
        var pageLocations = new Dictionary<string, (int Page, int Slice)>(StringComparer.OrdinalIgnoreCase);
        var pages = new List<object>();
        foreach (var group in cached.GroupBy(item => new
        {
            Width = item.GetProperty("width").GetUInt32(),
            Height = item.GetProperty("height").GetUInt32(),
            Mips = item.GetProperty("mipCount").GetUInt32(),
            FourCC = item.GetProperty("fourCC").GetUInt32(),
            Format = item.GetProperty("dxgiFormat").ValueKind == JsonValueKind.Null ?
                (uint?)null : item.GetProperty("dxgiFormat").GetUInt32()
        }).OrderBy(group => group.Key.Width).ThenBy(group => group.Key.Height)
          .ThenBy(group => group.Key.Mips).ThenBy(group => group.Key.FourCC).ThenBy(group => group.Key.Format))
        {
            var textures = group.OrderBy(item => item.GetProperty("sha256").GetString(), StringComparer.Ordinal).ToArray();
            for (var first = 0; first < textures.Length; first += PageSlices)
            {
                var slices = textures.Skip(first).Take(PageSlices).ToArray();
                var page = pages.Count;
                for (var slice = 0; slice < slices.Length; ++slice)
                    pageLocations[slices[slice].GetProperty("path").GetString()!.Replace('\\', '/')] = (page, slice);
                pages.Add(new
                {
                    page, width = group.Key.Width, height = group.Key.Height,
                    mipCount = group.Key.Mips, fourCC = group.Key.FourCC, dxgiFormat = group.Key.Format,
                    slices = slices.Select(item => new
                    {
                        path = item.GetProperty("path").GetString()!.Replace('\\', '/'),
                        sha256 = item.GetProperty("sha256").GetString(),
                        file = item.GetProperty("file").GetString()
                    })
                });
            }
        }
        var bindings = snow.Select(item =>
        {
            var required = item.Textures.Where(path => path.Length > 0).Distinct(StringComparer.OrdinalIgnoreCase).ToArray();
            var missing = required.Where(path => !sourceByPath.TryGetValue(path, out var source) ||
                source.GetProperty("status").GetString() != "cached").ToArray();
            return new
            {
                referenceKey = item.Reference, shapeName = item.Shape,
                ordinary = item.Ordinary, projected = item.Projected, landscape = item.Landscape,
                texturePaths = required, missingTextures = missing,
                projectionStatus = item.Projected ? "missing-native-projection-authority" : "not-required",
                status = item.Projected ? "incomplete:projection" :
                    missing.Length > 0 ? "incomplete:textures" : "prepared:source-only"
            };
        }).ToArray();
        if (pages.Count > MaximumPages)
            throw new InvalidDataException("Snow atlas exceeds the native texture pool capacity.");
        var atlasEntries = entries.Select(item =>
        {
            var path = item.GetProperty("path").GetString()!.Replace('\\', '/');
            var located = pageLocations.TryGetValue(path, out var address);
            return new
            {
                path, status = item.GetProperty("status").GetString(),
                sha256 = item.GetProperty("sha256").ValueKind == JsonValueKind.Null ? null :
                    item.GetProperty("sha256").GetString(),
                file = item.GetProperty("file").ValueKind == JsonValueKind.Null ? null :
                    item.GetProperty("file").GetString(),
                width = item.GetProperty("width").GetUInt32(), height = item.GetProperty("height").GetUInt32(),
                mipCount = item.GetProperty("mipCount").GetUInt32(), fourCC = item.GetProperty("fourCC").GetUInt32(),
                dxgiFormat = item.GetProperty("dxgiFormat").ValueKind == JsonValueKind.Null ?
                    (uint?)null : item.GetProperty("dxgiFormat").GetUInt32(),
                plannedPage = located ? address.Page : (int?)null,
                plannedSlice = located ? address.Slice : (int?)null,
                gpuResident = false
            };
        }).ToArray();
        var report = new SnowAtlasReport(snow.Count(item => item.Ordinary),
            snow.Count(item => item.Projected), snow.Count(item => item.Landscape),
            entries.Length, cached.Length, pages.Count, entries.Length - cached.Length,
            snow.Count(item => item.Projected), sourceRoot.GetProperty("sourceBytes").GetInt64());
        var manifest = new
        {
            schemaVersion = 1, cell, layoutDigest = digest, generation = "dx12-native-snow-v1",
            sourceDigest = sourceRoot.GetProperty("textureDigest").GetString(),
            inputAuthority = "nif-and-mod-assets", runtimeCoverageValid = false, gpuReady = false,
            pageType = "mip-preserving-logical-texture-arrays", pageSlices = PageSlices,
            pages, textures = atlasEntries, bindings, report
        };
        var bytes = JsonSerializer.SerializeToUtf8Bytes(manifest, options);
        var destination = Path.Combine(output, "cell-snow-atlas.json");
        File.WriteAllBytes(destination + ".tmp", bytes);
        File.Move(destination + ".tmp", destination, true);
        Console.WriteLine($"Snow atlas sources: {cached.Length}/{entries.Length} DDS; {pages.Count} logical pages; " +
            $"{report.Ordinary} ordinary / {report.Projected} projected; GPU publication pending.");
        return report;
    }

    private static string Canonical(string path)
    {
        var normalized = PgAssets.Normalize(path);
        if (!normalized.StartsWith("textures\\", StringComparison.Ordinal)) normalized = "textures\\" + normalized;
        return normalized.EndsWith(".dds", StringComparison.Ordinal) ? normalized.Replace('\\', '/') : "";
    }
}

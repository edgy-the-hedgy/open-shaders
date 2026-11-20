using System.Diagnostics;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using ICSharpCode.SharpZipLib.Checksum;
using NiflySharp;
using NiflySharp.Blocks;

internal static class PgReferenceIndex
{
    private const int SchemaVersion = 2;
    private const long MaximumNifBytes = 64L * 1024 * 1024;
    private const int MaximumMeshes = 150000;
    private const int MaximumFailures = 128;
    private static readonly JsonSerializerOptions Options = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase
    };

    private sealed record Shape(int BlockIndex, string Name, int MaterialIndex, string SnowCoverage);
    private sealed record Mesh(string Path, string Sha256, string? SourcePackage, Shape[] Shapes);
    private sealed record Material(string Id, string Classification, string ShaderType, uint ShaderFlags1,
        uint ShaderFlags2, float Alpha, int[] TextureSlots, string? ClampMode, string? UvOffset,
        string? UvScale, string Projection);

    internal static int Run(string dataDirectory, string pluginsFile)
    {
        var data = Path.GetFullPath(dataDirectory);
        var lockId = Digest(Encoding.UTF8.GetBytes(data.ToLowerInvariant()));
        using var gate = new Mutex(false, "Local\\OpenShaders_PgReferenceIndex_" + lockId);
        var acquired = false;
        try
        {
            try { acquired = gate.WaitOne(0); }
            catch (AbandonedMutexException) { acquired = true; }
            if (!acquired)
            {
                Console.Error.WriteLine("PG reference indexing is already running for this installation.");
                return 3;
            }
            return Build(data, pluginsFile);
        }
        finally { if (acquired) gate.ReleaseMutex(); }
    }

    private static int Build(string data, string pluginsFile)
    {
        try
        {
            if (!Directory.Exists(data) || !File.Exists(pluginsFile))
                throw new DirectoryNotFoundException("Skyrim Data or Plugins.txt is unavailable.");
            var cache = Path.Combine(data, "SKSE", "Plugins", "CommunityShaders", "DX12-cache", "pg-references");
            if (Directory.Exists(cache) && File.GetAttributes(cache).HasFlag(FileAttributes.ReparsePoint))
                throw new IOException("PG reference cache must not be redirected.");
            Directory.CreateDirectory(cache);
            using var pg = new PgAssets(data, cache, pgOutputOnly: true);
            var models = pg.EnumerateOutputMeshes().OrderBy(item => item.Path, StringComparer.Ordinal).ToArray();
            if (models.Length > MaximumMeshes)
                throw new InvalidDataException("PG mesh count exceeds the bounded indexing limit.");

            var meshes = new List<Mesh>();
            var materials = new List<Material>();
            var materialIds = new Dictionary<string, int>(StringComparer.Ordinal);
            var textures = new List<string>();
            var textureIds = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);
            var failures = new List<string>();
            var unresolved = 0;
            var clock = Stopwatch.StartNew();
            foreach (var model in models)
            {
                try
                {
                    var source = Path.GetFullPath(Path.Combine(data, model.Path));
                    if (!source.StartsWith(data + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
                        throw new InvalidDataException("PG mesh escaped the Data directory.");
                    var info = new FileInfo(source);
                    if (!info.Exists || info.Length is < 40 or > MaximumNifBytes)
                        throw new InvalidDataException("PG mesh missing or outside the NIF size limit.");
                    var bytes = File.ReadAllBytes(source);
                    if (model.ExpectedPatchedCrc32 is { } expected)
                    {
                        var crc = new Crc32();
                        crc.Update(bytes);
                        if (checked((uint)crc.Value) != expected)
                            throw new InvalidDataException("Deployed NIF differs from PG patched CRC.");
                    }
                    var nif = new NifFile();
                    if (nif.Load(source) != 0 || !nif.Valid || nif.HasUnknownBlocks)
                        throw new InvalidDataException("PG NIF could not be interpreted safely.");
                    var shapes = new List<Shape>();
                    foreach (var shape in nif.GetShapes())
                    {
                        if (nif.GetShader(shape) is not BSLightingShaderProperty shader) continue;
                        if (!float.IsFinite(shader.Alpha)) continue;
                        nif.GetBlockIndex(shape, out var blockIndex);
                        var slots = new int[9];
                        Array.Fill(slots, -1);
                        var sourceTextures = nif.GetBlock(shader.TextureSetRef)?.Textures;
                        if (sourceTextures != null)
                            foreach (var (texture, slot) in sourceTextures.Take(slots.Length)
                                .Select((texture, slot) => (texture, slot)))
                            {
                                var path = NormalizeTexture(texture.Content);
                                if (path == null) continue;
                                if (!textureIds.TryGetValue(path, out var textureIndex))
                                {
                                    textureIndex = textures.Count;
                                    textures.Add(path);
                                    textureIds.Add(path, textureIndex);
                                }
                                slots[slot] = textureIndex;
                            }
                        var flags = shader.ShaderFlags_SSPF1 + " " + shader.ShaderFlags_SSPF2;
                        var shaderType = shader.GetType().GetProperty("ShaderType")?.GetValue(shader)?.ToString() ?? "unknown";
                        var explicitSnow = flags.Contains("Snow", StringComparison.OrdinalIgnoreCase) ||
                            shaderType.Contains("Snow", StringComparison.OrdinalIgnoreCase);
                        var explicitIce = shaderType.Contains("Ice", StringComparison.OrdinalIgnoreCase);
                        var namedSnow = slots.Take(2).Any(slot => slot >= 0 && IsSnow(textures[slot]));
                        var namedIce = slots.Take(2).Any(slot => slot >= 0 && IsIce(textures[slot]));
                        var snow = explicitSnow || namedSnow;
                        var ice = explicitIce || namedIce;
                        var classification = snow && ice ? "ambiguous" : snow ? "snow" : ice ? "ice" : "ordinary";
                        var projected = flags.Contains("Projected", StringComparison.OrdinalIgnoreCase);
                        var coverage = classification != "snow" ? "none" : projected ? "projected-eligible" : "native-snow-material";
                        var clamp = Property(shader, "TextureClampMode");
                        var uvOffset = Property(shader, "UVOffset");
                        var uvScale = Property(shader, "UVScale");
                        var revision = JsonSerializer.Serialize(new { shaderType, flags1 = (uint)shader.ShaderFlags_SSPF1,
                            flags2 = (uint)shader.ShaderFlags_SSPF2, shader.Alpha, slots, clamp, uvOffset, uvScale, classification });
                        var materialId = Digest(Encoding.UTF8.GetBytes(revision));
                        if (!materialIds.TryGetValue(materialId, out var materialIndex))
                        {
                            materialIndex = materials.Count;
                            materials.Add(new(materialId, classification, shaderType, (uint)shader.ShaderFlags_SSPF1,
                                (uint)shader.ShaderFlags_SSPF2, shader.Alpha, slots, clamp, uvOffset, uvScale,
                                projected ? "projected-shader-flag; parameters-unresolved" : "none"));
                            materialIds.Add(materialId, materialIndex);
                        }
                        shapes.Add(new(blockIndex, shape.Name?.String ?? "", materialIndex, coverage));
                    }
                    meshes.Add(new(model.Path, Digest(bytes), model.SourcePackage,
                        shapes.OrderBy(shape => shape.BlockIndex).ToArray()));
                }
                catch (Exception error) when (error is not OutOfMemoryException)
                {
                    ++unresolved;
                    if (failures.Count < MaximumFailures)
                    {
                        var description = model.Path + ":" + error.GetType().Name + ":" + error.Message;
                        failures.Add(description[..Math.Min(240, description.Length)]);
                    }
                }
            }

            var diffPath = Path.Combine(data, "ParallaxGen_Diff.json");
            var deploymentPath = Path.Combine(data, "vortex.deployment.json");
            var document = new
            {
                kind = "dx12-pg-references", schemaVersion = SchemaVersion,
                generatedUtc = DateTimeOffset.UtcNow,
                discoveryScope = "pg-patcher-outputs-only", generatedBy = "explicit-confirmed-user-action",
                pgDiffSha256 = DigestFile(diffPath), deploymentSha256 = DigestFile(deploymentPath),
                pluginsSha256 = DigestFile(pluginsFile),
                meshCount = meshes.Count, shapeCount = meshes.Sum(mesh => mesh.Shapes.Length),
                materialCount = materials.Count, textureCount = textures.Count, unresolved,
                unsupportedCategories = new[] { "unreferenced-textures", "non-pg-meshes",
                    "implicit-engine-projection-bindings", "unresolved-projected-uvs" },
                textures, materials, meshes, failures
            };
            var destination = Path.Combine(cache, "index-v2.json");
            var temporary = destination + ".tmp";
            using (var stream = File.Create(temporary))
                JsonSerializer.Serialize(stream, document, Options);
            File.Move(temporary, destination, true);
            Console.WriteLine($"PG reference index v2: {meshes.Count} meshes, {document.shapeCount} shapes, {materials.Count} materials, {textures.Count} referenced textures; {unresolved} unresolved; {clock.ElapsedMilliseconds} ms.");
            return 0;
        }
        catch (Exception error) when (error is not OutOfMemoryException)
        {
            Console.Error.WriteLine("PG reference indexing failed: " + error.Message);
            return 1;
        }
    }

    private static string? Property(object value, string name) =>
        value.GetType().GetProperty(name)?.GetValue(value)?.ToString();

    private static string? NormalizeTexture(string? source)
    {
        if (string.IsNullOrWhiteSpace(source)) return null;
        try
        {
            var path = PgAssets.Normalize(source);
            if (!path.StartsWith("textures\\", StringComparison.Ordinal)) path = "textures\\" + path;
            return path.EndsWith(".dds", StringComparison.Ordinal) ? path.Replace('\\', '/') : null;
        }
        catch (InvalidDataException) { return null; }
    }

    private static bool IsSnow(string path)
    {
        if (path.Contains("/snow/", StringComparison.Ordinal)) return true;
        var name = Path.GetFileNameWithoutExtension(path);
        return name.StartsWith("snow", StringComparison.Ordinal) && !name.StartsWith("snowelf", StringComparison.Ordinal) ||
            name.Contains("_snow", StringComparison.Ordinal) || name.Contains("snow_", StringComparison.Ordinal);
    }

    private static bool IsIce(string path)
    {
        if (path.Contains("/ice/", StringComparison.Ordinal) || path.Contains("/glacier/", StringComparison.Ordinal)) return true;
        var name = Path.GetFileNameWithoutExtension(path);
        return name.StartsWith("ice", StringComparison.Ordinal) || name.StartsWith("icicle", StringComparison.Ordinal) ||
            name.StartsWith("glacier", StringComparison.Ordinal) || name.Contains("_ice", StringComparison.Ordinal) ||
            name.Contains("marshice", StringComparison.Ordinal);
    }

    private static string Digest(byte[] bytes) => Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();

    private static string DigestFile(string file)
    {
        using var stream = File.OpenRead(file);
        return Convert.ToHexString(SHA256.HashData(stream)).ToLowerInvariant();
    }
}

using System.Buffers.Binary;
using System.Diagnostics;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using ICSharpCode.SharpZipLib.Checksum;
using Mutagen.Bethesda;
using Mutagen.Bethesda.Archives;
using Mutagen.Bethesda.Plugins;
using Mutagen.Bethesda.Plugins.Order;
using NiflySharp;
using NiflySharp.Blocks;

internal static class GlobalSnowAtlas
{
    private const int SchemaVersion = 2;
    private const long MaximumDdsBytes = 128L * 1024 * 1024;
    private const long MaximumManifestBytes = 64L * 1024 * 1024;
    private const long MaximumPgNifBytes = 64L * 1024 * 1024;
    private const int MaximumPgModels = 150000;
    private const int MaximumFailuresReported = 128;
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true,
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase
    };

    private sealed record Asset(string Path, string Origin, string Status, string? Sha256,
        string? File, long Bytes, uint Width = 0, uint Height = 0, uint Mips = 0,
        uint FourCC = 0, uint? DxgiFormat = null, string[]? Roles = null);

    private sealed record Binding(string Id, string Model, int ShapeBlock, string ShaderType,
        uint ShaderFlags1, uint ShaderFlags2, string Classification, string[] Textures,
        string? ClampMode, string? UvOffset, string? UvScale, string ProjectionMetadata,
        string? PgSourcePackage);

    private sealed record Candidate(string Path, string Classification);

    private sealed class ArchiveIndex
    {
        internal readonly Dictionary<string, List<(string Name, IArchiveFile File)>> Files =
            new(StringComparer.OrdinalIgnoreCase);
        internal readonly List<string> Errors = [];
        internal void Add(string path, string name, IArchiveFile file)
        {
            if (!Files.TryGetValue(path, out var entries)) Files[path] = entries = [];
            entries.Add((name, file));
        }
    }

    internal static int Run(string dataDirectory, string pluginsFile)
    {
        var data = Path.GetFullPath(dataDirectory);
        var lockId = Digest(Encoding.UTF8.GetBytes(data.ToLowerInvariant()));
        using var generationGate = new Mutex(false, "Local\\OpenShaders_GlobalSnowAtlas_" + lockId);
        var acquired = false;
        try
        {
            try { acquired = generationGate.WaitOne(0); }
            catch (AbandonedMutexException) { acquired = true; }
            if (!acquired)
            {
                Console.Error.WriteLine("Global Snow Atlas: generator already running for this installation.");
                return 3;
            }
            return RunOwned(data, pluginsFile);
        }
        finally { if (acquired) generationGate.ReleaseMutex(); }
    }

    private static int RunOwned(string dataDirectory, string pluginsFile)
    {
        try
        {
            var data = Path.GetFullPath(dataDirectory);
            var plugins = Path.GetFullPath(pluginsFile);
            if (!Directory.Exists(data) || !File.Exists(plugins))
                throw new DirectoryNotFoundException("Skyrim Data or Plugins.txt is unavailable.");
            var cache = Path.Combine(data, "SKSE", "Plugins", "CommunityShaders", "DX12-cache", "global-snow");
            if (Directory.Exists(cache) && File.GetAttributes(cache).HasFlag(FileAttributes.ReparsePoint))
                throw new IOException("Global snow cache must not be a redirected directory.");
            Directory.CreateDirectory(cache);
            var clock = Stopwatch.StartNew();
            using var pg = new PgAssets(data, cache, pgOutputOnly: true);
            var pgModels = pg.EnumerateOutputMeshes().OrderBy(value => value.Path, StringComparer.OrdinalIgnoreCase).ToArray();
            var pgSnowDds = pg.EnumerateOutputSnowTextures().ToArray();
            if (pgModels.Length > MaximumPgModels)
                throw new InvalidDataException("PG Patcher NIF output exceeds the bounded discovery limit.");
            var activePlugins = GetActivePlugins(data, plugins);
            var iniPaths = GetIniFiles();
            var archives = GetActiveArchives(data, activePlugins, iniPaths);
            var inputFingerprint = ConfigurationFingerprint(data, plugins, activePlugins, archives,
                pgModels, pgSnowDds, iniPaths);
            var manifestPath = Path.Combine(cache, "manifest.json");
            var previousPaths = ReadCachedTexturePaths(manifestPath);
            var fingerprint = ReferencedTextureFingerprint(inputFingerprint, data, previousPaths);
            if (ValidCachedManifest(cache, manifestPath, fingerprint))
            {
                Console.WriteLine("Global Snow Atlas: verified PG-only cache hit; no CELL or game assets were inventoried.");
                return 0;
            }

            Console.WriteLine($"PG output inventory: {pgModels.Length} NIFs, {pgSnowDds.Length} snow DDS outputs; {clock.ElapsedMilliseconds} ms.");
            var bindings = new SortedDictionary<string, Binding>(StringComparer.Ordinal);
            var candidates = new SortedDictionary<string, Candidate>(StringComparer.OrdinalIgnoreCase);
            var roles = new Dictionary<string, HashSet<string>>(StringComparer.OrdinalIgnoreCase);
            var failures = new List<string>();
            var unresolved = 0;
            void Failure(string reason)
            {
                ++unresolved;
                if (failures.Count < MaximumFailuresReported) failures.Add(reason[..Math.Min(reason.Length, 240)]);
            }

            for (var index = 0; index < pgModels.Length; ++index)
            {
                var model = pgModels[index];
                try
                {
                    var source = ValidPgModelPath(data, model);
                    foreach (var binding in InspectPgModel(model, source))
                    {
                        bindings[binding.Id] = binding;
                        for (var slot = 0; slot < binding.Textures.Length; ++slot)
                        {
                            var path = binding.Textures[slot];
                            if (!roles.TryGetValue(path, out var tags)) roles[path] = tags = new(StringComparer.Ordinal);
                            tags.Add(slot == 0 ? "base-color:srgb" : slot == 1 ? "normal:linear" :
                                "slot-" + slot + ":interpretation-unresolved");
                        }
                    }
                }
                catch (Exception error) when (error is not OutOfMemoryException)
                {
                    Failure("pg-model:" + model.Path + ":" + error.GetType().Name + ":" + error.Message);
                }
                if ((index + 1) % 10000 == 0)
                    Console.WriteLine($"PG snow inspection: {index + 1}/{pgModels.Length} models; {bindings.Count} snow material candidates.");
            }
            foreach (var path in pgSnowDds)
            {
                candidates[path] = new Candidate(path, "pg-generated-snow-dds-candidate");
                if (!roles.TryGetValue(path, out var tags)) roles[path] = tags = new(StringComparer.Ordinal);
                if (tags.Count == 0)
                    tags.Add(path switch
                    {
                        "textures\\effects\\projecteddiffuse.dds" => "unbound:projected-diffuse:srgb",
                        "textures\\effects\\projectednormal.dds" => "unbound:projected-normal:linear",
                        "textures\\effects\\projectednoise.dds" => "unbound:projected-noise:linear",
                        "textures\\effects\\projectednormaldetail.dds" => "unbound:projected-detail-normal:linear",
                        _ => "unbound:interpretation-unresolved"
                    });
            }

            var needed = roles.Keys.ToHashSet(StringComparer.OrdinalIgnoreCase);
            var loose = EnumerateReferencedLooseAssets(data, needed);
            var looseEnabled = LooseOverridesEnabled(iniPaths);
            var archived = needed.Where(path => !looseEnabled || !loose.ContainsKey(path))
                .ToHashSet(StringComparer.OrdinalIgnoreCase);
            var archiveIndex = IndexArchives(archives, archived);
            if (archiveIndex.Errors.Count != 0)
                throw new InvalidDataException("An active archive needed by PG-referenced snow textures could not be indexed.");
            Console.WriteLine($"PG snow textures: {needed.Count} references, {loose.Count} loose paths, {archiveIndex.Files.Count} archived paths; {clock.ElapsedMilliseconds} ms.");

            var assets = new List<Asset>();
            long sourceBytes = 0;
            var stored = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (var path in roles.Keys.Order(StringComparer.OrdinalIgnoreCase))
            {
                try
                {
                    var bytes = ReadAsset(path, data, loose, archiveIndex, looseEnabled, MaximumDdsBytes, out var origin);
                    var metadata = ReadDdsMetadata(bytes);
                    var sha = Digest(bytes);
                    var relative = "textures/" + sha + ".dds";
                    var destination = Path.Combine(cache, "textures", sha + ".dds");
                    Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
                    if (!stored.Contains(sha) && (!File.Exists(destination) || DigestFile(destination) != sha))
                    {
                        File.WriteAllBytes(destination + ".tmp", bytes);
                        File.Move(destination + ".tmp", destination, true);
                    }
                    stored.Add(sha);
                    sourceBytes += bytes.Length;
                    assets.Add(new Asset(path, origin, "cached", sha, relative, bytes.Length,
                        metadata.Width, metadata.Height, metadata.Mips, metadata.FourCC,
                        metadata.Format, roles[path].Order(StringComparer.Ordinal).ToArray()));
                }
                catch (Exception error) when (error is not OutOfMemoryException)
                {
                    Failure("texture:" + path + ":" + error.GetType().Name + ":" + error.Message);
                    assets.Add(new Asset(path, "unresolved", "unresolved:" + error.GetType().Name,
                        null, null, 0, Roles: roles[path].Order(StringComparer.Ordinal).ToArray()));
                }
            }

            var materialList = bindings.Values.OrderBy(value => value.Id, StringComparer.Ordinal).ToArray();
            var textureList = assets.OrderBy(value => value.Path, StringComparer.Ordinal).ToArray();
            var manifestFingerprint = ReferencedTextureFingerprint(inputFingerprint, data, roles.Keys);
            var contentFingerprint = Digest(JsonSerializer.SerializeToUtf8Bytes(new
            {
                materials = materialList, textures = textureList, candidates = candidates.Values
            }, JsonOptions));
            var document = new
            {
                kind = "dx12-global-snow-atlas", schemaVersion = SchemaVersion,
                scope = "global", cellIndependent = true, configurationFingerprint = manifestFingerprint,
                contentFingerprint, generatedUtc = DateTimeOffset.UtcNow,
                materialCount = materialList.Length, textureCount = textureList.Length,
                resolvedTextures = textureList.Count(asset => asset.Status == "cached"),
                uniqueDdsBlobs = stored.Count,
                deduplicatedDdsPaths = textureList.Count(asset => asset.Status == "cached") - stored.Count,
                pgOutputModels = pgModels.Length, inspectedNonPgModels = 0,
                discoveryScope = "parallaxgen-output-meshes-and-pg-produced-snow-dds-only",
                unresolvedResources = unresolved, sourceBytes,
                validation = "DDS original bytes, headers and SHA-256; DX12 GPU residency is separate",
                incompleteCategories = new[]
                {
                    "all materials and textures outside verified ParallaxGen outputs are excluded by design",
                    "PG output shapes lacking explicit snow indicators cannot be classified reliably",
                    "engine projected snow defaults are not assumed unless referenced by PG output",
                    "per-material projected UVs and engine MATO interpretation remain unresolved",
                    "ambiguous BSA texture override precedence remains unresolved",
                    "native DX12 upload and descriptor validation requires a running Windows renderer"
                },
                archiveErrors = archiveIndex.Errors.Take(MaximumFailuresReported).ToArray(),
                failedCount = unresolved, failures, materials = materialList,
                textures = textureList, pathCandidates = candidates.Values.ToArray()
            };
            var manifest = JsonSerializer.Serialize(document, JsonOptions);
            if (Encoding.UTF8.GetByteCount(manifest) > MaximumManifestBytes)
                throw new InvalidDataException("PG snow manifest exceeds its bounded native inventory budget.");
            File.WriteAllText(manifestPath + ".tmp", manifest);
            File.Move(manifestPath + ".tmp", manifestPath, true);
            Console.WriteLine($"Global Snow Atlas (PG only): {materialList.Length} snow material candidates from {pgModels.Length} PG NIFs, {textureList.Count(item => item.Status == "cached")} DDS resolved, {stored.Count} unique, {textureList.Count(item => item.Status == "cached") - stored.Count} deduplicated, {unresolved} unresolved; {sourceBytes} bytes; {clock.ElapsedMilliseconds} ms.");
            return 0;
        }
        catch (Exception error) when (error is not OutOfMemoryException)
        {
            Console.Error.WriteLine("Global Snow Atlas failed: " + error.Message);
            return 1;
        }
    }

    private static bool IsSnowNamed(string path) =>
        path.EndsWith(".dds", StringComparison.OrdinalIgnoreCase) &&
        path.Contains("snow", StringComparison.OrdinalIgnoreCase);

    private static string[] ReadCachedTexturePaths(string manifest)
    {
        try
        {
            if (!File.Exists(manifest) || new FileInfo(manifest).Length > 64L * 1024 * 1024) return [];
            using var document = JsonDocument.Parse(File.ReadAllText(manifest));
            var root = document.RootElement;
            if (root.GetProperty("schemaVersion").GetInt32() != SchemaVersion ||
                root.GetProperty("discoveryScope").GetString() != "parallaxgen-output-meshes-and-pg-produced-snow-dds-only") return [];
            return root.GetProperty("textures").EnumerateArray().Select(value => value.GetProperty("path").GetString())
                .Where(value => value != null && value.StartsWith("textures\\", StringComparison.Ordinal) &&
                    value.EndsWith(".dds", StringComparison.OrdinalIgnoreCase) && !value.Contains("..", StringComparison.Ordinal))
                .Select(value => value!).Distinct(StringComparer.OrdinalIgnoreCase).ToArray();
        }
        catch (Exception error) when (error is not OutOfMemoryException) { return []; }
    }

    private static string ValidPgModelPath(string data, PgOutputMesh model)
    {
        var fullPath = Path.GetFullPath(Path.Combine(data, model.Path));
        if (!fullPath.StartsWith(data + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase) ||
            !File.Exists(fullPath))
            throw new InvalidDataException("PG mesh is missing or lies outside Data.");
        var length = new FileInfo(fullPath).Length;
        if (length < 40 || length > MaximumPgNifBytes)
            throw new InvalidDataException("PG mesh is empty or exceeds the NIF input limit.");
        if (model.ExpectedPatchedCrc32.HasValue)
        {
            var crc = new Crc32();
            using var input = File.OpenRead(fullPath);
            var buffer = new byte[64 * 1024];
            int count;
            while ((count = input.Read(buffer)) != 0)
                crc.Update(buffer, 0, count);
            if (checked((uint)crc.Value) != model.ExpectedPatchedCrc32.Value)
                throw new InvalidDataException("Deployed PG mesh does not match the patcher's CRC.");
        }
        return fullPath;
    }

    private static IEnumerable<Binding> InspectPgModel(PgOutputMesh model, string fullPath)
    {
        var nif = new NifFile();
        if (nif.Load(fullPath) != 0 || !nif.Valid || nif.HasUnknownBlocks)
            throw new InvalidDataException("PG mesh contains unreadable or unknown NIF blocks.");
        foreach (var shape in nif.GetShapes())
        {
            var shader = nif.GetShader(shape) as BSLightingShaderProperty;
            if (shader == null) continue;
            var textures = nif.GetBlock(shader.TextureSetRef)?.Textures?
                .Select(value => NormalizeTexture(value.Content)).Where(value => value != null)
                .Select(value => value!).ToArray() ?? [];
            var flags = shader.ShaderFlags_SSPF1 + " " + shader.ShaderFlags_SSPF2;
            var shaderType = shader.GetType().GetProperty("ShaderType")?.GetValue(shader)?.ToString() ?? "unknown";
            var explicitSnow = flags.Contains("Snow", StringComparison.OrdinalIgnoreCase) ||
                shaderType.Contains("Snow", StringComparison.OrdinalIgnoreCase);
            if (!explicitSnow && !textures.Any(IsSnowNamed)) continue;
            var projected = flags.Contains("Projected", StringComparison.OrdinalIgnoreCase);
            nif.GetBlockIndex(shape, out var blockIndex);
            var id = Digest(Encoding.UTF8.GetBytes("pg-snow:v2:" + model.Path + "#" + blockIndex));
            yield return new Binding(id, model.Path, blockIndex, shaderType,
                (uint)shader.ShaderFlags_SSPF1, (uint)shader.ShaderFlags_SSPF2,
                explicitSnow ? "pg-snow-shader" : "pg-snow-texture-candidate", textures,
                Property(shader, "TextureClampMode"), Property(shader, "UVOffset"),
                Property(shader, "UVScale"), projected ? "pg-projected-uv-flag; engine-parameters-unresolved" :
                "unresolved:pg-snow-projection-parameters", model.SourcePackage);
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
            return path.EndsWith(".dds", StringComparison.OrdinalIgnoreCase) ? path : null;
        }
        catch (InvalidDataException) { return null; }
    }

    private static string Digest(byte[] bytes) => Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();

    private static string DigestFile(string path)
    {
        using var input = File.OpenRead(path);
        return Convert.ToHexString(SHA256.HashData(input)).ToLowerInvariant();
    }

    private static ModKey[] GetActivePlugins(string data, string plugins)
    {
        string[] masters = ["Skyrim.esm", "Update.esm", "Dawnguard.esm", "HearthFires.esm", "Dragonborn.esm"];
        var ccc = Path.Combine(Path.GetDirectoryName(data)!, "Skyrim.ccc");
        var club = File.Exists(ccc) ? File.ReadLines(ccc).Select(value => value.Trim()).Where(value => value.Length > 0) : [];
        return masters.Concat(club).Where(name => File.Exists(Path.Combine(data, name)))
            .Select(name => ModKey.FromFileName(name)).Concat(PluginListings.LoadOrderListingsFromPath(plugins,
                GameRelease.SkyrimSE, data).Where(value => value.Enabled).Select(value => value.ModKey))
            .Distinct().ToArray();
    }

    private static string[] GetIniFiles()
    {
        var directory = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments),
            "My Games", "Skyrim Special Edition");
        return new[] { "Skyrim.ini", "SkyrimCustom.ini" }.Select(name => Path.Combine(directory, name))
            .Where(File.Exists).ToArray();
    }

    private static string[] GetActiveArchives(string data, ModKey[] plugins, string[] iniFiles)
    {
        var iniArchives = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var file in iniFiles)
        {
            var inArchive = false;
            foreach (var raw in File.ReadLines(file))
            {
                var line = raw.Split(';')[0].Trim();
                if (line.StartsWith('[')) { inArchive = line.Equals("[Archive]", StringComparison.OrdinalIgnoreCase); continue; }
                if (!inArchive) continue;
                var index = line.IndexOf('=');
                if (index < 0 || !(line[..index].Trim().Equals("sResourceArchiveList", StringComparison.OrdinalIgnoreCase) ||
                    line[..index].Trim().Equals("sResourceArchiveList2", StringComparison.OrdinalIgnoreCase))) continue;
                foreach (var name in line[(index + 1)..].Split(',', StringSplitOptions.TrimEntries | StringSplitOptions.RemoveEmptyEntries))
                    iniArchives.Add(name);
            }
        }
        return Directory.EnumerateFiles(data, "*.bsa", SearchOption.TopDirectoryOnly)
            .Where(path => iniArchives.Contains(Path.GetFileName(path)) ||
                plugins.Any(plugin => Archive.IsApplicable(GameRelease.SkyrimSE, plugin, Path.GetFileName(path))))
            .Order(StringComparer.OrdinalIgnoreCase).ToArray();
    }

    private static bool LooseOverridesEnabled(string[] iniFiles)
    {
        var enabled = false;
        foreach (var file in iniFiles)
        {
            var inArchive = false;
            foreach (var raw in File.ReadLines(file))
            {
                var line = raw.Split(';')[0].Trim();
                if (line.StartsWith('[')) { inArchive = line.Equals("[Archive]", StringComparison.OrdinalIgnoreCase); continue; }
                var index = line.IndexOf('=');
                if (inArchive && index > 0 && line[..index].Trim().Equals("bInvalidateOlderFiles", StringComparison.OrdinalIgnoreCase))
                    enabled = line[(index + 1)..].Trim() == "1";
            }
        }
        return enabled;
    }

    private static Dictionary<string, string> EnumerateReferencedLooseAssets(string data, HashSet<string> references)
    {
        var result = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        foreach (var path in references)
        {
            var source = Path.Combine(data, path);
            if (File.Exists(source)) result[path] = source;
        }
        return result;
    }

    private static string ConfigurationFingerprint(string data, string plugins, ModKey[] mods,
        string[] archives, PgOutputMesh[] pgModels, string[] pgSnowDds, string[] iniFiles)
    {
        var modFiles = mods.Select(mod => Path.Combine(data, mod.ToString())).Where(File.Exists);
        var pgManifests = new[] { Path.Combine(data, "ParallaxGen_Diff.json"),
            Path.Combine(data, "vortex.deployment.json") };
        var pgSources = pgModels.Select(model => Path.Combine(data, model.Path)).Where(File.Exists);
        var ccc = Path.Combine(Path.GetDirectoryName(data)!, "Skyrim.ccc");
        var sources = new[] { plugins }.Concat(iniFiles).Concat(archives).Concat(modFiles)
            .Concat(pgManifests).Concat(pgSources)
            .Concat(File.Exists(ccc) ? new[] { ccc } : Array.Empty<string>())
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .Order(StringComparer.OrdinalIgnoreCase);
        var config = new StringBuilder("global-snow-v2-pg-only\n").Append(Path.GetFullPath(data)).Append('\n');
        foreach (var mod in mods) config.Append("plugin:").Append(mod).Append('\n');
        foreach (var model in pgModels)
            config.Append("pg-mesh:").Append(model.Path).Append(':')
                .Append(model.ExpectedPatchedCrc32).Append(':').Append(model.SourcePackage).Append('\n');
        foreach (var path in pgSnowDds) config.Append("pg-dds:").Append(path).Append('\n');
        foreach (var source in sources)
        {
            var file = new FileInfo(source);
            config.Append(source).Append(':').Append(file.Length).Append(':')
                .Append(file.LastWriteTimeUtc.Ticks).Append('\n');
            if (source.Equals(plugins, StringComparison.OrdinalIgnoreCase) ||
                iniFiles.Contains(source, StringComparer.OrdinalIgnoreCase) ||
                pgManifests.Contains(source, StringComparer.OrdinalIgnoreCase) ||
                source.Equals(ccc, StringComparison.OrdinalIgnoreCase))
                config.Append("sha256:").Append(DigestFile(source)).Append('\n');
        }
        return Digest(Encoding.UTF8.GetBytes(config.ToString()));
    }

    private static string ReferencedTextureFingerprint(string inputFingerprint, string data,
        IEnumerable<string> referencedTextures)
    {
        var content = new StringBuilder(inputFingerprint).Append('\n');
        foreach (var path in referencedTextures.Distinct(StringComparer.OrdinalIgnoreCase)
            .Order(StringComparer.OrdinalIgnoreCase))
        {
            content.Append("reference:").Append(path).Append('\n');
            var full = Path.Combine(data, path);
            if (!File.Exists(full)) continue;
            var file = new FileInfo(full);
            content.Append(file.Length).Append(':').Append(file.LastWriteTimeUtc.Ticks).Append('\n');
        }
        return Digest(Encoding.UTF8.GetBytes(content.ToString()));
    }

    private static ArchiveIndex IndexArchives(string[] paths, HashSet<string> referenced)
    {
        var result = new ArchiveIndex();
        if (referenced.Count == 0) return result;
        foreach (var path in paths)
        {
            try
            {
                var reader = Archive.CreateReader(GameRelease.SkyrimSE, path);
                foreach (var file in reader.Files)
                {
                    if (!file.Path.EndsWith(".dds", StringComparison.OrdinalIgnoreCase)) continue;
                    try
                    {
                        var normalized = PgAssets.Normalize(file.Path);
                        if (referenced.Contains(normalized))
                            result.Add(normalized, Path.GetFileName(path), file);
                    }
                    catch (InvalidDataException) { }
                }
            }
            catch (Exception error) when (error is not OutOfMemoryException)
            {
                if (result.Errors.Count < MaximumFailuresReported)
                    result.Errors.Add(Path.GetFileName(path) + ":" + error.GetType().Name);
            }
        }
        return result;
    }

    private static byte[] ReadAsset(string path, string data, Dictionary<string, string> loose,
        ArchiveIndex archives, bool looseEnabled, long maximum, out string origin)
    {
        if (loose.TryGetValue(path, out var file))
        {
            if (!looseEnabled && archives.Files.ContainsKey(path))
                throw new InvalidDataException("Loose/archive override precedence is not established");
            if (new FileInfo(file).Length > maximum) throw new InvalidDataException("Asset exceeds source limit");
            origin = "loose:" + Path.GetRelativePath(data, file);
            return File.ReadAllBytes(file);
        }
        if (archives.Errors.Count > 0) throw new InvalidDataException("Incomplete active archive index");
        if (!archives.Files.TryGetValue(path, out var sources)) throw new FileNotFoundException("Missing asset", path);
        if (sources.Count != 1) throw new InvalidDataException("Ambiguous active archive precedence");
        if (sources[0].File.Size > maximum) throw new InvalidDataException("Archive asset exceeds source limit");
        origin = "archive:" + sources[0].Name;
        return sources[0].File.GetBytes();
    }

    private static (uint Width, uint Height, uint Mips, uint FourCC, uint? Format) ReadDdsMetadata(byte[] bytes)
    {
        if (bytes.Length < 128) throw new InvalidDataException("Truncated DDS");
        uint Read(int offset) => BinaryPrimitives.ReadUInt32LittleEndian(bytes.AsSpan(offset, 4));
        if (Read(0) != 0x20534444 || Read(4) != 124 || Read(76) != 32)
            throw new InvalidDataException("Invalid DDS header");
        var width = Read(16);
        var height = Read(12);
        var mips = Math.Max(1u, Read(28));
        if (width is 0 or > 16384 || height is 0 or > 16384 || mips > 15)
            throw new InvalidDataException("Invalid DDS dimensions or mips");
        var fourCC = Read(84);
        if (fourCC == 0x30315844 && bytes.Length < 148)
            throw new InvalidDataException("Truncated DX10 DDS");
        return (width, height, mips, fourCC, fourCC == 0x30315844 ? Read(128) : null);
    }

    private static bool ValidCachedManifest(string root, string manifest, string fingerprint)
    {
        if (!File.Exists(manifest)) return false;
        try
        {
            using var document = JsonDocument.Parse(File.ReadAllText(manifest));
            var value = document.RootElement;
            if (value.GetProperty("schemaVersion").GetInt32() != SchemaVersion ||
                value.GetProperty("scope").GetString() != "global" ||
                value.GetProperty("discoveryScope").GetString() != "parallaxgen-output-meshes-and-pg-produced-snow-dds-only" ||
                value.GetProperty("configurationFingerprint").GetString() != fingerprint) return false;
            var verified = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (var texture in value.GetProperty("textures").EnumerateArray())
            {
                if (texture.GetProperty("status").GetString() != "cached") continue;
                var hash = texture.GetProperty("sha256").GetString();
                if (hash == null || hash.Length != 64 || !hash.All(Uri.IsHexDigit)) return false;
                var expected = "textures/" + hash + ".dds";
                if (texture.GetProperty("file").GetString() != expected) return false;
                if (!verified.Add(hash)) continue;
                var location = Path.Combine(root, "textures", hash + ".dds");
                if (!File.Exists(location) || DigestFile(location) != hash) return false;
            }
            return true;
        }
        catch (Exception error) when (error is not OutOfMemoryException) { return false; }
    }
}

using System.Buffers.Binary;
using System.Diagnostics;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Mutagen.Bethesda;
using Mutagen.Bethesda.Archives;
using Mutagen.Bethesda.Plugins;
using Mutagen.Bethesda.Plugins.Order;
using NiflySharp;
using NiflySharp.Blocks;

internal static class GlobalSnowAtlas
{
    private const int SchemaVersion = 1;
    private const long MaximumDdsBytes = 128L * 1024 * 1024;
    private const long MaximumNifBytes = 64L * 1024 * 1024;
    private const int MaximumFailuresReported = 128;
    private const int ScanIndexVersion = 2;
    private const int MaximumModelScans = 300000;
    private const long MaximumScanIndexBytes = 128L * 1024 * 1024;
    private static readonly TimeSpan MaximumGenerationTime = TimeSpan.FromMinutes(25);
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true,
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase
    };

    private sealed record Asset(string Path, string Origin, string Status, string? Sha256,
        string? File, long Bytes, uint Width = 0, uint Height = 0, uint Mips = 0,
        uint FourCC = 0, uint? DxgiFormat = null, string[]? Roles = null);

    internal sealed record Binding(string Id, string Model, int ShapeBlock, string ShaderType,
        uint ShaderFlags1, uint ShaderFlags2, string Classification, string[] Textures,
        string? ClampMode, string? UvOffset, string? UvScale, string ProjectionMetadata);

    internal sealed record ModelScan(string Signature, Binding[] Materials, string? Error);

    internal sealed class ModelScanIndex
    {
        public ModelScanIndex() { }
        public int Version { get; set; } = ScanIndexVersion;
        public Dictionary<string, ModelScan> Models { get; set; } = new(StringComparer.OrdinalIgnoreCase);
    }

    private sealed record Candidate(string Path, string Classification);

    private sealed class ArchiveIndex
    {
        internal readonly Dictionary<string, List<(string Name, IArchiveFile File)>> Files =
            new(StringComparer.OrdinalIgnoreCase);
        internal readonly List<string> Errors = [];
        internal readonly Dictionary<string, string> Signatures = new(StringComparer.OrdinalIgnoreCase);

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
            var activePlugins = GetActivePlugins(data, plugins);
            var iniPaths = GetIniFiles();
            var archives = GetActiveArchives(data, activePlugins, iniPaths);
            var loose = EnumerateLooseAssets(data);
            var fingerprint = ConfigurationFingerprint(data, plugins, activePlugins, archives, loose, iniPaths);
            var manifestPath = Path.Combine(cache, "manifest.json");
            if (ValidCachedManifest(cache, manifestPath, fingerprint))
            {
                Console.WriteLine("Global Snow Atlas: verified cache hit; no scene or CELL was consulted.");
                return 0;
            }

            var archiveIndex = IndexArchives(archives);
            if (archiveIndex.Errors.Count != 0)
                throw new InvalidDataException("An active BSA could not be indexed; global snow material discovery cannot be trusted.");
            var looseEnabled = LooseOverridesEnabled(iniPaths);
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

            var models = loose.Keys.Concat(archiveIndex.Files.Keys)
                .Where(path => path.StartsWith("meshes\\", StringComparison.OrdinalIgnoreCase) &&
                    path.EndsWith(".nif", StringComparison.OrdinalIgnoreCase))
                .Distinct(StringComparer.OrdinalIgnoreCase).Order(StringComparer.OrdinalIgnoreCase).ToArray();
            if (models.Length > MaximumModelScans)
                throw new InvalidDataException("Global NIF inventory exceeds the supported scan limit.");
            var previousScans = ReadModelIndex(cache);
            var updatedScans = new Dictionary<string, ModelScan>(StringComparer.OrdinalIgnoreCase);
            var timer = Stopwatch.StartNew();
            var reusedModels = 0;
            var scratch = Path.Combine(cache, "scan.nif.tmp");
            try
            {
                for (var modelIndex = 0; modelIndex < models.Length; ++modelIndex)
                {
                    if (timer.Elapsed > MaximumGenerationTime)
                        throw new TimeoutException("Global NIF discovery exceeded its 25-minute bound; cached scan checkpoints are retained.");
                    var model = models[modelIndex];
                    var signature = ModelSignature(model, loose, archiveIndex, looseEnabled);
                    ModelScan scan;
                    if (previousScans.TryGetValue(model, out var previous) && previous.Signature == signature)
                    {
                        scan = previous;
                        ++reusedModels;
                    }
                    else
                    {
                        try
                        {
                            var bytes = ReadAsset(model, data, loose, archiveIndex, looseEnabled, MaximumNifBytes, out _);
                            scan = InspectModel(model, signature, bytes, scratch);
                        }
                        catch (Exception error) when (error is not OutOfMemoryException)
                        {
                            scan = new(signature, [], error.GetType().Name);
                        }
                    }
                    updatedScans.Add(model, scan);
                    if (scan.Error != null) Failure("model:" + model + ":" + scan.Error);
                    foreach (var binding in scan.Materials)
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
                    if ((modelIndex + 1) % 20000 == 0)
                    {
                        Console.WriteLine($"NIF inventory {modelIndex + 1}/{models.Length}; {reusedModels} cache hits.");
                        WriteModelIndex(cache, updatedScans);
                    }
                }
                WriteModelIndex(cache, updatedScans);
            }
            finally { if (File.Exists(scratch)) File.Delete(scratch); }

            foreach (var path in loose.Keys.Concat(archiveIndex.Files.Keys)
                .Where(path => path.StartsWith("textures\\", StringComparison.OrdinalIgnoreCase) && IsSnowNamed(path))
                .Distinct(StringComparer.OrdinalIgnoreCase).Order(StringComparer.OrdinalIgnoreCase))
            {
                candidates[path] = new Candidate(path,
                    path.Contains("projected", StringComparison.OrdinalIgnoreCase) ?
                        "projected-snow-path-candidate" : "snow-path-candidate");
                if (!roles.TryGetValue(path, out var tags)) roles[path] = tags = new(StringComparer.Ordinal);
                tags.Add("unbound:interpretation-unresolved");
            }

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
            var contentFingerprint = Digest(JsonSerializer.SerializeToUtf8Bytes(new
            {
                materials = materialList, textures = textureList, candidates = candidates.Values
            }, JsonOptions));
            var document = new
            {
                kind = "dx12-global-snow-atlas", schemaVersion = SchemaVersion,
                scope = "global", cellIndependent = true, configurationFingerprint = fingerprint,
                contentFingerprint, generatedUtc = DateTimeOffset.UtcNow,
                materialCount = materialList.Length, textureCount = textureList.Length,
                resolvedTextures = textureList.Count(asset => asset.Status == "cached"),
                uniqueDdsBlobs = stored.Count,
                deduplicatedDdsPaths = textureList.Count(asset => asset.Status == "cached") - stored.Count,
                scannedModels = models.Length, reusedModelScans = reusedModels,
                unresolvedResources = unresolved, sourceBytes,
                validation = "DDS original bytes, headers and SHA-256; DX12 GPU residency is separate",
                incompleteCategories = new[]
                {
                    "landscape material ID and TXST override bindings not yet classified",
                    "projected snow engine parameters and texture binding precedence not yet verified",
                    "modded snow materials with neither a snow shader flag nor a snow-named texture cannot be inferred",
                    "archive duplicates and unverified loose overrides are rejected until exact engine precedence is available",
                    "native DX12 upload and descriptor validation requires a running Windows renderer"
                },
                archiveErrors = archiveIndex.Errors.Take(MaximumFailuresReported).ToArray(),
                failedCount = unresolved, failures, materials = materialList,
                textures = textureList, pathCandidates = candidates.Values.ToArray()
            };
            File.WriteAllText(manifestPath + ".tmp", JsonSerializer.Serialize(document, JsonOptions));
            File.Move(manifestPath + ".tmp", manifestPath, true);
            Console.WriteLine($"Global Snow Atlas: {materialList.Length} material candidates, {textureList.Count(item => item.Status == "cached")} DDS resolved, {stored.Count} unique, {textureList.Count(item => item.Status == "cached") - stored.Count} deduplicated, {unresolved} unresolved; {sourceBytes} bytes.");
            return 0;
        }
        catch (Exception error) when (error is not OutOfMemoryException)
        {
            Console.Error.WriteLine("Global Snow Atlas failed: " + error.Message);
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
            return path.EndsWith(".dds", StringComparison.OrdinalIgnoreCase) ? path : null;
        }
        catch (InvalidDataException) { return null; }
    }

    private static bool IsSnowNamed(string path) =>
        path.EndsWith(".dds", StringComparison.OrdinalIgnoreCase) &&
        path.Contains("snow", StringComparison.OrdinalIgnoreCase);

    private static Dictionary<string, ModelScan> ReadModelIndex(string cache)
    {
        var location = Path.Combine(cache, "nif-scan-index.json");
        try
        {
            if (!File.Exists(location) || new FileInfo(location).Length > MaximumScanIndexBytes) return [];
            using var input = File.OpenRead(location);
            var index = JsonSerializer.Deserialize<ModelScanIndex>(input, JsonOptions);
            if (index?.Version != ScanIndexVersion || index.Models == null ||
                index.Models.Count > MaximumModelScans || index.Models.Values.Any(value =>
                    value == null || string.IsNullOrWhiteSpace(value.Signature) || value.Materials == null)) return [];
            return new Dictionary<string, ModelScan>(index.Models, StringComparer.OrdinalIgnoreCase);
        }
        catch (Exception error) when (error is not OutOfMemoryException) { return []; }
    }

    private static void WriteModelIndex(string cache, Dictionary<string, ModelScan> models)
    {
        var location = Path.Combine(cache, "nif-scan-index.json");
        var temporary = location + ".tmp";
        try
        {
            using (var output = File.Create(temporary))
                JsonSerializer.Serialize(output, new ModelScanIndex { Models = models }, JsonOptions);
            if (new FileInfo(temporary).Length > MaximumScanIndexBytes)
            {
                Console.WriteLine("NIF scan index exceeded its 128 MiB retention limit; preserving the prior index.");
                return;
            }
            File.Move(temporary, location, true);
        }
        finally { if (File.Exists(temporary)) File.Delete(temporary); }
    }

    private static string ModelSignature(string model, Dictionary<string, string> loose,
        ArchiveIndex archives, bool looseEnabled)
    {
        var source = new StringBuilder("looseOverrides:").Append(looseEnabled).Append('|');
        if (loose.TryGetValue(model, out var path))
        {
            var file = new FileInfo(path);
            source.Append("loose:").Append(file.FullName).Append(':').Append(file.Length)
                .Append(':').Append(file.LastWriteTimeUtc.Ticks).Append('|');
        }
        if (archives.Files.TryGetValue(model, out var sources))
            foreach (var entry in sources.OrderBy(value => value.Name, StringComparer.OrdinalIgnoreCase))
                source.Append("archive:").Append(entry.Name).Append(':')
                    .Append(archives.Signatures.GetValueOrDefault(entry.Name, "unknown"))
                    .Append(':').Append(entry.File.Size).Append('|');
        return Digest(Encoding.UTF8.GetBytes(source.ToString()));
    }

    private static ModelScan InspectModel(string model, string signature, byte[] bytes, string scratch)
    {
        File.WriteAllBytes(scratch, bytes);
        var nif = new NifFile();
        if (nif.Load(scratch) != 0 || !nif.Valid || nif.HasUnknownBlocks)
            return new(signature, [], "unreadable-or-unknown-nif-blocks");
        var materials = new List<Binding>();
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
            nif.GetBlockIndex(shape, out var blockIndex);
            var id = Digest(Encoding.UTF8.GetBytes(model + "#" + blockIndex));
            materials.Add(new Binding(id, model, blockIndex, shaderType,
                (uint)shader.ShaderFlags_SSPF1, (uint)shader.ShaderFlags_SSPF2,
                explicitSnow ? "nif-snow-shader" : "texture-name-candidate", textures,
                Property(shader, "TextureClampMode"), Property(shader, "UVOffset"),
                Property(shader, "UVScale"), "unresolved:engine-projected-snow-parameters"));
        }
        return new(signature, materials.ToArray(), null);
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

    private static Dictionary<string, string> EnumerateLooseAssets(string data)
    {
        var result = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        foreach (var folder in new[] { "textures", "meshes" })
        {
            var root = Path.Combine(data, folder);
            if (!Directory.Exists(root)) continue;
            foreach (var path in Directory.EnumerateFiles(root, "*", SearchOption.AllDirectories))
            {
                if (!path.EndsWith(".dds", StringComparison.OrdinalIgnoreCase) &&
                    !path.EndsWith(".nif", StringComparison.OrdinalIgnoreCase)) continue;
                var relative = Path.GetRelativePath(data, path);
                try { result[PgAssets.Normalize(relative)] = path; }
                catch (InvalidDataException) { }
            }
        }
        return result;
    }

    private static string ConfigurationFingerprint(string data, string plugins, ModKey[] mods,
        string[] archives, Dictionary<string, string> loose, string[] iniFiles)
    {
        var modFiles = mods.Select(mod => Path.Combine(data, mod.ToString())).Where(File.Exists);
        var ccc = Path.Combine(Path.GetDirectoryName(data)!, "Skyrim.ccc");
        var special = new[] { ccc, Path.Combine(data, "vortex.deployment.json"),
            Path.Combine(data, "ParallaxGen_Diff.json") }.Where(File.Exists);
        var sources = new[] { plugins }.Concat(iniFiles).Concat(archives).Concat(modFiles)
            .Concat(special).Concat(loose.Values).Distinct(StringComparer.OrdinalIgnoreCase)
            .Order(StringComparer.OrdinalIgnoreCase);
        var config = new StringBuilder("global-snow-v1\n").Append(Path.GetFullPath(data)).Append('\n');
        foreach (var mod in mods) config.Append("plugin:").Append(mod).Append('\n');
        foreach (var source in sources)
        {
            var file = new FileInfo(source);
            config.Append(source).Append(':').Append(file.Length).Append(':')
                .Append(file.LastWriteTimeUtc.Ticks).Append('\n');
            if (source.Equals(plugins, StringComparison.OrdinalIgnoreCase) ||
                iniFiles.Contains(source, StringComparer.OrdinalIgnoreCase) ||
                source.Equals(ccc, StringComparison.OrdinalIgnoreCase))
                config.Append("sha256:").Append(Digest(File.ReadAllBytes(source))).Append('\n');
        }
        return Digest(Encoding.UTF8.GetBytes(config.ToString()));
    }

    private static ArchiveIndex IndexArchives(string[] paths)
    {
        var result = new ArchiveIndex();
        foreach (var path in paths)
        {
            try
            {
                var archiveFile = new FileInfo(path);
                result.Signatures[archiveFile.Name] = $"{archiveFile.Length}:{archiveFile.LastWriteTimeUtc.Ticks}";
                var reader = Archive.CreateReader(GameRelease.SkyrimSE, path);
                foreach (var file in reader.Files)
                {
                    try
                    {
                        var normalized = PgAssets.Normalize(file.Path);
                        if ((normalized.StartsWith("textures\\", StringComparison.Ordinal) && normalized.EndsWith(".dds")) ||
                            (normalized.StartsWith("meshes\\", StringComparison.Ordinal) && normalized.EndsWith(".nif")))
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

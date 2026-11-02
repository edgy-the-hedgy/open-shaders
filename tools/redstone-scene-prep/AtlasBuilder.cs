using System.Security.Cryptography;
using System.Text.Json;
using Mutagen.Bethesda;
using Mutagen.Bethesda.Plugins;
using Mutagen.Bethesda.Plugins.Order;
using Mutagen.Bethesda.Skyrim;

// Offline-only catalogue. The game never invokes the .NET compiler.
internal static class AtlasBuilder
{
    internal static int Run(string[] args)
    {
        if (args.Length != 5)
        {
            Console.Error.WriteLine("Usage: RedstoneScenePrep --atlas <Data directory> <output root> <quality: 0|1|2> <Plugins.txt>");
            return 2;
        }
        try
        {
            var data = Path.GetFullPath(args[1]);
            var root = Path.GetFullPath(args[2]);
            if (root.Equals(data, StringComparison.OrdinalIgnoreCase) ||
                root.StartsWith(data + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
                throw new ArgumentException("Atlas output must be outside Skyrim Data.");
            var quality = int.Parse(args[3], System.Globalization.CultureInfo.InvariantCulture);
            if (quality is < 0 or > 2) throw new ArgumentOutOfRangeException(nameof(quality));
            var output = Path.Combine(root, "q" + quality);
            var pluginsPath = Path.GetFullPath(args[4]);
            var options = new JsonSerializerOptions { WriteIndented = true, PropertyNamingPolicy = JsonNamingPolicy.CamelCase };
            Directory.CreateDirectory(output);

            // A Q0-only build uses deployed NIFs as validated source inputs.
            // Keep the copy-based mode for historical multi-tier tooling.
            using var pg = new PgAssets(data, output, cacheNifSources: false);
            var compiler = new MeshCompiler(output, options, quality, sharedSources: data);
            var names = pg.EligibleModels().Distinct(StringComparer.OrdinalIgnoreCase)
                .Order(StringComparer.OrdinalIgnoreCase).ToArray();
            var assets = new List<CompiledNif>(names.Length);
            var failures = new List<object>();
            Console.WriteLine($"Atlas quality {quality}: {names.Length} PGPatcher-verified mesh candidates.");
            foreach (var name in names)
            {
                try
                {
                    var candidate = pg.Read(name);
                    var compiled = compiler.Compile(candidate);
                    assets.Add(compiled);
                }
                catch (Exception error) when (error is not OutOfMemoryException)
                {
                    failures.Add(new { path = name, reason = error.Message });
                }
                if ((assets.Count + failures.Count) % 250 == 0)
                    Console.WriteLine($"Atlas progress: {assets.Count + failures.Count}/{names.Length}");
            }

            return FinishLevel(data, output, pluginsPath, options, quality, assets, failures);
        }
        catch (Exception error)
        {
            Console.Error.WriteLine(error);
            return 1;
        }
    }

    // Default all-tier path: inspect and hash each PGPatcher NIF only once.
    // q0 and un-displaced q2 reuse q1's verified geometry/sidecars.
    internal static int RunBatch(string[] args)
    {
        if (args.Length is not (4 or 5) || (args.Length == 5 && args[4] is not ("0,1" or "0,1,2")))
        {
            Console.Error.WriteLine("Usage: RedstoneScenePrep --atlas-batch <Data directory> <output root> <Plugins.txt> [0,1|0,1,2]");
            return 2;
        }
        try
        {
            var data = Path.GetFullPath(args[1]);
            var root = Path.GetFullPath(args[2]);
            var plugins = Path.GetFullPath(args[3]);
            var includeQ2 = args.Length == 4 || args[4] == "0,1,2";
            var tiers = includeQ2 ? new[] { 0, 1, 2 } : new[] { 0, 1 };
            if (root.Equals(data, StringComparison.OrdinalIgnoreCase) ||
                root.StartsWith(data + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
                throw new ArgumentException("Atlas output must be outside Skyrim Data.");
            var options = new JsonSerializerOptions { WriteIndented = true, PropertyNamingPolicy = JsonNamingPolicy.CamelCase };
            var q1 = Path.Combine(root, "q1");
            var q0 = Path.Combine(root, "q0");
            var q2 = Path.Combine(root, "q2");
            foreach (var quality in tiers) Directory.CreateDirectory(Path.Combine(root, "q" + quality));
            using var pg = new PgAssets(data, q1);
            var normal = new MeshCompiler(q1, options, 1);
            var minimal = new MeshCompiler(q0, options, 0);
            var maximum = includeQ2 ? new MeshCompiler(q2, options, 2, sharedSources: q1) : null;
            var names = pg.EligibleModels().Distinct(StringComparer.OrdinalIgnoreCase)
                .Order(StringComparer.OrdinalIgnoreCase).ToArray();
            var all = tiers.ToDictionary(quality => quality, quality => new List<CompiledNif>(names.Length));
            var failures = new List<object>();
            var reused = 0;
            var compiledQ2 = 0;
            var watch = System.Diagnostics.Stopwatch.StartNew();
            Console.WriteLine($"Atlas batch: {names.Length} candidates, q0/q1{(includeQ2 ? "/q2" : "")}, shared NIF acquisition.");
            var processed = 0;
            var cacheHits = 0;
            long acquireTicks = 0, compileTicks = 0, deriveTicks = 0;
            foreach (var name in names)
            {
                try
                {
                    var timer = System.Diagnostics.Stopwatch.GetTimestamp();
                    var source = pg.Read(name);
                    acquireTicks += System.Diagnostics.Stopwatch.GetTimestamp() - timer;
                    timer = System.Diagnostics.Stopwatch.GetTimestamp();
                    var baseline = normal.Compile(source);
                    compileTicks += System.Diagnostics.Stopwatch.GetTimestamp() - timer;
                    if (baseline.CacheHit) ++cacheHits;
                    timer = System.Diagnostics.Stopwatch.GetTimestamp();
                    var basic = minimal.DeriveFrom(baseline, q1);
                    deriveTicks += System.Diagnostics.Stopwatch.GetTimestamp() - timer;
                    CompiledNif? full = null;
                    if (maximum != null)
                    {
                        if (maximum.NeedsOwnQ2Compile(baseline))
                        {
                            full = maximum.Compile(source);
                            ++compiledQ2;
                        }
                        else
                        {
                            full = maximum.DeriveFrom(baseline, q1);
                            ++reused;
                        }
                    }
                    // Do not publish a partial NIF entry in only some tiers.
                    all[1].Add(baseline);
                    all[0].Add(basic);
                    if (full != null) all[2].Add(full);
                }
                catch (Exception error) when (error is not OutOfMemoryException)
                {
                    failures.Add(new { path = name, reason = error.Message });
                    Console.Error.WriteLine($"[atlas] {name}: {error.Message}");
                }
                ++processed;
                if (processed % 100 == 0 || processed == names.Length)
                    Console.WriteLine($"Atlas progress {processed}/{names.Length} in {watch.Elapsed:g}" +
                        (includeQ2 ? $"; q2 reused {reused}, recompiled {compiledQ2}" : "") + ".");
            }

            int result = 0;
            foreach (var quality in tiers)
            {
                var output = Path.Combine(root, "q" + quality);
                var status = FinishLevel(data, output, plugins, options, quality, all[quality], failures);
                if (status != 0) result = status;
            }
            static TimeSpan Duration(long ticks) => TimeSpan.FromSeconds(
                double(ticks) / System.Diagnostics.Stopwatch.Frequency);
            Console.WriteLine($"Atlas tiers {string.Join(",", tiers)} finished in {watch.Elapsed:g}; " +
                $"source verification {Duration(acquireTicks):g}, q1 mesh stage {Duration(compileTicks):g}, " +
                $"q0 derivation {Duration(deriveTicks):g}, q1 cache hits {cacheHits}/{processed}." +
                (includeQ2 ? $" q2 reused {reused}, recompiled {compiledQ2}." : ""));
            return result;
        }
        catch (Exception error)
        {
            Console.Error.WriteLine(error);
            return 1;
        }
    }


    // Compact, runtime-readable lookup: only the rigid intersection geometry
    // and its local transform/bounds; no PBR DDS or q1/q2 build products.
    internal static int WriteQ0GeometryCatalog(string[] args)
    {
        if (args.Length != 2)
        {
            Console.Error.WriteLine("Usage: RedstoneScenePrep --geometry-catalog <existing q0 atlas directory>");
            return 2;
        }
        try
        {
            var output = Path.GetFullPath(args[1]);
            var compiled = Path.Combine(output, "compiled");
            if (!Directory.Exists(compiled)) throw new DirectoryNotFoundException(compiled);
            var indexFile = Path.Combine(output, "atlas-index.json");
            using var index = JsonDocument.Parse(File.ReadAllText(indexFile));
            if (index.RootElement.GetProperty("schemaVersion").GetInt32() != 1 ||
                index.RootElement.GetProperty("quality").GetInt32() != 0)
                throw new InvalidDataException("Q0 atlas index/schema mismatch.");
            var live = new Dictionary<string, (string Sha, string Key)>(StringComparer.OrdinalIgnoreCase);
            foreach (var asset in index.RootElement.GetProperty("assets").EnumerateArray())
            {
                if (asset.GetProperty("status").GetString() != "classified") continue;
                var path = asset.GetProperty("sourcePath").GetString();
                var sha = asset.GetProperty("sourceSha256").GetString();
                var key = asset.GetProperty("compilerKey").GetString();
                if (path != null && sha is { Length: 64 } && key is { Length: 64 })
                    live[path] = (sha, key);
            }
            var options = new JsonSerializerOptions { WriteIndented = false, PropertyNamingPolicy = JsonNamingPolicy.CamelCase };
            var models = new Dictionary<string, object>(StringComparer.OrdinalIgnoreCase);
            var inspected = 0;
            foreach (var filename in Directory.EnumerateFiles(compiled, "*.json", SearchOption.AllDirectories))
            {
                CompiledNif? nif;
                try { nif = JsonSerializer.Deserialize<CompiledNif>(File.ReadAllText(filename), options); }
                catch (JsonException) { continue; }
                if (nif == null || nif.Status != "classified" || string.IsNullOrWhiteSpace(nif.SourcePath))
                    continue;
                // Prevent stale compiler schemas or superseded mod sources
                // from leaking into the runtime-resolved geometry catalogue.
                if (!live.TryGetValue(nif.SourcePath, out var current) ||
                    current.Sha != nif.SourceSha256 || current.Key != nif.CompilerKey)
                    continue;
                var shapes = nif.Shapes.Where(shape =>
                    shape.Status == "compiled" && shape.OpaqueOccluderCandidate &&
                    shape.ShapeToModel is { Length: 12 } && shape.LocalBounds is { Length: 6 } &&
                    shape.GeometrySha256 is { Length: 64 } && shape.GeometryFile != null && shape.IndexCount >= 3)
                    .Select(shape => new {
                        shape.GeometrySha256, shape.GeometryFile, shape.ShapeToModel,
                        shape.LocalBounds, shape.IndexCount, shape.BlockIndex
                    }).ToArray();
                if (shapes.Length == 0) continue;
                var model = nif.SourcePath.Replace('/', '\\').ToLowerInvariant();
                if (!model.StartsWith("meshes\\", StringComparison.Ordinal)) model = "meshes\\" + model;
                models[model] = new { sourceSha256 = nif.SourceSha256, shapes };
                ++inspected;
            }
            var catalog = new {
                schemaVersion = 1, kind = "native-runtime-q0-geometry",
                inputScope = "verified static PGPatcher meshes; no skinned/animated/alpha geometry",
                models
            };
            Directory.CreateDirectory(output);
            var staging = Path.Combine(output, "geometry-catalog.json.tmp");
            File.WriteAllText(staging, JsonSerializer.Serialize(catalog, options));
            File.Move(staging, Path.Combine(output, "geometry-catalog.json"), true);
            Console.WriteLine($"Q0 geometry catalogue: {models.Count} models, {inspected} compatible NIF records.");
            return 0;
        }
        catch (Exception error)
        {
            Console.Error.WriteLine(error);
            return 1;
        }
    }

    private static int FinishLevel(string data, string output, string pluginsPath,
        JsonSerializerOptions options, int quality, IReadOnlyCollection<CompiledNif> assets, List<object> failures)
    {
        var fingerprints = assets.Select(asset => new {
            asset.SourcePath, asset.SourceSha256, asset.CompilerKey, asset.Status
        }).ToArray();
        var digest = Convert.ToHexString(
            SHA256.HashData(JsonSerializer.SerializeToUtf8Bytes(fingerprints, options))).ToLowerInvariant();
        if (quality == 2)
        {
            var official = new[] { "Skyrim.esm", "Update.esm", "Dawnguard.esm", "HearthFires.esm", "Dragonborn.esm" };
            var ccc = Path.Combine(Path.GetDirectoryName(data)!, "Skyrim.ccc");
            var clubs = File.Exists(ccc) ? File.ReadLines(ccc).Select(line => line.Trim()).Where(line => line.Length > 0) : [];
            var implicitPlugins = official.Concat(clubs)
                .Where(name => File.Exists(Path.Combine(data, name))).Select(name => ModKey.FromFileName(name));
            var active = PluginListings.LoadOrderListingsFromPath(pluginsPath, GameRelease.SkyrimSE, data)
                .Where(item => item.Enabled).Select(item => item.ModKey);
            TextureSources.WriteAtlas(data, output,
                implicitPlugins.Concat(active).Distinct().ToArray(), options, assets, digest);
        }
        var index = new
        {
            schemaVersion = 1, kind = "native-scene-universal-asset-atlas",
            quality, digest,
            inputScope = "deployed PGPatcher NIFs and deployment-linked generated duplicates; not all mod NIFs",
            assets = assets.Select(asset => new {
                asset.SourcePath, asset.SourceSha256, asset.CompilerKey, asset.Status,
                asset.EmbeddedLights,
                shapes = asset.Shapes.Select(shape => new {
                    shape.BlockIndex, shape.Name, shape.Status, shape.ShapeToModel,
                    shape.GeometryFile, shape.GeometrySha256, shape.ShadingFile, shape.ShadingSha256,
                    shape.Material
                })
            }),
            failures
        };
        var tmp = Path.Combine(output, "atlas-index.json.tmp");
        File.WriteAllText(tmp, JsonSerializer.Serialize(index, options));
        File.Move(tmp, Path.Combine(output, "atlas-index.json"), true);
        if (quality == 0)
        {
            var status = WriteQ0GeometryCatalog(["--geometry-catalog", output]);
            if (status != 0) return status;
        }
        Console.WriteLine($"Atlas q{quality} ready: {assets.Count} indexed NIFs; {failures.Count} source errors; digest {digest}.");
        return failures.Count == 0 ? 0 : 3;
    }

}

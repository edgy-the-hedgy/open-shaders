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

            using var pg = new PgAssets(data, output);
            var compiler = new MeshCompiler(output, options, quality);
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
        Console.WriteLine($"Atlas q{quality} ready: {assets.Count} indexed NIFs; {failures.Count} source errors; digest {digest}.");
        return failures.Count == 0 ? 0 : 3;
    }

}

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
        if (args.Length != 4)
        {
            Console.Error.WriteLine("Usage: RedstoneScenePrep --atlas-batch <Data directory> <output root> <Plugins.txt>");
            return 2;
        }
        try
        {
            var data = Path.GetFullPath(args[1]);
            var root = Path.GetFullPath(args[2]);
            var plugins = Path.GetFullPath(args[3]);
            if (root.Equals(data, StringComparison.OrdinalIgnoreCase) ||
                root.StartsWith(data + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
                throw new ArgumentException("Atlas output must be outside Skyrim Data.");
            var options = new JsonSerializerOptions { WriteIndented = true, PropertyNamingPolicy = JsonNamingPolicy.CamelCase };
            var q1 = Path.Combine(root, "q1");
            var q0 = Path.Combine(root, "q0");
            var q2 = Path.Combine(root, "q2");
            foreach (var dir in new[] { q0, q1, q2 }) Directory.CreateDirectory(dir);
            using var pg = new PgAssets(data, q1);
            var normal = new MeshCompiler(q1, options, 1);
            var minimal = new MeshCompiler(q0, options, 0);
            var maximum = new MeshCompiler(q2, options, 2, sharedSources: q1);
            var names = pg.EligibleModels().Distinct(StringComparer.OrdinalIgnoreCase)
                .Order(StringComparer.OrdinalIgnoreCase).ToArray();
            var all = new[] {
                new List<CompiledNif>(names.Length), new List<CompiledNif>(names.Length),
                new List<CompiledNif>(names.Length)
            };
            var failures = new List<object>();
            var reused = 0;
            var compiledQ2 = 0;
            var watch = System.Diagnostics.Stopwatch.StartNew();
            Console.WriteLine($"Atlas batch: {names.Length} candidates, three tiers, shared NIF acquisition.");
            foreach (var name in names)
            {
                try
                {
                    var source = pg.Read(name);
                    var baseline = normal.Compile(source);
                    all[1].Add(baseline);
                    all[0].Add(minimal.DeriveFrom(baseline, q1));
                    if (maximum.NeedsOwnQ2Compile(baseline))
                    {
                        all[2].Add(maximum.Compile(source));
                        ++compiledQ2;
                    }
                    else
                    {
                        all[2].Add(maximum.DeriveFrom(baseline, q1));
                        ++reused;
                    }
                }
                catch (Exception error) when (error is not OutOfMemoryException)
                {
                    failures.Add(new { path = name, reason = error.Message });
                    Console.Error.WriteLine($"[atlas] {name}: {error.Message}");
                }
                var count = all[1].Count + failures.Count;
                if (count % 100 == 0 || count == names.Length)
                    Console.WriteLine($"Atlas progress {count}/{names.Length} in {watch.Elapsed:g}; q2 reused {reused}, recompiled {compiledQ2}.");
            }

            int result = 0;
            for (var quality = 0; quality < 3; ++quality)
            {
                var output = Path.Combine(root, "q" + quality);
                var status = FinishLevel(data, output, plugins, options, quality, all[quality], failures);
                if (status != 0) result = status;
            }
            Console.WriteLine($"Atlas three-tier total: {watch.Elapsed:g}; q2 reused {reused}, recompiled {compiledQ2}.");
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

using System.Numerics;
using System.Text.Json;
using Mutagen.Bethesda;
using Mutagen.Bethesda.Archives;
using Mutagen.Bethesda.Environments;
using Mutagen.Bethesda.Plugins;
using Mutagen.Bethesda.Plugins.Cache;
using Mutagen.Bethesda.Plugins.Order;
using Mutagen.Bethesda.Plugins.Records;
using Mutagen.Bethesda.Skyrim;
using NiflySharp;
using NiflySharp.Blocks;
using NiflySharp.Enums;

// One bounded, renderer-independent preparation request. The native host owns
// the temporary output file and removes it even when this process fails.
if (args.Length == 4 && args[0] == "--serve") return await ServeAsync(args);
if (args.Length != 10 || args[0] != "--tile") return 2;
try
{
    var data = Path.GetFullPath(args[1]);
    var plugins = Path.GetFullPath(args[2]);
    var scope = new FormKey(ModKey.FromFileName(args[3]), Convert.ToUInt32(args[4], 16));
    var x = long.Parse(args[5], System.Globalization.CultureInfo.InvariantCulture);
    var y = long.Parse(args[6], System.Globalization.CultureInfo.InvariantCulture);
    var size = int.Parse(args[7], System.Globalization.CultureInfo.InvariantCulture);
    var worldspace = args[8] == "world";
    var output = Path.GetFullPath(args[9]);
    if (size is not (4096 or 6144 or 8192 or 10240 or 12288 or 14336 or 16384) ||
        Math.Abs(x) > 1_000_000 || Math.Abs(y) > 1_000_000)
        return 2;
    // Runtime requests always identify their source form, never an arbitrary
    // plugin-controlled file path. Only a file in the native scratch directory
    // may be written; the host validates and owns the directory's lifetime.
    if (!File.Exists(Path.Combine(data, scope.ModKey.ToString())) ||
        !Directory.Exists(Path.GetDirectoryName(output))) return 2;
    var result = SceneTile.Prepare(data, plugins, scope, x, y, size, worldspace, Path.GetDirectoryName(output)!);
    TileTransfer.Write(output, result, new JsonSerializerOptions
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase
    }, FileMode.CreateNew);
    return 0;
}
catch (Exception)
{
    // No persistent diagnostics, logs, or partial publication.
    return 1;
}

static async Task<int> ServeAsync(string[] arguments)
{
    // One isolated companion process owns the runtime's load-order context.
    // Only a small mailbox is written to its private temporary session directory.
    try
    {
        var data = Path.GetFullPath(arguments[1]);
        var plugins = Path.GetFullPath(arguments[2]);
        var sessionRoot = Path.GetFullPath(arguments[3]);
        if (!Directory.Exists(sessionRoot) ||
            !File.Exists(Path.Combine(data, "Skyrim.esm"))) return 2;
        var requestFile = Path.Combine(sessionRoot, "request.json");
        var replyFile = Path.Combine(sessionRoot, "reply.tile");
        var replyTemp = Path.Combine(sessionRoot, "reply.tmp");
        var options = new JsonSerializerOptions { PropertyNamingPolicy = JsonNamingPolicy.CamelCase };
        Func<FormKey, long, long, int, bool, string, bool, TileResult>? prepare = null;
        IDisposable? owner = null;
        DeploymentContext? deployment = null;
        bool? sessionProfile = null;
        try
        {
            while (true)
            {
                if (!File.Exists(requestFile))
                {
                    await Task.Delay(25);
                    continue;
                }
                TileResult? result = null;
                try
                {
                    using var message = JsonDocument.Parse(await File.ReadAllTextAsync(requestFile));
                    File.Delete(requestFile);
                    var root = message.RootElement;
                    var scope = new FormKey(ModKey.FromFileName(root.GetProperty("scopePlugin").GetString()!),
                        root.GetProperty("localScopeID").GetUInt32());
                    var x = root.GetProperty("x").GetInt64();
                    var y = root.GetProperty("y").GetInt64();
                    var size = root.GetProperty("size").GetInt32();
                    var worldspace = root.GetProperty("worldspace").GetBoolean();
                    var fullNative = root.TryGetProperty("fullNative", out var profile) && profile.GetBoolean();
                    if (size is not (4096 or 6144 or 8192 or 10240 or 12288 or 14336 or 16384) ||
                        Math.Abs(x) > 1_000_000 || Math.Abs(y) > 1_000_000 ||
                        !File.Exists(Path.Combine(data, scope.ModKey.ToString())))
                        throw new InvalidDataException("Invalid companion scope");
                    if (sessionProfile.HasValue && sessionProfile.Value != fullNative)
                        throw new InvalidDataException("Companion profile changed; restart its session");
                    if (prepare == null)
                    {
                        if (fullNative)
                        {
                            var loaded = root.GetProperty("loadedPlugins").Deserialize<string[]>() ?? [];
                            var light = root.GetProperty("loadedLightPlugins").Deserialize<string[]>() ?? [];
                            var mo2Vfs = root.TryGetProperty("mo2Vfs", out var vfs) && vfs.GetBoolean();
                            deployment = DeploymentContext.Discover(data, plugins, loaded, light, mo2Vfs);
                        }
                        (prepare, owner) = SceneTile.OpenSession(data, deployment?.PluginsPath ?? plugins, deployment);
                        sessionProfile = fullNative;
                    }
                    deployment?.VerifyUnchanged();
                    result = prepare(scope, x, y, size, worldspace, sessionRoot, fullNative);
                }
                catch (Exception error) when (error is not OutOfMemoryException)
                {
                    // Return the reason through the ephemeral mailbox, never a persistent log.
                    // Native geometry publication must fail closed on a request failure.
                    result = new TileResult(1, 0, 1, false, [], [], DescribeFailure("Request", error));
                }
                // Publish the complete bounded binary envelope atomically.
                TileTransfer.Write(replyTemp, result ?? new TileResult(1, 0, 1, false, [], []), options, FileMode.Create);
                File.Move(replyTemp, replyFile, true);
            }
        }
        finally { owner?.Dispose(); }
    }
    catch (Exception error) when (error is not OutOfMemoryException)
    {
        // Load-order initialization can fail before the request loop starts.
        // Publish a bounded error response so the in-game status page can explain why.
        try
        {
            if (arguments.Length == 4)
            {
                var sessionRoot = Path.GetFullPath(arguments[3]);
                if (Directory.Exists(sessionRoot))
                {
                    var temp = Path.Combine(sessionRoot, "reply.tmp");
                    var reply = Path.Combine(sessionRoot, "reply.tile");
                    var failure = new TileResult(1, 0, 1, false, [], [],
                        DescribeFailure("Initialize", error));
                    TileTransfer.Write(temp, failure, new JsonSerializerOptions
                    {
                        PropertyNamingPolicy = JsonNamingPolicy.CamelCase
                    }, FileMode.Create);
                    File.Move(temp, reply, true);
                }
            }
        }
        catch (Exception) { /* No persistent diagnostics and no failed-output retention. */ }
        return 1;
    }
}

static string DescribeFailure(string stage, Exception error)
{
    var detail = $"{stage}: {error.GetType().Name}: {error.Message}";
    return detail.Length <= 320 ? detail : detail[..320];
}

internal sealed record TileShape(ulong Identity, Vector3[] Positions, uint[] Indices, float[] Transform,
    byte[]? Shading = null, MaterialSource? Material = null, string ReferencePlugin = "", uint ReferenceLocal = 0, PlacementMaterial? Override = null, bool AlphaTest = false);
internal sealed record CompanionTelemetry(string Manager, string Profile, int PgEntries, long CacheBytes, int CacheEntries,
    ulong CacheHits, ulong CacheMisses, ulong CacheEvictions, int ExteriorIndexed, int ExteriorTotal,
    bool ExteriorFinished, bool ExteriorIncomplete);
internal sealed record PlacementMaterial(string TextureSet, string MaterialObject, string[]? Textures, bool Ambiguous);
internal sealed record TileNifSource(string ReferencePlugin, uint ReferenceLocal, string NifPath);
internal sealed record TileMissingNifShape(string ReferencePlugin, uint ReferenceLocal, string NifPath,
    int BlockIndex, string Reason);
internal sealed record TileLight(string ReferencePlugin, uint ReferenceLocal, string BasePlugin, uint BaseLocal,
    float[] Position, float[] Color, float Radius, float Fade, float EmitterRadius, float FalloffExponent, uint Flags);
internal sealed record TileResult(uint Version, uint EligibleShapes, uint FailedShapes, bool EnumerationComplete,
    List<TileShape> Shapes, List<TileLight> Lights, string Failure = "",
    Dictionary<string, uint>? RejectionReasons = null,
    List<TileNifSource>? NifSources = null, List<TileMissingNifShape>? MissingShapes = null, bool FullNative = false, CompanionTelemetry? Telemetry = null);
internal sealed record ModelShape(int Index, Vector3[] Positions, uint[] Indices, Matrix4x4 ToModel,
    byte[]? Shading = null, MaterialSource? Material = null);
internal sealed record MissingModelShape(int Index, string Reason);
internal sealed record ModelExtraction(ModelShape[] Shapes, MissingModelShape[] Missing,
    Dictionary<string, uint> Rejections);

internal static class SceneTile
{
    private const int MaxShapes = 4096;
    private const int MaxLights = 2048;
    private const int MaxNifSources = 4096;
    private const int MaxMissingShapes = 4096;
    private const long MaxGeometryBytes = 48L * 1024 * 1024;
    private const long MaxDecodedCacheBytes = 48L * 1024 * 1024;
    private const uint MaxArchiveMeshBytes = 16u * 1024 * 1024;

    internal static TileResult Prepare(string data, string pluginsPath, FormKey scope, long tileX, long tileY, int size,
        bool isWorldspace, string scratch)
    {
        var (run, owner) = OpenSession(data, pluginsPath);
        using (owner) return run(scope, tileX, tileY, size, isWorldspace, scratch, false);
    }

    internal static (Func<FormKey, long, long, int, bool, string, bool, TileResult> Run, IDisposable Owner)
        OpenSession(string data, string pluginsPath, DeploymentContext? deployment = null)
    {
        var official = new[] { "Skyrim.esm", "Update.esm", "Dawnguard.esm", "HearthFires.esm", "Dragonborn.esm" };
        var ccc = Path.Combine(Path.GetDirectoryName(data)!, "Skyrim.ccc");
        var club = File.Exists(ccc) ? File.ReadLines(ccc).Select(line => line.Trim()).Where(line => line.Length != 0) : [];
        var implicitPlugins = official.Concat(club).Where(name => File.Exists(Path.Combine(data, name)))
            .Select(name => ModKey.FromFileName(name));
        var order = deployment?.OrderedPlugins.Select(name => ModKey.FromFileName(name)).ToArray() ??
            implicitPlugins.Concat(PluginListings.LoadOrderListingsFromPath(pluginsPath, GameRelease.SkyrimSE, data)
                .Where(entry => entry.Enabled).Select(entry => entry.ModKey)).Distinct().ToArray();
        var environment = GameEnvironment.Typical.Builder<ISkyrimMod, ISkyrimModGetter>(GameRelease.SkyrimSE)
            .WithTargetDataFolder(data).WithLoadOrder(order).Build();
        var archives = new Lazy<IArchiveReader[]>(() => Archive.GetApplicableArchivePaths(GameRelease.SkyrimSE, data)
            .Select(path => Archive.CreateReader(GameRelease.SkyrimSE, path)).Reverse().ToArray());

        // These are placements only, not mesh geometry. The cache is replaced whenever
        // scope, size or the bounded region changes. It never indexes the whole game.
        Dictionary<FormKey, IPlacedGetter>? placementWindow = null;
        FormKey placementScope = default;
        bool placementWorldspace = false, placementFoundScope = false;
        int placementSize = 0;
        long placementMinX = 0, placementMinY = 0, placementMaxX = 0, placementMaxY = 0;
        FullNativeContext? native = null;
        var placementBounds = new Dictionary<FormKey, (Vector2 Low, Vector2 High)>();
        var modelBounds = new Dictionary<string, (Vector3 Low, Vector3 High)?>(StringComparer.OrdinalIgnoreCase);
        FormKey[] exteriorKeys = [];
        int exteriorCursor = 0;
        bool exteriorIncomplete = false;

        // Keep Mutagen and archive readers alive across the 5x5 request stream.
        // Model triangles remain per-tile and are not stored between preparations.
        TileResult Run(FormKey scope, long tileX, long tileY, int size, bool isWorldspace, string scratch, bool fullNative)
        {
        // Never initialize the persistent cache or PG resolver for a lightweight request.
        var full = fullNative ? (native ??= new FullNativeContext(data, deployment ?? throw new InvalidDataException("Missing Full Native deployment context"))) : null;
        // Last override wins. No mesh processing occurs outside the requested
        // scope; for exterior tiles even candidates are bounded by a halo.
        // A 9x9 placement window contains the complete candidate origin halo
        // for the current 5x5 preparation order (tile position +/- 2, origin +/- 2).
        // Reload only when movement escapes the previous bounded window.
        var reuseWindow = placementWindow != null && placementScope == scope &&
            placementSize == size && placementWorldspace == isWorldspace &&
            (!isWorldspace || fullNative || (tileX - 2 >= placementMinX && tileY - 2 >= placementMinY &&
                tileX + 3 <= placementMaxX && tileY + 3 <= placementMaxY));
        if (!reuseWindow)
        {
            placementWindow = new Dictionary<FormKey, IPlacedGetter>();
            placementBounds.Clear();
            modelBounds.Clear();
            exteriorKeys = [];
            exteriorCursor = 0;
            exteriorIncomplete = false;
            placementScope = scope;
            placementSize = size;
            placementWorldspace = isWorldspace;
            placementMinX = tileX - 4;
            placementMinY = tileY - 4;
            placementMaxX = tileX + 5;
            placementMaxY = tileY + 5;
            placementFoundScope = false;
            foreach (var listing in environment.LoadOrder.ListedOrder)
            {
                if (listing.Mod is not { } mod) continue;
                IEnumerable<ICellGetter> cells;
                if (fullNative && isWorldspace)
                {
                    var worlds = mod.EnumerateMajorRecords<IWorldspaceGetter>().Where(world => world.FormKey == scope).ToArray();
                    if (worlds.Length != 0) placementFoundScope = true;
                    cells = worlds.SelectMany(world => world.EnumerateMajorRecords<ICellGetter>()
                        .Concat(world.TopCell is { } top ? new[] { top } : Array.Empty<ICellGetter>()));
                }
                else cells = mod.EnumerateMajorRecords<ICellGetter>();
                foreach (var cell in cells)
                {
                    if (!isWorldspace && cell.FormKey != scope) continue;
                    if (cell.FormKey == scope) placementFoundScope = true;
                    foreach (var reference in cell.Persistent.Concat(cell.Temporary))
                    {
                        if (isWorldspace && !fullNative)
                        {
                            if (reference.Placement is not { } p) continue;
                            var px = p.Position.X;
                            var py = p.Position.Y;
                            if (!float.IsFinite(px) || !float.IsFinite(py) ||
                                px < placementMinX * (double)size || px >= placementMaxX * (double)size ||
                                py < placementMinY * (double)size || py >= placementMaxY * (double)size)
                                continue;
                        }
                        placementWindow[reference.FormKey] = reference;
                        // Bound reference metadata independently of NIF/triangle budgets.
                        // A heavy scope cannot silently become authoritative coverage.
                        if (placementWindow.Count > (fullNative && isWorldspace ? 1_000_000 : 150_000))
                        {
                            placementWindow = null;
                            return new TileResult(1, 0, 1, false, [], [], FullNative: fullNative);
                        }
                    }
                }
            }
            if (fullNative && isWorldspace)
                exteriorKeys = placementWindow.Keys.ToArray();
        }
        var candidates = placementWindow!;
        if (fullNative && isWorldspace)
        {
            // Advance the world bounds catalog incrementally within each tile
            // request. Decode only one temporary model at a time, retaining its
            // conservative model-space AABB, not triangles, textures or a disk
            // package. The normal tile path still has active priority.
            var timer = System.Diagnostics.Stopwatch.StartNew();
            var indexModels = new Dictionary<string, ModelExtraction>(StringComparer.OrdinalIgnoreCase);
            long indexBytes = 0;
            int decoded = 0;
            while (exteriorCursor < exteriorKeys.Length && timer.ElapsedMilliseconds < 1200 && decoded < 32)
            {
                var key = exteriorKeys[exteriorCursor++];
                try
                {
                    var context = environment.LinkCache.ResolveContext<IPlaced, IPlacedGetter>(key);
                    if (!context.TryGetParent<IWorldspaceGetter>(out var world) || world.FormKey != scope ||
                        context.Record.IsDeleted) continue;
                    if (context.Record is not IPlacedObjectGetter obj) continue;
                    var baseRecord = obj.Base.TryResolve(environment.LinkCache);
                    if (baseRecord == null) { exteriorIncomplete = true; continue; }
                    if (baseRecord is ILightGetter) continue;
                    if (baseRecord is not (IStaticGetter or IFurnitureGetter or IMoveableStaticGetter))
                    {
                        if (baseRecord is IModeledGetter) exteriorIncomplete = true;
                        continue;
                    }
                    if (obj.Placement is not { } placement || (obj.MajorRecordFlagsRaw & 0x800) != 0 ||
                        obj.EnableParent != null || obj.VirtualMachineAdapter != null || baseRecord is IMoveableStaticGetter)
                    {
                        exteriorIncomplete = true;
                        continue;
                    }
                    var model = (baseRecord as IModeledGetter)?.Model?.File.ToString();
                    if (string.IsNullOrWhiteSpace(model)) { exteriorIncomplete = true; continue; }
                    var path = NormalizeNifPath(model);
                    if (!modelBounds.TryGetValue(path, out var bounds))
                    {
                        ++decoded;
                        // No FullNativeContext here: an exterior metadata index
                        // must never populate the persistent compiled mesh cache.
                        indexModels.Clear(); indexBytes = 0;
                        var extraction = LoadModel(data, path, indexModels, ref indexBytes, archives, scratch, null);
                        bounds = ModelBounds(extraction);
                        modelBounds[path] = bounds;
                        if (extraction.Rejections.Count != 0 || bounds == null) exteriorIncomplete = true;
                        indexModels.Clear(); indexBytes = 0;
                    }
                    if (bounds is not { } box) { exteriorIncomplete = true; continue; }
                    var worldTransform = PlacementTransform.FromRecord(
                        [placement.Position.X, placement.Position.Y, placement.Position.Z],
                        [placement.Rotation.X, placement.Rotation.Y, placement.Rotation.Z], obj.Scale ?? 1.0f);
                    placementBounds[key] = TransformBounds(box, worldTransform);
                }
                catch (Exception error) when (error is not OutOfMemoryException)
                {
                    exteriorIncomplete = true;
                }
            }
        }

        var foundScope = placementFoundScope;
        var shapes = new List<TileShape>();
        var lights = new List<TileLight>();
        bool lightCapacityExceeded = false;
        // Retain NIF provenance even when a rigid shape cannot be decoded yet.
        // Runtime supplementation can refer back to the installed loose/BSA mesh.
        var nifSources = new List<TileNifSource>();
        var missingShapes = new List<TileMissingNifShape>();
        var models = new Dictionary<string, ModelExtraction>(StringComparer.OrdinalIgnoreCase);
        long cachedModelBytes = 0;
        uint eligible = 0, failed = 0;
        var rejections = new Dictionary<string, uint>(StringComparer.Ordinal);
        void Reject(string reason, uint count = 1)
        {
            failed += count;
            if (!rejections.ContainsKey(reason) && rejections.Count >= 12) reason = "other";
            rejections[reason] = rejections.GetValueOrDefault(reason) + count;
        }
        void AddMissing(FormKey reference, string nifPath, int blockIndex, string reason)
        {
            if (missingShapes.Count >= MaxMissingShapes) {
                Reject("missing shape manifest capacity");
                return;
            }
            missingShapes.Add(new TileMissingNifShape(reference.ModKey.ToString(), reference.ID,
                nifPath, blockIndex, reason.Length > 80 ? reason[..80] : reason));
        }
        void RememberMissingModel(FormKey reference, string? meshPath, string reason)
        {
            if (string.IsNullOrWhiteSpace(meshPath)) return;
            var nifPath = NormalizeNifPath(meshPath);
            if (nifPath.Length > 260) {
                Reject("NIF path exceeds manifest limit");
                return;
            }
            if (nifSources.Count < MaxNifSources)
                nifSources.Add(new TileNifSource(reference.ModKey.ToString(), reference.ID, nifPath));
            else
                Reject("NIF source manifest capacity");
            AddMissing(reference, nifPath, -1, reason);
        }
        long bytes = 0, shadingBytes = 0;
        var admittedShading = new HashSet<byte[]>();
        var deferredShading = new HashSet<byte[]>();
        foreach (var candidate in candidates.Values)
        {
            // The cached placement window is wider than one subcell's origin
            // halo; discard irrelevant candidates before resolving any records.
            if (isWorldspace)
            {
                if (candidate.Placement is not { } placement) continue;
                var px = placement.Position.X;
                var py = placement.Position.Y;
                if (fullNative && placementBounds.TryGetValue(candidate.FormKey, out var indexed))
                {
                    if (!BoundsOverlap(indexed.Low, indexed.High, tileX, tileY, size)) continue;
                }
                else if (px < (tileX - 2) * (double)size || px >= (tileX + 3) * (double)size ||
                    py < (tileY - 2) * (double)size || py >= (tileY + 3) * (double)size)
                    continue;
            }
            string? unresolvedNif = null;
            try
            {
                var context = environment.LinkCache.ResolveContext<IPlaced, IPlacedGetter>(candidate.FormKey);
                if (isWorldspace)
                {
                    if (!context.TryGetParent<IWorldspaceGetter>(out var world) || world.FormKey != scope) continue;
                }
                else if (!context.TryGetParent<ICellGetter>(out var cell) || cell.FormKey != scope)
                    continue;
                var placed = context.Record;
                if (placed.IsDeleted) continue;
                var obj = placed as IPlacedObjectGetter;
                if (obj == null) { Reject("unsupported placement record"); continue; }
                var baseRecord = obj.Base.TryResolve(environment.LinkCache);
                if (baseRecord == null)
                {
                    // An unresolved base may describe a shadow-casting static.
                    // Never advertise the tile as complete when its role is unknown.
                    Reject("unresolved base record");
                    continue;
                }
                if (placed.Placement is not { } placement) {
                    if (baseRecord is IStaticGetter or IFurnitureGetter or IMoveableStaticGetter) Reject("missing placement");
                    continue;
                }
                var position = new[] { placement.Position.X, placement.Position.Y, placement.Position.Z };
                if (baseRecord is ILightGetter light)
                {
                    if (Inside(position[0], position[1], tileX, tileY, size))
                    {
                        if (lights.Count < MaxLights)
                            lights.Add(new TileLight(placed.FormKey.ModKey.ToString(), placed.FormKey.ID,
                                obj.Base.FormKey.ModKey.ToString(), obj.Base.FormKey.ID, position,
                                [light.Color.R / 255f, light.Color.G / 255f, light.Color.B / 255f],
                                light.Radius, light.FadeValue, 0, light.FalloffExponent, (uint)light.Flags));
                        else
                            lightCapacityExceeded = true;
                    }
                    continue;
                }
                if (baseRecord is not (IStaticGetter or IFurnitureGetter or IMoveableStaticGetter))
                {
                    // Doors, activators, containers, trees, and other modeled
                    // records can become static raster shadow casters. This
                    // geometry-only companion cannot guarantee their coverage.
                    // Do not silently certify a tile with missing static casters.
                    if (baseRecord is IModeledGetter modeled &&
                        modeled.Model?.File is { } file &&
                        !string.IsNullOrWhiteSpace(file.ToString()) &&
                        (!isWorldspace || Inside(position[0], position[1], tileX, tileY, size)))
                    {
                        Reject("unsupported modeled record");
                        RememberMissingModel(placed.FormKey, file.ToString(), "unsupported modeled record");
                    }
                    continue;
                }

                // Runtime-dependent placement cannot be certified from plugin data.
                if ((placed.MajorRecordFlagsRaw & 0x800) != 0 || obj.EnableParent != null ||
                    obj.VirtualMachineAdapter != null || baseRecord is IMoveableStaticGetter)
                {
                    Reject("runtime-dependent placement");
                    RememberMissingModel(placed.FormKey, (baseRecord as IModeledGetter)?.Model?.File.ToString(),
                        "runtime-dependent placement");
                    continue;
                }
                var model = (baseRecord as IModeledGetter)?.Model?.File.ToString();
                if (string.IsNullOrWhiteSpace(model)) { Reject("missing mesh path"); continue; }
                unresolvedNif = NormalizeNifPath(model);
                if (unresolvedNif.Length > 260)
                    throw new InvalidDataException("NIF path exceeds manifest limit");
                if (nifSources.Count < MaxNifSources)
                    nifSources.Add(new TileNifSource(placed.FormKey.ModKey.ToString(),
                        placed.FormKey.ID, unresolvedNif));
                else
                    Reject("NIF source manifest capacity");
                if (placementBounds.TryGetValue(placed.FormKey, out var known) &&
                    !BoundsOverlap(known.Low, known.High, tileX, tileY, size)) continue;
                var extraction = LoadModel(data, unresolvedNif, models, ref cachedModelBytes, archives, scratch, full);
                foreach (var (reason, count) in extraction.Rejections)
                    Reject(reason, count);
                foreach (var missing in extraction.Missing)
                    AddMissing(placed.FormKey, unresolvedNif, missing.Index, missing.Reason);
                var mesh = extraction.Shapes;
                var rotation = new[] { placement.Rotation.X, placement.Rotation.Y, placement.Rotation.Z };
                var toWorld = PlacementTransform.FromRecord(position, rotation, obj.Scale ?? 1.0f);
                if (mesh.Length > 0 && extraction.Rejections.Count == 0 && !placementBounds.ContainsKey(placed.FormKey))
                {
                    var low = new Vector2(float.PositiveInfinity);
                    var high = new Vector2(float.NegativeInfinity);
                    foreach (var item in mesh)
                    {
                        var bounds = Bounds(item.Positions, item.ToModel * toWorld);
                        low = Vector2.Min(low, bounds.Low); high = Vector2.Max(high, bounds.High);
                    }
                    placementBounds[placed.FormKey] = (low, high);
                }
                foreach (var shape in mesh)
                {
                    var transform = shape.ToModel * toWorld;
                    if (!Overlaps(shape.Positions, transform, tileX, tileY, size)) continue;
                    ++eligible;
                    var shapeBytes = checked((long)shape.Positions.Length * 12 + (long)shape.Indices.Length * 4);
                    var budgetReason = shapes.Count >= MaxShapes ? "tile shape count exceeded" :
                        shapeBytes > MaxGeometryBytes - bytes ? "tile geometry byte budget exceeded" : null;
                    if (budgetReason != null)
                    {
                        Reject(budgetReason);
                        AddMissing(placed.FormKey, unresolvedNif, shape.Index, budgetReason);
                        continue;
                    }
                    bytes += shapeBytes;
                    var shading = shape.Shading;
                    if (shading != null && deferredShading.Contains(shading)) shading = null;
                    if (shading != null && !admittedShading.Contains(shading))
                    {
                        if (shading.Length > MaxGeometryBytes - shadingBytes)
                        {
                            deferredShading.Add(shading);
                            shading = null;
                            Reject("tile shading byte budget exceeded");
                        }
                        else { admittedShading.Add(shading); shadingBytes += shading.Length; }
                    }
                    if (shape.Material?.AlphaTest == true && shading == null)
                    {
                        bytes -= shapeBytes;
                        Reject("alpha-tested shape shading budget");
                        AddMissing(placed.FormKey, unresolvedNif, shape.Index, "alpha-tested shape shading budget");
                        continue;
                    }
                    PlacementMaterial? materialOverride = null;
                    if (fullNative && shading != null && shape.Material is { } original)
                    {
                        try
                        {
                            var alternates = (baseRecord as IModeledGetter)?.Model?.AlternateTextures;
                            var matches = alternates?.Where(a => a.Index == original.ShapeOrdinal &&
                                a.Name == original.ShapeName).ToArray() ?? [];
                            var ambiguous = matches.Length > 1 || (matches.Length == 0 &&
                                alternates?.Any(a => a.Name == original.ShapeName || a.Index == original.ShapeOrdinal) == true);
                            var txst = matches.Length == 1 ? matches[0].NewTexture.TryResolve(environment.LinkCache) : null;
                            if (matches.Length == 1 && txst == null) ambiguous = true;
                            var mato = baseRecord is IStaticGetter stat ? stat.Material.TryResolve(environment.LinkCache) : null;
                            var matoId = mato != null && mato.Flags.HasFlag(MaterialObject.Flag.SinglePass) ? mato.EditorID ?? "" : "";
                            string[]? paths = txst == null ? null : new[] {
                                txst.Diffuse?.ToString(), txst.NormalOrGloss?.ToString(), txst.EnvironmentMaskOrSubsurfaceTint?.ToString(),
                                txst.GlowOrDetailMap?.ToString(), txst.Height?.ToString(), txst.Environment?.ToString(),
                                txst.Multilayer?.ToString(), txst.BacklightMaskOrSpecular?.ToString()
                            }.Select(NativeSource.NormalizeTexture).ToArray();
                            if (txst != null || matoId.Length != 0 || ambiguous)
                                materialOverride = new(txst?.EditorID ?? "", matoId, paths, ambiguous);
                        }
                        catch (Exception error) when (error is not OutOfMemoryException)
                        {
                            materialOverride = new("", "", null, true);
                        }
                    }
                    shapes.Add(new TileShape(Identity(placed.FormKey.ToString(), shape.Index),
                        shape.Positions, shape.Indices, PlacementTransform.ToRows(transform),
                        shading, shading != null ? shape.Material : null,
                        fullNative ? placed.FormKey.ModKey.ToString() : "", fullNative ? placed.FormKey.ID : 0, materialOverride,
                        fullNative && shape.Material?.AlphaTest == true));
                }
            }
            catch (Exception error) when (error is not OutOfMemoryException)
            {
                var message = error is InvalidDataException ? error.Message : error.GetType().Name;
                Reject(message.Length <= 80 ? message : message[..80]);
                if (unresolvedNif != null)
                    AddMissing(candidate.FormKey, unresolvedNif, -1, message);
            }
        }
        // A bounded exterior anchor halo cannot prove that a huge off-tile
        // model does not overlap this tile. Exteriors remain incomplete until
        // an authoritative bounds index is available; never offload on a guess.
        if (lightCapacityExceeded && rejections.Count < 12)
            rejections["tile light capacity"] = 1;
        var indexFinished = fullNative && isWorldspace && exteriorCursor == exteriorKeys.Length;
        var complete = foundScope && !lightCapacityExceeded && (!isWorldspace || indexFinished && !exteriorIncomplete);
        return new TileResult(1, eligible, failed, complete,
            shapes, lights, "", rejections, nifSources, missingShapes, fullNative,
            full?.Telemetry(exteriorCursor, exteriorKeys.Length, indexFinished, exteriorIncomplete));
        }
        return (Run, environment);
    }

    private static (Vector3 Low, Vector3 High)? ModelBounds(ModelExtraction extraction)
    {
        if (extraction.Shapes.Length == 0) return null;
        var low = new Vector3(float.PositiveInfinity);
        var high = new Vector3(float.NegativeInfinity);
        foreach (var shape in extraction.Shapes)
            foreach (var vertex in shape.Positions)
            {
                var point = Vector3.Transform(vertex, shape.ToModel);
                if (!float.IsFinite(point.X) || !float.IsFinite(point.Y) || !float.IsFinite(point.Z))
                    return null;
                low = Vector3.Min(low, point); high = Vector3.Max(high, point);
            }
        return (low, high);
    }

    private static (Vector2 Low, Vector2 High) TransformBounds((Vector3 Low, Vector3 High) box, Matrix4x4 transform)
    {
        // All eight corners are required: a model's Z extent may rotate into
        // XY, and a reference's origin can lie arbitrarily far from its mesh.
        var corners = new Vector3[8];
        for (int i = 0; i < corners.Length; ++i)
            corners[i] = new((i & 1) == 0 ? box.Low.X : box.High.X,
                (i & 2) == 0 ? box.Low.Y : box.High.Y, (i & 4) == 0 ? box.Low.Z : box.High.Z);
        return Bounds(corners, transform);
    }

    private static bool Inside(float x, float y, long tx, long ty, int size) =>
        x >= tx * (double)size && x < (tx + 1) * (double)size &&
        y >= ty * (double)size && y < (ty + 1) * (double)size;

    private static (Vector2 Low, Vector2 High) Bounds(Vector3[] vertices, Matrix4x4 transform)
    {
        var low = new Vector2(float.PositiveInfinity);
        var high = new Vector2(float.NegativeInfinity);
        foreach (var vertex in vertices)
        {
            var p = Vector3.Transform(vertex, transform);
            if (!float.IsFinite(p.X) || !float.IsFinite(p.Y)) throw new InvalidDataException("Invalid placed bounds");
            low = Vector2.Min(low, new Vector2(p.X, p.Y));
            high = Vector2.Max(high, new Vector2(p.X, p.Y));
        }
        return (low, high);
    }

    private static bool BoundsOverlap(Vector2 low, Vector2 high, long tx, long ty, int size) =>
        low.X < (tx + 1) * (double)size && high.X >= tx * (double)size &&
        low.Y < (ty + 1) * (double)size && high.Y >= ty * (double)size;

    private static bool Overlaps(Vector3[] vertices, Matrix4x4 transform, long tx, long ty, int size)
    {
        var b = Bounds(vertices, transform);
        return BoundsOverlap(b.Low, b.High, tx, ty, size);
    }

    private static string NormalizeNifPath(string path)
    {
        path = path.Replace('/', '\\');
        return path.StartsWith("meshes\\", StringComparison.OrdinalIgnoreCase)
            ? "meshes\\" + path[7..] : "meshes\\" + path;
    }

    private static ModelExtraction LoadModel(string data, string path, Dictionary<string, ModelExtraction> cache,
        ref long cachedBytes, Lazy<IArchiveReader[]> archives, string scratch, FullNativeContext? full)
    {
        path = NormalizeNifPath(path);
        if (Path.IsPathRooted(path) || path.Split('\\').Any(part => part is ".." or "." or ""))
            throw new InvalidDataException("Invalid mesh path");
        if (cache.TryGetValue(path, out var cached)) return cached;
        var filename = Path.GetFullPath(Path.Combine(data, path));
        if (!filename.StartsWith(data + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("Mesh escaped Data");
        var nif = new NifFile();
        var temporary = string.Empty;
        string? sourceHash = null, provenance = null;
        try
        {
            if (!File.Exists(filename))
            {
                foreach (var archive in archives.Value)
                {
                    if (!archive.TryGetFolder(Path.GetDirectoryName(path)!, out var folder)) continue;
                    var entry = folder.Files.FirstOrDefault(file =>
                        string.Equals(file.Path.Replace('/', '\\'), path, StringComparison.OrdinalIgnoreCase));
                    if (entry == null) continue;
                    if (entry.Size == 0 || entry.Size > MaxArchiveMeshBytes)
                        throw new InvalidDataException("Archive mesh exceeds extraction budget");
                    temporary = Path.Combine(scratch, "archive-mesh.nif");
                    File.WriteAllBytes(temporary, entry.GetBytes());
                    filename = temporary;
                    break;
                }
            }
            if (full != null && File.Exists(filename))
            {
                (sourceHash, provenance) = full.Verify(path, filename, temporary.Length == 0);
                if (sourceHash != null && full.TryRead(sourceHash, path, provenance!, out var prior))
                {
                    // Use the same bounded per-tile cache accounting as a fresh decode.
                    RememberDecoded(path, prior, cache, ref cachedBytes);
                    return prior;
                }
            }
            if (nif.Load(filename) != 0)
                throw new InvalidDataException("NIF unavailable or unsupported");
        }
        finally
        {
            if (temporary.Length != 0)
                File.Delete(temporary);
        }
        if (!nif.Valid || !nif.Header.Version.IsSSE())
            throw new InvalidDataException("NIF unavailable or unsupported");
        var hierarchy = new NifTransforms(nif);
        var output = new List<ModelShape>();
        var rejected = new Dictionary<string, uint>(StringComparer.Ordinal);
        var missing = new List<MissingModelShape>();
        void Skip(string reason, int index)
        {
            rejected[reason] = rejected.GetValueOrDefault(reason) + 1;
            missing.Add(new MissingModelShape(index, reason));
        }
        // Unknown blocks may coexist with valid rigid geometry. Keep the
        // supported shapes, but never certify complete shadow coverage.
        if (nif.HasUnknownBlocks)
            Skip("unknown NIF blocks (incomplete coverage)", -1);
        var shapeOrdinal = -1;
    foreach (var entry in nif.GetShapes())
        {
            ++shapeOrdinal;
            nif.GetBlockIndex(entry, out var index);
            // Retain safe opaque rigid triangles even if a NIF contains unsupported shapes.
            if (entry is not BSTriShape shape || shape.HasSkinInstance || shape.IsSkinned ||
                entry.GetType().Name is not ("BSTriShape" or "BSSubIndexTriShape" or
                    "BSMeshLODTriShape" or "BSSegmentedTriShape"))
            {
                Skip("unsupported animated or nontriangle shape", index);
                continue;
            }
            try
            {
                // Lightweight profiles still exclude all alpha-dependent shapes.
                // Full Native admits only PG-verified, nonblended supported alpha tests.
                var alphaBlock = nif.GetBlock(entry.AlphaPropertyRef);
                byte alphaThreshold = 0, alphaFunction = 0;
                if (alphaBlock != null && (full == null || sourceHash == null ||
                    alphaBlock is not NiAlphaProperty alpha ||
                    !NativeSource.TryReadAlphaTest(alpha, out alphaThreshold, out alphaFunction)))
                {
                    Skip("alpha-dependent shape (unsupported or unverified alpha test)", index);
                    continue;
                }
                var vertices = shape.VertexPositions.ToArray();
                var triangles = entry.Triangles;
                if (vertices.Length == 0 || triangles.Count == 0 ||
                    vertices.Any(v => !float.IsFinite(v.X) || !float.IsFinite(v.Y) || !float.IsFinite(v.Z)))
                    throw new InvalidDataException("Invalid shape vertices");
                var indices = new uint[triangles.Count * 3];
                for (var i = 0; i < triangles.Count; ++i)
                {
                    var t = triangles[i];
                    if (t.V1 >= vertices.Length || t.V2 >= vertices.Length || t.V3 >= vertices.Length)
                        throw new InvalidDataException("Invalid triangle index");
                    indices[i * 3] = t.V1;
                    indices[i * 3 + 1] = t.V2;
                    indices[i * 3 + 2] = t.V3;
                }
                byte[]? shading = null;
                MaterialSource? material = null;
                if (full != null && sourceHash != null)
                {
                    try
                    {
                        material = NativeSource.ReadMaterial(nif, shape, path, sourceHash, provenance!, index, shapeOrdinal);
                        if (material != null) shading = ShadingBlob.Build(shape);
                    }
                    catch (Exception error) when (error is not OutOfMemoryException)
                    {
                        // Optional shading failures cannot remove independently valid geometry.
                        shading = null;
                        material = null;
                    }
                }
                if (alphaBlock != null)
                {
                    if (material == null || shading == null || material.Alpha != 1.0f ||
                        (material.ShaderFlags2 & (1u << 23)) == 0 || material.ClampMode < 0 ||
                        material.UvTransform?.Length != 4 || material.Textures.Length < 6 ||
                        string.IsNullOrEmpty(material.Textures[0]) || string.IsNullOrEmpty(material.Textures[1]) ||
                        string.IsNullOrEmpty(material.Textures[5]))
                    {
                        Skip("alpha-tested shape missing native material inputs", index);
                        continue;
                    }
                    material = material with { AlphaTest = true, AlphaThreshold = alphaThreshold, AlphaFunction = alphaFunction };
                }
                output.Add(new ModelShape(index, vertices, indices, hierarchy.ForShape(index), shading, material));
            }
            catch (Exception error) when (error is not OutOfMemoryException)
            {
                var reason = error is InvalidDataException ? error.Message : "shape extraction: " + error.GetType().Name;
                // Keep stable bounded categories; no exception stack or persistent dump.
                Skip(reason.Length > 80 ? reason[..80] : reason, index);
            }
        }
        if (output.Count == 0 && missing.Count == 0)
            Skip("no supported opaque rigid triangles", -1);
        var extracted = new ModelExtraction(output.ToArray(), missing.ToArray(), rejected);
        if (full != null && sourceHash != null)
            full.Store(sourceHash, extracted);
        RememberDecoded(path, extracted, cache, ref cachedBytes);
        return extracted;
    }

    private static void RememberDecoded(string path, ModelExtraction extracted,
        Dictionary<string, ModelExtraction> cache, ref long cachedBytes)
    {
        // Decoded model geometry has an independent per-tile RAM cap. Bypassing
        // the cache does not discard a valid model or alter tile coverage.
        long decodedBytes = 256L + (long)path.Length * 2;
        foreach (var shape in extracted.Shapes)
            decodedBytes += 128L + (long)shape.Positions.Length * 12 + (long)shape.Indices.Length * 4 + (shape.Shading?.Length ?? 0);
        foreach (var unresolved in extracted.Missing)
            decodedBytes += 64L + (long)unresolved.Reason.Length * 2;
        foreach (var reason in extracted.Rejections.Keys)
            decodedBytes += 32L + (long)reason.Length * 2;
        if (decodedBytes <= MaxDecodedCacheBytes - cachedBytes)
        {
            cache[path] = extracted;
            cachedBytes += decodedBytes;
        }
    }

    private static ulong Identity(string formKey, int blockIndex)
    {
        ulong hash = 14695981039346656037UL;
        foreach (var ch in formKey + ":" + blockIndex.ToString(System.Globalization.CultureInfo.InvariantCulture))
        {
            hash ^= ch;
            hash = unchecked(hash * 1099511628211UL);
        }
        return hash == 0 ? 1 : hash;
    }
}

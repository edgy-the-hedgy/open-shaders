using System.Security.Cryptography;

// Full Native only. No resolver persists deployment metadata.
internal sealed record AssetOrigin(string Origin, string? PhysicalPath = null);

internal abstract class DeploymentContext
{
    internal string PluginsPath { get; }
    internal string[] OrderedPlugins { get; }
    private readonly Dictionary<string, (long Length, DateTime Stamp)> stamps = new(StringComparer.OrdinalIgnoreCase);
    private readonly Dictionary<string, AssetOrigin?> winners = new(StringComparer.OrdinalIgnoreCase);

    protected DeploymentContext(string plugins, string[] order, IEnumerable<string> inputs)
    {
        PluginsPath = plugins;
        OrderedPlugins = order;
        foreach (var path in inputs.Distinct(StringComparer.OrdinalIgnoreCase))
            stamps[path] = Stamp(path);
    }

    private static (long, DateTime) Stamp(string path)
    {
        var file = new FileInfo(path);
        return file.Exists ? (file.Length, file.LastWriteTimeUtc) : (-1, DateTime.MinValue);
    }

    internal void VerifyUnchanged()
    {
        foreach (var (path, stamp) in stamps)
            if (Stamp(path) != stamp)
                throw new InvalidDataException("Deployment configuration changed; restart Skyrim with the intended profile");
    }

    internal AssetOrigin? Winner(string path)
    {
        path = path.Replace('/', '\\');
        if (Path.IsPathRooted(path) || path.Split('\\').Any(x => x is "" or "." or "..") || path.Contains(':'))
            throw new InvalidDataException("Invalid deployment asset path");
        if (winners.TryGetValue(path, out var cached)) return cached;
        var origin = Resolve(path);
        if (winners.Count >= 8192) winners.Clear();
        winners[path] = origin;
        return origin;
    }

    protected abstract AssetOrigin? Resolve(string path);

    internal static DeploymentContext Discover(string data, string fallbackPlugins, string[] full, string[] light, bool mo2Vfs)
    {
        if (full.Length == 0 || full.Length > 254 || light.Length > 4096)
            throw new InvalidDataException("Runtime loaded plugin snapshot unavailable or invalid");
        var mo2 = MO2ProfileResolver.Discover(data, full, light, mo2Vfs);
        return mo2 ?? (DeploymentContext)new VortexDeploymentResolver(data, fallbackPlugins, full, light);
    }

    internal static bool SameBytes(string path, byte[] visible)
    {
        try
        {
            var file = new FileInfo(path);
            if (!file.Exists || file.Length != visible.LongLength) return false;
            using var stream = file.OpenRead();
            return SHA256.HashData(stream).SequenceEqual(SHA256.HashData(visible));
        }
        catch (IOException) { return false; }
        catch (UnauthorizedAccessException) { return false; }
    }

    internal static string[] ReadOrder(string data, string plugins, string? loadOrder, string[] full, string[] light)
    {
        if (!File.Exists(plugins)) throw new InvalidDataException("Active plugins.txt unavailable");
        var lines = File.ReadLines(plugins).Select(x => x.Trim()).Where(x => x.Length > 0 && !x.StartsWith('#')).ToArray();
        // Skyrim SE's asterisk format. Official implicit masters and CCC files
        // need not be present in the profile's plugins.txt.
        var enabled = lines.Where(x => x.StartsWith('*')).Select(x => x[1..].Trim()).ToArray();
        var implicitNames = new List<string> { "Skyrim.esm", "Update.esm", "Dawnguard.esm", "HearthFires.esm", "Dragonborn.esm" };
        var ccc = Path.Combine(Path.GetDirectoryName(data)!, "Skyrim.ccc");
        if (File.Exists(ccc))
            implicitNames.AddRange(File.ReadLines(ccc).Select(x => x.Trim()).Where(x => x.Length > 0));
        var active = implicitNames.Where(x => File.Exists(Path.Combine(data, x))).Concat(enabled)
            .Distinct(StringComparer.OrdinalIgnoreCase).ToArray();
        if (active.Any(x => x.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0 ||
            x.Contains('/') || x.Contains('\\') || !File.Exists(Path.Combine(data, x))))
            throw new InvalidDataException("Active plugin missing from effective Data");
        var actual = full.Concat(light).ToHashSet(StringComparer.OrdinalIgnoreCase);
        if (!actual.SetEquals(active))
            throw new InvalidDataException("Profile enabled plugins differ from running Skyrim");
        var order = active;
        if (loadOrder != null && File.Exists(loadOrder))
        {
            var listed = File.ReadLines(loadOrder).Select(x => x.Trim())
                .Where(x => x.Length > 0 && !x.StartsWith('#') && actual.Contains(x));
            var listedOrder = listed.ToArray();
            var listedSet = listedOrder.ToHashSet(StringComparer.OrdinalIgnoreCase);
            order = implicitNames.Where(x => actual.Contains(x) && !listedSet.Contains(x)).Concat(listedOrder)
                .Distinct(StringComparer.OrdinalIgnoreCase).ToArray();
            if (!actual.SetEquals(order))
                throw new InvalidDataException("Profile loadorder.txt omits active plugins");
        }
        // Engine indexes give separate full/light sequences, not their interleaving.
        // Use profile ordering for the interleaving and verify both index sequences.
        var fullSet = full.ToHashSet(StringComparer.OrdinalIgnoreCase);
        var lightSet = light.ToHashSet(StringComparer.OrdinalIgnoreCase);
        if (!order.Where(fullSet.Contains).SequenceEqual(full, StringComparer.OrdinalIgnoreCase) ||
            !order.Where(lightSet.Contains).SequenceEqual(light, StringComparer.OrdinalIgnoreCase))
            throw new InvalidDataException("Profile plugin order differs from running Skyrim");
        return order;
    }
}

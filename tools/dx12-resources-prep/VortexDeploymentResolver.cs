using System.Text.Json;

internal sealed class VortexDeploymentResolver : DeploymentContext
{
    private readonly Dictionary<string, string> files = new(StringComparer.OrdinalIgnoreCase);
    internal VortexDeploymentResolver(string data, string plugins, string[] full, string[] light)
        : base(plugins, ReadOrder(data, plugins, null, full, light),
            [plugins, Path.Combine(data, "vortex.deployment.json"), Path.Combine(data, "ParallaxGen_Diff.json")])
    {
        using var deployment = JsonDocument.Parse(File.ReadAllText(Path.Combine(data, "vortex.deployment.json")));
        foreach (var entry in deployment.RootElement.GetProperty("files").EnumerateArray())
        {
            var path = entry.GetProperty("relPath").GetString();
            var source = entry.GetProperty("source").GetString();
            if (path != null && source != null) files[path.Replace('/', '\\')] = source;
        }
    }

    protected override AssetOrigin? Resolve(string path) =>
        files.TryGetValue(path, out var source) ? new AssetOrigin(source) : null;
}

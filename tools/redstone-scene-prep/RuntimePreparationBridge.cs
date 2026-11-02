using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

internal static class RuntimePreparationBridge
{
    [StructLayout(LayoutKind.Sequential)]
    private struct Request
    {
        public nint DataDirectory;
        public nint PluginsFile;
        public nint CacheDirectory;
        public nint ErrorBuffer;
        public int ErrorCapacity;
        public int Quality;
        public int Force;
    }

    private const string GuestRoom = "04FD2B:Unslaad.esm";
    private static readonly Regex PayloadName = new(
        @"^(geometry/[0-9a-f]{64}\.rsgm|shading/[0-9a-f]{64}\.rsga|textures/[0-9a-f]{64}\.dds)$",
        RegexOptions.Compiled | RegexOptions.CultureInvariant);

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    public static int Prepare(nint pointer, int size)
    {
        if (pointer == 0 || size != Marshal.SizeOf<Request>()) return -1;
        var request = Marshal.PtrToStructure<Request>(pointer);
        try
        {
            if (request.Quality is < 0 or > 2) throw new ArgumentOutOfRangeException(nameof(request.Quality));
            var data = Path.GetFullPath(Marshal.PtrToStringUni(request.DataDirectory)!);
            var plugins = Path.GetFullPath(Marshal.PtrToStringUni(request.PluginsFile)!);
            var destination = Path.GetFullPath(Marshal.PtrToStringUni(request.CacheDirectory)!);
            var local = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                "OpenShaders", "NativeScene", "Unslaad.esm", "04FD2B");
            if (request.Force == 0 && CacheValid(data, plugins, destination, request.Quality)) return 0;
            Directory.CreateDirectory(local);
            if (ScenePreparation.Run([data, "Unslaad.esm:04FD2B", local, plugins], request.Quality) != 0)
                throw new InvalidDataException(ScenePreparation.LastFailure);
            Deploy(data, plugins, local, destination, request.Quality);
            return 1;
        }
        catch (Exception error)
        {
            WriteError(request, error.Message);
            return -1;
        }
    }

    private static void WriteError(Request request, string message)
    {
        if (request.ErrorBuffer == 0 || request.ErrorCapacity <= 0) return;
        var chars = message.AsSpan(0, Math.Min(message.Length, request.ErrorCapacity - 1)).ToString().ToCharArray();
        Marshal.Copy(chars, 0, request.ErrorBuffer, chars.Length);
        Marshal.WriteInt16(request.ErrorBuffer, chars.Length * sizeof(char), 0);
    }

    private static string Fingerprint(string data, string plugins, string records)
    {
        using var doc = JsonDocument.Parse(File.ReadAllText(records));
        var input = new List<string> { plugins, Path.Combine(data, "ParallaxGen_Diff.json"),
            Path.Combine(data, "vortex.deployment.json") };
        foreach (var plugin in doc.RootElement.GetProperty("pluginSources").EnumerateArray())
            input.Add(Path.Combine(data, plugin.GetProperty("plugin").GetString()!));
        using var digest = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        foreach (var file in input.Distinct(StringComparer.OrdinalIgnoreCase))
        {
            var pathBytes = Encoding.UTF8.GetBytes(Path.GetFullPath(file).ToLowerInvariant());
            digest.AppendData(pathBytes);
            using var stream = File.OpenRead(file);
            digest.AppendData(SHA256.HashData(stream));
        }
        return Convert.ToHexString(digest.GetHashAndReset()).ToLowerInvariant();
    }

    private static (string Digest, string[] Payloads) ReadPayloads(string directory, int quality)
    {
        using var layout = JsonDocument.Parse(File.ReadAllText(Path.Combine(directory, "cell-layout.json")));
        var root = layout.RootElement;
        if (root.GetProperty("rendererSchemaVersion").GetInt32() != 1 ||
            root.GetProperty("cell").GetString() != GuestRoom)
            throw new InvalidDataException("Prepared scene identity/schema mismatch.");
        var instances = root.GetProperty("instances");
        if (instances.GetArrayLength() == 0) throw new InvalidDataException("Empty guest-room geometry.");
        var payloads = new HashSet<string>(StringComparer.Ordinal);
        foreach (var instance in instances.EnumerateArray())
        {
            payloads.Add(instance.GetProperty("geometryFile").GetString()!);
            if (quality > 0 && instance.GetProperty("shadingStatus").GetString() == "compiled")
                payloads.Add(instance.GetProperty("shadingFile").GetString()!);
        }
        using var textures = JsonDocument.Parse(File.ReadAllText(Path.Combine(directory, "cell-textures.json")));
        if (textures.RootElement.GetProperty("schemaVersion").GetInt32() != 1 ||
            textures.RootElement.GetProperty("cell").GetString() != GuestRoom ||
            textures.RootElement.GetProperty("layoutDigest").GetString() != root.GetProperty("layoutDigest").GetString())
            throw new InvalidDataException("Prepared texture manifest mismatch.");
        foreach (var texture in textures.RootElement.GetProperty("textures").EnumerateArray())
        {
            if (texture.GetProperty("status").GetString() != "cached")
                throw new InvalidDataException("A prepared texture source could not be resolved.");
            payloads.Add(texture.GetProperty("file").GetString()!);
        }
        foreach (var payload in payloads)
        {
            if (!PayloadName.IsMatch(payload)) throw new InvalidDataException("Unsafe prepared payload path.");
            var path = Path.Combine(directory, payload.Replace('/', Path.DirectorySeparatorChar));
            var hash = Path.GetFileNameWithoutExtension(path);
            using var file = File.OpenRead(path);
            if (!Convert.ToHexString(SHA256.HashData(file)).Equals(hash, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("Prepared payload digest mismatch: " + payload);
        }
        return (root.GetProperty("layoutDigest").GetString()!, payloads.ToArray());
    }

    private static bool CacheValid(string data, string plugins, string destination, int quality)
    {
        try
        {
            using var stamp = JsonDocument.Parse(File.ReadAllText(Path.Combine(destination, "runtime-preparation.json")));
            var root = stamp.RootElement;
            if (root.GetProperty("quality").GetInt32() != quality || root.GetProperty("schemaVersion").GetInt32() != 1)
                return false;
            ReadPayloads(destination, quality);
            return Fingerprint(data, plugins, Path.Combine(destination, "cell-records.json")) ==
                root.GetProperty("sourceFingerprint").GetString();
        }
        catch (Exception) { return false; }
    }

    private static void Deploy(string data, string plugins, string source, string destination, int quality)
    {
        var (_, payloads) = ReadPayloads(source, quality);
        var sourceFingerprint = Fingerprint(data, plugins, Path.Combine(source, "cell-records.json"));
        var staging = destination + ".stage-" + Guid.NewGuid().ToString("N");
        var backup = destination + ".previous-" + Guid.NewGuid().ToString("N");
        try
        {
            Directory.CreateDirectory(staging);
            foreach (var payload in payloads)
            {
                var relative = payload.Replace('/', Path.DirectorySeparatorChar);
                var target = Path.Combine(staging, relative);
                Directory.CreateDirectory(Path.GetDirectoryName(target)!);
                File.Copy(Path.Combine(source, relative), target);
            }
            foreach (var filename in new[] { "cell-layout.json", "cell-textures.json", "cell-records.json" })
                File.Copy(Path.Combine(source, filename), Path.Combine(staging, filename));
            File.WriteAllText(Path.Combine(staging, "runtime-preparation.json"),
                JsonSerializer.Serialize(new { schemaVersion = 1, quality, sourceFingerprint }));
            ReadPayloads(staging, quality);
            if (Directory.Exists(destination)) Directory.Move(destination, backup);
            try { Directory.Move(staging, destination); }
            catch
            {
                if (Directory.Exists(backup)) Directory.Move(backup, destination);
                throw;
            }
        }
        finally
        {
            if (Directory.Exists(staging)) Directory.Delete(staging, true);
            if (Directory.Exists(backup) && Directory.Exists(destination)) Directory.Delete(backup, true);
        }
    }
}

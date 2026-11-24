using System.IO.Compression;
using System.Numerics;
using System.Security.Cryptography;
using System.Text.Json;

// Full Native only. Never constructed by Shadow Offloading or Raster Comparison.
// Cache is source-content-addressed, compressed, bounded and disposable.
internal sealed class FullNativeContext
{
    private const long MaxCacheBytes = 512L * 1024 * 1024;
    private const int MaxEntries = 8192;
    private const int MaxDecodedBytes = 96 * 1024 * 1024;
    private readonly string root;
    private readonly Dictionary<string, uint> pg = new(StringComparer.OrdinalIgnoreCase);
    private readonly DeploymentContext deployment;
    private readonly string? pgMod;
    private readonly JsonSerializerOptions json = new() { IncludeFields = true };
    private long cacheBytes;
    private int cacheEntries;
    private ulong hits, misses, evictions;

    internal CompanionTelemetry Telemetry(int exteriorIndexed, int exteriorTotal, bool exteriorFinished, bool exteriorIncomplete) => new(deployment is MO2ProfileResolver ? "MO2" : "Vortex",
        deployment is MO2ProfileResolver ? Path.GetFileName(Path.GetDirectoryName(deployment.PluginsPath)) ?? "" : "",
        pg.Count, cacheBytes, cacheEntries, hits, misses, evictions, exteriorIndexed, exteriorTotal, exteriorFinished, exteriorIncomplete);

    internal FullNativeContext(string data, DeploymentContext deployment)
    {
        using (var diff = JsonDocument.Parse(File.ReadAllText(Path.Combine(data, "ParallaxGen_Diff.json"))))
            foreach (var item in diff.RootElement.EnumerateObject())
                if (item.Value.TryGetProperty("crc32patched", out var crc) && crc.TryGetUInt32(out var expected))
                    pg[item.Name.Replace('/', '\\')] = expected;

        this.deployment = deployment;
        pgMod = deployment.Winner("ParallaxGen_Diff.json")?.Origin;

        root = Path.Combine(data, "SKSE", "Plugins", "CommunityShaders", "DX12-cache", "full-native-v1");
        try
        {
            Directory.CreateDirectory(root);
            foreach (var file in new DirectoryInfo(root).EnumerateFiles("*.tmp"))
                if (file.LastWriteTimeUtc < DateTime.UtcNow.AddDays(-1)) file.Delete();
            Trim(0, 0);
        }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
    }

    internal (string? Hash, string? Provenance) Verify(string path, string filename, bool loose)
    {
        if (!loose || new FileInfo(filename).Length > 16L * 1024 * 1024) return (null, null);
        var resolved = deployment.Winner(path);
        var winner = resolved?.Origin;
        var tracked = pg.TryGetValue(path, out var expected);
        var generated = path.StartsWith("meshes\\_pgpatcher_dups\\", StringComparison.OrdinalIgnoreCase) &&
            pgMod != null && winner != null && string.Equals(winner, pgMod, StringComparison.OrdinalIgnoreCase);
        if (!tracked && !generated) return (null, null);
        var bytes = File.ReadAllBytes(filename);
        if (tracked && Crc32(bytes) != expected) return (null, null);
        if (!tracked && generated && resolved?.PhysicalPath is { } physical &&
            !DeploymentContext.SameBytes(physical, bytes)) return (null, null);
        return (Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant(),
            (winner ?? "PG:crc-verified")[..Math.Min((winner ?? "PG:crc-verified").Length, 260)]);
    }

    private static uint Crc32(byte[] bytes)
    {
        uint crc = uint.MaxValue;
        foreach (var b in bytes)
        {
            crc ^= b;
            for (int i = 0; i < 8; ++i) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
        return ~crc;
    }

    private string CachePath(string hash) => Path.Combine(root, hash + ".bin");

    internal bool TryRead(string hash, string path, string provenance, out ModelExtraction value)
    {
        value = null!;
        ++misses;
        try
        {
            var file = CachePath(hash);
            if (!File.Exists(file)) return false;
            using var input = File.OpenRead(file);
            using var reader = new BinaryReader(input);
            var count = reader.ReadInt32();
            if (count <= 0 || count > MaxDecodedBytes || input.Length < 36 || input.Length > MaxDecodedBytes) return false;
            var digest = reader.ReadBytes(32);
            using var compressed = new BrotliStream(input, CompressionMode.Decompress);
            var raw = new byte[count];
            compressed.ReadExactly(raw);
            if (compressed.ReadByte() != -1 || !SHA256.HashData(raw).SequenceEqual(digest)) return false;
            using var payload = new BinaryReader(new MemoryStream(raw));
            if (payload.ReadString() != "native-model-v3" || payload.ReadString() != hash) return false;
            var n = payload.ReadInt32();
            if (n < 0 || n > 4096) return false;
            var shapes = new List<ModelShape>(n);
            for (int i = 0; i < n; ++i)
            {
                var block = payload.ReadInt32();
                var vertices = payload.ReadInt32();
                var indices = payload.ReadInt32();
                if (vertices <= 0 || indices <= 0 || indices % 3 != 0 ||
                    (long)vertices * 12 + (long)indices * 4 > raw.Length - payload.BaseStream.Position) return false;
                var transform = new float[16];
                for (int j = 0; j < 16; ++j) transform[j] = payload.ReadSingle();
                var matrix = new Matrix4x4(transform[0],transform[1],transform[2],transform[3],
                    transform[4],transform[5],transform[6],transform[7],transform[8],transform[9],transform[10],transform[11],
                    transform[12],transform[13],transform[14],transform[15]);
                var p = new Vector3[vertices];
                for (int j = 0; j < vertices; ++j) p[j] = new(payload.ReadSingle(),payload.ReadSingle(),payload.ReadSingle());
                var ix = new uint[indices];
                for (int j = 0; j < indices; ++j) ix[j] = payload.ReadUInt32();
                var shadingLength = payload.ReadInt32();
                if (shadingLength < 0 || shadingLength > raw.Length - payload.BaseStream.Position) return false;
                var shading = shadingLength == 0 ? null : payload.ReadBytes(shadingLength);
                var material = JsonSerializer.Deserialize<MaterialSource>(payload.ReadString(), json);
                if (material != null) material = material with { SourceNif = path, SourceHash = hash, Provenance = provenance };
                if (!transform.All(float.IsFinite) || p.Any(v => !float.IsFinite(v.X) || !float.IsFinite(v.Y) || !float.IsFinite(v.Z)) ||
                    ix.Any(index => index >= vertices)) return false;
                shapes.Add(new(block,p,ix,matrix,shading,material));
            }
            var missing = JsonSerializer.Deserialize<MissingModelShape[]>(payload.ReadString(), json);
            var rejections = JsonSerializer.Deserialize<Dictionary<string,uint>>(payload.ReadString(), json);
            if (missing == null || rejections == null || payload.BaseStream.Position != raw.Length) return false;
            value = new(shapes.ToArray(),missing,rejections);
            --misses; ++hits;
            try { File.SetLastWriteTimeUtc(file, DateTime.UtcNow); } catch (IOException) { } catch (UnauthorizedAccessException) { }
            return true;
        }
        catch (Exception error) when (error is not OutOfMemoryException) { return false; }
    }

    internal void Store(string hash, ModelExtraction model)
    {
        string? temp = null;
        try
        {
            if (model.Shapes.Length > 4096) return;
            long estimated = model.Shapes.Sum(s => (long)s.Positions.Length * 12 + (long)s.Indices.Length * 4 + (s.Shading?.Length ?? 0) + 4096);
            if (estimated > MaxDecodedBytes) return;
            using var raw = new MemoryStream();
            using (var writer = new BinaryWriter(raw, System.Text.Encoding.UTF8, true))
            {
                writer.Write("native-model-v3");
                writer.Write(hash);
                writer.Write(model.Shapes.Length);
                foreach (var s in model.Shapes)
                {
                    writer.Write(s.Index); writer.Write(s.Positions.Length); writer.Write(s.Indices.Length);
                    var t = s.ToModel;
                    foreach (var f in new[] {t.M11,t.M12,t.M13,t.M14,t.M21,t.M22,t.M23,t.M24,t.M31,t.M32,t.M33,t.M34,t.M41,t.M42,t.M43,t.M44}) writer.Write(f);
                    foreach (var p in s.Positions) { writer.Write(p.X);writer.Write(p.Y);writer.Write(p.Z); }
                    foreach (var i in s.Indices) writer.Write(i);
                    writer.Write(s.Shading?.Length ?? 0);
                    if (s.Shading != null) writer.Write(s.Shading);
                    writer.Write(JsonSerializer.Serialize(s.Material, json));
                }
                writer.Write(JsonSerializer.Serialize(model.Missing, json));
                writer.Write(JsonSerializer.Serialize(model.Rejections, json));
            }
            if (raw.Length > MaxDecodedBytes) return;
            var bytes = raw.ToArray();
            var destination = CachePath(hash);
            temp = destination + "." + Guid.NewGuid().ToString("N") + ".tmp";
            Directory.CreateDirectory(root);
            using (var file = new FileStream(temp,FileMode.CreateNew,FileAccess.Write,FileShare.None))
            {
                using var writer = new BinaryWriter(file,System.Text.Encoding.UTF8,true);
                writer.Write(bytes.Length); writer.Write(SHA256.HashData(bytes)); writer.Flush();
                using var compressed = new BrotliStream(file,CompressionLevel.Fastest,true);
                compressed.Write(bytes);
            }
            var length = new FileInfo(temp).Length;
            if (cacheBytes + length > MaxCacheBytes || cacheEntries >= MaxEntries) Trim(length,1);
            if (cacheBytes + length > MaxCacheBytes || cacheEntries >= MaxEntries) return;
            var old = File.Exists(destination) ? new FileInfo(destination).Length : 0;
            File.Move(temp,destination,true);
            cacheBytes += length - old;
            if (old == 0) ++cacheEntries;
        }
        catch (Exception error) when (error is not OutOfMemoryException) { }
        finally { if (temp != null) try { File.Delete(temp); } catch (IOException) { } catch (UnauthorizedAccessException) { } }
    }

    private void Trim(long incoming, int entries)
    {
        var files = new DirectoryInfo(root).EnumerateFiles("*.bin").OrderBy(f => f.LastWriteTimeUtc).ToArray();
        cacheBytes = files.Sum(f => f.Length); cacheEntries = files.Length;
        foreach (var f in files)
        {
            if (cacheBytes + incoming <= MaxCacheBytes && cacheEntries + entries <= MaxEntries) break;
            var length = f.Length;
            f.Delete(); cacheBytes -= length; --cacheEntries; ++evictions;
        }
    }
}

using System.Diagnostics;
using System.Text;

internal sealed class MO2ProfileResolver : DeploymentContext
{
    private const string IniName = "ModOrganizer.ini";
    private readonly string mods;
    private readonly string? overwrite;
    private readonly string[] order;

    private MO2ProfileResolver(string data, string profile, string mods, string? overwrite, string? ini,
        string[] full, string[] light)
        : base(Path.Combine(profile, "plugins.txt"),
            ReadOrder(data, Path.Combine(profile, "plugins.txt"), Path.Combine(profile, "loadorder.txt"), full, light),
            new[] { Path.Combine(profile, "modlist.txt"), Path.Combine(profile, "plugins.txt"),
                Path.Combine(profile, "loadorder.txt"), Path.Combine(profile, "archives.txt"),
                Path.Combine(profile, "Skyrim.ini"), Path.Combine(data, "ParallaxGen_Diff.json") }
                .Concat(ini == null ? [] : new[] { ini }))
    {
        this.mods = mods;
        this.overwrite = overwrite;
        if (!Directory.Exists(mods)) throw new InvalidDataException("MO2 mods directory is unavailable");
        order = File.ReadLines(Path.Combine(profile, "modlist.txt"))
            .Where(line => line.StartsWith('+')).Select(line => line[1..].Trim())
            .Where(name => name.Length > 0 && name.IndexOfAny(Path.GetInvalidFileNameChars()) < 0 &&
                !name.Contains('/') && !name.Contains('\\') && name is not "." and not "..").ToArray();
        // Verify the PG manifest's physical winner against the inherited VFS.
        // Matching plugin names alone are insufficient for profile identification.
        var visible = Path.Combine(data, "ParallaxGen_Diff.json");
        var winner = Winner("ParallaxGen_Diff.json");
        if (winner?.PhysicalPath is not { } source || !File.Exists(visible) ||
            new FileInfo(visible).Length > 64L * 1024 * 1024 || !SameBytes(source, File.ReadAllBytes(visible)))
            throw new InvalidDataException("MO2 PG diff winner does not match virtual Data; check USVFS inheritance");
    }

    protected override AssetOrigin? Resolve(string path)
    {
        if (!string.IsNullOrWhiteSpace(overwrite))
        {
            var candidate = Path.Combine(overwrite, path);
            if (File.Exists(candidate)) return new("MO2:overwrite", candidate);
        }
        foreach (var name in order)
        {
            var candidate = Path.Combine(mods, name, path);
            if (File.Exists(candidate)) return new("MO2:" + name, candidate);
        }
        return null;
    }

    internal static MO2ProfileResolver? Discover(string data, string[] full, string[] light, bool mo2Vfs)
    {
        var explicitProfile = Environment.GetEnvironmentVariable("CS_MO2_PROFILE");
        var explicitMods = Environment.GetEnvironmentVariable("CS_MO2_MODS");
        var explicitOverwrite = Environment.GetEnvironmentVariable("CS_MO2_OVERWRITE");
        if (!string.IsNullOrWhiteSpace(explicitProfile) || !string.IsNullOrWhiteSpace(explicitMods) ||
            !string.IsNullOrWhiteSpace(explicitOverwrite))
        {
            if (string.IsNullOrWhiteSpace(explicitProfile) || string.IsNullOrWhiteSpace(explicitMods))
                throw new InvalidDataException("MO2 override requires CS_MO2_PROFILE and CS_MO2_MODS");
            return new(data, Path.GetFullPath(explicitProfile), Path.GetFullPath(explicitMods),
                string.IsNullOrWhiteSpace(explicitOverwrite) ? null : Path.GetFullPath(explicitOverwrite), null, full, light);
        }
        var explicitIni = Environment.GetEnvironmentVariable("CS_MO2_INI");
        if (!string.IsNullOrWhiteSpace(explicitIni))
            return FromIni(data, Path.GetFullPath(explicitIni), full, light);

        var running = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var process in Process.GetProcessesByName("ModOrganizer"))
        {
            using (process)
            {
                try
                {
                    if (process.MainModule?.FileName is { } exe)
                    {
                        var ini = Path.Combine(Path.GetDirectoryName(exe)!, IniName);
                        if (File.Exists(ini)) running.Add(ini);
                    }
                }
                catch (System.ComponentModel.Win32Exception) { }
                catch (InvalidOperationException) { }
            }
        }
        var global = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var globalRoot = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "ModOrganizer");
        if (Directory.Exists(globalRoot))
            foreach (var directory in Directory.EnumerateDirectories(globalRoot).Take(256))
            {
                var ini = Path.Combine(directory, IniName);
                if (File.Exists(ini)) global.Add(ini);
            }
        // Use the host's USVFS module observation, not an unrelated MO2
        // process, to distinguish a running MO2 game from a Vortex deployment.
        var vortex = File.Exists(Path.Combine(data, "vortex.deployment.json"));
        if (!mo2Vfs && vortex) return null;

        var candidates = running.Concat(global).Distinct(StringComparer.OrdinalIgnoreCase).ToArray();
        var verified = new List<MO2ProfileResolver>();
        foreach (var ini in candidates)
        {
            try { verified.Add(FromIni(data, ini, full, light)); }
            catch (Exception error) when (error is IOException or InvalidDataException or UnauthorizedAccessException or
                ArgumentException or NotSupportedException) { }
        }
        if (verified.Count == 1) return verified[0];
        if (verified.Count > 1)
            throw new InvalidDataException("Multiple MO2 instances match runtime plugins and PG diff; set CS_MO2_INI");
        throw new InvalidDataException("No verified MO2 profile found; set CS_MO2_INI or explicit profile directories");
    }

    private static MO2ProfileResolver FromIni(string data, string ini, string[] full, string[] light)
    {
        var values = ReadIni(ini);
        var iniDirectory = Path.GetDirectoryName(ini)!;
        string? Get(string section, string key) => values.GetValueOrDefault(section + "/" + key);
        var game = Get("General", "gamePath");
        if (string.IsNullOrWhiteSpace(game) ||
            !string.Equals(Canonical(game, iniDirectory), Canonical(Path.GetDirectoryName(data)!, iniDirectory), StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("MO2 instance manages a different game path");
        var selected = Get("General", "selected_profile");
        if (string.IsNullOrWhiteSpace(selected) || selected.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0 ||
            selected.Contains('/') || selected.Contains('\\') || selected is "." or "..")
            throw new InvalidDataException("Invalid MO2 selected_profile");
        var baseDir = Canonical(Get("Settings", "base_directory") ?? iniDirectory, iniDirectory);
        string DirectorySetting(string key, string fallback) =>
            Canonical((Get("Settings", key) ?? ("%BASE_DIR%/" + fallback))
                .Replace("%BASE_DIR%", baseDir, StringComparison.OrdinalIgnoreCase), baseDir);
        var profiles = DirectorySetting("profiles_directory", "profiles");
        return new(data, Path.Combine(profiles, selected), DirectorySetting("mod_directory", "mods"),
            DirectorySetting("overwrite_directory", "overwrite"), ini, full, light);
    }

    private static string Canonical(string path, string relativeTo) =>
        Path.TrimEndingDirectorySeparator(Path.GetFullPath(path.Replace('/', '\\'), relativeTo));

    internal static Dictionary<string, string> ReadIni(string ini)
    {
        var result = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        var section = "General";
        foreach (var raw in File.ReadLines(ini))
        {
            var line = raw.Trim();
            if (line.Length == 0 || line.StartsWith(';') || line.StartsWith('#')) continue;
            if (line.StartsWith('[') && line.EndsWith(']')) { section = line[1..^1]; continue; }
            var equal = line.IndexOf('=');
            if (equal <= 0) continue;
            result[section + "/" + line[..equal].Trim()] = DecodeQt(line[(equal + 1)..].Trim());
        }
        return result;
    }

    // QSettings stores QByteArray values (gamePath and selected_profile) using
    // @ByteArray(...) with escaped UTF-8 bytes. Ordinary paths commonly use '/'.
    internal static string DecodeQt(string value)
    {
        if (value.Length >= 2 && value[0] == '"' && value[^1] == '"') value = value[1..^1];
        var bytes = value.StartsWith("@ByteArray(", StringComparison.Ordinal) && value.EndsWith(')');
        if (bytes) value = value[11..^1];
        else if (value.StartsWith("@@", StringComparison.Ordinal)) value = value[1..];
        using var output = new MemoryStream();
        for (int i = 0; i < value.Length; ++i)
        {
            var ch = value[i];
            if (ch == '\\' && i + 1 < value.Length)
            {
                var next = value[i + 1];
                if (next == '\\' || next == '"') { output.WriteByte((byte)next); ++i; continue; }
                if (next == 'x')
                {
                    var start = i + 2;
                    var end = start;
                    while (end < value.Length && end - start < (bytes ? 2 : 4) && Uri.IsHexDigit(value[end])) ++end;
                    if (end > start)
                    {
                        var code = Convert.ToInt32(value[start..end], 16);
                        if (bytes) output.WriteByte((byte)code);
                        else output.Write(Encoding.UTF8.GetBytes(char.ConvertFromUtf32(code)));
                        i = end - 1; continue;
                    }
                }
                // Preserve literal backslashes in hand-written Windows paths.
            }
            var length = char.IsHighSurrogate(ch) && i + 1 < value.Length && char.IsLowSurrogate(value[i + 1]) ? 2 : 1;
            output.Write(Encoding.UTF8.GetBytes(value.Substring(i, length)));
            i += length - 1;
        }
        return Encoding.UTF8.GetString(output.ToArray());
    }
}

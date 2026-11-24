using System.Reflection;
using NiflySharp;
using NiflySharp.Blocks;
using NiflySharp.Enums;

internal sealed record MaterialSource(string SourceNif, string SourceHash, string Provenance,
    int BlockIndex, uint ShaderFlags1, uint ShaderFlags2, float Alpha, bool TwoSided,
    int ClampMode, float[]? UvTransform, string[] Textures, string ShapeName);

internal static class NativeSource
{
    internal static MaterialSource? ReadMaterial(NifFile file, BSTriShape shape,
        string path, string hash, string provenance, int index)
    {
        if (file.GetShader(shape) is not BSLightingShaderProperty shader || !float.IsFinite(shader.Alpha)) return null;
        var textures = file.GetBlock(shader.TextureSetRef)?.Textures?.Select(texture => NormalizeTexture(texture.Content)).ToArray() ?? [];
        if (textures.Length > 16) return null;
        return new(path, hash, provenance, index, (uint)shader.ShaderFlags_SSPF1,
            (uint)shader.ShaderFlags_SSPF2, shader.Alpha,
            shader.ShaderFlags_SSPF2.HasFlag(SkyrimShaderPropertyFlags2.Double_Sided),
            (int?)ReadTextureClampMode(shader) ?? -1, ReadUvTransform(shader), textures, shape.Name?.String ?? "");
    }

    private static string NormalizeTexture(string? value)
    {
        if (string.IsNullOrWhiteSpace(value)) return "";
        var path = value.Replace('/', '\\').ToLowerInvariant();
        if (!path.StartsWith("textures\\")) path = "textures\\" + path;
        if (path.Length > 260 || path.Contains(':') || path.Split('\\').Any(p => p is "" or "." or ".."))
            throw new InvalidDataException("Invalid material texture path");
        return path;
    }

    private static uint? ReadTextureClampMode(BSLightingShaderProperty shader)
    {
        var type = shader.GetType();
        // Nifly 1.1.0 generates the clamp field but has no public accessor.
        var value = type.GetProperty("TextureClampMode")?.GetValue(shader) ??
            type.GetField("_textureClampMode", BindingFlags.Instance | BindingFlags.NonPublic)?.GetValue(shader);
        if (value is not IConvertible number) return null;
        var mode = number.ToInt64(null);
        return mode is >= 0 and <= 3 ? (uint)mode : null;
    }

    private static float[]? ReadUvTransform(BSLightingShaderProperty shader)
    {
        static float? Component(object value, string first, string second)
        {
            var type = value.GetType();
            var member = type.GetProperty(first) ?? type.GetProperty(second);
            var field = type.GetField(first) ?? type.GetField(second);
            var component = member?.GetValue(value) ?? field?.GetValue(value);
            return component is IConvertible number ? Convert.ToSingle(number) : null;
        }
        var type = shader.GetType();
        var offset = (type.GetProperty("UVOffset") ?? type.GetProperty("UvOffset"))?.GetValue(shader);
        var scale = (type.GetProperty("UVScale") ?? type.GetProperty("UvScale"))?.GetValue(shader);
        if (offset == null || scale == null) return null;
        var ox = Component(offset, "X", "U");
        var oy = Component(offset, "Y", "V");
        var sx = Component(scale, "X", "U");
        var sy = Component(scale, "Y", "V");
        return ox.HasValue && oy.HasValue && sx.HasValue && sy.HasValue &&
            float.IsFinite(ox.Value) && float.IsFinite(oy.Value) &&
            float.IsFinite(sx.Value) && float.IsFinite(sy.Value) ?
            [ox.Value, oy.Value, sx.Value, sy.Value] : null;
    }
}

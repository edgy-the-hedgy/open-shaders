using System.Reflection;
using NiflySharp;
using NiflySharp.Blocks;
using NiflySharp.Enums;

internal sealed record MaterialSource(string SourceNif, string SourceHash, string Provenance,
    int BlockIndex, uint ShaderFlags1, uint ShaderFlags2, float Alpha, bool TwoSided,
    int ClampMode, float[]? UvTransform, string[] Textures, string ShapeName, int ShapeOrdinal, bool AlphaTest = false, byte AlphaThreshold = 0, byte AlphaFunction = 0);

internal static class NativeSource
{
    internal static MaterialSource? ReadMaterial(NifFile file, BSTriShape shape,
        string path, string hash, string provenance, int index, int ordinal)
    {
        if (file.GetShader(shape) is not BSLightingShaderProperty shader || !float.IsFinite(shader.Alpha)) return null;
        if (shader.Controller != null && !shader.Controller.IsEmpty() ||
            shader.HasVertexAlpha || shader.HasGreyscaleToPaletteAlpha ||
            (shader.ShaderFlags_SSPF1 & (SkyrimShaderPropertyFlags1.Refraction | SkyrimShaderPropertyFlags1.Temp_Refraction | SkyrimShaderPropertyFlags1.Fire_Refraction)) != 0 ||
            (shader.ShaderFlags_SSPF2 & (SkyrimShaderPropertyFlags2.Tree_Anim | SkyrimShaderPropertyFlags2.LOD_Landscape)) != 0)
            return null;
        var textures = file.GetBlock(shader.TextureSetRef)?.Textures?.Select(texture => NormalizeTexture(texture.Content)).ToArray() ?? [];
        if (textures.Length > 16) return null;
        return new(path, hash, provenance, index, (uint)shader.ShaderFlags_SSPF1,
            (uint)shader.ShaderFlags_SSPF2, shader.Alpha,
            shader.ShaderFlags_SSPF2.HasFlag(SkyrimShaderPropertyFlags2.Double_Sided),
            (int?)ReadTextureClampMode(shader) ?? -1, ReadUvTransform(shader), textures, shape.Name?.String ?? "", ordinal);
    }

    internal static string NormalizeTexture(string? value)
    {
        if (string.IsNullOrWhiteSpace(value)) return "";
        var path = value.Replace('/', '\\').ToLowerInvariant();
        if (path.Length > 1024 || path.Contains(':') || path.Any(char.IsControl) ||
            path.Split('\\').Any(p => p is "" or "." or ".."))
            throw new InvalidDataException("Invalid material texture path");
        while (path.StartsWith("data\\", StringComparison.Ordinal) || path.StartsWith("textures\\", StringComparison.Ordinal))
            path = path[(path.StartsWith("data\\", StringComparison.Ordinal) ? 5 : 9)..];
        if (path is "" or "data" or "textures" || path.Length > 251)
            throw new InvalidDataException("Invalid material texture path");
        return "textures\\" + path;
    }

    // Nifly generates the NiAlphaProperty flags as a bitfield with Value;
    // read public or private generated fields without depending on a specific
    // generated accessor. Missing/unknown metadata is never treated as opaque.
    internal static bool TryReadAlphaTest(NiAlphaProperty property, out byte threshold, out byte function)
    {
        threshold = function = 0;
        static object? Read(object value, string name)
        {
            const BindingFlags flags = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic;
            var type = value.GetType();
            return type.GetProperty(name, flags)?.GetValue(value) ??
                type.GetField("_" + char.ToLowerInvariant(name[0]) + name[1..], flags)?.GetValue(value);
        }
        var flagValue = Read(property, "Flags");
        var raw = flagValue is IConvertible ? flagValue : flagValue == null ? null : Read(flagValue, "Value");
        if (raw is not IConvertible bits || Read(property, "Threshold") is not IConvertible limit) return false;
        var flagsValue = bits.ToUInt16(null);
        threshold = limit.ToByte(null);
        function = (byte)((flagsValue >> 10) & 7);
        return (flagsValue & 1) == 0 && (flagsValue & (1 << 9)) != 0 && function is 4 or 6;
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

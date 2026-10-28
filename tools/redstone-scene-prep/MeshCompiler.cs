using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using NiflySharp;
using NiflySharp.Blocks;
using NiflySharp.Enums;

internal sealed record CompiledMaterial(uint ShaderFlags1, uint ShaderFlags2, string ShaderType, bool TwoSided,
    float Alpha, uint? AlphaFlags, string[] Textures, float[] UvOffsetScale);
internal sealed record CompiledShape(int BlockIndex, string Name, string BlockType, string Status, string? GeometryFile,
    string? GeometrySha256, int VertexCount, int IndexCount, float[]? ShapeToModel, float[]? LocalBounds, CompiledMaterial? Material,
    bool OpaqueOccluderCandidate = false);
internal sealed record CompiledNif(string SourcePath, string SourceSha256, string CompilerKey, string Status, bool CacheHit,
    int EmbeddedLights, CompiledShape[] Shapes);

internal sealed class MeshCompiler
{
    private readonly string output;
    private readonly JsonSerializerOptions options;
    private readonly string compilerKey;

    public MeshCompiler(string outputDirectory, JsonSerializerOptions jsonOptions)
    {
        output = outputDirectory;
        options = jsonOptions;
        var executable = File.ReadAllBytes(typeof(MeshCompiler).Assembly.Location);
        compilerKey = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes("rigid-opaque-v1/Nifly-1.1.0/" + Convert.ToHexString(SHA256.HashData(executable))))).ToLowerInvariant();
    }

    public CompiledNif Compile(PgAsset asset)
    {
        if (asset.CachedSource == null || asset.Sha256 == null) return new(asset.Path, "", compilerKey, asset.Status, false, 0, []);
        var cacheFile = Path.Combine(output, "compiled", compilerKey, asset.Sha256 + ".json");
        if (File.Exists(cacheFile))
        {
            try
            {
                var cached = JsonSerializer.Deserialize<CompiledNif>(File.ReadAllText(cacheFile), options);
                if (cached != null && cached.CompilerKey == compilerKey && cached.SourceSha256 == asset.Sha256 && cached.Shapes != null &&
                    cached.Shapes.All(shape => shape != null && (shape.GeometryFile == null ? shape.Status != "compiled" && !shape.Status.StartsWith("preview-only:", StringComparison.Ordinal) : VerifyGeometry(shape))))
                    return cached with { SourcePath = asset.Path, CacheHit = true };
            }
            catch (JsonException) { }
        }
        CompiledNif result;
        try
        {
            var source = Path.Combine(output, asset.CachedSource);
            if (!Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(source))).Equals(asset.Sha256, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("Cached NIF source hash mismatch.");
            var file = new NifFile();
            if (file.Load(source) != 0 || !file.Valid) throw new InvalidDataException("NIF reader rejected file.");
            if (!file.Header.Version.IsSSE()) throw new InvalidDataException($"Unsupported NIF version: {file.Header.Version.VersionString}, user {file.Header.Version.UserVersion}, stream {file.Header.Version.StreamVersion}.");
            if (file.HasUnknownBlocks) throw new InvalidDataException("Unknown NIF blocks prevent classification.");
            var transforms = new NifTransforms(file);
            var shapes = file.GetShapes().Select(shape => CompileShape(file, shape, transforms)).ToArray();
            result = new(asset.Path, asset.Sha256, compilerKey, "classified", false, file.Blocks.OfType<NiLight>().Count(), shapes);
        }
        catch (Exception error) when (error is not OutOfMemoryException)
        {
            result = new(asset.Path, asset.Sha256, compilerKey, "excluded:" + error.Message, false, 0, []);
        }
        Directory.CreateDirectory(Path.GetDirectoryName(cacheFile)!);
        File.WriteAllText(cacheFile + ".tmp", JsonSerializer.Serialize(result, options));
        File.Move(cacheFile + ".tmp", cacheFile, true);
        return result;
    }

    private bool VerifyGeometry(CompiledShape shape)
    {
        if (shape.GeometrySha256 == null || shape.GeometrySha256.Length != 64 || !shape.GeometrySha256.All(Uri.IsHexDigit) ||
            shape.GeometryFile != $"geometry/{shape.GeometrySha256}.rsgm") return false;
        var path = Path.Combine(output, shape.GeometryFile);
        return File.Exists(path) && Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(path))).Equals(shape.GeometrySha256, StringComparison.OrdinalIgnoreCase);
    }

    private CompiledShape CompileShape(NifFile file, INiShape shape, NifTransforms transforms)
    {
        file.GetBlockIndex(shape, out var index);
        var name = shape.Name?.String ?? "";
        var type = shape.GetType().Name;
        var transform = transforms.ForShape(index);
        var shader = file.GetShader(shape) as BSLightingShaderProperty;
        var alpha = file.GetBlock(shape.AlphaPropertyRef);
        CompiledMaterial? material = null;
        if (shader != null)
        {
            var textures = file.GetBlock(shader.TextureSetRef)?.Textures?.Select(texture => texture.Content).ToArray() ?? [];
            material = new((uint)shader.ShaderFlags_SSPF1, (uint)shader.ShaderFlags_SSPF2, shader.ShaderType_SK_FO4.ToString(),
                shader.ShaderFlags_SSPF2.HasFlag(SkyrimShaderPropertyFlags2.Double_Sided), shader.Alpha,
                alpha == null ? null : alpha.Flags.Value, textures, [shader.UVOffset.U, shader.UVOffset.V, shader.UVScale.U, shader.UVScale.V]);
        }
        var reason = shape is not BSTriShape || type is not ("BSTriShape" or "BSSubIndexTriShape") ? "unsupported-shape-type" :
            shape.HasSkinInstance || shape.IsSkinned ? "skinned-shape" : transform.Exclusion;
        if (reason == null && shader == null) reason = "non-lighting-material";
        if (reason == null && shader!.Controller != null && !shader.Controller.IsEmpty()) reason = "controlled-material";
        if (reason == null && (alpha?.Flags.AlphaBlend == true || alpha?.Flags.AlphaTest == true)) reason = "alpha-test-or-blend";
        if (reason == null && (!float.IsFinite(shader!.Alpha) || shader.Alpha != 1)) reason = "non-opaque-material";
        if (reason == null && (shader!.HasVertexAlpha || shader.HasGreyscaleToPaletteAlpha)) reason = "vertex-or-palette-alpha";
        if (reason == null && (shader!.ShaderFlags_SSPF1 & (SkyrimShaderPropertyFlags1.Refraction | SkyrimShaderPropertyFlags1.Temp_Refraction | SkyrimShaderPropertyFlags1.Fire_Refraction)) != 0)
            reason = "refractive-material";
        if (reason == null && (shader!.ShaderFlags_SSPF2 & (SkyrimShaderPropertyFlags2.Tree_Anim | SkyrimShaderPropertyFlags2.LOD_Landscape)) != 0)
            reason = "deformed-or-landscape-material";
        if (reason == null && (!shader!.ShaderFlags_SSPF1.HasFlag(SkyrimShaderPropertyFlags1.Cast_Shadows) || !shader.ShaderFlags_SSPF2.HasFlag(SkyrimShaderPropertyFlags2.ZBuffer_Write)))
            reason = "shadow-or-depth-flags";
        var materialController = shader == null ? null : file.GetBlock(shader.Controller);
        var previewOnly = reason == "alpha-test-or-blend" ||
            (reason == "vertex-or-palette-alpha" && !shader!.HasGreyscaleToPaletteAlpha) ||
            (reason == "controlled-material" && materialController is BSLightingShaderPropertyColorController or BSLightingShaderPropertyFloatController);
        if (reason != null && !previewOnly) return new(index, name, type, "excluded:" + reason, null, null, 0, 0, transform.Matrix, null, material);
        try
        {
            var vertices = ((BSTriShape)shape).VertexPositions;
            var triangles = shape.Triangles;
            var blob = GeometryBlob.Write(output, vertices, triangles);
            return new(index, name, type, previewOnly ? "preview-only:" + reason : "compiled", blob.Path, blob.Sha256,
                vertices.Count, checked(triangles.Count * 3), transform.Matrix, blob.Bounds, material, !previewOnly);
        }
        catch (InvalidDataException error)
        {
            return new(index, name, type, "excluded:" + error.Message, null, null, 0, 0, transform.Matrix, null, material);
        }
    }
}

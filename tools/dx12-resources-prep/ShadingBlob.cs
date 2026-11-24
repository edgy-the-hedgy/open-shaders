using System.Numerics;
using NiflySharp.Blocks;

internal static class ShadingBlob
{
    private const uint Magic = 0x41475352;
    private const uint LegacyStride = 36;
    private const uint TangentStride = 60;

    public static byte[]? Build(BSTriShape shape)
    {
        if (!shape.HasNormals) return null;
        if (!shape.HasUVs) return null;
        var count = shape.VertexPositions.Count;
        var normals = shape.Normals;
        var uvs = shape.UVs;
        var colors = shape.HasVertexColors ? shape.VertexColors : null;
        if (count == 0 || normals == null || normals.Count != count) return null;
        if (uvs == null || uvs.Count != count) return null;
        if (shape.HasVertexColors && (colors == null || colors.Count != count)) return null;
        var tangents = shape.HasTangents ? shape.Tangents : null;
        var bitangents = shape.HasTangents ? shape.Bitangents : null;
        var tangentStatus = !shape.HasTangents ? "absent:source-tangents" :
            tangents == null || bitangents == null || tangents.Count != count || bitangents.Count != count ? "invalid:tangent-count" : "compiled:source-basis";
        if (tangentStatus == "compiled:source-basis")
            for (var index = 0; index < count; ++index)
            {
                var normal = normals[index];
                var n = new Vector3(normal.X, normal.Y, normal.Z);
                var tangent = tangents![index];
                var bitangent = bitangents![index];
                var determinant = Vector3.Dot(Vector3.Cross(n, tangent), bitangent);
                if (!Finite(tangent) || !Finite(bitangent) || !float.IsFinite(determinant) || determinant == 0)
                {
                    tangentStatus = "invalid:source-tangent-basis";
                    break;
                }
            }
        var hasTangents = tangentStatus == "compiled:source-basis";
        using var stream = new MemoryStream();
        using var writer = new BinaryWriter(stream);
        writer.Write(Magic);
        writer.Write(hasTangents ? 2u : 1u);
        writer.Write(checked((uint)count));
        writer.Write(hasTangents ? TangentStride : LegacyStride);
        for (var index = 0; index < count; ++index)
        {
            var normal = normals[index];
            var uv = uvs[index];
            var color = colors == null ? new NiflySharp.Structs.Color4 { R = 1, G = 1, B = 1, A = 1 } : colors[index];
            float[] values = [normal.X, normal.Y, normal.Z, uv.U, uv.V, color.R, color.G, color.B, color.A];
            if (!values.All(float.IsFinite)) return null;
            if (normal.X == 0 && normal.Y == 0 && normal.Z == 0) return null;
            foreach (var value in values) writer.Write(value);
            if (hasTangents)
            {
                var tangent = tangents![index];
                var bitangent = bitangents![index];
                foreach (var value in new[] { tangent.X, tangent.Y, tangent.Z, bitangent.X, bitangent.Y, bitangent.Z }) writer.Write(value);
            }
        }
        writer.Flush();
        return stream.ToArray();
    }

    private static bool Finite(Vector3 value) => float.IsFinite(value.X) && float.IsFinite(value.Y) && float.IsFinite(value.Z);
}

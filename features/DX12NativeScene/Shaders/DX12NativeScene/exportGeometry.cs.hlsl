Buffer<uint> Vertices : register(t0);
Buffer<uint> Indices : register(t1);
RWTexture2D<uint> Export : register(u0);
cbuffer Layout : register(b0)
{
	uint VertexStrideWords;
	uint VertexCount;
	uint2 Padding;
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	uint width, height, vertexWords, indexCount;
	Export.GetDimensions(width, height);
	if (id.x >= width || id.y >= height) return;
	Vertices.GetDimensions(vertexWords);
	Indices.GetDimensions(indexCount);
	uint index = id.y * width + id.x;
	uint validationIndex = vertexWords + indexCount;
	if (index == validationIndex) return;
	uint value = 0;
	bool invalid = false;
	if (index < vertexWords) {
		value = Vertices[index];
		if (index % VertexStrideWords < 3 && !isfinite(asfloat(value))) {
			value = 0;
			invalid = true;
		}
	} else if (index - vertexWords < indexCount) {
		value = Indices[index - vertexWords];
		if (value >= VertexCount) {
			value = 0;
			invalid = true;
		}
	}
	Export[id.xy] = value;
	if (invalid) InterlockedOr(Export[uint2(validationIndex % width, validationIndex / width)], 1);
}

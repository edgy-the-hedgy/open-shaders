Texture2D<float4> rrOutput : register(t0);
Texture2D<float4> rrDiffuseOutput : register(t1);
Texture2D<float> rrDiffuseOcclusion : register(t2);
Texture2D<float> rrSpecularOcclusion : register(t3);
Texture2D<float2> resolvedOcclusion : register(t4);
RWTexture2D<float4> preview : register(u0);

cbuffer PreviewSettings : register(b0)
{
	uint2 inputSize;
	float exposureEV;
	uint previewView;
	float specularScale;
	float diffuseScale;
	uint checkerboard;
	uint checkerboardOrigin;
	uint signalMask;
	uint rawOutputs;
	uint2 padding;
};

[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
	if (any(pixel >= inputSize)) return;
	uint view = previewView;
	uint2 sourcePixel = pixel;
	if (view == 6) {
		uint2 split = inputSize / 2u;
		uint2 panel = uint2(pixel.x >= split.x, pixel.y >= split.y);
		uint2 start = panel * split;
		uint2 extent = uint2(panel.x ? inputSize.x - split.x : split.x, panel.y ? inputSize.y - split.y : split.y);
		sourcePixel = min(inputSize - 1u, (pixel - start) * inputSize / max(extent, 1u));
		view = panel.y ? (panel.x ? 4u : 3u) : (panel.x ? 2u : 1u);
	}
	if (previewView == 6 && checkerboard && ((pixel.x + pixel.y) & 1u) == checkerboardOrigin)
		sourcePixel.x = min(inputSize.x - 1u, (sourcePixel.x & ~1u) | ((sourcePixel.y ^ checkerboardOrigin) & 1u));
	uint requiredBit = view == 1 ? 1u : view == 2 ? 2u : view == 3 ? 4u : view == 4 ? 8u : 0u;
	if ((requiredBit && !(signalMask & requiredBit)) || (checkerboard && (((previewView == 6 ? pixel.x + pixel.y : sourcePixel.x + sourcePixel.y) & 1u) != checkerboardOrigin))) {
		preview[pixel] = float4(0, 0, 0, 1);
		return;
	}
	uint2 signalPixel = uint2(checkerboard ? sourcePixel.x >> 1u : sourcePixel.x, sourcePixel.y);
	if (view >= 3 && view <= 5) {
		float2 visibility = 0;
		if (rawOutputs) {
			visibility = resolvedOcclusion[sourcePixel];
		} else {
			if (signalMask & 4u) visibility.x = rrDiffuseOcclusion[signalPixel];
			if (signalMask & 8u) visibility.y = rrSpecularOcclusion[signalPixel];
		}
		visibility = float2((signalMask & 4u) ? visibility.x : 0, (signalMask & 8u) ? visibility.y : 0);
		visibility = all(isfinite(visibility)) ? saturate(visibility) : 0;
		float3 display = view == 3 ? visibility.xxx : view == 4 ? visibility.yyy : float3(visibility, 0);
		preview[pixel] = float4(display, 1);
		return;
	}
	float3 radiance = 0;
	if (view != 1 && (signalMask & 2u)) radiance += rrOutput.Load(int3(signalPixel, 0)).rgb * specularScale;
	if (view != 2 && (signalMask & 1u)) radiance += rrDiffuseOutput.Load(int3(signalPixel, 0)).rgb * diffuseScale;
	radiance = all(isfinite(radiance)) ? max(radiance, 0.0) * exp2(exposureEV) : 0.0;
	float3 mapped = radiance / (1.0 + radiance);
	float3 display = float3(
		mapped.r <= 0.0031308 ? mapped.r * 12.92 : 1.055 * pow(mapped.r, 1.0 / 2.4) - 0.055,
		mapped.g <= 0.0031308 ? mapped.g * 12.92 : 1.055 * pow(mapped.g, 1.0 / 2.4) - 0.055,
		mapped.b <= 0.0031308 ? mapped.b * 12.92 : 1.055 * pow(mapped.b, 1.0 / 2.4) - 0.055);
	preview[pixel] = float4(saturate(display), 1.0);
}

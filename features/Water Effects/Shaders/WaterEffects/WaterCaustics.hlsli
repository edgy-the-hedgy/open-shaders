Texture2D<float4> WaterCaustics : register(t65);

namespace WaterEffects
{
<<<<<<< HEAD
	static const float CausticsUVScale = 0.005;
	static const float CausticsDispersionScale = 0.025;

||||||| 12b261724
=======
	/** @brief Panned caustics UV, unwrapped; wrap it with WrapCausticsUV before sampling. */
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
	float2 PanCausticsUV(float2 uv, float speed, float tiling)
	{
<<<<<<< HEAD
		return frac((float2(1, 0) * SharedData::Timer * speed * SharedData::csUtilitySettings.waterCausticsSpeed) + (uv * tiling));
||||||| 12b261724
		return frac((float2(1, 0) * SharedData::Timer * speed) + (uv * tiling));
=======
		return (float2(1, 0) * SharedData::Timer * speed) + (uv * tiling);
	}

	/**
	 * @brief frac() with the integer part taken from the quad's top-left pixel.
	 * The UV stays continuous inside each 2x2 quad, so implicit derivatives see no wrap seam.
	 */
	float4 WrapCausticsUV(float4 uv, float2 quadPixel)
	{
		return uv - Math::QuadReadTopLeft(floor(uv), quadPixel);
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
	}

	float SampleCaustics(float2 uv, float2 uvDdx, float2 uvDdy)
	{
<<<<<<< HEAD
		return WaterCaustics.SampleGrad(SampColorSampler, uv, uvDdx, uvDdy).x;
||||||| 12b261724
		return WaterCaustics.Sample(SampColorSampler, uv).x;
=======
		return WaterCaustics.SampleBias(SampColorSampler, uv, SharedData::MipBias).x;
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
	}

	// Approximate wavelength-dependent refraction by offsetting red/blue around green.
	float3 SampleCausticsDispersion(float2 uv, float2 dispersionOffset, float2 uvDdx, float2 uvDdy, float2 dispersionDdx, float2 dispersionDdy)
	{
		float center = SampleCaustics(uv, uvDdx, uvDdy);
		float3 dispersed = float3(
			SampleCaustics(uv - dispersionOffset * 0.75, uvDdx - dispersionDdx * 0.75, uvDdy - dispersionDdy * 0.75),
			center,
			SampleCaustics(uv + dispersionOffset, uvDdx + dispersionDdx, uvDdy + dispersionDdy));
		return lerp(center.xxx, dispersed, 0.5);
	}

<<<<<<< HEAD
	float3 ComputeCaustics(float4 waterData, float3 worldPosition, uint eyeIndex)
||||||| 12b261724
	float3 ComputeCaustics(float4 waterData, float3 worldPosition)
=======
	/** @brief Caustics multiplier for the directional light. @param pixelPos SV_Position.xy. */
	float3 ComputeCaustics(float4 waterData, float3 worldPosition, float2 pixelPos)
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
	{
		float3 result = 1.0.xxx;
		float causticsDistToWater = waterData.w - worldPosition.z;
		float shoreFactorCaustics = saturate(causticsDistToWater / 64.0);

<<<<<<< HEAD
		// Gradients come from the unwrapped UV and are taken outside the branch: PanCausticsUV's frac() would put a
		// mip seam at every wrap, and screen derivatives are undefined in divergent control flow.
		float2 causticsUV = (worldPosition.xy + FrameBuffer::CameraPosAdjust[eyeIndex].xy) * (CausticsUVScale * SharedData::csUtilitySettings.waterCausticsTiling);
		float dispersionAmount = CausticsDispersionScale * shoreFactorCaustics * saturate(causticsDistToWater / 256.0) * SharedData::csUtilitySettings.waterCausticsDispersion;
		float2 dispersionOffset = float2(0.6, 0.8) * dispersionAmount;
		float2 causticsUVDdx = ddx(causticsUV);
		float2 causticsUVDdy = ddy(causticsUV);
		float2 dispersionDdx = float2(0.6, 0.8) * ddx(dispersionAmount);
		float2 dispersionDdy = float2(0.6, 0.8) * ddy(dispersionAmount);

		if (shoreFactorCaustics > 0.0 && SharedData::csUtilitySettings.waterCausticsStrength > 0.0) {
||||||| 12b261724
		if (shoreFactorCaustics > 0.0) {
=======
		// Wrapped before the branch, since QuadReadTopLeft needs the whole quad active.
		float2 quadPixel = Math::QuadPixel(pixelPos);
		float2 causticsUV = (worldPosition.xy + FrameBuffer::CameraPosAdjust.xy) * 0.005;
		float4 causticsHighUV = WrapCausticsUV(float4(PanCausticsUV(causticsUV, 0.5 * 0.2, 1.0), PanCausticsUV(causticsUV, 1.0 * 0.2, -0.5)), quadPixel);
		causticsUV *= 0.5;
		float4 causticsLowUV = WrapCausticsUV(float4(PanCausticsUV(causticsUV, 0.5 * 0.1, 1.0), PanCausticsUV(causticsUV, 1.0 * 0.1, -0.5)), quadPixel);

		// Quads that diverge here sit at the shoreline, where shoreFactorCaustics is near 0 and hides the samples.
		if (shoreFactorCaustics > 0.0) {
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
			float causticsFade = 1.0 - saturate(causticsDistToWater / 1024.0);
			causticsFade *= causticsFade;

<<<<<<< HEAD
			float2 causticsUV1 = PanCausticsUV(causticsUV, 0.5 * 0.2, 1.0);
			float2 causticsUV2 = PanCausticsUV(causticsUV, 1.0 * 0.2, -0.5);

||||||| 12b261724
			float2 causticsUV = (worldPosition.xy + FrameBuffer::CameraPosAdjust.xy) * 0.005;
			float2 dispersionOffset = float2(0.6, 0.8) * (0.025 * shoreFactorCaustics * saturate(causticsDistToWater / 256.0));

			float2 causticsUV1 = PanCausticsUV(causticsUV, 0.5 * 0.2, 1.0);
			float2 causticsUV2 = PanCausticsUV(causticsUV, 1.0 * 0.2, -0.5);

=======
			float2 dispersionOffset = float2(0.6, 0.8) * (0.025 * shoreFactorCaustics * saturate(causticsDistToWater / 256.0));

>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
			const float3 causticsHigh =
<<<<<<< HEAD
				(causticsFade > 0.0) ? (min(SampleCausticsDispersion(causticsUV1, dispersionOffset, causticsUVDdx, causticsUVDdy, dispersionDdx, dispersionDdy), SampleCausticsDispersion(causticsUV2, dispersionOffset, causticsUVDdx * -0.5, causticsUVDdy * -0.5, dispersionDdx, dispersionDdy)) * 4.0) : 1.0.xxx;
||||||| 12b261724
				(causticsFade > 0.0)
					? (min(SampleCausticsDispersion(causticsUV1, dispersionOffset), SampleCausticsDispersion(causticsUV2, dispersionOffset)) * 4.0)
					: 1.0.xxx;
=======
				(causticsFade > 0.0) ? (min(SampleCausticsDispersion(causticsHighUV.xy, dispersionOffset), SampleCausticsDispersion(causticsHighUV.zw, dispersionOffset)) * 4.0) : 1.0.xxx;
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb

			dispersionOffset *= 0.5;
			causticsUVDdx *= 0.5;
			causticsUVDdy *= 0.5;
			dispersionDdx *= 0.5;
			dispersionDdy *= 0.5;

			const float3 causticsLow =
<<<<<<< HEAD
				(causticsFade < 1.0) ? (min(SampleCausticsDispersion(causticsUV1, dispersionOffset, causticsUVDdx, causticsUVDdy, dispersionDdx, dispersionDdy), SampleCausticsDispersion(causticsUV2, dispersionOffset, causticsUVDdx * -0.5, causticsUVDdy * -0.5, dispersionDdx, dispersionDdy)) * 4.0) : 1.0.xxx;
||||||| 12b261724
				(causticsFade < 1.0)
					? (min(SampleCausticsDispersion(causticsUV1, dispersionOffset), SampleCausticsDispersion(causticsUV2, dispersionOffset)) * 4.0)
					: 1.0.xxx;
=======
				(causticsFade < 1.0) ? (min(SampleCausticsDispersion(causticsLowUV.xy, dispersionOffset), SampleCausticsDispersion(causticsLowUV.zw, dispersionOffset)) * 4.0) : 1.0.xxx;
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb

			const float3 caustics = lerp(causticsLow, causticsHigh, causticsFade);
			result = max(0.0, lerp(1.0.xxx, caustics, shoreFactorCaustics * SharedData::csUtilitySettings.waterCausticsStrength));
		}

		return result;
	}
}

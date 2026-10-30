#pragma once

namespace DevBenchBridge::ToolSpecs
{
	/** Devbench descriptor JSON for openshaders.feature. {brand} is expanded at registration. */
	inline constexpr const char* kFeatureTool =
		R"({"description":")"
		R"(All {brand} graphics-feature operations — enumerate, inspect settings, mutate settings, restore defaults, toggle on/off, read live diagnostics, )"
		R"(read/write runtime-only debug flags. Action-dispatched. list: returns an array of {name,shortName,loaded,version,category,isCore,supportsVR,inMenu,favorite,enabledAtBoot}; )"
		R"(features with restart-gated settings also include restartFields:[{key,label,pending}]. get: params shortName, returns the )"
		R"(SaveSettings blob (null if the feature has no override; set/reset then no-op). Anti-Lag 2 is automatic for a supported FSR Frame Generation session, with no independent setting or FPS cap. )"
		R"(RedstoneGI.Selected selects RGI at runtime in eligible FSR-FG sessions; false selects an available native provider. Read actual state with the RedstoneGI redstoneStatus query. RedstoneGI.OccluderDepth controls assumed occluder depth for all four Redstone signals (1..300 world units, default 300), independently of original SSGI. )"
		R"(Prepared RR now has opaque, exposure-adjusted previews and separate full-screen inputs/outputs, including direct diffuse/specular/combined, indirect diffuse/specular/combined and primary validity. setPreparedRRDebug requires source off|inputs|outputs; optional signal 0..6, buffer 0..15, exposure -8..8 EV. Debug selection is unsaved and exclusive with raymarched full-screen RR debug. The default-open Prepared RR buffers panel labels logical RR grid, physical packed storage and context capacity distinctly. Alpha in raw RR signals is data, never preview opacity. This is diagnostic presentation, not evidence of nonzero valid radiance. )"
		R"(The RR suite is consolidated in Upscaling > FSR-RayRegeneration: reconstruction controls, prepared-lighting enable/buffer budget, shared SDK tuning, context diagnostics, input/output/guide buffer viewers, and raymarched full-screen debug/exposure controls. SDK overrides apply across active direct, DXR-indirect and raymarched contexts and persist under Upscaling.FSRRayRegeneration.configuration; old RedstoneGI.RRConfiguration migrates even when the reconstruction block already exists. GI settings dynamically show exclusive producer selection plus DXR rays/intensity or the selected raymarched estimator controls; inactive controls are not displayed. Existing RedstoneGI/Upscaling compatibility configuration commands update the shared RR overrides. This UI/configuration change does not require geometry cache regeneration. )"
		R"(Indirect producers are exclusive: RedstoneShadows.setIndirectProducer requires producer raymarched or dxr; optional rays integer 1..8 sets normalized one-bounce DXR samples per native pixel. DXR claims the indirect provider without raymarch fallback when unavailable. Cached roomSchemaVersion 1 links/volumes resolve to current native room identities; legacy caches require regeneration for DXR indirect. Upscaling.setRayRegeneration controls shared resolutionMode 0..2, checkerboard and enabled booleans, saved under Upscaling.FSRRayRegeneration and displayed in Upscaling > FSR-RayRegeneration. Raymarcher settings no longer persist reconstruction resolution/checkerboarding. Direct and selected indirect signals use separate typed RR contexts; DXR tracing remains native resolution and is independent of RR packing resolution/checkerboard. Unknown secondary room/material data is unavailable, never converted to unrestricted lighting. )"
		R"(RedstoneShadows connects existing selected RedstoneGI output with ConnectIndirectLighting (default true), IndirectStrength (0..8, default 1) and UseDirectRayRegeneration (default true). setPreparedDirectLighting accepts optional indirect/rr booleans. Direct RR uses genuine separated RT direct diffuse/specular, matching RT material guides and current native motion/depth, then composes its successful output with emission and current RGI indirect lighting. RR budget/provider failure retains raw direct output. setLightingSampling accepts optional diffuseSlices/specularSlices integers 1..10 for existing RGI screen-space work; each slice traces both directions, normalized independently of brightness. Normal Save persists sampling in RedstoneGI. DXR indirect is selected separately with setIndirectProducer. shadowStatus directRRDispatches/directRRFrame/directRRBudgetDeferrals and indirectCompositeSubmissions distinguish historical submissions from runtime acceptance. )"
		R"(RedstoneShadows exposes live DirectStrength, DiffuseStrength, SpecularStrength and EmissionStrength, each finite 0..8, default 1. The first scales direct diffuse/specular; emission is independent. Source-material and visibility views are unaffected. )"
		R"(RedstoneShadows UseAllPreparedLights defaults false. setPreparedDirectLighting and setPreparedMaterials accept optional allLights boolean. True accumulates the complete prepared light set for direct views/scene output, bounded to 1..32 lights; visibility retains the baseline selector. Each light uses exact live identity, current parameters, room admission and geometric visibility. Material evaluation and emission occur once per receiver. Missing/unsupported/ambiguous requested lights reject the pixel; excess/empty sets report lightSelectionFailure without truncation. shadowStatus reports allPreparedLights, requestedLightCount and maximumDirectLights as submitted-input evidence, not accepted GPU coverage. )"
		R"(Prepared RT light selection now requires exact resolved placed-reference and base IDs; no spatial tolerance. PortalStrict is admitted per native opaque receiver room mask from the same frame/table generation. Unknown metadata fails closed. shadowStatus sourceReference/sourceBase/sourceRoomGeneration/sourceRoomMetadata are submitted-input evidence; GPU admission outcomes remain diagnostic colors: cyan partial coverage without code, pale cyan partial coverage with code, dark cyan zero coverage/code, green zero code at full coverage, lime invalid code, royal blue stale generation, gold missing generation, teal no target. Opaque draw coverage is independent of material alpha; blended draws remain conservative. The optional extended opaque target preserves vertex-AO/category channels, supports runtime switching, and retains the original VR format. )"
		R"(RedstoneShadows exposes AcquireScene (boolean, default false), MaxInstances (1..256) and BufferBudgetMB (16..512). NodesPerFrame is retained only for old settings compatibility and no longer controls a scene walk. Opt-in acquisition samples one rotating SCM local-light slot only when it naturally accumulates, then prepares opaque BLAS/TLAS asynchronously. Native shadow production remains active. Runtime flag CaptureDebug enables a temporary flat-screen overlay of copied caster bounds, light slot/lifetime, capture age, omissions and live copy/export/AS progress, export budget/deferral reasons, discarded batches, retained worker counter samples and last completed TLAS identity, plus selection/capture/callback counts, first-failure admission reasons and epoch discards; default false, not saved. SSE rigid positions use float XYZ regardless of FULLPREC; admission validates a 16-byte position prefix and following attribute offsets. Counters include admittedWithoutFullPrecisionFlag and unsupportedPositionLayout. Bounds are partial diagnostic snapshots, not triangle or visibility proof. Runtime flags TraceDirectional and PreviewDirectional are always false and reject enabling because this local-light scope cannot supply directional coverage. shadowStatus reports source revisions, queue/worker progress, partial coverage and costs. SCM observation does not trace; prepared-mode trace counters are independent. RR, production lighting consumption and CPU bypass remain false. )"
		R"(RedstoneShadows also exposes UsePreparedScene and PreviewPreparedScene (booleans, default false) and PreparedLight (0..255 baseline light index). Prepared mode replaces SCM acquisition as geometry input, reads the current interior's prepared package once on a worker, and builds all supplied mesh instances including material-masked geometry. Optional local-light previews trace after opaque world lighting and before first-person camera rebasing, at most once every eight rendered frames. Preview rejection colors distinguish missing receivers, missed geometry, unsupported materials, missing lights and depth mismatch. Native lighting and GI remain authoritative. shadowStatus reports prepared loading, AS, trace and preview counters; no runtime coverage or production consumption is implied. )"
		R"(RedstoneShadows additionally exposes UsePreparedMaterials, PreparedMaterialView (0..12), MaterialTextureBudgetMB (64..1024), UsePreparedDirectLighting (default false), and DirectLightingBudgetMB (64..1024, default 256). Views 10/11/12 are selected-light direct diffuse/specular/combined plus emission previews. Direct scene mode replaces supported depth-matched opaque pixels with current-frame selected-point-light radiance before forward transparency. Replaced pixels omit unlisted direct lights; current selected RedstoneGI indirect output is connected by default; unsupported pixels keep native results. This opt-in consumer supersedes the preview-only description when enabled; native CPU shadow work remains active; direct RR uses an independent context alongside RGI indirect RR. )"
		R"(Upscaling exposes enableDLSSFrameGen (boolean, default false); )"
		R"(existing configurations must opt in explicitly and restart to use NVIDIA DLSS-G. PostProcessing includes cinematic_camera settings for the optional linked physical camera; )"
		R"(use get/set with shortName PostProcessing to inspect or change them without saving. LinearLighting uses Jiaye's per-channel diffuse calibration with fixed gamma 2.2, reflectance 0.5 at encoded 0.5, )"
		R"(and white reflectance 1.0. These calibration values are not settings. PBR texture inputs bypass calibration. )"
		R"(Scene storage gamma is fixed at 1.6. conversionSaturation (0-1, default 0.92) compensates gamma-authored inputs entering linear lighting, including diffuse, tint, glow, lights, effects, sky, fog, water and cached weather colors. It preserves linear-sRGB luminance and neutral colors; )"
		R"(1 is unchanged. Already-linear material and light inputs bypass it; converted shared lighting still affects PBR surfaces. )"
		R"(Scene storage and gamma round trips do not apply compensation again. ambientMult (0-5, default 0.32) scales ambient lighting. CSUtility atmosphere settings include independent sky/cloud brightness, )"
		R"(saturation and gamma, vanilla fog intensity (opacity multiplier), volumetric lighting intensity and sun glare intensity (0 off, 1 unchanged), useAmbientEffectLighting (boolean, default false; )"
		R"(replaces weather Effect Lighting and )"
		R"(Sky )"
		R"(Statics lighting with ambient/IBL plus shadowed directional light), effect brightness and sky static brightness (0-2, scale their respective weather or ambient/directional lighting; )"
		R"(1 unchanged at the center) and sky static transparency (0 unchanged, 1 fully transparent). CSUtility Light and Materials color controls are nested objects containing saturation (0-2, )"
		R"(default 1) and gammaOffset (-1 to 1, default 0): sceneColor, ambientLightColor, directionalLightColor, pointLightColor, linearPointLightColor, spotlightColor, linearSpotlightColor, )"
		R"(omnidirectionalBulbColor, linearOmnidirectionalBulbColor, emitColor, )"
		R"(glowmapColor and effectLightingColor. Existing multiplier keys remain unchanged. sceneColor adjusts the lighting and atmosphere inputs, not the finished image. The atmosphere adds effectColor, )"
		R"(skyStaticColor and sunGlareColor with the same object shape, plus fogSaturation and vlSaturation (0-2, default 1). Existing sky/cloud saturation, )"
		R"(gamma offsets and opacity controls retain their meanings. set: params shortName, settings (object) — a partial blob merged over the current settings, so it MUST use the same shape get returns, )"
		R"(including nested groups (e.g. LightLimitFix's shadow settings live under settings.ShadowSettings.*, NOT at the top level; )"
		R"(its flicker-shadow keys are TolerantSplitCache, FlickerShadowMode (0 live, 1 pinned, 2 warped), WarpVisibilityGate, WarpZeroDemandSkip, )"
		R"(SpotLitRegion (scores spot lights by the frustum they light), DemandTileCap (holds weakly lit lights at a quarter-size tile) and ProtectPlayerShadows (ranks lights that reach the player first and lets them redraw past the budget)). Keys the feature does not define are rejected with unknownKeys rather than silently ignored; )"
		R"(call get first if unsure of the shape. Restart-gated keys (see list's restartFields) apply on the next launch, )"
		R"(so verify with get rather than assuming a set took effect immediately. reset: params shortName, calls RestoreDefaultSettings; with key (and path, empty for a top-level setting) it restores only that one catalogued setting, the same as the right-click Reset to default in the menu. toggle: params shortName, enabled (boolean, )"
		R"(OPTIONAL — omit to flip the current loaded state); )"
		R"(flips Feature::loaded. Rejects enabling a feature unsupported on the current runtime unless Developer Mode is on, )"
		R"(which force-enables it with a logged warning instead. Rejects enabling a feature that was not loaded at boot or turned itself off during boot (a missing companion plugin or a conflicting mod), )"
		R"(since its resources were never set up; )"
		R"(enable it with boot and restart. diagnostics: params shortName, returns the feature's live runtime stats via GetDiagnostics (an empty object if the feature does not override it); )"
		R"(use this instead of adding a new inspect kind for a new counter. Upscaling FSR-session diagnostics include antiLag2Active (boolean, accepted driver enable state; not measured latency). runtimeGet: params shortName, returns the feature's live runtime-only debug flags via GetRuntimeFlags as {name: bool} (an empty object if the feature does not override it) — these are deliberately never persisted to )"
		R"(SettingsUser.json (e.g. a debug instrumentation toggle that would otherwise cost every user extra GPU work on every load), so 'set' cannot reach them and they reset to their code default on every relaunch. runtimeSet: params shortName, name (string), value (boolean) — sets one flag via )"
		R"(SetRuntimeFlag; fails with an error if the feature has no runtime flag by that name (call runtimeGet first to see valid names). favorite: params shortName, enabled (required boolean); )"
		R"(automatically saves favorite status for a loaded menu feature. Unloaded favorites retain their status but remain in Unloaded Features and cannot be changed until loaded. boot: params shortName, enabled (required boolean); )"
		R"(automatically saves whether the feature loads next launch, without changing its current loaded state. Both actions persist only the selected preference and preserve unrelated unsaved settings; failures return an error.")"
		R"(,"inputSchema":{
	"type": "object",
	"properties": {
		"action": {
			"type": "string",
			"enum": ["list","get","set","reset","toggle","diagnostics","runtimeGet","runtimeSet","favorite","boot"]
		},
		"shortName": {"type":"string"},
		"path": {"type":"string", "description":"reset only: catalogue settingPath of the setting to restore, empty for a top-level one"},
		"key": {"type":"string", "description":"reset only: catalogue settingKey; restores just that setting instead of the whole feature"},
		"settings": {
			"type": "object",
			"description": "Partial )"
		R"(SaveSettings object from get. Upscaling exposes enableDLSSFrameGen (boolean, default false, )"
		R"(restart required). Upscaling Neural Rendering material strengths live under settings.neuralRenderingTuning.materialStrength (the master switch), strengthSkin, strengthHair, strengthEyes, )"
		R"(strengthFoliage, strengthLandscape, strengthCloth, strengthMetal and strengthOther (each 0 to 1) and strengthEdgeSoftness (0 to 4 pixels); )"
		R"(read the live state and the active strengths back with the neuralRenderingStatus query. The material map debug view adds settings.neuralRenderingTuning.showMaterialMap (boolean), )"
		R"(materialMapMode (0 category colours, 1 strength ramp) and materialMapFilter (bitmask of the categories it draws: bit 0 None, 1 )"
		R"(Skin, 2 Hair, 3 Eyes, 4 Foliage, 5 Landscape, 6 Cloth, 7 Metal; the low eight bits only). Eye-staggered gain reuse is settings.neuralRenderingTuning.skipFrameReuse (boolean, default false; )"
		R"(VR only, on in the Performance profile, and forced off while frame generation is active) with settings.neuralRenderingTuning.skipFrameGapMode (0 notify the model, 1 reset the eye's history); )"
		R"(read the live state and the per-eye reuse from the neuralRenderingStatus query. PostProcessing exposes cinematic_camera including Enabled, Lens.FocalLengthMM, )"
		R"(Lens.FNumber and Exposure.ISO. LinearLighting keys: enableLinearLighting (integer, 0/1), enableACEScg (integer, 0/1), conversionSaturation (number, 0-1, default 0.92), ambientMult (number, 0-5, )"
		R"(default 0.32). CSUtility atmosphere keys: useAmbientEffectLighting (boolean), skyBrightness, cloudBrightness, skySaturation, cloudSaturation, skyGammaOffset, cloudGammaOffset, effectBrightness, )"
		R"(skyStaticBrightness, skyStaticTransparency, fogGammaOffset, fogAlphaGammaOffset, fogIntensity, vlGammaOffset, vlIntensity, )"
		R"(sunGlareIntensity. CSUtility Light and Materials color controls are nested objects containing saturation (0-2, default 1) and gammaOffset (-1 to 1, default 0): sceneColor, ambientLightColor, )"
		R"(directionalLightColor, pointLightColor, linearPointLightColor, spotlightColor, linearSpotlightColor, omnidirectionalBulbColor, linearOmnidirectionalBulbColor, emitColor, )"
		R"(glowmapColor and effectLightingColor. Existing multiplier keys remain unchanged. sceneColor adjusts the lighting and atmosphere inputs, not the finished image. The atmosphere adds effectColor, )"
		R"(skyStaticColor and sunGlareColor with the same object shape, plus fogSaturation and vlSaturation (0-2, default 1). Existing sky/cloud saturation, )"
		R"(gamma offsets and opacity controls retain their meanings."
		},
		"enabled": {"type":"boolean"},
		"name": {"type":"string","description":"Runtime flag name from runtimeGet; RedstoneShadows CaptureDebug toggles temporary capture bounds, pipeline diagnostics and selection/capture/callback/admission counters; totals persist across toggles."},
		"value": {"type":"boolean"}
	},
	"allOf": [{
		"if": {
			"properties":{"action":{"const":"runtimeSet"},"shortName":{"const":"RedstoneShadows"},"name":{"enum":["TraceDirectional","PreviewDirectional"]}},
			"required":["action","shortName","name"]
		},
		"then":{"properties":{"value":{"const":false}}}
	}],
	"oneOf": [
		{
			"properties": {
				"action": {
					"enum": ["favorite","boot"]
				},
				"shortName": {"minLength":1}
			},
			"required": ["action","shortName","enabled"]
		},
		{
			"properties": {
				"action": {
					"not": {
						"enum": ["favorite","boot"]
					}
				}
			}
		}
	]
})"
		R"(})";
}

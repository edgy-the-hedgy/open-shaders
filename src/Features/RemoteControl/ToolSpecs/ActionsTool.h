#pragma once

namespace DevBenchBridge::ToolSpecs
{
	/** Devbench descriptor JSON for openshaders.actions. {brand} is expanded at registration. */
	inline constexpr const char* kActionsTool =
		R"({"description":")"
		R"(Devbench-registered one-shot commands and read-only queries -- the imperative/derived-state counterpart to openshaders.feature's settings get/set. A feature exposes these via FEATURE_COMMAND/FEATURE_QUERY in its Feature::RegisterUxActions() override (see Utils/DevBenchUx.h); )"
		R"(Prepared RR now has opaque, exposure-adjusted previews and separate full-screen inputs/outputs, including direct diffuse/specular/combined, indirect diffuse/specular/combined and primary validity. setPreparedRRDebug requires source off|inputs|outputs; optional signal 0..6, buffer 0..15, exposure -8..8 EV. Debug selection is unsaved and exclusive with raymarched full-screen RR debug. The default-open Prepared RR buffers panel labels logical RR grid, physical packed storage and context capacity distinctly. Alpha in raw RR signals is data, never preview opacity. This is diagnostic presentation, not evidence of nonzero valid radiance. )"
		R"(The RR suite is consolidated in Upscaling > FSR-RayRegeneration: reconstruction controls, prepared-lighting enable/buffer budget, shared SDK tuning, context diagnostics, input/output/guide buffer viewers, and raymarched full-screen debug/exposure controls. SDK overrides apply across active direct, DXR-indirect and raymarched contexts and persist under Upscaling.FSRRayRegeneration.configuration; old RedstoneGI.RRConfiguration migrates even when the reconstruction block already exists. GI settings dynamically show exclusive producer selection plus DXR rays/intensity or the selected raymarched estimator controls; inactive controls are not displayed. Existing RedstoneGI/Upscaling compatibility configuration commands update the shared RR overrides. This UI/configuration change does not require geometry cache regeneration. )"
		R"(Indirect producers are exclusive: RedstoneShadows.setIndirectProducer requires producer raymarched or dxr; optional rays integer 1..8 sets normalized one-bounce DXR samples per native pixel. DXR claims the indirect provider without raymarch fallback when unavailable. Cached roomSchemaVersion 1 links/volumes resolve to current native room identities; legacy caches require regeneration for DXR indirect. Upscaling.setRayRegeneration controls shared resolutionMode 0..2, checkerboard and enabled booleans, saved under Upscaling.FSRRayRegeneration and displayed in Upscaling > FSR-RayRegeneration. Raymarcher settings no longer persist reconstruction resolution/checkerboarding. Direct and selected indirect signals use separate typed RR contexts; DXR tracing remains native resolution and is independent of RR packing resolution/checkerboard. Unknown secondary room/material data is unavailable, never converted to unrestricted lighting. )"
		R"(RedstoneShadows connects existing selected RedstoneGI output with ConnectIndirectLighting (default true), IndirectStrength (0..8, default 1) and UseDirectRayRegeneration (default true). setPreparedDirectLighting accepts optional indirect/rr booleans. Direct RR uses genuine separated RT direct diffuse/specular, matching RT material guides and current native motion/depth, then composes its successful output with emission and current RGI indirect lighting. RR budget/provider failure retains raw direct output. setLightingSampling accepts optional diffuseSlices/specularSlices integers 1..10 for existing RGI screen-space work; each slice traces both directions, normalized independently of brightness. Normal Save persists sampling in RedstoneGI. DXR indirect is selected separately with setIndirectProducer. shadowStatus directRRDispatches/directRRFrame/directRRBudgetDeferrals and indirectCompositeSubmissions distinguish historical submissions from runtime acceptance. )"
		R"(RedstoneShadows setLightingStrengths accepts optional direct/diffuse/specular/emission/indirect numbers 0..8 for live multipliers, default 1. Direct scales diffuse/specular only. Applies next dispatch without resetting resources; normal Save persists. )"
		R"(RedstoneShadows UseAllPreparedLights defaults false. setPreparedDirectLighting and setPreparedMaterials accept optional allLights boolean. True accumulates the complete prepared light set for direct views/scene output, bounded to 1..32 lights; visibility retains the baseline selector. Each light uses exact live identity, current parameters, room admission and geometric visibility. Material evaluation and emission occur once per receiver. Missing/unsupported/ambiguous requested lights reject the pixel; excess/empty sets report lightSelectionFailure without truncation. shadowStatus reports allPreparedLights, requestedLightCount and maximumDirectLights as submitted-input evidence, not accepted GPU coverage. )"
		R"(Prepared RT light selection now requires exact resolved placed-reference and base IDs; no spatial tolerance. PortalStrict is admitted per native opaque receiver room mask from the same frame/table generation. Unknown metadata fails closed. shadowStatus sourceReference/sourceBase/sourceRoomGeneration/sourceRoomMetadata are submitted-input evidence; GPU admission outcomes remain diagnostic colors. The optional extended opaque target preserves vertex-AO/category channels, supports runtime switching, and retains the original VR format. )"
		R"(RedstoneShadows exposes shadowStatus (no params), setSceneAcquisition (required enabled boolean), setPreparedScene (required enabled boolean; optional preview boolean and light integer 0..255), and resetShadowScene (no params). Prepared mode loads offline cell geometry on a worker, builds native BLAS/TLAS including flagged material geometry, and optionally traces a baseline local light into a diagnostic preview. Unresolved material instances are masked from traversal. Native lighting remains active. SCM acquisition and CaptureDebug retain their independent local-light observation diagnostics. TraceDirectional and PreviewDirectional remain false. shadowStatus includes prepared loading, acceleration and historical trace/preview counters; runtime coverage, RR, production consumption and CPU bypass remain unvalidated. )"
		R"(RedstoneShadows also exposes setPreparedMaterials (required enabled; optional view 0..12 and textureBudgetMB 64..1024) and setPreparedDirectLighting (required enabled; optional bufferBudgetMB 64..1024). Direct views 10/11/12 show diffuse/specular/combined plus emission. Direct scene mode opts into current-frame native-resolution selected-point-light replacement on supported PBR surfaces matched to opaque depth, before forward transparency. Unlisted direct lights are absent; connected current RedstoneGI output is added once; unsupported pixels retain native lighting. Enabling direct scene mode also enables prepared geometry/materials. Native shadow production remains active. shadowStatus exposes historical direct and scene-composite submissions separately from validated pixel consumption. )"
		R"(every feature also gets two built-in queries for free: matchesPerformanceProfile and profilePreviewText (params: profile=performance|balanced|quality), )"
		R"(mirroring Feature::MatchesPerformanceProfile/GetProfilePreviewText so a profile button's highlighted/tooltip state is readable without re-deriving it from raw settings. Action-dispatched. listCommands/listQueries: params shortName, )"
		R"(returns [{name,description}]. invokeCommand: params shortName, name, args (object, optional) -- queued onto the main thread, fire-and-forget, )"
		R"(same as openshaders.feature toggle/set. invokeQuery: params shortName, name, args (object, optional) -- runs synchronously on the main thread and returns the result (queries read live feature state, )"
		R"(so they marshal the same way a settings get does). RedstoneGI command setRedstoneRenderer selects renderer ssgi or rgi at the next world-render boundary without restart; only RGI requires an eligible FSR-FG session. RedstoneGI owns a registered settings page and imports Upscaling.redstoneGI only when its new settings block is absent. Legacy Upscaling commands forward to RedstoneGI. SSGI retains its own page and settings. Query redstoneStatus for actual state. setRedstoneSampling accepts occluderDepth (1..300 world units, default 300) for the assumed thickness shared by IL and AO; changes reset history and normal Save persists them. RedstoneGI command setRedstoneDebugView accepts geometryDepth for native working linear depth: fixed zero white, per-frame farthest valid surface black, invalid depth red; independent of RR success, source and exposure. RedstoneGI command setRedstoneDenoiserConfiguration sets SDK RR overrides; null restores provider defaults and reset clears all overrides. Values apply at the next RR dispatch and are saved through normal Save. getRedstoneStatus exposes defaults, overrides and pending state. Unknown shortName/name returns a plain error, never a crash. Wind command setTreeBendEnabled takes mesh (string) and enabled (boolean), changing broad bending independently of leaf flutter.")"
		R"(,"inputSchema":{
	"type": "object",
	"properties": {
		"action": {
			"type": "string",
			"enum": ["listCommands","listQueries","invokeCommand","invokeQuery"]
		},
		"shortName": {"type":"string"},
		"name": {"type":"string"},
		"args": {"type":"object","properties":{"mesh":{"type":"string","description":"Mesh path for Wind setTreeBendEnabled."},"enabled":{"type":"boolean","description":"Enable Wind broad bending or opt-in RedstoneShadows SCM observation, according to the selected command."}}}
	},
	"allOf": [{
		"if":{"properties":{"shortName":{"const":"RedstoneShadows"},"name":{"const":"setPreparedRRDebug"}},"required":["shortName","name"]},
		"then":{"properties":{"args":{"type":"object","properties":{"source":{"enum":["off","inputs","outputs"]},"signal":{"type":"integer","minimum":0,"maximum":6},"buffer":{"type":"integer","minimum":0,"maximum":15},"exposure":{"type":"number","minimum":-8,"maximum":8}},"required":["source"],"additionalProperties":false}},"required":["args"]}
	}, {
		"if":{"properties":{"shortName":{"const":"Upscaling"},"name":{"const":"setRayRegeneration"}},"required":["shortName","name"]},
		"then":{"properties":{"args":{"type":"object","properties":{"resolutionMode":{"type":"integer","minimum":0,"maximum":2},"checkerboard":{"type":"boolean"},"enabled":{"type":"boolean"}},"additionalProperties":false}}}
	}, {
		"if":{"properties":{"shortName":{"const":"RedstoneShadows"},"name":{"const":"setIndirectProducer"}},"required":["shortName","name"]},
		"then":{"properties":{"args":{"type":"object","properties":{"producer":{"enum":["raymarched","dxr"]},"rays":{"type":"integer","minimum":1,"maximum":8}},"required":["producer"],"additionalProperties":false}},"required":["args"]}
	}, {
		"if":{"properties":{"shortName":{"const":"RedstoneShadows"},"name":{"const":"setLightingSampling"}},"required":["shortName","name"]},
		"then":{"properties":{"args":{"type":"object","properties":{"diffuseSlices":{"type":"integer","minimum":1,"maximum":10},"specularSlices":{"type":"integer","minimum":1,"maximum":10}},"additionalProperties":false}}}
	}, {
		"if":{"properties":{"shortName":{"const":"RedstoneShadows"},"name":{"const":"setLightingStrengths"}},"required":["shortName","name"]},
		"then":{"properties":{"args":{"type":"object","properties":{"direct":{"type":"number","minimum":0,"maximum":8},"diffuse":{"type":"number","minimum":0,"maximum":8},"specular":{"type":"number","minimum":0,"maximum":8},"emission":{"type":"number","minimum":0,"maximum":8},"indirect":{"type":"number","minimum":0,"maximum":8}},"additionalProperties":false}}}
	}, {
		"if": {
			"properties":{"shortName":{"const":"RedstoneShadows"},"name":{"const":"setPreparedDirectLighting"}},
			"required":["shortName","name"]
		},
		"then":{"properties":{"args":{
			"type":"object","properties":{"enabled":{"type":"boolean"},"bufferBudgetMB":{"type":"integer","minimum":64,"maximum":1024},"allLights":{"type":"boolean"},"indirect":{"type":"boolean"},"rr":{"type":"boolean"}},
			"required":["enabled"],"additionalProperties":false
		}},"required":["args"]}
	}, {
		"if": {
			"properties":{"shortName":{"const":"RedstoneShadows"},"name":{"const":"setPreparedMaterials"}},
			"required":["shortName","name"]
		},
		"then":{"properties":{"args":{
			"type":"object","properties":{"enabled":{"type":"boolean"},"view":{"type":"integer","minimum":0,"maximum":12},"textureBudgetMB":{"type":"integer","minimum":64,"maximum":1024},"allLights":{"type":"boolean"}},
			"required":["enabled"],"additionalProperties":false
		}},"required":["args"]}
	}, {
		"if": {
			"properties":{"shortName":{"const":"RedstoneShadows"},"name":{"const":"setPreparedScene"}},
			"required":["shortName","name"]
		},
		"then":{"properties":{"args":{
			"type":"object","properties":{"enabled":{"type":"boolean"},"preview":{"type":"boolean"},"light":{"type":"integer","minimum":0,"maximum":255}},
			"required":["enabled"],"additionalProperties":false
		}},"required":["args"]}
	}, {
		"if": {
			"properties":{"shortName":{"const":"RedstoneShadows"},"name":{"const":"setSceneAcquisition"}},
			"required":["shortName","name"]
		},
		"then":{"properties":{"args":{
			"type":"object","properties":{"enabled":{"type":"boolean"}},"required":["enabled"],"additionalProperties":false
		}},"required":["args"]}
	}, {
		"if": {
			"properties":{"shortName":{"enum":["RedstoneGI","Upscaling"]},"name":{"const":"setRedstoneRenderer"}},
			"required":["shortName","name"]
		},
		"then":{"properties":{"args":{
			"type":"object","properties":{"renderer":{"type":"string","enum":["ssgi","rgi"]}},"required":["renderer"]
		}}}
	}, {
		"if": {
			"properties": {"shortName":{"enum":["RedstoneGI","Upscaling"]},"name":{"const":"setRedstoneDenoiserConfiguration"}},
			"required":["shortName","name"]
		},
		"then": {"properties":{"args":{"properties":{
			"reset":{"type":"boolean"},
			"normalStrength":{"type":["number","null"],"minimum":0,"maximum":1},
			"stabilityBias":{"type":["number","null"],"minimum":0,"maximum":1},
			"maxRadiance":{"type":["number","null"],"minimum":0,"maximum":65504},
			"radianceClipStdK":{"type":["number","null"],"minimum":0,"maximum":65504},
			"gaussianRelaxation":{"type":["number","null"],"minimum":0,"maximum":1},
			"disocclusionThreshold":{"type":["number","null"],"minimum":0.01,"maximum":0.05},
			"debugDepthMin":{"type":["number","null"],"minimum":0},
			"debugDepthMax":{"type":["number","null"],"minimum":0}
		}}}}
	}, {
		"if": {
			"properties":{"shortName":{"enum":["RedstoneGI","Upscaling"]},"name":{"const":"setRedstoneSampling"}},
			"required":["shortName","name"]
		},
		"then":{"properties":{"args":{"properties":{
			"occluderDepth":{"type":"number","minimum":1,"maximum":300,"default":300}
		}}}}
	}, {
		"if": {
			"properties": {"shortName":{"enum":["RedstoneGI","Upscaling"]},"name":{"const":"setRedstoneDebugView"}},
			"required":["shortName","name"]
		},
		"then": {"properties":{"args":{
			"type":"object",
			"properties":{
				"view":{"type":"string","enum":["combined","diffuse","specular","diffuseAO","specularOcclusion","combinedOcclusion","allSignals","geometryDepth"]},
				"source":{"type":"string","enum":["inputs","outputs"],"default":"outputs"}
			},
			"required":["view"]
		}}}
	}]
})"
		R"(})";
}

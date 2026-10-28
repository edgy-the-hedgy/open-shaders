#pragma once

namespace DevBenchBridge::ToolSpecs
{
	/** Devbench descriptor JSON for openshaders.actions. {brand} is expanded at registration. */
	inline constexpr const char* kActionsTool =
		R"({"description":")"
		R"(Devbench-registered one-shot commands and read-only queries -- the imperative/derived-state counterpart to openshaders.feature's settings get/set. A feature exposes these via FEATURE_COMMAND/FEATURE_QUERY in its Feature::RegisterUxActions() override (see Utils/DevBenchUx.h); )"
		R"(RedstoneShadows exposes shadowStatus (no params), setSceneAcquisition (required enabled boolean), setPreparedScene (required enabled boolean; optional preview boolean and light integer 0..255), and resetShadowScene (no params). Prepared mode loads offline cell geometry on a worker, builds native BLAS/TLAS including flagged material geometry, and optionally traces a baseline local light into a diagnostic preview. Unresolved material instances are masked from traversal. Native lighting remains active. SCM acquisition and CaptureDebug retain their independent local-light observation diagnostics. TraceDirectional and PreviewDirectional remain false. shadowStatus includes prepared loading, acceleration and historical trace/preview counters; runtime coverage, RR, production consumption and CPU bypass remain unvalidated. )"
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

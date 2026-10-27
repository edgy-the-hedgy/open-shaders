#pragma once

namespace DevBenchBridge::ToolSpecs
{
	/** Devbench descriptor JSON for openshaders.actions. {brand} is expanded at registration. */
	inline constexpr const char* kActionsTool =
		R"({"description":")"
		R"(Devbench-registered one-shot commands and read-only queries -- the imperative/derived-state counterpart to openshaders.feature's settings get/set. A feature exposes these via FEATURE_COMMAND/FEATURE_QUERY in its Feature::RegisterUxActions() override (see Utils/DevBenchUx.h); )"
		R"(every feature also gets two built-in queries for free: matchesPerformanceProfile and profilePreviewText (params: profile=performance|balanced|quality), )"
		R"(mirroring Feature::MatchesPerformanceProfile/GetProfilePreviewText so a profile button's highlighted/tooltip state is readable without re-deriving it from raw settings. Action-dispatched. listCommands/listQueries: params shortName, )"
		R"(returns [{name,description}]. invokeCommand: params shortName, name, args (object, optional) -- queued onto the main thread, fire-and-forget, )"
		R"(same as openshaders.feature toggle/set. invokeQuery: params shortName, name, args (object, optional) -- runs synchronously on the main thread and returns the result (queries read live feature state, )"
		R"(so they marshal the same way a settings get does). Upscaling command setRedstoneRenderer selects renderer ssgi or rgi at the next world-render boundary without restart; only RGI requires an eligible FSR-FG session. The RGI settings page appears below Upscaling while active, and SSGI retains its own page and settings. Query redstoneStatus for actual state. Upscaling command setRedstoneDebugView accepts geometryDepth for native working linear depth: fixed zero white, per-frame farthest valid surface black, invalid depth red; independent of RR success, source and exposure. Upscaling command setRedstoneDenoiserConfiguration sets SDK RR overrides; null restores provider defaults and reset clears all overrides. Values apply at the next RR dispatch and are saved through normal Save. getRedstoneStatus exposes defaults, overrides and pending state. Unknown shortName/name returns a plain error, never a crash. Wind command setTreeBendEnabled takes mesh (string) and enabled (boolean), changing broad bending independently of leaf flutter.")"
		R"(,"inputSchema":{
	"type": "object",
	"properties": {
		"action": {
			"type": "string",
			"enum": ["listCommands","listQueries","invokeCommand","invokeQuery"]
		},
		"shortName": {"type":"string"},
		"name": {"type":"string"},
		"args": {"type":"object","properties":{"mesh":{"type":"string","description":"Mesh path for Wind setTreeBendEnabled."},"enabled":{"type":"boolean","description":"Broad bending enabled for Wind setTreeBendEnabled."}}}
	},
	"allOf": [{
		"if": {
			"properties":{"shortName":{"const":"Upscaling"},"name":{"const":"setRedstoneRenderer"}},
			"required":["shortName","name"]
		},
		"then":{"properties":{"args":{
			"type":"object","properties":{"renderer":{"type":"string","enum":["ssgi","rgi"]}},"required":["renderer"]
		}}}
	}, {
		"if": {
			"properties": {"shortName":{"const":"Upscaling"},"name":{"const":"setRedstoneDenoiserConfiguration"}},
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
			"properties": {"shortName":{"const":"Upscaling"},"name":{"const":"setRedstoneDebugView"}},
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

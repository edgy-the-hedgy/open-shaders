import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class ArchitectureLintTests(unittest.TestCase):
    def test_dynamic_gpu_passes_are_limited_to_names_built_at_run_time(self):
        # CS_GPU_PASS_DYNAMIC allocates per call: fixed names use CS_GPU_PASS, _SELECT or _INDEXED.
        allowed = {
            "src/Features/Effects11.cpp": 2,
            "src/Features/Effects11/EffectManager.cpp": 1,
            "src/Features/Effects11/Effects/Effect.cpp": 1,
        }
        found = {}
        for path in sorted((ROOT / "src").rglob("*")):
            if path.suffix not in {".cpp", ".h"} or path.name == "GpuPass.h":
                continue
            count = path.read_text(encoding="utf-8", errors="replace").count("CS_GPU_PASS_DYNAMIC(")
            if count:
                found[path.relative_to(ROOT).as_posix()] = count
        self.assertEqual(found, allowed)

    def test_eye_count_and_width_use_the_shared_helpers(self):
        # Hand-rolled eye math drifts per feature: use Util::EyeCount()/EyeWidth() (Utils/Game.h).
        eyes = r"(?:2u?|2\.0f|0\.5f|k\w*EyeCount)"
        flat = r"(?:1u?|1\.0f)"
        idiom = re.compile(rf"(?<!!)isVR\s*\?\s*{eyes}\s*:\s*{flat}|!\s*[\w:]*isVR\s*\?\s*{flat}\s*:\s*{eyes}")
        offenders = []
        for path in sorted((ROOT / "src").rglob("*")):
            if path.suffix not in {".cpp", ".h"} or path.name == "Game.h":
                continue
            if idiom.search(path.read_text(encoding="utf-8", errors="replace")):
                offenders.append(path.relative_to(ROOT).as_posix())
        self.assertEqual(offenders, [])

    def test_shaders_use_the_shared_eye_count(self):
        # A per-shader `#ifdef VR eyeCount = 2` duplicates kEyeCount in Common/StereoEye.hlsli.
        idiom = re.compile(r"(?:eyeCount|EyeCount)\s*=\s*[12]\s*;")
        offenders = []
        for base in (ROOT / "features", ROOT / "package/Shaders"):
            for path in sorted(base.rglob("*.hl*")):
                if path.name == "StereoEye.hlsli":
                    continue
                if idiom.search(path.read_text(encoding="utf-8", errors="replace")):
                    offenders.append(path.relative_to(ROOT).as_posix())
        self.assertEqual(offenders, [])

    def test_skylighting_warmup_covers_shader_confidence_fade(self):
        # A warmup shorter than the shader's fadeInThreshold shows half-faded probes after a reset.
        shader = (ROOT / "features/Skylighting/Shaders/Skylighting/UpdateProbesCS.hlsl").read_text(encoding="utf-8")
        header = (ROOT / "src/Features/Skylighting.h").read_text(encoding="utf-8")
        samples = int(re.search(r"fadeInThreshold = (\d+)", shader).group(1))
        frames = int(re.search(r"probeHistoryWarmupFrames = (\d+)", header).group(1))
        self.assertGreaterEqual(frames, 4 * samples)
        self.assertEqual(frames % 4, 0)

    # How each Post Processing shader folder handles the side-by-side VR target. A new folder fails
    # until classified here and in docs/development/vr-eye-handling.md.
    POST_PROCESSING_STEREO = {
        "Border": "stereo",
        "Camera": "stereo",
        "Composite": "stereo",
        "DoF": "stereo",
        "LensFlare": "stereo",
        "LocalExposure": "stereo",
        "MotionBlur": "stereo",
        "Vignette": "stereo",
        "PhysicalLensFlare": "eye-local",
        "PhysicalLensImaging": "eye-local",
        "ColorGrading": "per-pixel",
        "LUT": "per-pixel",
        "HistogramAutoExposure": "global",
        "CODBloom": "stereo",
        "PhysicalGlare": "eye-local",
    }

    def test_post_processing_shaders_declare_their_vr_stereo_handling(self):
        shaders = ROOT / "features/Post Processing/Shaders/PostProcessing"
        folders = {p.name for p in shaders.iterdir() if p.is_dir()}
        self.assertEqual(folders, set(self.POST_PROCESSING_STEREO), "classify new folders in POST_PROCESSING_STEREO")
        for folder, kind in self.POST_PROCESSING_STEREO.items():
            text = "".join(
                p.read_text(encoding="utf-8", errors="replace")
                for p in (shaders / folder).rglob("*")
                if p.suffix in {".hlsl", ".hlsli"}
            )
            uses_stereo = "Stereo::" in text or "StereoEye.hlsli" in text
            if kind == "stereo":
                self.assertTrue(uses_stereo, f"{folder} is declared stereo but never converts eye UVs")
            elif kind in {"per-pixel", "global"}:
                self.assertFalse(uses_stereo, f"{folder} is declared {kind} but uses stereo helpers: reclassify it")
        for source in ("PhysicalLens", "PhysicalGlare"):
            text = (ROOT / f"src/Features/PostProcessing/{source}.cpp").read_text(encoding="utf-8")
            self.assertIn("eyeCount", text, f"{source} eye-local shaders need the per-eye dispatch loop")


    def test_full_native_cubemap_acquisition_has_no_legacy_publication_bridge(self):
        native = (ROOT / "src/Features/NativeCubemaps/NativeCubemaps.cpp").read_text(encoding="utf-8")
        legacy = (ROOT / "src/Features/DynamicCubemaps.cpp").read_text(encoding="utf-8")
        self.assertNotIn("globals::features::dynamicCubemaps", native)
        self.assertNotIn("nativeCubemaps", legacy)
        self.assertIn("CaptureRaw(backend,sceneColor)", native)
        self.assertIn("processing->Dispatch", native)
        shader = (ROOT / "features/Native Composite/Shaders/NativeComposite/NativeCubemapFilter.hlsl6").read_text(encoding="utf-8")
        self.assertIn("ReadNativeRadiance", shader)
        self.assertIn("RasterIdentityCandidate", shader)
        self.assertIn("NativeDirect.Load2", shader)

    def test_full_native_shader_inputs_share_one_boot_latched_cache_identity(self):
        cache = (ROOT / "src/ShaderCache.cpp").read_text(encoding="utf-8")
        self.assertIn('"FULL_NATIVE_INPUT"', cache)
        self.assertIn('L"_full_native_input_v6_coverage2"', cache)
        self.assertNotIn('L"_native_pbr_input_v1"', cache)
        for name in ("Lighting", "Effect", "Sky", "DistantTree"):
            source = (ROOT / f"package/Shaders/{name}.hlsl").read_text(encoding="utf-8")
            self.assertIn('#include "Common/FullNativeInput.hlsli"', source)

    def test_native_cube_processing_is_staged_as_dxil(self):
        cmake = (ROOT / "cmake/DX12NativeComposite.cmake").read_text(encoding="utf-8")
        shader = (ROOT / "features/Native Composite/Shaders/NativeComposite/NativeCubemapFilter.hlsl6").read_text(encoding="utf-8")
        self.assertIn("NativeCubemapHistory NativeCubemapFilter", cmake)
        self.assertIn("set(_full_native_entry historyMain)", cmake)
        self.assertIn("void historyMain(", shader)
        self.assertIn("void main(", shader)

    def test_native_payload_is_verified_and_protected_during_shader_only_sync(self):
        verification = (ROOT / "cmake/VerifyAioPayload.cmake").read_text(encoding="utf-8")
        sync = (ROOT / "cmake/SyncShaderDeploy.cmake").read_text(encoding="utf-8")
        for path in ("DX12NativeScene/CharacterSkinning.dxil", "NativeComposite/NativeCubemapHistory.dxil",
                     "NativeComposite/NativeCubemapFilter.dxil"):
            self.assertIn(f'"Shaders/{path}"', verification)
            self.assertIn(f'"{path}"', sync)
        for name in ("NativeLLF", "NativeIBL", "NativeCubemaps", "NativePBR"):
            self.assertIn(f'"Shaders/Features/{name}.ini"', verification)
        self.assertIn('"Shaders/Common/FullNativeInput.hlsli"', verification)
        self.assertIn('"Shaders/NativeCubemaps/CaptureRaw.cs.hlsl"', verification)


    def test_native_primary_material_uses_independent_raster_authority(self):
        probe = (ROOT / "src/Features/DX12/DX12NativeScreenProbe.h").read_text(encoding="utf-8")
        raw = (ROOT / "src/Features/NativePBR/NativeRawInputCapture.cpp").read_text(encoding="utf-8")
        shader = (ROOT / "features/Native Composite/Shaders/NativeComposite/NativePBRProbe.hlsl6").read_text(encoding="utf-8")
        coherent = probe.split("bool IsCoherent(", 1)[1].split("GetMaterialFrame(", 1)[0]
        self.assertIn("rasterMaterialEvaluation", coherent)
        self.assertNotIn("!scene ||", coherent)
        self.assertNotIn("scene->", coherent)
        self.assertIn("MatchesRasterPublication(stamp, requested)", raw)
        self.assertIn("RasterMaterialLookup.Load", shader)
        self.assertIn("RasterProjectionLookup.Load", shader)
        self.assertIn("HasRasterRayMapping!=0u", shader)
        self.assertIn("raw.borrowedLandscape", shader)

    def test_native_input_is_published_by_original_pass(self):
        raw = (ROOT / "package/Shaders/Common/NativeInputMRT.hlsli").read_text(encoding="utf-8")
        capture = (ROOT / "src/Features/NativePBR/NativeRawInputCapture.cpp").read_text(encoding="utf-8")
        probe = (ROOT / "src/Features/DX12/DX12NativeScreenProbe.h").read_text(encoding="utf-8")
        consumer = (ROOT / "features/Native Composite/Shaders/NativeComposite/NativeRawRasterInput.hlsli").read_text(encoding="utf-8")
        lighting = (ROOT / "package/Shaders/Lighting.hlsl").read_text(encoding="utf-8")
        begin = lighting.index("// Full Native deferred Lighting is strictly input-only")
        end = lighting.index("#\telse\nFUR_EARLY_DEPTH_STENCIL", begin)
        visibility = lighting[begin:end]
        self.assertIn("MotionBlur::GetSSMotionVector", visibility)
        self.assertIn("NativeInputCoverage::Alpha", visibility)
        self.assertIn("NativeInputMRT::Write", visibility)
        self.assertIn("AuxLandscape", visibility)
        self.assertIn("AuxProjection", visibility)
        for forbidden in ("EvaluateBRDF", "NativeForwardEnvironment::Sample", "GetDynamicCubemap", "ParallaxUV", "ShadowSampling::"):
            self.assertNotIn(forbidden, visibility)
        self.assertNotIn("psout.Diffuse.xyz", visibility)
        for target in ("IdentityUV : SV_Target3", "ColorGradDepth : SV_Target4", "BasisFlags : SV_Target5", "MaterialTechnique : SV_Target6"):
            self.assertIn(target, raw)
        self.assertIn("ddx(uv)", raw)
        self.assertIn("ddy(uv)", raw)
        self.assertIn("CopySubresourceRegion(active->planes[i]->resource11", capture)
        self.assertIn("drawRecords.size_bytes()", capture)
        self.assertIn("drawTableCapacity < tableBytes", capture)
        self.assertIn("materialSources.resize", capture)
        self.assertIn("drawSourceViews.assign", capture)
        self.assertIn("NativeInstance=input.InstanceID", lighting)
        self.assertIn("AuxBorrowedLandscape", raw)
        self.assertIn("ObserveNativeInputSourceViews", (ROOT / "src/Globals.cpp").read_text(encoding="utf-8"))
        self.assertIn("instanceOrdinal*stride+primitive", raw)
        for shader in ("RunGrass", "DistantTree", "Effect"):
            source = (ROOT / f"package/Shaders/{shader}.hlsl").read_text(encoding="utf-8")
            self.assertIn("NativeInstance", source)
            self.assertIn("NativeInputMRT::Write", source)
        self.assertIn("renderWidth, renderHeight", capture)
        for obsolete in ("OMGetRenderTargets", "OMSetRenderTargets", "PSGetShader", "PSSetShader", "StandaloneShaderClass"):
            self.assertNotIn(obsolete, capture)
        hooks = (ROOT / "src/Globals.cpp").read_text(encoding="utf-8")
        begin = hooks.index("struct ID3D11DeviceContext_DrawIndexedNativeInput")
        end = hooks.index("void InstallD3DHooks", begin)
        draw_hook = hooks[begin:end]
        self.assertEqual(draw_hook.count("func(context, count, start, base)"), 1)
        self.assertIn("StampFullNativeIndexedDraw", draw_hook)
        self.assertIn("slot->rawInputs->readyFence", probe)
        self.assertIn("slot->rawInputs->evidence->readyFence", probe)
        self.assertNotIn("RawRasterTokenTable", consumer)
        self.assertIn("RawRasterParticipation", consumer)
        scene = (ROOT / "src/Features/DX12/DX12NativeScene.cpp").read_text(encoding="utf-8")
        self.assertNotIn("draws.tokens.push_back", scene)
        self.assertNotIn("RasterTokenBase(", scene)
        self.assertIn("abs(rawDepth-depth)", consumer)
        self.assertIn("DecodeRasterDraw(RawRasterDrawTable", consumer)
        self.assertIn("ReadRawVisibility", consumer)
        self.assertIn("any(mapped!=uint2(instance,primitive))", consumer)
        self.assertIn("RawRasterIdentityUV", consumer)
        self.assertIn("draws.drawRecords.size()+1u", scene)
        self.assertIn("nativeInputIndexOffset", scene)
        self.assertIn("draws.allWritersKnown && draws.knownWriters != 0", scene)
        self.assertIn("InvalidateNativeInputVisibility", hooks)
        self.assertIn("ConsumerDrawCount({frame,slot->scope", probe)
        self.assertIn("MatchesRasterPublication(stamp, requested)", capture)
        self.assertIn("void DX12NativeScene::ObserveNativeInputSourceViews(", scene)
        self.assertIn("void DX12NativeScene::ResetNativeInputBindingState(", scene)
        self.assertIn("localInvalidator", scene)
        self.assertIn("RasterTriangleBound", scene)
        deferred = (ROOT / "src/Deferred.cpp").read_text(encoding="utf-8")
        self.assertIn("DXGI_FORMAT_R32G32B32A32_UINT", deferred)
        self.assertIn("BeginFullNativeRasterInputs", deferred)

if __name__ == "__main__":
    unittest.main()

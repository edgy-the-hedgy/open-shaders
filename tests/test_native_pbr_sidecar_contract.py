"""Source-contract checks that run without Skyrim, Direct3D or a Windows compiler.

These cannot replace pixel-level GPU validation in the mandatory commit-3 checkpoint.
"""

import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


class NativePBRSidecarContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.pixel = (ROOT / "package/Shaders/NativePBR/MaterialSidecar.ps.hlsl").read_text()
        cls.validate = (ROOT / "package/Shaders/NativePBR/ValidateSidecar.cs.hlsl").read_text()
        cls.contract = (ROOT / "src/Features/NativePBR/MaterialSidecarContract.h").read_text()
        cls.capture = (ROOT / "src/Features/DX12NativeScene/RasterCapture.cpp").read_text()
        cls.deferred = (ROOT / "src/Deferred.cpp").read_text()

    def test_all_six_sidecar_targets_are_independent(self):
        self.assertEqual(set(re.findall(r"SV_Target(\d+)", self.pixel)), set("012345"))
        self.assertIn("DXGI_FORMAT_R32_UINT", self.capture)
        self.assertIn("DXGI_FORMAT_R16G16B16A16_FLOAT", self.capture)
        self.assertIn("pbrSidecarValid", self.capture)

    def test_gradients_and_facing_reach_the_sidecar(self):
        self.assertIn("ddx(input.uv)", self.pixel)
        self.assertIn("ddy(input.uv)", self.pixel)
        self.assertIn("input.position.z", self.pixel)
        self.assertIn("SV_IsFrontFace", self.pixel)
        self.assertEqual(self.pixel.count("float4(input.basis"), 3)

    def test_coverage_is_zero_clear_and_validated_against_final_depth(self):
        self.assertIn("RWTexture2D<uint> SidecarIdentity", self.validate)
        self.assertIn("FinalOpaqueDepth.Load", self.validate)
        self.assertIn("SidecarIdentity[xy] = 0", self.validate)
        self.assertIn("ClearRenderTargetView(targets[0].rtv", (ROOT / "src/Features/NativePBR/MaterialSidecar.cpp").read_text())

    def test_identity_is_losslessly_resolved_from_fenced_draw_table(self):
        self.assertIn("uint64_t materialIdentity", self.contract)
        self.assertIn("draw.ordinal == a_ordinal", self.contract)
        self.assertIn("slot->pbrDraws = std::move(a_pbrDraws)", self.capture)
        self.assertIn("sourceDepthFormat", self.capture)

    def test_original_eight_deferred_targets_still_exist(self):
        self.assertIn("RE::RENDER_TARGET targets[8]", self.deferred)
        self.assertIn("Feature::ForEachLoadedFeature(\"OnOpaqueRasterReady\"", self.deferred)


if __name__ == "__main__":
    unittest.main()

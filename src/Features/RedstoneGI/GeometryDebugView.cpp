#include "RedstoneGI.h"
#include "GpuPass.h"
#include "State.h"
#include "Util.h"

void RedstoneGI::PrepareGeometryDebugPreview(const std::array<uint, 2>& a_size)
{
	if (rrPreviewFailed || !texWorkingDepth || !texRRSpecularOutput || !a_size[0] || !a_size[1]) return;
	CS_GPU_PASS("RedstoneGI::GeometryDebugPreview");
	try {
		const auto* path = L"Data\\Shaders\\RedstoneGI\\geometryDebugPreview.cs.hlsl";
		auto* reduce = geometryDepthReduceShader.Get(path, {}, "cs_5_0", "ReduceDepth", "RedstoneGI::GeometryDepthReduce CS");
		auto* display = geometryDepthPreviewShader.Get(path, {}, "cs_5_0", "main", "RedstoneGI::GeometryDepthPreview CS");
		if (!reduce || !display) {
			rrPreviewFailed = true;
			return;
		}
		EnsureRRDebugPreviewTexture(a_size);
		if (!geometryDepthMaximum) {
			auto buffer = eastl::make_unique<StructuredBuffer>(StructuredBufferDesc<uint32_t>(1, true, false), 1, "RedstoneGI::GeometryDepthMaximum");
			buffer->CreateSRV();
			buffer->CreateUAV();
			geometryDepthMaximum = std::move(buffer);
		}
		struct alignas(16) Settings
		{
			std::array<uint, 2> size;
			std::array<uint, 2> padding{};
		};
		if (!geometryPreviewSettings)
			geometryPreviewSettings = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<Settings>(), "RedstoneGI::GeometryPreviewSettings");
		geometryPreviewSettings->Update(Settings{ a_size });
		auto* context = globals::d3d::context;
		winrt::com_ptr<ID3D11Buffer> previousCB;
		context->CSGetConstantBuffers(0, 1, previousCB.put());
		auto* cb = geometryPreviewSettings->CB();
		context->CSSetConstantBuffers(0, 1, &cb);
		std::array<ID3D11ShaderResourceView*, 3> inputs{
			texWorkingDepth->srv.get(), Util::GetCurrentSceneDepthSRV(), nullptr };
		std::array<ID3D11UnorderedAccessView*, 2> outputs{ nullptr, geometryDepthMaximum->UAV() };
		constexpr UINT clear[4]{};
		context->ClearUnorderedAccessViewUint(geometryDepthMaximum->UAV(), clear);
		context->CSSetShaderResources(0, static_cast<UINT>(inputs.size()), inputs.data());
		{
			CS_GPU_PASS("RedstoneGI::GeometryDepthMaximum");
			context->CSSetUnorderedAccessViews(0, static_cast<UINT>(outputs.size()), outputs.data(), nullptr);
			context->CSSetShader(reduce, nullptr, 0);
			context->Dispatch((a_size[0] + 15u) / 16u, (a_size[1] + 15u) / 16u, 1);
		}
		outputs.fill(nullptr);
		context->CSSetUnorderedAccessViews(0, static_cast<UINT>(outputs.size()), outputs.data(), nullptr);
		inputs[2] = geometryDepthMaximum->SRV();
		outputs[0] = rrPreview->uav.get();
		{
			CS_GPU_PASS("RedstoneGI::GeometryDepthDisplay");
			context->CSSetShaderResources(0, static_cast<UINT>(inputs.size()), inputs.data());
			context->CSSetUnorderedAccessViews(0, static_cast<UINT>(outputs.size()), outputs.data(), nullptr);
			context->CSSetShader(display, nullptr, 0);
			context->Dispatch((a_size[0] + 7u) / 8u, (a_size[1] + 7u) / 8u, 1);
		}
		inputs.fill(nullptr);
		outputs.fill(nullptr);
		context->CSSetShaderResources(0, static_cast<UINT>(inputs.size()), inputs.data());
		context->CSSetUnorderedAccessViews(0, static_cast<UINT>(outputs.size()), outputs.data(), nullptr);
		cb = previousCB.get();
		context->CSSetConstantBuffers(0, 1, &cb);
		rrPreviewSize = a_size;
		rrPreviewGameFrame = static_cast<uint32_t>(*globals::game::frameCounter);
		rrPreviewSequence = globals::state->frameCount;
		rrPreviewView = RRDebugView::GeometryDepth;
		rrPreviewSource = GetRRDebugSource();
		rrPreviewSignalMask = GetSignalMask();
		rrPreviewBypassed = !rrEnabled;
		rrPreviewValid = true;
	} catch (const std::exception& e) {
		rrPreviewFailed = true;
		logger::error("[Redstone] Geometry depth preview unavailable: {}", e.what());
	}
}

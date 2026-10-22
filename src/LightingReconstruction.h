#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>

struct ID3D11Texture2D;

/** @brief Optional indirect-lighting reconstruction provider; callers retain ownership of inputs. */
struct LightingReconstruction
{
	/** @brief Allows providers to release their own resources through the service interface. */
	virtual ~LightingReconstruction() = default;
	/** @brief Reports whether indirect-specular reconstruction is available. */
	virtual bool CanDispatchRayRegeneration() const = 0;
	/** @brief Reconstructs the current signal through the provider's synchronized resource bridge. */
	virtual bool DispatchRayRegeneration(ID3D11Texture2D* a_signal, ID3D11Texture2D* a_linearDepth, ID3D11Texture2D* a_motionVectors,
		ID3D11Texture2D* a_normalRoughness, ID3D11Texture2D* a_specularAlbedo, ID3D11Texture2D* a_diffuseAlbedo,
		ID3D11Texture2D* a_output, uint32_t a_width, uint32_t a_height, bool a_resetHistory) = 0;
	/** @brief Reports provider counters, context dimensions and availability without dispatching work. */
	virtual nlohmann::json GetRayRegenerationDiagnostics() const = 0;
	/** @brief Selects reconstruction diagnostics. */
	virtual void SetRRDiagnosticsEnabled(bool a_enabled) = 0;
};

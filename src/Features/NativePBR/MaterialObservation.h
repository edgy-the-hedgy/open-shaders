#pragma once

#include "NativePBRRegistry.h"

namespace NativePBR
{
	/** @brief Publish current Skyrim PBR material inputs without acquiring geometry ownership. */
	void ObserveLightingMaterial(Registry& a_registry, const RE::BSLightingShaderMaterialBase& a_material, MaterialKind a_kind);
	/** @brief Associate a visible Skyrim instance with its observed PBR material. */
	void ObserveVisibleInstance(Registry& a_registry, const RE::BSLightingShaderProperty& a_property, RE::BSGeometry& a_geometry);
}

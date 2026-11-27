#pragma once

#include "Feature.h"

namespace DX12
{
	// Uses the existing Feature registry, but is selected only by the session backend.
	class FullNativeFeature : public Feature
	{
	public:
		std::string_view GetCategory() const override { return FeatureCategories::kDX12; }
		bool IsCore() const override { return true; }
		bool IsAlwaysEnabled() const override { return true; }
		bool SupportsVR() override { return false; }
		bool IsInMenu() const override { return false; }
		bool IsInFeatureList() const override { return false; }
		bool UsesMainSettings() const override { return false; }
	};
}

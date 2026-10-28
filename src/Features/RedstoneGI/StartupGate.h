#pragma once

namespace Redstone
{
	/** @brief Probes FSR-FG backend eligibility once without changing backend lifetime at runtime. */
	struct StartupGate
	{
		bool checked = false;
		bool eligible = false;

		template <class HardwareProbe>
		constexpr bool Select(bool a_fsrFrameGeneration, bool a_requested, HardwareProbe&& a_probe)
		{
			if (!a_fsrFrameGeneration || checked)
				return false;
			checked = true;
			eligible = a_probe();
			return eligible && a_requested;
		}
	};
}

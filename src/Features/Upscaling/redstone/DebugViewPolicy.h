#pragma once

namespace Redstone
{
	/** @brief Present resets State before ImGui, advancing the producer's sequence by one. */
	constexpr bool IsDebugPreviewCurrent(bool a_valid, unsigned a_producerGameFrame, unsigned a_currentGameFrame,
		unsigned a_producerSequence, unsigned a_presentSequence)
	{
		return a_valid && a_producerGameFrame == a_currentGameFrame && a_producerSequence + 1u == a_presentSequence;
	}
}

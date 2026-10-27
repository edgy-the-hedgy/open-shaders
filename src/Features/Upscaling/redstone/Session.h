#pragma once
#include "RedstoneGI.h"
#include "RuntimeDenoiser.h"

/** @brief Owns runtime-switchable RGI resources for a supported FSR-FG session. */
struct RedstoneSession
{
	RedstoneDenoiser denoiser;
	RedstoneGI gi{ denoiser };
};

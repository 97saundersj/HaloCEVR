#pragma once
#include <cstdint>

namespace Helpers
{
	// First-person weapon animation globals (not stored on the weapon object).
	// Layout from Halo PC 1.0.10 — baseAnimId @ +0x1E, baseAnimFrame @ +0x20.
	constexpr int FirstPersonAnimBaseAnimIdOffset = 0x1E;
	constexpr int FirstPersonAnimBaseAnimFrameOffset = 0x20;

	uintptr_t GetFirstPersonAnimBase();
	bool HasFirstPersonAnimBase();

	uint16_t GetFirstPersonBaseAnimId();
	uint16_t GetFirstPersonBaseAnimFrame();
	void SetFirstPersonBaseAnimId(uint16_t animId);
	void SetFirstPersonBaseAnimFrame(uint16_t frame);
}

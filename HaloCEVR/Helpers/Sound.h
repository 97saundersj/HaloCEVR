#pragma once

#include "../WeaponHandler.h"

namespace Helpers
{
	// Install DirectSoundCreate hooks when dsound.dll is loaded. Safe to call repeatedly.
	void InitSoundHook();

	bool IsSoundHookActive();

	// Start capturing DS activity during reload→eject; clear mute bookkeeping.
	void BeginActiveSoundCapture();

	// Drop remembered mute/capture state (e.g. on weapon change).
	void ClearRememberedReloadSounds();

	// Identify reload via weapon→sound tag → SoundsGlobal → playback pool; volume-mute that buffer.
	void PauseActiveSounds(WeaponType weaponType);

	// Restore identity bookkeeping; resume gated DS buffer (seek by skipSeconds).
	void ClearActiveSounds(float skipSeconds = 0.0f);

	// Cancel pause state when a reload cycle is aborted.
	void ResumeActiveSounds(bool bStopActiveSources = false);
}

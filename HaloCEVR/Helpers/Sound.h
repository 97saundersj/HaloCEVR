#pragma once

#include "../WeaponHandler.h"

namespace Helpers
{
	// Install DirectSoundCreate hooks when dsound.dll is loaded. Safe to call repeatedly.
	void InitSoundHook();

	bool IsSoundHookActive();

	// Start capturing DS activity during reload→eject; clear mute bookkeeping.
	void BeginActiveSoundCapture();

	// Record NEW weapon-parented sound tags while the reload anim plays (before eject pause).
	// Tags already active at capture begin are ignored so leftover companions cannot win.
	void SnapshotCaptureWeaponTags();

	// Drop remembered mute/capture state (e.g. on weapon change).
	void ClearRememberedReloadSounds();

	// Identify reload via weapon→tag → SoundsGlobal slot → playback pool; gate that DS buffer.
	void PauseActiveSounds(WeaponType weaponType);

	// Restore identity bookkeeping; resume gated DS buffer (seek by skipSeconds).
	void ClearActiveSounds(float skipSeconds = 0.0f);

	// Cancel pause state when a reload cycle is aborted.
	void ResumeActiveSounds(bool bStopActiveSources = false);
}

#pragma once

#include "../WeaponHandler.h"

namespace Helpers
{
	// Install DirectSoundCreate hooks when dsound.dll is loaded. Safe to call repeatedly.
	void InitSoundHook();

	bool IsSoundHookActive();

	// Start capturing DS activity during reload→eject; clear mute bookkeeping.
	void BeginActiveSoundCapture();

	// Record/refresh weapon-parented sound tags while the reload anim plays (before eject).
	// Freezes the start-time DS buffer on first resolve; later snapshots only fill null binds.
	void SnapshotCaptureWeaponTags();

	// Drop remembered mute/capture state (e.g. on weapon change).
	void ClearRememberedReloadSounds();

	// Called when Halo starts a sound (new SoundsGlobal slot with tag+parent).
	void OnSoundStarted(int slot, uint32_t tagId);

	// Called when Halo assigns a SoundsGlobal slot to a playback-pool channel (entry+0x8C).
	void OnSoundChannelAssigned(short slot);

	// Prefer SoundStart→channel-assign bind; gate that DS buffer (fallbacks if needed).
	void PauseActiveSounds(WeaponType weaponType);

	// Restore identity bookkeeping; resume gated DS buffer (seek by skipSeconds).
	void ClearActiveSounds(float skipSeconds = 0.0f);

	// Cancel pause state when a reload cycle is aborted.
	void ResumeActiveSounds(bool bStopActiveSources = false);
}

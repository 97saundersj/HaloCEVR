#pragma once
#include "Objects.h"

namespace Helpers
{
	// Start capturing the reload sound's DirectSound buffer(s); call just before the reload SFX begins.
	void BeginPhysicalReloadSoundCapture();
	// Mute/freeze reload sounds while paused at magazine eject (DirectSound/DSOAL + retail manager fallback).
	void PausePhysicalReloadSounds();
	// Resume audio after insert; stops frozen reload sources when using DSOAL.
	void ClearPhysicalReloadSounds();
	// Unsuspend DirectSound output when physical reload is cancelled or aborted.
	void ResumePhysicalReloadSounds(bool bStopActiveSources = false);
}

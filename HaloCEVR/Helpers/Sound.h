#pragma once

namespace Helpers
{
	// Capture DirectSound buffers that start playing after this call.
	void BeginActiveSoundCapture();

	// Mute/freeze captured (and retail-manager fallback) sounds after stopDelayMs.
	void PauseActiveSounds(unsigned int stopDelayMs);

	// Resume captured buffers from their stopped position, then clear pause state.
	void ClearActiveSounds();

	// Unsuspend output when a capture session is cancelled or aborted.
	void ResumeActiveSounds(bool bStopActiveSources = false);
}

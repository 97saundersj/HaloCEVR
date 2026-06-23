#pragma once

namespace Helpers
{
	namespace DirectSoundHook
	{
		// Install DirectSoundCreate hooks when dsound.dll is loaded. Safe to call repeatedly.
		void Init();

			bool IsActive();

			// Start recording which buffer(s) the reload sound plays on (call before reload SFX starts).
			void BeginCapture();

			// Block reload-SFX re-triggers immediately and stop the captured reload buffer(s)
			// after stopDelayMs, so the start of the reload sound is still heard. Called every
			// frame during PausedAtEject; only the first call latches the timer.
			void EnterPause(unsigned int stopDelayMs);

			// Resume normal playback; optionally stop active buffers first (insert finish).
			void ExitPause(bool bStopActive);

			// On magazine insert: replay the captured reload buffer(s) from their stopped position
			// so the tail of the reload sound plays, then clear pause state.
			void ResumeReloadSound();
	}
}

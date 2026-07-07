#pragma once

namespace Helpers
{
	namespace DirectSoundHook
	{
		// Install DirectSoundCreate hooks when dsound.dll is loaded. Safe to call repeatedly.
		void Init();

		bool IsActive();

		// Start recording which buffer(s) begin playing after this call.
		void BeginCapture();

		// Block captured-buffer re-triggers immediately and stop captured buffers after
		// stopDelayMs so the start of the sound is still heard. Safe to call every frame;
		// only the first call latches the timer.
		void EnterPause(unsigned int stopDelayMs);

		// Resume normal playback; optionally stop active buffers first.
		void ExitPause(bool bStopActive);

		// Replay captured buffers from their stopped position, then clear pause state.
		void ResumeCaptured();
	}
}

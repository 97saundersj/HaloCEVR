#include "DirectSoundHook.h"
#include "../Logger.h"
#include "../Hooking/Hook.h"
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <mmsystem.h>
#include <dsound.h>
#include <algorithm>
#include <mutex>
#include <vector>

#pragma comment(lib, "dsound.lib")

namespace
{
	// IDirectSound(8) vtable: QueryInterface=0, AddRef=1, Release=2, CreateSoundBuffer=3, ...
	constexpr int kDeviceVtableCreateSoundBuffer = 3;
	// IDirectSoundBuffer(8) vtable order (after IUnknown):
	// Release=2, GetStatus=9, Play=12, Stop=18.
	constexpr int kBufferVtableRelease = 2;
	constexpr int kBufferVtableGetStatus = 9;
	constexpr int kBufferVtablePlay = 12;
	constexpr int kBufferVtableStop = 18;

	HRESULT STDMETHODCALLTYPE HookedCreateSoundBuffer(IDirectSound8* device, LPCDSBUFFERDESC pcBufferDesc, LPDIRECTSOUNDBUFFER* ppBuffer, LPUNKNOWN pUnkOuter);
	HRESULT STDMETHODCALLTYPE HookedBufferPlay(IDirectSoundBuffer* self, DWORD dwReserved1, DWORD dwPriority, DWORD dwFlags);
	HRESULT STDMETHODCALLTYPE HookedBufferStop(IDirectSoundBuffer* self);
	ULONG STDMETHODCALLTYPE HookedBufferRelease(IDirectSoundBuffer* self);

	void RegisterBuffer(IDirectSoundBuffer* buffer);

	using DirectSoundCreate8_t = HRESULT(WINAPI*)(LPCGUID, LPDIRECTSOUND8*, LPUNKNOWN);
	using DirectSoundCreate_t = HRESULT(WINAPI*)(LPCGUID, LPDIRECTSOUND*, LPUNKNOWN);
	using CreateSoundBuffer_t = HRESULT(STDMETHODCALLTYPE*)(IDirectSound8*, LPCDSBUFFERDESC, LPDIRECTSOUNDBUFFER*, LPUNKNOWN);
	using BufferPlay_t = HRESULT(STDMETHODCALLTYPE*)(IDirectSoundBuffer*, DWORD, DWORD, DWORD);
	using BufferStop_t = HRESULT(STDMETHODCALLTYPE*)(IDirectSoundBuffer*);
	using BufferRelease_t = ULONG(STDMETHODCALLTYPE*)(IDirectSoundBuffer*);
	using BufferGetStatus_t = HRESULT(STDMETHODCALLTYPE*)(IDirectSoundBuffer*, DWORD*);

	DirectSoundCreate8_t g_origDirectSoundCreate8 = nullptr;
	DirectSoundCreate_t g_origDirectSoundCreate = nullptr;
	CreateSoundBuffer_t g_origCreateSoundBuffer = nullptr;
	BufferPlay_t g_origBufferPlay = nullptr;
	BufferStop_t g_origBufferStop = nullptr;
	BufferRelease_t g_origBufferRelease = nullptr;
	BufferGetStatus_t g_origBufferGetStatus = nullptr;

	std::mutex g_mutex;
	std::vector<IDirectSoundBuffer*> g_trackedBuffers;
	// Buffers the reload sound played on, captured during the eject phase so we can silence
	// only those during the pause instead of stopping/blocking all game audio.
	std::vector<IDirectSoundBuffer*> g_reloadBuffers;

	bool g_hooksInstalled = false;
	bool g_initAttempted = false;
	bool g_deviceVtablePatched = false;
	bool g_bufferVtablePatched = false;
	bool g_captureReloadBuffers = false;
	bool g_suppressReloadBuffers = false;
	bool g_inPause = false;
	bool g_debugLogging = false;
	// Defer the actual stop so the start of the captured SFX is still heard before it cuts out.
	bool g_reloadStopped = false;
	ULONGLONG g_pauseStartTick = 0;
	unsigned int g_stopDelayMs = 0;
	IDirectSound8* g_soundDevice8 = nullptr;
	IDirectSound* g_soundDeviceLegacy = nullptr;

	bool IsValidComObjectPointer(void* ptr)
	{
		if (!ptr)
		{
			return false;
		}

		const uintptr_t address = reinterpret_cast<uintptr_t>(ptr);
		if (address < 0x10000 || (address & 0x3) != 0)
		{
			return false;
		}

		void** vtable = *reinterpret_cast<void***>(ptr);
		if (!vtable)
		{
			return false;
		}

		const uintptr_t vtableAddress = reinterpret_cast<uintptr_t>(vtable);
		return vtableAddress >= 0x10000;
	}

	void ReleaseStoredDevices()
	{
		if (g_soundDevice8)
		{
			g_soundDevice8->Release();
			g_soundDevice8 = nullptr;
		}

		if (g_soundDeviceLegacy)
		{
			g_soundDeviceLegacy->Release();
			g_soundDeviceLegacy = nullptr;
		}
	}

	void StoreSoundDevice8(IDirectSound8* device)
	{
		if (!device)
		{
			return;
		}

		ReleaseStoredDevices();
		g_soundDevice8 = device;
		g_soundDevice8->AddRef();
	}

	void StoreSoundDeviceLegacy(IDirectSound* device)
	{
		if (!device)
		{
			return;
		}

		ReleaseStoredDevices();
		g_soundDeviceLegacy = device;
		g_soundDeviceLegacy->AddRef();
	}

	bool PatchVtableEntry(void** vtable, int index, void* replacement, void** outOriginal)
	{
		if (!vtable || !replacement)
		{
			return false;
		}

		if (outOriginal)
		{
			*outOriginal = vtable[index];
		}

		DWORD oldProtect = 0;
		if (!VirtualProtect(&vtable[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect))
		{
			return false;
		}

		vtable[index] = replacement;
		VirtualProtect(&vtable[index], sizeof(void*), oldProtect, &oldProtect);
		return true;
	}

	void RegisterBuffer(IDirectSoundBuffer* buffer)
	{
		if (!IsValidComObjectPointer(buffer))
		{
			return;
		}

		std::lock_guard<std::mutex> lock(g_mutex);

		if (!g_bufferVtablePatched)
		{
			void** vtable = *reinterpret_cast<void***>(buffer);
			void* originalPlay = nullptr;
			void* originalStop = nullptr;
			void* originalRelease = nullptr;

			if (!PatchVtableEntry(vtable, kBufferVtablePlay, reinterpret_cast<void*>(&HookedBufferPlay), &originalPlay)
				|| !PatchVtableEntry(vtable, kBufferVtableStop, reinterpret_cast<void*>(&HookedBufferStop), &originalStop)
				|| !PatchVtableEntry(vtable, kBufferVtableRelease, reinterpret_cast<void*>(&HookedBufferRelease), &originalRelease))
			{
				return;
			}

			g_origBufferPlay = reinterpret_cast<BufferPlay_t>(originalPlay);
			g_origBufferStop = reinterpret_cast<BufferStop_t>(originalStop);
			g_origBufferRelease = reinterpret_cast<BufferRelease_t>(originalRelease);
			g_origBufferGetStatus = reinterpret_cast<BufferGetStatus_t>(vtable[kBufferVtableGetStatus]);
			g_bufferVtablePatched = true;
		}

		if (std::find(g_trackedBuffers.begin(), g_trackedBuffers.end(), buffer) == g_trackedBuffers.end())
		{
			g_trackedBuffers.push_back(buffer);
		}
	}

	void PatchDeviceCreateSoundBufferHook(IUnknown* device)
	{
		if (!device || g_deviceVtablePatched)
		{
			return;
		}

		void** vtable = *reinterpret_cast<void***>(device);
		void* originalCreate = nullptr;
		if (!PatchVtableEntry(vtable, kDeviceVtableCreateSoundBuffer, reinterpret_cast<void*>(&HookedCreateSoundBuffer), &originalCreate))
		{
			return;
		}

		g_origCreateSoundBuffer = reinterpret_cast<CreateSoundBuffer_t>(originalCreate);
		g_deviceVtablePatched = true;
	}

	void OnSoundDeviceCreated8(IDirectSound8* device)
	{
		if (!device)
		{
			return;
		}

		StoreSoundDevice8(device);
		PatchDeviceCreateSoundBufferHook(device);
	}

	void OnSoundDeviceCreatedLegacy(IDirectSound* device)
	{
		if (!device)
		{
			return;
		}

		StoreSoundDeviceLegacy(device);
		PatchDeviceCreateSoundBufferHook(device);
	}

	HRESULT STDMETHODCALLTYPE HookedCreateSoundBuffer(
		IDirectSound8* device,
		LPCDSBUFFERDESC pcBufferDesc,
		LPDIRECTSOUNDBUFFER* ppBuffer,
		LPUNKNOWN pUnkOuter)
	{
		if (!g_origCreateSoundBuffer)
		{
			return DSERR_GENERIC;
		}

		const HRESULT result = g_origCreateSoundBuffer(device, pcBufferDesc, ppBuffer, pUnkOuter);
		if (SUCCEEDED(result) && ppBuffer && IsValidComObjectPointer(*ppBuffer))
		{
			// Skip the primary mixing buffer; we only track secondary (per-sound) buffers.
			const bool bIsPrimary = pcBufferDesc && (pcBufferDesc->dwFlags & DSBCAPS_PRIMARYBUFFER) != 0;
			if (!bIsPrimary)
			{
				RegisterBuffer(*ppBuffer);
			}
		}

		return result;
	}

	HRESULT STDMETHODCALLTYPE HookedBufferPlay(IDirectSoundBuffer* self, DWORD dwReserved1, DWORD dwPriority, DWORD dwFlags)
	{
		if (g_captureReloadBuffers)
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			if (std::find(g_reloadBuffers.begin(), g_reloadBuffers.end(), self) == g_reloadBuffers.end())
			{
				g_reloadBuffers.push_back(self);
			}
		}
		else if (g_suppressReloadBuffers)
		{
			bool bIsReloadBuffer = false;
			{
				std::lock_guard<std::mutex> lock(g_mutex);
				bIsReloadBuffer = std::find(g_reloadBuffers.begin(), g_reloadBuffers.end(), self) != g_reloadBuffers.end();
			}

			if (bIsReloadBuffer)
			{
				// Block re-triggers of the reload SFX while paused; leave all other audio alone.
				return DS_OK;
			}
		}

		return g_origBufferPlay ? g_origBufferPlay(self, dwReserved1, dwPriority, dwFlags) : DSERR_GENERIC;
	}

	HRESULT STDMETHODCALLTYPE HookedBufferStop(IDirectSoundBuffer* self)
	{
		return g_origBufferStop ? g_origBufferStop(self) : DSERR_GENERIC;
	}

	ULONG STDMETHODCALLTYPE HookedBufferRelease(IDirectSoundBuffer* self)
	{
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			g_trackedBuffers.erase(
				std::remove(g_trackedBuffers.begin(), g_trackedBuffers.end(), self),
				g_trackedBuffers.end());
			g_reloadBuffers.erase(
				std::remove(g_reloadBuffers.begin(), g_reloadBuffers.end(), self),
				g_reloadBuffers.end());
		}

		return g_origBufferRelease ? g_origBufferRelease(self) : 0;
	}

	HRESULT WINAPI HookedDirectSoundCreate8(LPCGUID lpcGuidDevice, LPDIRECTSOUND8* ppDS8, LPUNKNOWN pUnkOuter)
	{
		const HRESULT result = g_origDirectSoundCreate8(lpcGuidDevice, ppDS8, pUnkOuter);
		if (SUCCEEDED(result) && ppDS8 && *ppDS8)
		{
			OnSoundDeviceCreated8(*ppDS8);
		}

		return result;
	}

	HRESULT WINAPI HookedDirectSoundCreate(LPCGUID lpcGuidDevice, LPDIRECTSOUND* ppDS, LPUNKNOWN pUnkOuter)
	{
		const HRESULT result = g_origDirectSoundCreate(lpcGuidDevice, ppDS, pUnkOuter);
		if (SUCCEEDED(result) && ppDS && *ppDS)
		{
			OnSoundDeviceCreatedLegacy(*ppDS);
		}

		return result;
	}

	int StopReloadBuffers()
	{
		std::lock_guard<std::mutex> lock(g_mutex);

		if (!g_origBufferGetStatus || !g_origBufferStop)
		{
			return 0;
		}

		int stoppedCount = 0;
		for (IDirectSoundBuffer* buffer : g_reloadBuffers)
		{
			if (!IsValidComObjectPointer(buffer))
			{
				continue;
			}

			DWORD status = 0;
			if (SUCCEEDED(g_origBufferGetStatus(buffer, &status)) && (status & DSBSTATUS_PLAYING))
			{
				g_origBufferStop(buffer);
				stoppedCount++;
			}
		}

		return stoppedCount;
	}

	void LogInitStatus(bool bSuccess, const char* detail)
	{
		if (!g_debugLogging)
		{
			return;
		}

		Logger::log << "[DirectSoundHook] "
			<< (bSuccess ? "installed" : "failed")
			<< " (" << detail << ")" << std::endl;
	}
}

void Helpers::DirectSoundHook::Init()
{
	if (g_hooksInstalled)
	{
		return;
	}

	const HMODULE dsoundModule = GetModuleHandleA("dsound.dll");
	if (!dsoundModule)
	{
		return;
	}

	if (g_initAttempted)
	{
		return;
	}

	g_initAttempted = true;

	const HMODULE dsoalDriver = GetModuleHandleA("dsoal-aldrv.dll");
	if (g_debugLogging)
	{
		Logger::log << "[DirectSoundHook] dsound.dll=0x" << std::hex << dsoundModule
			<< " dsoal-aldrv.dll=0x" << dsoalDriver << std::dec << std::endl;
	}

	auto createExportHook = [dsoundModule](const char* exportName, LPVOID detour, LPVOID* original) -> bool
	{
		FARPROC exportAddress = GetProcAddress(dsoundModule, exportName);
		if (!exportAddress)
		{
			return false;
		}

		if (MH_CreateHook(exportAddress, detour, original) != MH_OK)
		{
			return false;
		}

		return MH_EnableHook(exportAddress) == MH_OK;
	};

	bool bCreate8Hooked = createExportHook(
		"DirectSoundCreate8",
		reinterpret_cast<LPVOID>(&HookedDirectSoundCreate8),
		reinterpret_cast<LPVOID*>(&g_origDirectSoundCreate8));

	bool bCreateHooked = createExportHook(
		"DirectSoundCreate",
		reinterpret_cast<LPVOID>(&HookedDirectSoundCreate),
		reinterpret_cast<LPVOID*>(&g_origDirectSoundCreate));

	g_hooksInstalled = bCreate8Hooked || bCreateHooked;
	if (!g_hooksInstalled)
	{
		g_initAttempted = false;
	}
	LogInitStatus(g_hooksInstalled, g_hooksInstalled ? "DirectSoundCreate8/Create hooked" : "no exports hooked");
}

bool Helpers::DirectSoundHook::IsActive()
{
	return g_hooksInstalled;
}

void Helpers::DirectSoundHook::SetDebugLogging(bool enabled)
{
	g_debugLogging = enabled;
}

void Helpers::DirectSoundHook::BeginCapture()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	g_reloadBuffers.clear();
	g_captureReloadBuffers = true;
}

void Helpers::DirectSoundHook::EnterPause(unsigned int stopDelayMs)
{
	Init();

	g_captureReloadBuffers = false;

	if (!g_inPause)
	{
		// On entry: block captured-buffer re-triggers immediately, but let buffers that are
		// already playing keep going until the delay elapses so the start of the sound is heard.
		g_inPause = true;
		g_suppressReloadBuffers = true;
		g_reloadStopped = false;
		g_stopDelayMs = stopDelayMs;
		g_pauseStartTick = GetTickCount64();
	}

	if (g_reloadStopped)
	{
		return;
	}

	if ((GetTickCount64() - g_pauseStartTick) < g_stopDelayMs)
	{
		return;
	}

	g_reloadStopped = true;
	const int stoppedCount = StopReloadBuffers();

	if (g_debugLogging)
	{
		size_t reloadCount = 0;
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			reloadCount = g_reloadBuffers.size();
		}

		Logger::log << "[DirectSoundHook] pause stoppedBuffers="
			<< stoppedCount << " capturedBuffers=" << reloadCount
			<< " delayMs=" << g_stopDelayMs << std::endl;
	}
}

void Helpers::DirectSoundHook::ExitPause(bool bStopActive)
{
	int stoppedCount = 0;
	if (bStopActive)
	{
		stoppedCount = StopReloadBuffers();
	}

	g_inPause = false;
	g_suppressReloadBuffers = false;
	g_captureReloadBuffers = false;
	g_reloadStopped = false;
	g_pauseStartTick = 0;
	g_stopDelayMs = 0;

	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_reloadBuffers.clear();
	}

	if (bStopActive && g_debugLogging)
	{
		Logger::log << "[DirectSoundHook] resume stoppedBuffers=" << stoppedCount << std::endl;
	}
}

void Helpers::DirectSoundHook::ResumeCaptured()
{
	int resumedCount = 0;

	{
		std::lock_guard<std::mutex> lock(g_mutex);

		// DirectSound keeps each buffer's play position after Stop, so calling Play again here
		// continues the sound from where it was cut off instead of restarting it. Buffers still
		// playing (resume before the stop delay elapsed) are left untouched so they finish
		// naturally. Use the original Play to bypass our hook entirely.
		if (g_origBufferPlay)
		{
			for (IDirectSoundBuffer* buffer : g_reloadBuffers)
			{
				if (!IsValidComObjectPointer(buffer))
				{
					continue;
				}

				DWORD status = 0;
				const bool bPlaying = g_origBufferGetStatus
					&& SUCCEEDED(g_origBufferGetStatus(buffer, &status))
					&& (status & DSBSTATUS_PLAYING);

				if (!bPlaying)
				{
					g_origBufferPlay(buffer, 0, 0, 0);
					resumedCount++;
				}
			}
		}

		g_reloadBuffers.clear();
	}

	g_inPause = false;
	g_suppressReloadBuffers = false;
	g_captureReloadBuffers = false;
	g_reloadStopped = false;
	g_pauseStartTick = 0;
	g_stopDelayMs = 0;

	if (g_debugLogging)
	{
		Logger::log << "[DirectSoundHook] resume captured resumedBuffers=" << resumedCount << std::endl;
	}
}

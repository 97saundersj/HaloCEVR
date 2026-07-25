#include "Sound.h"
#include "../Hooking/Hooks.h"
#include "../Hooking/Hook.h"
#include "../Logger.h"
#include "Objects.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <mmsystem.h>
#include <dsound.h>

#pragma comment(lib, "dsound.lib")

namespace
{
	// --- SoundsGlobal: identity + volume mute backup -------------------------

	constexpr int kSoundEntrySize = 176;
	constexpr int kMaxSoundSlots = 100;
	constexpr int kSoundManagerActiveCountOffset = 48;
	constexpr int kSoundManagerSoundArrayOffset = 52;
	constexpr int kSoundTagIdOffset = 8;
	constexpr int kSoundParentOffset = 12;
	constexpr int kSoundScaleOffset = 24;
	constexpr int kSoundGainOffset = 28;
	constexpr int kSoundOrderIdOffset = 140;
	constexpr int kSoundFirstPersonOffset = 172;
	constexpr uint16_t kSoundNotPlaying = 0xFFFF;
	constexpr int kMaxGatedReloadBuffers = 1;

	struct MutedRetailSound
	{
		int slot = -1;
		uint32_t tagId = 0;
		float savedScale = 1.0f;
		float savedGain = 1.0f;
		uintptr_t entry = 0;
	};

	std::atomic_bool g_inPause = false;
	std::atomic_bool g_loggedRetailThisPause = false;
	std::atomic_bool g_loggedPoolMissThisPause = false;
	std::atomic_bool g_retailIdentityKnown = false;
	std::vector<MutedRetailSound> g_mutedRetailSounds;
	uint32_t g_primaryMuteTagId = 0;
	WeaponType g_pauseWeaponType = WeaponType::Unknown;

	// WeaponType → Halo reload sound tag. Unmapped weapons fail closed (no pause).
	uint32_t GetReloadSoundTagForWeapon(WeaponType weaponType)
	{
		switch (weaponType)
		{
		case WeaponType::Pistol:
			return 0xe36401f0;
		default:
			return 0;
		}
	}

	// WeaponType → reload permutation DS buffer fingerprint (sample rate + buffer bytes).
	// This is how we deterministically identify the audible reload buffer(s): Halo creates
	// one DS buffer per loaded sound permutation, so a playing buffer whose capacity equals
	// the reload clip size IS a reload voice. Ambient/other sounds have different sizes and
	// are never touched. Unmapped weapons return false = no gating.
	bool GetReloadFingerprintForWeapon(WeaponType weaponType, DWORD& outRate, DWORD& outBytes)
	{
		switch (weaponType)
		{
		case WeaponType::Pistol:
			outRate = 22050;
			outBytes = 132300;
			return true;
		default:
			outRate = 0;
			outBytes = 0;
			return false;
		}
	}

	bool IsValidSoundTagId(uint32_t tagId)
	{
		return tagId != 0 && tagId != 0xffffffffu;
	}

	// Forward decls (DS helpers defined later).
	bool IsValidComObjectPointer(void* ptr);
	void SnapshotPlayingAtMute();
	void ProbeChannelForTrackedBuffer(uintptr_t entry);
	// One-shot diagnostic: find where the reload's SoundsGlobal identity is stored inside
	// the playback pool channel struct, so we can bridge channel -> DS buffer deterministically.
	void DiagnosePlaybackPoolIdentity(int matchSlot, uintptr_t matchEntry, uint32_t tag, uint32_t parent, WeaponType weaponType);
	// One-shot diagnostic: chase pointers from each active pool channel to find the reload tag,
	// exposing the deterministic channel<->sound bridge.
	void DiagnoseChannelSourceIdentity(uint32_t expectedTag);

	uint32_t PackObjectId(const HaloID& id)
	{
		uint32_t packed = 0;
		memcpy(&packed, &id, sizeof(packed));
		return packed;
	}

	bool MatchesSoundParent(uint32_t parentId, const HaloID& objectId)
	{
		if (parentId == 0 || parentId == 0xFFFFFFFF)
		{
			return false;
		}

		const uint32_t packed = PackObjectId(objectId);
		if (parentId == packed)
		{
			return true;
		}

		return (parentId & 0xFFFF) == objectId.index;
	}

	uintptr_t GetSoundManagerBase()
	{
		const uintptr_t globalAddress = static_cast<uintptr_t>(Hooks::o.SoundsGlobal);
		if (!globalAddress)
		{
			static bool loggedNullGlobal = false;
			if (!loggedNullGlobal)
			{
				loggedNullGlobal = true;
				Logger::log << "[Sound] SoundsGlobal is null" << std::endl;
			}
			return 0;
		}

		const uintptr_t manager = *reinterpret_cast<uintptr_t*>(globalAddress);
		if (!manager)
		{
			return 0;
		}

		const uintptr_t soundArray = *reinterpret_cast<uintptr_t*>(manager + kSoundManagerSoundArrayOffset);
		return soundArray ? manager : 0;
	}

	uintptr_t GetSoundManager()
	{
		const uintptr_t manager = GetSoundManagerBase();
		if (!manager)
		{
			return 0;
		}

		const uint16_t activeCount = *reinterpret_cast<uint16_t*>(manager + kSoundManagerActiveCountOffset);
		return activeCount > 0 ? manager : 0;
	}

	template<typename Fn>
	void ForEachActiveSound(uintptr_t manager, Fn callback)
	{
		const uint16_t activeCount = *reinterpret_cast<uint16_t*>(manager + kSoundManagerActiveCountOffset);
		const uintptr_t soundArray = *reinterpret_cast<uintptr_t*>(manager + kSoundManagerSoundArrayOffset);
		if (!soundArray || activeCount == 0)
		{
			return;
		}

		int seen = 0;
		for (int i = 0; i < kMaxSoundSlots && seen < activeCount; i++)
		{
			const uintptr_t entry = soundArray + static_cast<uintptr_t>(i) * kSoundEntrySize;
			const uint16_t orderId = *reinterpret_cast<uint16_t*>(entry + kSoundOrderIdOffset);
			if (orderId == kSoundNotPlaying)
			{
				continue;
			}

			seen++;
			callback(i, entry);
		}
	}

	bool IsLocalPlayerOrWeaponSound(uintptr_t entry, const HaloID& playerId, const HaloID& weaponId)
	{
		if (*reinterpret_cast<int32_t*>(entry + kSoundFirstPersonOffset) != 0)
		{
			return true;
		}

		const uint32_t parentId = *reinterpret_cast<uint32_t*>(entry + kSoundParentOffset);
		return MatchesSoundParent(parentId, playerId) || MatchesSoundParent(parentId, weaponId);
	}

	uintptr_t GetSoundEntry(uintptr_t manager, int slot)
	{
		if (!manager || slot < 0 || slot >= kMaxSoundSlots)
		{
			return 0;
		}

		const uintptr_t soundArray = *reinterpret_cast<uintptr_t*>(manager + kSoundManagerSoundArrayOffset);
		if (!soundArray)
		{
			return 0;
		}

		return soundArray + static_cast<uintptr_t>(slot) * kSoundEntrySize;
	}

	MutedRetailSound* FindMutedSlot(int slot)
	{
		for (MutedRetailSound& muted : g_mutedRetailSounds)
		{
			if (muted.slot == slot)
			{
				return &muted;
			}
		}
		return nullptr;
	}

	void MuteSoundEntry(uintptr_t entry, int slot, uint32_t tagId)
	{
		if (!entry)
		{
			return;
		}

		float* scale = reinterpret_cast<float*>(entry + kSoundScaleOffset);
		float* gain = reinterpret_cast<float*>(entry + kSoundGainOffset);

		if (!FindMutedSlot(slot))
		{
			MutedRetailSound muted;
			muted.slot = slot;
			muted.tagId = tagId;
			muted.savedScale = (*scale > 0.0f) ? *scale : 1.0f;
			muted.savedGain = (*gain > 0.0f) ? *gain : 1.0f;
			muted.entry = entry;
			g_mutedRetailSounds.push_back(muted);
		}

		g_retailIdentityKnown.store(true, std::memory_order_release);

		// Do NOT zero retail gain/scale — that marks the Halo channel finished
		// (finishedWhileMuted) and tears down voices. Silence is DS SetVolume only.
	}

	int RestoreMutedRetailSounds()
	{
		const size_t pending = g_mutedRetailSounds.size();
		const uintptr_t manager = GetSoundManagerBase();
		if (!manager || pending == 0)
		{
			if (pending > 0 || manager == 0)
			{
				Logger::log << "[Sound] retail restore skip pending=" << pending
					<< " manager=" << (manager ? 1 : 0) << std::endl;
			}
			g_mutedRetailSounds.clear();
			return 0;
		}

		int aliveCount = 0;
		int finishedCount = 0;
		int tagMismatchCount = 0;

		for (const MutedRetailSound& muted : g_mutedRetailSounds)
		{
			const uintptr_t entry = GetSoundEntry(manager, muted.slot);
			if (!entry)
			{
				continue;
			}

			const uint16_t orderId = *reinterpret_cast<uint16_t*>(entry + kSoundOrderIdOffset);
			if (orderId == kSoundNotPlaying)
			{
				finishedCount++;
				continue;
			}

			const uint32_t tagId = *reinterpret_cast<uint32_t*>(entry + kSoundTagIdOffset);
			if (tagId != muted.tagId)
			{
				tagMismatchCount++;
				continue;
			}

			aliveCount++;
		}

		Logger::log << "[Sound] retail identity alive=" << aliveCount
			<< " finishedWhilePaused=" << finishedCount
			<< " tagMismatch=" << tagMismatchCount
			<< " pending=" << pending
			<< std::endl;

		g_mutedRetailSounds.clear();
		return aliveCount;
	}

	void MuteLocalRetailSounds(WeaponType weaponType)
	{
		const uint32_t expectedTag = GetReloadSoundTagForWeapon(weaponType);
		if (!IsValidSoundTagId(expectedTag))
		{
			bool bExpected = false;
			if (g_loggedRetailThisPause.compare_exchange_strong(bExpected, true, std::memory_order_acq_rel))
			{
				Logger::log << "[Sound] retail skip unmapped weaponType="
					<< static_cast<int>(weaponType) << std::endl;
			}
			return;
		}

		const uintptr_t manager = GetSoundManager();
		if (!manager)
		{
			return;
		}

		HaloID playerId{};
		if (!Helpers::GetLocalPlayerID(playerId))
		{
			return;
		}

		HaloID weaponId{};
		BaseDynamicObject* player = Helpers::GetLocalPlayer();
		if (player && player->weapon.id != 0xffff)
		{
			weaponId = player->weapon;
		}

		const uint16_t activeCount = *reinterpret_cast<uint16_t*>(manager + kSoundManagerActiveCountOffset);
		g_primaryMuteTagId = expectedTag;

		int matchSlot = -1;
		uintptr_t matchEntry = 0;
		uint32_t matchParent = 0;
		int32_t matchFp = 0;
		int localCount = 0;

		ForEachActiveSound(manager, [&](int slot, uintptr_t entry)
		{
			if (!IsLocalPlayerOrWeaponSound(entry, playerId, weaponId))
			{
				return;
			}

			localCount++;
			const uint32_t tagId = *reinterpret_cast<uint32_t*>(entry + kSoundTagIdOffset);
			if (!IsValidSoundTagId(tagId) || tagId != expectedTag)
			{
				return;
			}

			// First exact tag match wins (one reload channel).
			if (matchSlot < 0)
			{
				matchSlot = slot;
				matchEntry = entry;
				matchParent = *reinterpret_cast<uint32_t*>(entry + kSoundParentOffset);
				matchFp = *reinterpret_cast<int32_t*>(entry + kSoundFirstPersonOffset);
			}
		});

		bool bExpectedMute = false;
		if (g_loggedRetailThisPause.compare_exchange_strong(bExpectedMute, true, std::memory_order_acq_rel))
		{
			Logger::log << "[Sound] retail active=" << activeCount
				<< " local=" << localCount
				<< " expectedTag=0x" << std::hex << expectedTag << std::dec
				<< " weaponType=" << static_cast<int>(weaponType)
				<< std::endl;

			if (matchSlot >= 0)
			{
				Logger::log << "[Sound] retail match slot=" << matchSlot
					<< " tag=0x" << std::hex << expectedTag
					<< " parent=0x" << matchParent << std::dec
					<< " fp=" << matchFp
					<< std::endl;
			}
			else
			{
				Logger::log << "[Sound] retail no channel for expectedTag=0x"
					<< std::hex << expectedTag << std::dec << std::endl;
			}
		}

		if (matchSlot < 0 || !matchEntry)
		{
			return;
		}

		MuteSoundEntry(matchEntry, matchSlot, expectedTag);
	}

	// --- DirectSound: playhead for retail-identified reload only --------------

	constexpr int kDeviceVtableCreateSoundBuffer = 3;
	constexpr int kBufferVtableRelease = 2;
	constexpr int kBufferVtableGetStatus = 9;
	constexpr int kBufferVtablePlay = 12;
	constexpr int kBufferVtableSetCurrentPosition = 13;
	constexpr int kBufferVtableSetVolume = 15;
	constexpr int kBufferVtableStop = 18;

	HRESULT STDMETHODCALLTYPE HookedCreateSoundBuffer(IDirectSound8* device, LPCDSBUFFERDESC pcBufferDesc, LPDIRECTSOUNDBUFFER* ppBuffer, LPUNKNOWN pUnkOuter);
	HRESULT STDMETHODCALLTYPE HookedBufferPlay(IDirectSoundBuffer* self, DWORD dwReserved1, DWORD dwPriority, DWORD dwFlags);
	HRESULT STDMETHODCALLTYPE HookedBufferSetCurrentPosition(IDirectSoundBuffer* self, DWORD dwNewPosition);
	HRESULT STDMETHODCALLTYPE HookedBufferSetVolume(IDirectSoundBuffer* self, LONG lVolume);
	HRESULT STDMETHODCALLTYPE HookedBufferStop(IDirectSoundBuffer* self);
	ULONG STDMETHODCALLTYPE HookedBufferRelease(IDirectSoundBuffer* self);

	using DirectSoundCreate8_t = HRESULT(WINAPI*)(LPCGUID, LPDIRECTSOUND8*, LPUNKNOWN);
	using DirectSoundCreate_t = HRESULT(WINAPI*)(LPCGUID, LPDIRECTSOUND*, LPUNKNOWN);
	using CreateSoundBuffer_t = HRESULT(STDMETHODCALLTYPE*)(IDirectSound8*, LPCDSBUFFERDESC, LPDIRECTSOUNDBUFFER*, LPUNKNOWN);
	using BufferPlay_t = HRESULT(STDMETHODCALLTYPE*)(IDirectSoundBuffer*, DWORD, DWORD, DWORD);
	using BufferSetCurrentPosition_t = HRESULT(STDMETHODCALLTYPE*)(IDirectSoundBuffer*, DWORD);
	using BufferSetVolume_t = HRESULT(STDMETHODCALLTYPE*)(IDirectSoundBuffer*, LONG);
	using BufferStop_t = HRESULT(STDMETHODCALLTYPE*)(IDirectSoundBuffer*);
	using BufferRelease_t = ULONG(STDMETHODCALLTYPE*)(IDirectSoundBuffer*);
	using BufferGetStatus_t = HRESULT(STDMETHODCALLTYPE*)(IDirectSoundBuffer*, DWORD*);

	DirectSoundCreate8_t g_origDirectSoundCreate8 = nullptr;
	DirectSoundCreate_t g_origDirectSoundCreate = nullptr;
	CreateSoundBuffer_t g_origCreateSoundBuffer = nullptr;
	BufferPlay_t g_origBufferPlay = nullptr;
	BufferSetCurrentPosition_t g_origBufferSetCurrentPosition = nullptr;
	BufferSetVolume_t g_origBufferSetVolume = nullptr;
	BufferStop_t g_origBufferStop = nullptr;
	BufferRelease_t g_origBufferRelease = nullptr;
	BufferGetStatus_t g_origBufferGetStatus = nullptr;

	std::mutex g_dsMutex;
	std::vector<IDirectSoundBuffer*> g_trackedBuffers;
	// Capture-window candidates; promoted into g_reloadBuffers once retail identity is known.
	std::vector<IDirectSoundBuffer*> g_candidateBuffers;
	std::vector<IDirectSoundBuffer*> g_reloadBuffers;
	// Play cursor recorded per gated reload buffer at the moment it was first gated.
	// Parallel to g_reloadBuffers; resume seeks each buffer from its own frozen cursor.
	std::vector<DWORD> g_reloadCursors;
	// Last successfully gated reload buffer — preseeded into candidates if still playing.
	std::vector<IDirectSoundBuffer*> g_rememberedReloadBuffers;
	// Tracked buffers that were playing at the moment we first muted the retail reload.
	std::vector<IDirectSoundBuffer*> g_playingAtMute;
	bool g_playingAtMuteCaptured = false;
	// Playing/cursor snapshot at capture begin — distinguishes mid-clip reload from eject SFX.
	struct CaptureBaseline
	{
		IDirectSoundBuffer* buffer = nullptr;
		bool bPlaying = false;
		DWORD playCursor = 0;
	};
	std::vector<CaptureBaseline> g_captureBaselines;
	// Format fingerprint from last successful gate (identifies reload among playingAtMute).
	DWORD g_reloadFpRate = 0;
	DWORD g_reloadFpBytes = 0;
	// Play cursor frozen at first pause gate — resume seeks from this, not the live cursor
	// (volume-mute leaves the buffer "playing" so the cursor would otherwise keep advancing).
	DWORD g_pausePlayCursor = 0;
	bool g_pausePlayCursorValid = false;
	bool g_reloadFpValid = false;
	// Buffer found by equality-matching a muted channel dword to a tracked DS buffer.
	IDirectSoundBuffer* g_probedReloadBuffer = nullptr;
	int g_probedReloadOffset = -1;

	bool g_hooksInstalled = false;
	bool g_initAttempted = false;
	bool g_deviceVtablePatched = false;
	bool g_bufferVtablePatched = false;
	bool g_reloadListFinalized = false;
	std::atomic_bool g_captureReloadBuffers = false;
	std::atomic_bool g_suppressReloadBuffers = false;
	std::atomic_bool g_loggedDsPause = false;

	bool IsReadableMemory(const void* ptr, size_t bytes)
	{
		MEMORY_BASIC_INFORMATION mbi{};
		if (!VirtualQuery(ptr, &mbi, sizeof(mbi)))
		{
			return false;
		}

		if (mbi.State != MEM_COMMIT)
		{
			return false;
		}

		const uintptr_t start = reinterpret_cast<uintptr_t>(ptr);
		const uintptr_t end = start + bytes;
		const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
		if (end < start || end > regionEnd)
		{
			return false;
		}

		const DWORD protect = mbi.Protect & 0xFF;
		switch (protect)
		{
		case PAGE_READONLY:
		case PAGE_READWRITE:
		case PAGE_WRITECOPY:
		case PAGE_EXECUTE_READ:
		case PAGE_EXECUTE_READWRITE:
		case PAGE_EXECUTE_WRITECOPY:
			return true;
		default:
			return false;
		}
	}

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

		if (!IsReadableMemory(ptr, sizeof(void*)))
		{
			return false;
		}

		void** vtable = nullptr;
		__try
		{
			vtable = *reinterpret_cast<void***>(ptr);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}

		if (!vtable || reinterpret_cast<uintptr_t>(vtable) < 0x10000)
		{
			return false;
		}

		return IsReadableMemory(vtable, sizeof(void*) * 20);
	}

	// SEH-safe COM helpers (no C++ objects with destructors inside __try).
	int SafeBufferGetStatus(IDirectSoundBuffer* buffer, DWORD* outStatus)
	{
		if (!buffer || !outStatus || !IsValidComObjectPointer(buffer))
		{
			return 0;
		}

		__try
		{
			HRESULT hr = E_FAIL;
			if (g_origBufferGetStatus)
			{
				hr = g_origBufferGetStatus(buffer, outStatus);
			}
			else
			{
				void** vtable = *reinterpret_cast<void***>(buffer);
				auto getStatus = reinterpret_cast<BufferGetStatus_t>(vtable[kBufferVtableGetStatus]);
				if (!getStatus)
				{
					return 0;
				}
				hr = getStatus(buffer, outStatus);
			}
			return SUCCEEDED(hr) ? 1 : 0;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return 0;
		}
	}

	int SafeBufferStop(IDirectSoundBuffer* buffer)
	{
		if (!buffer || !g_origBufferStop || !IsValidComObjectPointer(buffer))
		{
			return 0;
		}

		__try
		{
			return SUCCEEDED(g_origBufferStop(buffer)) ? 1 : 0;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return 0;
		}
	}

	int SafeBufferPlay(IDirectSoundBuffer* buffer)
	{
		if (!buffer || !g_origBufferPlay || !IsValidComObjectPointer(buffer))
		{
			return 0;
		}

		__try
		{
			return SUCCEEDED(g_origBufferPlay(buffer, 0, 0, 0)) ? 1 : 0;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return 0;
		}
	}

	int SafeBufferSetVolume(IDirectSoundBuffer* buffer, LONG volume)
	{
		if (!buffer || !IsValidComObjectPointer(buffer))
		{
			return 0;
		}

		__try
		{
			HRESULT hr = g_origBufferSetVolume
				? g_origBufferSetVolume(buffer, volume)
				: buffer->SetVolume(volume);
			return SUCCEEDED(hr) ? 1 : 0;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return 0;
		}
	}

	int SafeBufferGetVolume(IDirectSoundBuffer* buffer, LONG* outVolume)
	{
		if (!buffer || !outVolume || !IsValidComObjectPointer(buffer))
		{
			return 0;
		}

		__try
		{
			return SUCCEEDED(buffer->GetVolume(outVolume)) ? 1 : 0;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return 0;
		}
	}

	int SafeBufferAddRef(IDirectSoundBuffer* buffer)
	{
		if (!buffer || !IsValidComObjectPointer(buffer))
		{
			return 0;
		}

		__try
		{
			buffer->AddRef();
			return 1;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return 0;
		}
	}

	void SafeBufferRelease(IDirectSoundBuffer* buffer)
	{
		if (!buffer || !IsValidComObjectPointer(buffer))
		{
			return;
		}

		__try
		{
			buffer->Release();
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
	}

	bool IsBufferPlaying(IDirectSoundBuffer* buffer)
	{
		DWORD status = 0;
		return SafeBufferGetStatus(buffer, &status) != 0 && (status & DSBSTATUS_PLAYING) != 0;
	}

	bool IsInListLocked(const std::vector<IDirectSoundBuffer*>& list, IDirectSoundBuffer* buffer)
	{
		return std::find(list.begin(), list.end(), buffer) != list.end();
	}

	bool IsReloadBufferLocked(IDirectSoundBuffer* buffer)
	{
		return IsInListLocked(g_reloadBuffers, buffer);
	}

	void ReleaseBufferListLocked(std::vector<IDirectSoundBuffer*>& list)
	{
		for (IDirectSoundBuffer* buffer : list)
		{
			SafeBufferRelease(buffer);
		}
		list.clear();
	}

	bool TryAddCandidateLocked(IDirectSoundBuffer* buffer, const char* reason)
	{
		if (!IsValidComObjectPointer(buffer)
			|| IsInListLocked(g_candidateBuffers, buffer)
			|| IsReloadBufferLocked(buffer))
		{
			return false;
		}

		if (!SafeBufferAddRef(buffer))
		{
			return false;
		}

		g_candidateBuffers.push_back(buffer);

		while (static_cast<int>(g_candidateBuffers.size()) > 4)
		{
			IDirectSoundBuffer* oldest = g_candidateBuffers.front();
			g_candidateBuffers.erase(g_candidateBuffers.begin());
			SafeBufferRelease(oldest);
		}

		Logger::log << "[Sound] ds candidate " << reason << " buffer=" << buffer
			<< " candidates=" << g_candidateBuffers.size() << std::endl;
		return true;
	}

	DWORD SafeGetPlayCursor(IDirectSoundBuffer* buffer)
	{
		if (!buffer || !IsValidComObjectPointer(buffer))
		{
			return 0xFFFFFFFFu;
		}

		DWORD playCursor = 0xFFFFFFFFu;
		__try
		{
			if (FAILED(buffer->GetCurrentPosition(&playCursor, nullptr)))
			{
				return 0xFFFFFFFFu;
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return 0xFFFFFFFFu;
		}
		return playCursor;
	}

	bool SafeGetBufferFingerprint(IDirectSoundBuffer* buffer, DWORD& outRate, DWORD& outBytes)
	{
		outRate = 0;
		outBytes = 0;
		if (!buffer || !IsValidComObjectPointer(buffer))
		{
			return false;
		}

		__try
		{
			WAVEFORMATEX format{};
			if (FAILED(buffer->GetFormat(&format, sizeof(format), nullptr)) || format.nSamplesPerSec == 0)
			{
				return false;
			}

			DSBCAPS caps{};
			caps.dwSize = sizeof(caps);
			if (FAILED(buffer->GetCaps(&caps)) || caps.dwBufferBytes == 0)
			{
				return false;
			}

			outRate = format.nSamplesPerSec;
			outBytes = caps.dwBufferBytes;
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	bool MatchesReloadFingerprint(IDirectSoundBuffer* buffer)
	{
		if (!g_reloadFpValid)
		{
			return false;
		}

		DWORD rate = 0;
		DWORD bytes = 0;
		if (!SafeGetBufferFingerprint(buffer, rate, bytes))
		{
			return false;
		}

		return rate == g_reloadFpRate && bytes == g_reloadFpBytes;
	}

	void StoreReloadFingerprintLocked(IDirectSoundBuffer* buffer)
	{
		DWORD rate = 0;
		DWORD bytes = 0;
		if (!SafeGetBufferFingerprint(buffer, rate, bytes))
		{
			return;
		}

		g_reloadFpRate = rate;
		g_reloadFpBytes = bytes;
		g_reloadFpValid = true;

		Logger::log << "[Sound] ds fingerprint rate=" << rate
			<< " bytes=" << bytes
			<< " tag=0x" << std::hex << g_primaryMuteTagId << std::dec
			<< std::endl;
	}

	void RememberReloadBuffersLocked()
	{
		ReleaseBufferListLocked(g_rememberedReloadBuffers);
		if (!g_reloadBuffers.empty() && SafeBufferAddRef(g_reloadBuffers.front()))
		{
			g_rememberedReloadBuffers.push_back(g_reloadBuffers.front());
		}
	}

	void SnapshotPlayingAtMute()
	{
		std::lock_guard<std::mutex> lock(g_dsMutex);
		if (g_playingAtMuteCaptured)
		{
			return;
		}

		ReleaseBufferListLocked(g_playingAtMute);
		for (IDirectSoundBuffer* buffer : g_trackedBuffers)
		{
			if (!IsBufferPlaying(buffer))
			{
				continue;
			}
			if (SafeBufferAddRef(buffer))
			{
				g_playingAtMute.push_back(buffer);
			}
		}
		g_playingAtMuteCaptured = true;

		Logger::log << "[Sound] ds playingAtMute=" << g_playingAtMute.size() << std::endl;
	}

	uintptr_t SafeReadUIntPtr(uintptr_t address)
	{
		uintptr_t value = 0;
		__try
		{
			value = *reinterpret_cast<uintptr_t*>(address);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return 0;
		}
		return value;
	}

	constexpr int kPlaybackChannelSize = 0x678;
	constexpr int kPlaybackChannelDsBufferOffset = 0x670;
	// Looping/ambient channels store a sound name here; one-shot SFX leave it null/empty.
	constexpr int kPlaybackChannelNameOffset = 0x88;
	// Pistol reload permutation size observed across successful Seek gates.
	constexpr DWORD kBootstrapReloadRate = 22050;
	constexpr DWORD kBootstrapReloadBytes = 132300;

	IDirectSoundBuffer* ResolveDsBufferCandidate(IDirectSoundBuffer* candidate, bool bAlreadyHoldingDsMutex)
	{
		if (!candidate)
		{
			return nullptr;
		}

		if (bAlreadyHoldingDsMutex)
		{
			if (IsInListLocked(g_trackedBuffers, candidate))
			{
				return candidate;
			}
		}
		else
		{
			std::lock_guard<std::mutex> lock(g_dsMutex);
			if (IsInListLocked(g_trackedBuffers, candidate))
			{
				return candidate;
			}
		}

		if (IsValidComObjectPointer(candidate))
		{
			return candidate;
		}

		return nullptr;
	}

	// Live DS buffer for a SoundsGlobal channel slot (playback pool, not the tag definition table).
	IDirectSoundBuffer* GetBufferForChannelSlot(int slot, bool bAlreadyHoldingDsMutex)
	{
		const uintptr_t poolBase = static_cast<uintptr_t>(Hooks::o.SoundPlaybackPool);
		if (!poolBase)
		{
			Logger::log << "[Sound] playbackPool miss noBase slot=" << slot << std::endl;
			return nullptr;
		}

		if (slot < 0 || slot >= kMaxSoundSlots)
		{
			Logger::log << "[Sound] playbackPool miss badSlot slot=" << slot << std::endl;
			return nullptr;
		}

		const uintptr_t channel = poolBase
			+ static_cast<uintptr_t>(slot) * static_cast<uintptr_t>(kPlaybackChannelSize);
		const uint16_t channelWord = static_cast<uint16_t>(SafeReadUIntPtr(channel) & 0xFFFF);
		const uintptr_t bufferAddr = SafeReadUIntPtr(channel + static_cast<uintptr_t>(kPlaybackChannelDsBufferOffset));
		if (!bufferAddr)
		{
			Logger::log << "[Sound] playbackPool miss nullBuf slot=" << slot
				<< " word=" << channelWord
				<< " pool=0x" << std::hex << poolBase << std::dec << std::endl;
			return nullptr;
		}

		IDirectSoundBuffer* resolved = ResolveDsBufferCandidate(
			reinterpret_cast<IDirectSoundBuffer*>(bufferAddr),
			bAlreadyHoldingDsMutex);
		if (!resolved)
		{
			Logger::log << "[Sound] playbackPool miss badCom slot=" << slot
				<< " buf=0x" << std::hex << bufferAddr << std::dec
				<< " word=" << channelWord << std::endl;
			return nullptr;
		}

		return resolved;
	}

	// Raw, SEH-guarded scan of a single pool channel (POD only — no C++ unwinding objects here,
	// so __try is legal). Fills the identity offsets and the DS buffer pointer at +0x670.
	bool ScanPoolChannelIdentityRaw(
		uintptr_t channelBase,
		uint32_t tag,
		uint32_t parent,
		uint32_t entry32,
		uint32_t slot32,
		int& tagOff,
		int& parentOff,
		int& entryOff,
		int& slotOff,
		uintptr_t& bufferPtr)
	{
		tagOff = -1;
		parentOff = -1;
		entryOff = -1;
		slotOff = -1;
		bufferPtr = 0;

		__try
		{
			bufferPtr = *reinterpret_cast<uintptr_t*>(channelBase + kPlaybackChannelDsBufferOffset);
			for (int off = 0; off + 4 <= kPlaybackChannelSize; off += 4)
			{
				const uint32_t val = *reinterpret_cast<uint32_t*>(channelBase + off);
				if (val == tag && tagOff < 0) { tagOff = off; }
				if (parent != 0 && parent != 0xFFFFFFFFu && val == parent && parentOff < 0) { parentOff = off; }
				if (entry32 != 0 && val == entry32 && entryOff < 0) { entryOff = off; }
				if (val == slot32 && slotOff < 0) { slotOff = off; }
			}
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	// Scan every playback-pool channel for the reload's SoundsGlobal identity (tag / parent /
	// entry address / slot index) and cross-reference each channel's DS buffer (+0x670) against
	// the tracked/playing/fingerprint state. The goal is to discover a stable field inside the
	// 0x678-byte channel struct that maps a SoundsGlobal reload channel to its exact DS buffer,
	// replacing the ambiguous size-fingerprint gate. Runs once per pause; all reads SEH-guarded.
	void DiagnosePlaybackPoolIdentity(int matchSlot, uintptr_t matchEntry, uint32_t tag, uint32_t parent, WeaponType weaponType)
	{
		const uintptr_t poolBase = static_cast<uintptr_t>(Hooks::o.SoundPlaybackPool);
		if (!poolBase)
		{
			Logger::log << "[Sound] poolDiag noBase" << std::endl;
			return;
		}

		DWORD expectedRate = 0;
		DWORD expectedBytes = 0;
		GetReloadFingerprintForWeapon(weaponType, expectedRate, expectedBytes);

		const uint32_t entry32 = static_cast<uint32_t>(matchEntry & 0xFFFFFFFFu);
		const uint32_t slot32 = static_cast<uint32_t>(matchSlot);

		Logger::log << "[Sound] poolDiag base=0x" << std::hex << poolBase
			<< " tag=0x" << tag
			<< " parent=0x" << parent
			<< " entry=0x" << matchEntry << std::dec
			<< " slot=" << matchSlot
			<< " stride=0x" << std::hex << kPlaybackChannelSize << std::dec
			<< std::endl;

		int hitChannels = 0;
		for (int chan = 0; chan < kMaxSoundSlots; chan++)
		{
			const uintptr_t channelBase = poolBase + static_cast<uintptr_t>(chan) * kPlaybackChannelSize;
			if (!IsReadableMemory(reinterpret_cast<const void*>(channelBase), kPlaybackChannelSize))
			{
				continue;
			}

			int tagOff = -1;
			int parentOff = -1;
			int entryOff = -1;
			int slotOff = -1;
			uintptr_t bufferPtr = 0;

			if (!ScanPoolChannelIdentityRaw(channelBase, tag, parent, entry32, slot32,
				tagOff, parentOff, entryOff, slotOff, bufferPtr))
			{
				continue;
			}

			const bool bIdentity = (tagOff >= 0 || parentOff >= 0 || entryOff >= 0);

			// Cross-reference the channel's DS buffer state (only touch COM if it's one we created).
			int tracked = 0;
			int playing = 0;
			DWORD rate = 0;
			DWORD bytes = 0;
			bool bReloadBuf = false;
			{
				std::lock_guard<std::mutex> lock(g_dsMutex);
				IDirectSoundBuffer* buf = reinterpret_cast<IDirectSoundBuffer*>(bufferPtr);
				if (bufferPtr && IsInListLocked(g_trackedBuffers, buf))
				{
					tracked = 1;
					playing = IsBufferPlaying(buf) ? 1 : 0;
					if (SafeGetBufferFingerprint(buf, rate, bytes))
					{
						bReloadBuf = (expectedBytes != 0 && rate == expectedRate && bytes == expectedBytes);
					}
				}
			}

			// Report channels that either carry the identity OR whose buffer is a playing reload
			// voice — this reveals both "does the link exist" and "which channel holds the audio".
			if (!bIdentity && !(tracked && playing && bReloadBuf))
			{
				continue;
			}

			hitChannels++;
			Logger::log << "[Sound] poolDiag hit chan=" << chan
				<< " tagOff=" << tagOff
				<< " parentOff=" << parentOff
				<< " entryOff=" << entryOff
				<< " slotOff=" << slotOff
				<< " buf=0x" << std::hex << bufferPtr << std::dec
				<< " tracked=" << tracked
				<< " playing=" << playing
				<< " rate=" << rate
				<< " bytes=" << bytes
				<< " reloadFp=" << (bReloadBuf ? 1 : 0)
				<< std::endl;
		}

		Logger::log << "[Sound] poolDiag done hits=" << hitChannels << std::endl;
	}

	// Read a NUL-terminated string from possibly-bogus memory (bounded + validated).
	std::string ReadCStringSafe(uintptr_t ptr, int maxLen)
	{
		if (!ptr || !IsReadableMemory(reinterpret_cast<const void*>(ptr), static_cast<size_t>(maxLen) + 1))
		{
			return std::string();
		}
		const char* s = reinterpret_cast<const char*>(ptr);
		std::string out;
		for (int i = 0; i < maxLen && s[i]; i++)
		{
			const char c = s[i];
			out.push_back((c >= 32 && c < 127) ? c : '.');
		}
		return out;
	}

	bool LooksLikePointer(uint32_t v)
	{
		return v >= 0x00010000u && (v & 3u) == 0u;
	}

	// Chase pointers out of a pool-channel struct looking for a dword equal to `tag`.
	// Halo doesn't store the tag id as a raw dword in the channel, but the channel must hold a
	// pointer to its source sound (whose tag id IS 0xe36401f0). depth 1: channel[off] -> [+k]==tag.
	// depth 2: channel[off] -> [+k] -> [+m]==tag. Every candidate pointer is VirtualQuery-validated
	// before dereference, so bogus values are safe.
	bool ChannelChaseForTag(uintptr_t channelBase, uint32_t tag,
		int& outOff, int& outK, int& outM, int& outDepth)
	{
		outOff = outK = outM = -1;
		outDepth = 0;
		constexpr int kLeafScan = 0x60;

		for (int off = 0; off + 4 <= kPlaybackChannelSize; off += 4)
		{
			const uint32_t p1 = *reinterpret_cast<const uint32_t*>(channelBase + off);
			if (!LooksLikePointer(p1)
				|| !IsReadableMemory(reinterpret_cast<const void*>(static_cast<uintptr_t>(p1)), kLeafScan))
			{
				continue;
			}
			for (int k = 0; k + 4 <= kLeafScan; k += 4)
			{
				if (*reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(p1) + k) == tag)
				{
					outOff = off; outK = k; outDepth = 1;
					return true;
				}
			}
		}

		for (int off = 0; off + 4 <= kPlaybackChannelSize; off += 4)
		{
			const uint32_t p1 = *reinterpret_cast<const uint32_t*>(channelBase + off);
			if (!LooksLikePointer(p1)
				|| !IsReadableMemory(reinterpret_cast<const void*>(static_cast<uintptr_t>(p1)), kLeafScan))
			{
				continue;
			}
			for (int k = 0; k + 4 <= kLeafScan; k += 4)
			{
				const uint32_t p2 = *reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(p1) + k);
				if (!LooksLikePointer(p2)
					|| !IsReadableMemory(reinterpret_cast<const void*>(static_cast<uintptr_t>(p2)), kLeafScan))
				{
					continue;
				}
				for (int m = 0; m + 4 <= kLeafScan; m += 4)
				{
					if (*reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(p2) + m) == tag)
					{
						outOff = off; outK = k; outM = m; outDepth = 2;
						return true;
					}
				}
			}
		}
		return false;
	}

	// Walk every active pool channel (count at poolBase-8) and report which one references the
	// reload tag via pointer chase, plus its DS buffer state and name strings (+0x88/+0x8c).
	// This is the candidate deterministic bridge: reload tag -> channel (by chased tag) -> DS
	// buffer (+0x670), independent of whether the buffer currently reports "playing".
	void DiagnoseChannelSourceIdentity(uint32_t expectedTag)
	{
		const uintptr_t poolBase = static_cast<uintptr_t>(Hooks::o.SoundPlaybackPool);
		if (!poolBase || !IsReadableMemory(reinterpret_cast<const void*>(poolBase - 8), 8))
		{
			Logger::log << "[Sound] srcId noBase" << std::endl;
			return;
		}

		int count = *reinterpret_cast<const uint16_t*>(poolBase - 8);
		if (count <= 0 || count > 256)
		{
			count = kMaxSoundSlots;
		}

		int found = 0;
		for (int chan = 0; chan < count; chan++)
		{
			const uintptr_t channelBase = poolBase + static_cast<uintptr_t>(chan) * kPlaybackChannelSize;
			if (!IsReadableMemory(reinterpret_cast<const void*>(channelBase), kPlaybackChannelSize))
			{
				continue;
			}

			const uint16_t state = *reinterpret_cast<const uint16_t*>(channelBase);
			const uint8_t svc = *reinterpret_cast<const uint8_t*>(channelBase + 9);
			if (state == 0 && svc == 0)
			{
				continue; // inactive channel
			}

			int off = -1, k = -1, m = -1, depth = 0;
			const bool hasTag = ChannelChaseForTag(channelBase, expectedTag, off, k, m, depth);

			const uintptr_t bufPtr = SafeReadUIntPtr(channelBase + kPlaybackChannelDsBufferOffset);
			int tracked = 0;
			int playing = 0;
			DWORD rate = 0;
			DWORD bytes = 0;
			{
				std::lock_guard<std::mutex> lock(g_dsMutex);
				IDirectSoundBuffer* buf = reinterpret_cast<IDirectSoundBuffer*>(bufPtr);
				if (bufPtr && IsInListLocked(g_trackedBuffers, buf))
				{
					tracked = 1;
					playing = IsBufferPlaying(buf) ? 1 : 0;
					SafeGetBufferFingerprint(buf, rate, bytes);
				}
			}

			const std::string name1 = ReadCStringSafe(SafeReadUIntPtr(channelBase + 0x88), 31);
			const std::string name2 = ReadCStringSafe(SafeReadUIntPtr(channelBase + 0x8c), 31);

			if (hasTag)
			{
				found++;
			}

			Logger::log << "[Sound] srcId chan=" << chan
				<< " state=0x" << std::hex << state << std::dec
				<< " svc=" << static_cast<int>(svc)
				<< " tagPath=" << (hasTag ? 1 : 0)
				<< " depth=" << depth
				<< " off=" << off << " k=" << k << " m=" << m
				<< " buf=0x" << std::hex << bufPtr << std::dec
				<< " tracked=" << tracked << " playing=" << playing
				<< " bytes=" << bytes
				<< " n1=[" << name1 << "]"
				<< " n2=[" << name2 << "]"
				<< std::endl;
		}

		Logger::log << "[Sound] srcId done tagChannels=" << found << std::endl;
	}

	bool MatchesReloadOrBootstrapFingerprint(IDirectSoundBuffer* buffer)
	{
		if (MatchesReloadFingerprint(buffer))
		{
			return true;
		}
		if (g_reloadFpValid)
		{
			return false;
		}

		DWORD rate = 0;
		DWORD bytes = 0;
		if (!SafeGetBufferFingerprint(buffer, rate, bytes))
		{
			return false;
		}
		return rate == kBootstrapReloadRate && bytes == kBootstrapReloadBytes;
	}

	void ClearProbedReloadBufferLocked()
	{
		if (g_probedReloadBuffer)
		{
			SafeBufferRelease(g_probedReloadBuffer);
			g_probedReloadBuffer = nullptr;
		}
		g_probedReloadOffset = -1;
	}

	// Safe link attempt: only accept dwords that equal an already-tracked DS buffer pointer.
	// Never call COM on unknown values (avoids the old 0x3F800000 crash).
	void ProbeChannelForTrackedBuffer(uintptr_t entry)
	{
		if (!entry || !IsReadableMemory(reinterpret_cast<const void*>(entry), kSoundEntrySize))
		{
			return;
		}

		std::lock_guard<std::mutex> lock(g_dsMutex);
		ClearProbedReloadBufferLocked();

		IDirectSoundBuffer* hits[8]{};
		int hitOffsets[8]{};
		int hitCount = 0;

		for (int offset = 0; offset + 4 <= kSoundEntrySize; offset += 4)
		{
			// Skip known non-pointer fields.
			if (offset == kSoundTagIdOffset
				|| offset == kSoundParentOffset
				|| offset == kSoundScaleOffset
				|| offset == kSoundGainOffset
				|| offset == kSoundOrderIdOffset
				|| offset == kSoundFirstPersonOffset)
			{
				continue;
			}

			const uintptr_t value = SafeReadUIntPtr(entry + static_cast<uintptr_t>(offset));
			if (!value)
			{
				continue;
			}

			IDirectSoundBuffer* asBuffer = reinterpret_cast<IDirectSoundBuffer*>(value);
			if (!IsInListLocked(g_trackedBuffers, asBuffer))
			{
				continue;
			}

			bool bDuplicate = false;
			for (int i = 0; i < hitCount; i++)
			{
				if (hits[i] == asBuffer)
				{
					bDuplicate = true;
					break;
				}
			}
			if (bDuplicate)
			{
				continue;
			}

			if (hitCount < 8)
			{
				hits[hitCount] = asBuffer;
				hitOffsets[hitCount] = offset;
				hitCount++;
			}

			Logger::log << "[Sound] channelProbe hit offset=" << offset
				<< " buffer=" << asBuffer
				<< " playing=" << (IsBufferPlaying(asBuffer) ? 1 : 0)
				<< std::endl;
		}

		IDirectSoundBuffer* chosen = nullptr;
		int chosenOffset = -1;
		if (hitCount == 1)
		{
			chosen = hits[0];
			chosenOffset = hitOffsets[0];
		}
		else if (hitCount > 1)
		{
			for (int i = 0; i < hitCount; i++)
			{
				if (IsBufferPlaying(hits[i]) && IsInListLocked(g_playingAtMute, hits[i]))
				{
					chosen = hits[i];
					chosenOffset = hitOffsets[i];
					break;
				}
			}
			if (!chosen)
			{
				for (int i = 0; i < hitCount; i++)
				{
					if (IsBufferPlaying(hits[i]))
					{
						chosen = hits[i];
						chosenOffset = hitOffsets[i];
						break;
					}
				}
			}
			if (!chosen)
			{
				chosen = hits[0];
				chosenOffset = hitOffsets[0];
			}
		}

		if (chosen && SafeBufferAddRef(chosen))
		{
			g_probedReloadBuffer = chosen;
			g_probedReloadOffset = chosenOffset;
			Logger::log << "[Sound] channelProbe chosen offset=" << chosenOffset
				<< " buffer=" << chosen
				<< " hits=" << hitCount
				<< std::endl;
		}
		else
		{
			Logger::log << "[Sound] channelProbe none hits=" << hitCount << std::endl;
		}
	}

	const CaptureBaseline* FindBaselineLocked(IDirectSoundBuffer* buffer)
	{
		for (const CaptureBaseline& entry : g_captureBaselines)
		{
			if (entry.buffer == buffer)
			{
				return &entry;
			}
		}
		return nullptr;
	}

	void SnapshotCaptureBaselinesLocked()
	{
		g_captureBaselines.clear();
		g_captureBaselines.reserve(g_trackedBuffers.size());
		for (IDirectSoundBuffer* buffer : g_trackedBuffers)
		{
			CaptureBaseline baseline;
			baseline.buffer = buffer;
			baseline.bPlaying = IsBufferPlaying(buffer);
			baseline.playCursor = baseline.bPlaying ? SafeGetPlayCursor(buffer) : 0;
			if (baseline.playCursor == 0xFFFFFFFFu)
			{
				baseline.playCursor = 0;
			}
			g_captureBaselines.push_back(baseline);
		}
	}

	int TryRememberedPlayingLocked(IDirectSoundBuffer*& outBuffer, const char*& outReason)
	{
		IDirectSoundBuffer* best = nullptr;
		DWORD bestCursor = 0;
		for (IDirectSoundBuffer* buffer : g_rememberedReloadBuffers)
		{
			if (!IsBufferPlaying(buffer) || !IsInListLocked(g_playingAtMute, buffer))
			{
				continue;
			}
			const DWORD cursor = SafeGetPlayCursor(buffer);
			if (cursor == 0xFFFFFFFFu || cursor <= 8192)
			{
				continue;
			}
			if (!best || cursor >= bestCursor)
			{
				best = buffer;
				bestCursor = cursor;
			}
		}
		if (!best || !SafeBufferAddRef(best))
		{
			return 0;
		}
		outBuffer = best;
		outReason = "remembered";
		return 1;
	}

	void DumpGateFailureLocked()
	{
		for (IDirectSoundBuffer* buffer : g_candidateBuffers)
		{
			DWORD rate = 0;
			DWORD bytes = 0;
			SafeGetBufferFingerprint(buffer, rate, bytes);
			Logger::log << "[Sound] fail candidate buffer=" << buffer
				<< " playing=" << (IsBufferPlaying(buffer) ? 1 : 0)
				<< " cursor=" << SafeGetPlayCursor(buffer)
				<< " inMute=" << (IsInListLocked(g_playingAtMute, buffer) ? 1 : 0)
				<< " rate=" << rate
				<< " bytes=" << bytes
				<< std::endl;
		}

		for (IDirectSoundBuffer* buffer : g_playingAtMute)
		{
			DWORD rate = 0;
			DWORD bytes = 0;
			SafeGetBufferFingerprint(buffer, rate, bytes);
			const CaptureBaseline* baseline = FindBaselineLocked(buffer);
			Logger::log << "[Sound] fail playingAtMute buffer=" << buffer
				<< " cursor=" << SafeGetPlayCursor(buffer)
				<< " baselinePlaying=" << (baseline && baseline->bPlaying ? 1 : 0)
				<< " baselineCursor=" << (baseline ? baseline->playCursor : 0)
				<< " rate=" << rate
				<< " bytes=" << bytes
				<< " fpMatch=" << (MatchesReloadFingerprint(buffer) ? 1 : 0)
				<< std::endl;
		}
	}

	IDirectSoundBuffer* PickBestReloadBufferLocked(const char*& outReason)
	{
		outReason = "none";

		auto releaseAllExcept = [](std::vector<IDirectSoundBuffer*>& list, IDirectSoundBuffer* keep)
		{
			for (IDirectSoundBuffer* buffer : list)
			{
				if (buffer != keep)
				{
					SafeBufferRelease(buffer);
				}
			}
			list.clear();
		};

		auto pickHighestCursorPlaying = [&](bool requirePlayingAtMute, bool requireFingerprint) -> IDirectSoundBuffer*
		{
			IDirectSoundBuffer* best = nullptr;
			DWORD bestCursor = 0;
			for (IDirectSoundBuffer* buffer : g_candidateBuffers)
			{
				if (!IsBufferPlaying(buffer))
				{
					continue;
				}
				if (requirePlayingAtMute && !IsInListLocked(g_playingAtMute, buffer))
				{
					continue;
				}
				if (requireFingerprint && !MatchesReloadFingerprint(buffer))
				{
					continue;
				}
				const DWORD cursor = SafeGetPlayCursor(buffer);
				if (cursor == 0xFFFFFFFFu)
				{
					continue;
				}
				if (!best || cursor >= bestCursor)
				{
					best = buffer;
					bestCursor = cursor;
				}
			}
			return best;
		};

		// 1) Playing Seek/Play candidate in playingAtMute — highest cursor.
		if (IDirectSoundBuffer* chosen = pickHighestCursorPlaying(true, false))
		{
			outReason = "candidate+mute";
			releaseAllExcept(g_candidateBuffers, chosen);
			return chosen;
		}

		// 2) Playing Seek/Play candidate (any).
		if (IDirectSoundBuffer* chosen = pickHighestCursorPlaying(false, false))
		{
			outReason = "candidate";
			releaseAllExcept(g_candidateBuffers, chosen);
			return chosen;
		}

		// Idle Seek/Play only when present.
		{
			IDirectSoundBuffer* bestIdle = nullptr;
			for (int i = static_cast<int>(g_candidateBuffers.size()) - 1; i >= 0; i--)
			{
				IDirectSoundBuffer* buffer = g_candidateBuffers[static_cast<size_t>(i)];
				if (IsBufferPlaying(buffer))
				{
					continue;
				}
				if (g_reloadFpValid && !MatchesReloadFingerprint(buffer))
				{
					continue;
				}
				bestIdle = buffer;
				break;
			}
			if (bestIdle)
			{
				outReason = g_reloadFpValid ? "candidateIdleFp" : "candidateIdle";
				releaseAllExcept(g_candidateBuffers, bestIdle);
				return bestIdle;
			}
		}

		// Cursor reset during capture (= Halo Seek we missed). Only if exactly one
		// reload-sized buffer reset — multiple resets are ambiguous twins.
		{
			IDirectSoundBuffer* resets[4]{};
			int resetCount = 0;
			for (IDirectSoundBuffer* buffer : g_playingAtMute)
			{
				if (!IsBufferPlaying(buffer) || !MatchesReloadOrBootstrapFingerprint(buffer))
				{
					continue;
				}
				const CaptureBaseline* baseline = FindBaselineLocked(buffer);
				if (!baseline || !baseline->bPlaying)
				{
					continue;
				}
				const DWORD cursor = SafeGetPlayCursor(buffer);
				if (cursor == 0xFFFFFFFFu || baseline->playCursor <= cursor + 4096)
				{
					continue;
				}
				if (resetCount < 4)
				{
					resets[resetCount++] = buffer;
				}
			}
			if (resetCount == 1 && SafeBufferAddRef(resets[0]))
			{
				outReason = "cursorReset";
				releaseAllExcept(g_candidateBuffers, nullptr);
				return resets[0];
			}
			if (resetCount > 1)
			{
				Logger::log << "[Sound] cursorReset ambiguous count=" << resetCount << std::endl;
			}
		}

		// Leave candidates intact so Finalize can dump them on reason=none.
		outReason = "none";
		return nullptr;
	}

	// Deterministic gate: mute+freeze EVERY currently-playing DS buffer whose capacity
	// matches the reload permutation for this weapon. Halo makes one DS buffer per loaded
	// sound permutation, so a playing buffer of the reload's exact size IS a reload voice
	// (ambient/other sounds differ in size and are never touched). Rescanned every tick so
	// duplicate voices (local=2) and any buffer Halo migrates/restarts the reload onto
	// mid-hold are all caught. This replaces the unreliable pool[slot]+0x670 mapping.
	// True if a pool channel carries a looping/ambient sound (teleporter hum, music, ...).
	// Those channels populate a name string at +0x88; one-shot SFX like the pistol reload leave
	// it null/empty. This is how we exclude the same-sized teleporter buffer from the reload gate.
	bool ChannelHasName(uintptr_t channelBase)
	{
		const uintptr_t namePtr = SafeReadUIntPtr(channelBase + kPlaybackChannelNameOffset);
		if (!namePtr || !IsReadableMemory(reinterpret_cast<const void*>(namePtr), 1))
		{
			return false;
		}
		return *reinterpret_cast<const char*>(namePtr) != '\0';
	}

	// Identify reload voices by walking Halo's playback pool directly (count at poolBase-8),
	// NOT g_trackedBuffers (which drops buffers on Release while Halo keeps reusing them).
	// A channel is a reload voice when it is active, has no name (one-shot, not a looping
	// ambient), and its DS buffer (+0x670) matches the weapon's reload fingerprint. We read the
	// buffer straight from the channel, so this works even after we've dropped it from tracking.
	void FinalizeReloadBuffersFromIdentityLocked()
	{
		DWORD expectedRate = 0;
		DWORD expectedBytes = 0;
		if (!GetReloadFingerprintForWeapon(g_pauseWeaponType, expectedRate, expectedBytes))
		{
			return;
		}

		// Only gate while a local reload voice actually exists (identity confirmed), so we
		// never mute an unrelated same-sized sound outside a reload.
		if (!g_retailIdentityKnown.load(std::memory_order_acquire))
		{
			return;
		}

		const uintptr_t poolBase = static_cast<uintptr_t>(Hooks::o.SoundPlaybackPool);
		if (!poolBase || !IsReadableMemory(reinterpret_cast<const void*>(poolBase - 8), 8))
		{
			return;
		}

		int count = *reinterpret_cast<const uint16_t*>(poolBase - 8);
		if (count <= 0 || count > 256)
		{
			count = kMaxSoundSlots;
		}

		for (int chan = 0; chan < count; chan++)
		{
			const uintptr_t channelBase = poolBase + static_cast<uintptr_t>(chan) * kPlaybackChannelSize;
			if (!IsReadableMemory(reinterpret_cast<const void*>(channelBase), kPlaybackChannelSize))
			{
				continue;
			}

			// Active channel only (word[+0] != 0). Excludes idle/servicing-only slots.
			if (*reinterpret_cast<const uint16_t*>(channelBase) == 0)
			{
				continue;
			}

			// Skip looping/ambient (named) channels — teleporter hum, music, etc.
			if (ChannelHasName(channelBase))
			{
				continue;
			}

			IDirectSoundBuffer* buffer = reinterpret_cast<IDirectSoundBuffer*>(
				SafeReadUIntPtr(channelBase + kPlaybackChannelDsBufferOffset));
			if (!buffer || !IsValidComObjectPointer(buffer) || IsReloadBufferLocked(buffer))
			{
				continue;
			}

			DWORD rate = 0;
			DWORD bytes = 0;
			if (!SafeGetBufferFingerprint(buffer, rate, bytes))
			{
				continue;
			}
			if (rate != expectedRate || bytes != expectedBytes)
			{
				continue;
			}

			if (!SafeBufferAddRef(buffer))
			{
				continue;
			}

			const DWORD cursor = SafeGetPlayCursor(buffer);
			g_reloadBuffers.push_back(buffer);
			g_reloadCursors.push_back(cursor == 0xFFFFFFFFu ? 0u : cursor);

			Logger::log << "[Sound] reload gate chan=" << chan
				<< " buffer=" << buffer
				<< " cursor=" << cursor
				<< " bytes=" << bytes
				<< " count=" << g_reloadBuffers.size()
				<< std::endl;
		}
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

		std::lock_guard<std::mutex> lock(g_dsMutex);

		if (!g_bufferVtablePatched)
		{
			void** vtable = *reinterpret_cast<void***>(buffer);
			void* originalPlay = nullptr;
			void* originalSetPos = nullptr;
			void* originalSetVolume = nullptr;
			void* originalStop = nullptr;
			void* originalRelease = nullptr;

			if (!PatchVtableEntry(vtable, kBufferVtablePlay, reinterpret_cast<void*>(&HookedBufferPlay), &originalPlay)
				|| !PatchVtableEntry(vtable, kBufferVtableSetCurrentPosition, reinterpret_cast<void*>(&HookedBufferSetCurrentPosition), &originalSetPos)
				|| !PatchVtableEntry(vtable, kBufferVtableSetVolume, reinterpret_cast<void*>(&HookedBufferSetVolume), &originalSetVolume)
				|| !PatchVtableEntry(vtable, kBufferVtableStop, reinterpret_cast<void*>(&HookedBufferStop), &originalStop)
				|| !PatchVtableEntry(vtable, kBufferVtableRelease, reinterpret_cast<void*>(&HookedBufferRelease), &originalRelease))
			{
				return;
			}

			g_origBufferPlay = reinterpret_cast<BufferPlay_t>(originalPlay);
			g_origBufferSetCurrentPosition = reinterpret_cast<BufferSetCurrentPosition_t>(originalSetPos);
			g_origBufferSetVolume = reinterpret_cast<BufferSetVolume_t>(originalSetVolume);
			g_origBufferStop = reinterpret_cast<BufferStop_t>(originalStop);
			g_origBufferRelease = reinterpret_cast<BufferRelease_t>(originalRelease);
			g_origBufferGetStatus = reinterpret_cast<BufferGetStatus_t>(vtable[kBufferVtableGetStatus]);
			g_bufferVtablePatched = true;
		}

		if (!IsInListLocked(g_trackedBuffers, buffer))
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
		Logger::log << "[Sound] ds device CreateSoundBuffer hooked" << std::endl;
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
		if (g_suppressReloadBuffers.load(std::memory_order_acquire))
		{
			bool bReload = false;
			{
				std::lock_guard<std::mutex> lock(g_dsMutex);
				bReload = IsReloadBufferLocked(self);
			}
			if (bReload)
			{
				return DS_OK;
			}
		}

		return g_origBufferPlay ? g_origBufferPlay(self, dwReserved1, dwPriority, dwFlags) : DSERR_GENERIC;
	}

	HRESULT STDMETHODCALLTYPE HookedBufferSetCurrentPosition(IDirectSoundBuffer* self, DWORD dwNewPosition)
	{
		if (g_suppressReloadBuffers.load(std::memory_order_acquire))
		{
			std::lock_guard<std::mutex> lock(g_dsMutex);
			if (IsReloadBufferLocked(self))
			{
				// Swallow seeks so Halo cannot restart/advance the muted reload buffer.
				return DS_OK;
			}
		}

		return g_origBufferSetCurrentPosition
			? g_origBufferSetCurrentPosition(self, dwNewPosition)
			: DSERR_GENERIC;
	}

	HRESULT STDMETHODCALLTYPE HookedBufferSetVolume(IDirectSoundBuffer* self, LONG lVolume)
	{
		if (g_suppressReloadBuffers.load(std::memory_order_acquire))
		{
			std::lock_guard<std::mutex> lock(g_dsMutex);
			if (IsReloadBufferLocked(self))
			{
				// Halo re-applies gain→volume every tick; keep the gated reload silent.
				return g_origBufferSetVolume
					? g_origBufferSetVolume(self, DSBVOLUME_MIN)
					: DSERR_GENERIC;
			}
		}

		return g_origBufferSetVolume ? g_origBufferSetVolume(self, lVolume) : DSERR_GENERIC;
	}

	HRESULT STDMETHODCALLTYPE HookedBufferStop(IDirectSoundBuffer* self)
	{
		if (g_suppressReloadBuffers.load(std::memory_order_acquire))
		{
			std::lock_guard<std::mutex> lock(g_dsMutex);
			if (IsReloadBufferLocked(self))
			{
				// Keep the gated reload buffer alive for mid-clip resume.
				return DS_OK;
			}
		}

		return g_origBufferStop ? g_origBufferStop(self) : DSERR_GENERIC;
	}

	ULONG STDMETHODCALLTYPE HookedBufferRelease(IDirectSoundBuffer* self)
	{
		{
			std::lock_guard<std::mutex> lock(g_dsMutex);
			g_trackedBuffers.erase(
				std::remove(g_trackedBuffers.begin(), g_trackedBuffers.end(), self),
				g_trackedBuffers.end());
			// Keep candidate/reload entries — we AddRef'd them for resume.
		}
		return g_origBufferRelease ? g_origBufferRelease(self) : 0;
	}

	HRESULT WINAPI HookedDirectSoundCreate8(LPCGUID lpcGuidDevice, LPDIRECTSOUND8* ppDS8, LPUNKNOWN pUnkOuter)
	{
		const HRESULT result = g_origDirectSoundCreate8(lpcGuidDevice, ppDS8, pUnkOuter);
		if (SUCCEEDED(result) && ppDS8 && *ppDS8)
		{
			PatchDeviceCreateSoundBufferHook(*ppDS8);
		}
		return result;
	}

	HRESULT WINAPI HookedDirectSoundCreate(LPCGUID lpcGuidDevice, LPDIRECTSOUND* ppDS, LPUNKNOWN pUnkOuter)
	{
		const HRESULT result = g_origDirectSoundCreate(lpcGuidDevice, ppDS, pUnkOuter);
		if (SUCCEEDED(result) && ppDS && *ppDS)
		{
			PatchDeviceCreateSoundBufferHook(*ppDS);
		}
		return result;
	}

	int SilenceGatedDsBuffers()
	{
		std::lock_guard<std::mutex> lock(g_dsMutex);
		FinalizeReloadBuffersFromIdentityLocked();

		int silenced = 0;
		for (size_t i = 0; i < g_reloadBuffers.size(); i++)
		{
			IDirectSoundBuffer* buffer = g_reloadBuffers[i];
			const DWORD cursor = (i < g_reloadCursors.size()) ? g_reloadCursors[i] : 0u;

			// Freeze each gated buffer's playhead at its own recorded cursor every tick and
			// hold it silent, so none can advance/finish the reload SFX during the hold.
			if (g_origBufferSetCurrentPosition)
			{
				g_origBufferSetCurrentPosition(buffer, cursor);
			}

			if (SafeBufferSetVolume(buffer, DSBVOLUME_MIN))
			{
				silenced++;
			}
		}
		return silenced;
	}

	int SafeSeekBufferForward(IDirectSoundBuffer* buffer, float skipSeconds, DWORD baseCursor, bool bBaseCursorValid)
	{
		if (!buffer || skipSeconds <= 0.0f || !IsValidComObjectPointer(buffer))
		{
			return 1;
		}

		__try
		{
			WAVEFORMATEX format{};
			if (FAILED(buffer->GetFormat(&format, sizeof(format), nullptr)) || format.nAvgBytesPerSec == 0)
			{
				return 1;
			}

			DSBCAPS caps{};
			caps.dwSize = sizeof(caps);
			if (FAILED(buffer->GetCaps(&caps)) || caps.dwBufferBytes == 0)
			{
				return 1;
			}

			DWORD playCursor = 0;
			if (bBaseCursorValid)
			{
				playCursor = baseCursor;
			}
			else if (FAILED(buffer->GetCurrentPosition(&playCursor, nullptr)))
			{
				return 1;
			}

			DWORD skipBytes = static_cast<DWORD>(skipSeconds * static_cast<float>(format.nAvgBytesPerSec) + 0.5f);
			if (format.nBlockAlign > 1)
			{
				skipBytes -= skipBytes % format.nBlockAlign;
			}

			const ULONGLONG newPos = static_cast<ULONGLONG>(playCursor) + skipBytes;
			if (newPos >= caps.dwBufferBytes)
			{
				return 0;
			}

			HRESULT hr = g_origBufferSetCurrentPosition
				? g_origBufferSetCurrentPosition(buffer, static_cast<DWORD>(newPos))
				: buffer->SetCurrentPosition(static_cast<DWORD>(newPos));
			return SUCCEEDED(hr) ? 1 : 0;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return 0;
		}
	}

	int ResumeGatedDsBuffers(float skipSeconds)
	{
		std::vector<IDirectSoundBuffer*> toResume;
		std::vector<DWORD> toResumeCursors;
		{
			std::lock_guard<std::mutex> lock(g_dsMutex);
			FinalizeReloadBuffersFromIdentityLocked();
			toResume = g_reloadBuffers;
			toResumeCursors = g_reloadCursors;
			g_reloadBuffers.clear();
			g_reloadCursors.clear();
			ReleaseBufferListLocked(g_candidateBuffers);
			g_reloadListFinalized = false;
			g_pausePlayCursorValid = false;
			g_pausePlayCursor = 0;
		}

		int resumed = 0;
		for (size_t i = 0; i < toResume.size(); i++)
		{
			IDirectSoundBuffer* buffer = toResume[i];
			const DWORD pauseCursor = (i < toResumeCursors.size()) ? toResumeCursors[i] : 0u;

			if (!IsValidComObjectPointer(buffer))
			{
				Logger::log << "[Sound] ds resume skip badCom buffer=" << buffer << std::endl;
				SafeBufferRelease(buffer);
				continue;
			}

			SafeBufferSetVolume(buffer, DSBVOLUME_MAX);

			LONG volumeAfter = 0;
			const int volRead = SafeBufferGetVolume(buffer, &volumeAfter);

			// Each buffer resumes from its own frozen cursor + the mid-clip skip.
			const int seekOk = SafeSeekBufferForward(buffer, skipSeconds, pauseCursor, true);
			if (!seekOk)
			{
				Logger::log << "[Sound] ds resume seekPastEnd buffer=" << buffer
					<< " skipSeconds=" << skipSeconds
					<< " pauseCursor=" << pauseCursor
					<< " (playing from current)"
					<< std::endl;
			}

			const bool bWasPlaying = IsBufferPlaying(buffer);
			int playOk = 1;
			if (!bWasPlaying)
			{
				playOk = SafeBufferPlay(buffer);
				if (!playOk)
				{
					Logger::log << "[Sound] ds resume playFail buffer=" << buffer << std::endl;
				}
			}

			if (playOk)
			{
				resumed++;
			}

			Logger::log << "[Sound] ds resume buffer=" << buffer
				<< " volume=" << (volRead ? volumeAfter : 0x7FFFFFFF)
				<< " seekOk=" << seekOk
				<< " wasPlaying=" << (bWasPlaying ? 1 : 0)
				<< " playOk=" << playOk
				<< " nowPlaying=" << (IsBufferPlaying(buffer) ? 1 : 0)
				<< " pauseCursor=" << pauseCursor
				<< std::endl;

			SafeBufferRelease(buffer);
		}
		return resumed;
	}
}

void Helpers::InitSoundHook()
{
	if (g_hooksInstalled)
	{
		return;
	}

	const HMODULE dsoundModule = GetModuleHandleA("dsound.dll");
	if (!dsoundModule || g_initAttempted)
	{
		return;
	}

	g_initAttempted = true;

	auto createExportHook = [dsoundModule](const char* exportName, LPVOID detour, LPVOID* original) -> bool
	{
		LPVOID exportAddress = reinterpret_cast<LPVOID>(GetProcAddress(dsoundModule, exportName));
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

	const bool bCreate8 = createExportHook(
		"DirectSoundCreate8",
		reinterpret_cast<LPVOID>(&HookedDirectSoundCreate8),
		reinterpret_cast<LPVOID*>(&g_origDirectSoundCreate8));
	const bool bCreate = createExportHook(
		"DirectSoundCreate",
		reinterpret_cast<LPVOID>(&HookedDirectSoundCreate),
		reinterpret_cast<LPVOID*>(&g_origDirectSoundCreate));

	g_hooksInstalled = bCreate8 || bCreate;
	if (!g_hooksInstalled)
	{
		g_initAttempted = false;
	}

	Logger::log << "[Sound] "
		<< (g_hooksInstalled ? "installed" : "failed")
		<< " (retail identify + gated DS pause, SEH-safe)"
		<< std::endl;
}

bool Helpers::IsSoundHookActive()
{
	return g_hooksInstalled;
}

void Helpers::BeginActiveSoundCapture()
{
	InitSoundHook();
	g_inPause.store(false, std::memory_order_release);
	g_loggedRetailThisPause.store(false, std::memory_order_release);
	g_loggedPoolMissThisPause.store(false, std::memory_order_release);
	g_loggedDsPause.store(false, std::memory_order_release);
	g_retailIdentityKnown.store(false, std::memory_order_release);
	g_suppressReloadBuffers.store(false, std::memory_order_release);
	g_mutedRetailSounds.clear();
	g_primaryMuteTagId = 0;

	{
		std::lock_guard<std::mutex> lock(g_dsMutex);
		ReleaseBufferListLocked(g_candidateBuffers);
		ReleaseBufferListLocked(g_reloadBuffers);
		g_reloadCursors.clear();
		ReleaseBufferListLocked(g_playingAtMute);
		ClearProbedReloadBufferLocked();
		// Drop stale multi-buffer memory from older aggressive gates.
		while (g_rememberedReloadBuffers.size() > 1)
		{
			IDirectSoundBuffer* extra = g_rememberedReloadBuffers.back();
			g_rememberedReloadBuffers.pop_back();
			SafeBufferRelease(extra);
		}
		g_reloadListFinalized = false;
		g_playingAtMuteCaptured = false;
		g_pausePlayCursorValid = false;
		g_pausePlayCursor = 0;
		SnapshotCaptureBaselinesLocked();

		int baselinePlaying = 0;
		for (const CaptureBaseline& baseline : g_captureBaselines)
		{
			if (baseline.bPlaying)
			{
				baselinePlaying++;
			}
		}

		Logger::log << "[Sound] capture begin"
			<< " vtable=" << (g_bufferVtablePatched ? 1 : 0)
			<< " device=" << (g_deviceVtablePatched ? 1 : 0)
			<< " tracked=" << g_trackedBuffers.size()
			<< " baselinePlaying=" << baselinePlaying
			<< " remembered=" << g_rememberedReloadBuffers.size()
			<< std::endl;
	}

	g_captureReloadBuffers.store(true, std::memory_order_release);
}

void Helpers::ClearRememberedReloadSounds()
{
	g_mutedRetailSounds.clear();
	g_primaryMuteTagId = 0;
	g_pauseWeaponType = WeaponType::Unknown;
	g_reloadFpRate = 0;
	g_reloadFpBytes = 0;
	g_reloadFpValid = false;
	g_retailIdentityKnown.store(false, std::memory_order_release);
	std::lock_guard<std::mutex> lock(g_dsMutex);
	ReleaseBufferListLocked(g_candidateBuffers);
	ReleaseBufferListLocked(g_reloadBuffers);
	g_reloadCursors.clear();
	ReleaseBufferListLocked(g_rememberedReloadBuffers);
	ReleaseBufferListLocked(g_playingAtMute);
	ClearProbedReloadBufferLocked();
	g_captureBaselines.clear();
	g_reloadListFinalized = false;
	g_playingAtMuteCaptured = false;
}

void Helpers::PauseActiveSounds(WeaponType weaponType)
{
	InitSoundHook();
	g_captureReloadBuffers.store(false, std::memory_order_release);
	g_inPause.store(true, std::memory_order_release);
	g_suppressReloadBuffers.store(true, std::memory_order_release);
	g_pauseWeaponType = weaponType;

	MuteLocalRetailSounds(weaponType);
	const int silenced = SilenceGatedDsBuffers();

	bool bExpected = false;
	if (g_loggedDsPause.compare_exchange_strong(bExpected, true, std::memory_order_acq_rel))
	{
		size_t captured = 0;
		{
			std::lock_guard<std::mutex> lock(g_dsMutex);
			captured = g_reloadBuffers.size();
		}
		Logger::log << "[Sound] ds pause silenced=" << silenced
			<< " gated=" << captured
			<< " identity=" << (g_retailIdentityKnown.load(std::memory_order_acquire) ? 1 : 0)
			<< " weaponType=" << static_cast<int>(weaponType)
			<< std::endl;
	}
}

void Helpers::ClearActiveSounds(float skipSeconds)
{
	g_inPause.store(false, std::memory_order_release);
	g_suppressReloadBuffers.store(false, std::memory_order_release);
	g_captureReloadBuffers.store(false, std::memory_order_release);
	g_loggedRetailThisPause.store(false, std::memory_order_release);
	g_loggedPoolMissThisPause.store(false, std::memory_order_release);
	g_loggedDsPause.store(false, std::memory_order_release);

	const int retailRestored = RestoreMutedRetailSounds();
	const int dsResumed = ResumeGatedDsBuffers(skipSeconds);
	g_retailIdentityKnown.store(false, std::memory_order_release);
	g_primaryMuteTagId = 0;
	g_pauseWeaponType = WeaponType::Unknown;
	{
		std::lock_guard<std::mutex> lock(g_dsMutex);
		ReleaseBufferListLocked(g_playingAtMute);
		ClearProbedReloadBufferLocked();
		g_playingAtMuteCaptured = false;
		g_captureBaselines.clear();
	}

	Logger::log << "[Sound] clear pause retailAlive=" << retailRestored
		<< " dsResumed=" << dsResumed
		<< " skipSeconds=" << skipSeconds
		<< std::endl;
}

void Helpers::ResumeActiveSounds(bool bStopActiveSources)
{
	const bool bWasPaused = g_inPause.exchange(false, std::memory_order_acq_rel);
	g_suppressReloadBuffers.store(false, std::memory_order_release);
	g_captureReloadBuffers.store(false, std::memory_order_release);
	g_loggedRetailThisPause.store(false, std::memory_order_release);
	g_loggedPoolMissThisPause.store(false, std::memory_order_release);
	g_loggedDsPause.store(false, std::memory_order_release);

	g_retailIdentityKnown.store(false, std::memory_order_release);
	g_primaryMuteTagId = 0;
	g_pauseWeaponType = WeaponType::Unknown;

	int retailRestored = 0;
	if (bWasPaused)
	{
		retailRestored = RestoreMutedRetailSounds();
	}

	{
		std::lock_guard<std::mutex> lock(g_dsMutex);
		for (IDirectSoundBuffer* buffer : g_reloadBuffers)
		{
			// Always restore volume on abort so a cancelled reload cannot leave
			// the gated buffer stuck silent.
			SafeBufferSetVolume(buffer, DSBVOLUME_MAX);
			if (bStopActiveSources && IsBufferPlaying(buffer))
			{
				SafeBufferStop(buffer);
			}
		}
		ReleaseBufferListLocked(g_candidateBuffers);
		ReleaseBufferListLocked(g_reloadBuffers);
		g_reloadCursors.clear();
		ReleaseBufferListLocked(g_playingAtMute);
		ClearProbedReloadBufferLocked();
		g_captureBaselines.clear();
		g_reloadListFinalized = false;
		g_playingAtMuteCaptured = false;
		g_pausePlayCursorValid = false;
		g_pausePlayCursor = 0;
	}

	g_retailIdentityKnown.store(false, std::memory_order_release);

	if (bWasPaused || bStopActiveSources)
	{
		Logger::log << "[Sound] abort/resume"
			<< " stopActive=" << (bStopActiveSources ? 1 : 0)
			<< " wasPaused=" << (bWasPaused ? 1 : 0)
			<< " retailRestored=" << retailRestored
			<< std::endl;
	}
}

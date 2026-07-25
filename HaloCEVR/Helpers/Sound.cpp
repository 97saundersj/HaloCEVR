#include "Sound.h"
#include "../Hooking/Hooks.h"
#include "../Hooking/Hook.h"
#include "../Logger.h"
#include "Objects.h"
#include "Assets.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <sstream>
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
	// Index of the SoundPlaybackPool channel this voice occupies, 0xFFFF while idle. This
	// doubles as the "is playing" flag and is the only reliable entry -> DS buffer link.
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
	// Set when Halo finishes the reload during the hold — blocks rediscovery/re-gating.
	std::atomic_bool g_reloadIdentityExpired = false;
	std::vector<MutedRetailSound> g_mutedRetailSounds;
	uint32_t g_primaryMuteTagId = 0;
	WeaponType g_pauseWeaponType = WeaponType::Unknown;
	// Weapon-parented voices already active at capture begin (leftover companions, or the
	// previous reload's SFX still ringing out). Tracked per voice rather than per tag: a
	// rapid re-reload starts a new voice with the SAME tag, and excluding it by tag alone
	// left identity unknown and the reload unpaused.
	struct BaselineVoice
	{
		uint32_t tagId = 0;
		int slot = -1;
		int channel = -1;
	};
	std::vector<BaselineVoice> g_baselineWeaponVoices;
	// Halo CE sound tag class (tag data +0x04). weapon_reload is the long magazine SFX.
	constexpr int16_t kSoundClassWeaponReload = 6;

	// Weapon-parented tags that appear AFTER capture begin. On first sight we bind the
	// SoundsGlobal slot → SoundPlaybackPool DS buffer (the engine's start-time link).
	struct CaptureTagSighting
	{
		uint32_t tagId = 0;
		DWORD firstSeenMs = 0;
		int slot = -1;
		int16_t soundClass = -1;
		IDirectSoundBuffer* buffer = nullptr; // AddRef'd pool buffer at first sight
	};
	std::vector<CaptureTagSighting> g_captureWeaponTags;
	DWORD g_captureStartTickMs = 0;
	// Fresh DirectSound Play() starts during the capture window (before eject). Earliest
	// size-matching start is the reload voice; companions/grenades start later.
	struct CapturePlayStart
	{
		IDirectSoundBuffer* buffer = nullptr;
		DWORD startTickMs = 0;
		DWORD rate = 0;
		DWORD bytes = 0;
	};
	std::vector<CapturePlayStart> g_capturePlayStarts;
	// DS-buffer sizes accepted by the reload gate, discovered at runtime from the reload
	// sound tag(s). Only touched on the game thread (PauseActiveSounds → gate), so no lock.
	std::vector<DWORD> g_reloadAllowedBytes;

	bool IsValidSoundTagId(uint32_t tagId)
	{
		return tagId != 0 && tagId != 0xffffffffu;
	}

	// Minimal view of a Halo CE sound tag. tag+0x04 is the sound class (confirmed: it is
	// passed as a parameter in the sound-update path), so tag+0x06 is the sample-rate enum.
	struct SoundTagData
	{
		int32_t flags;      // 0x00
		int16_t soundClass; // 0x04
		int16_t sampleRate; // 0x06 (0=22050Hz, 1=44100Hz, 2=32000Hz)
	};

	// Sound asset table entry (0x20 stride, same as every other tag): data pointer at +0x14.
	struct Asset_Sound : public Asset_Base
	{
		char pad04[12];     // 0x04
		char* SoundPath;    // 0x10
		SoundTagData* Data; // 0x14
	};

	bool IsReadableMemory(const void* ptr, size_t bytes);

	// Resolve a sound tag's native sample rate (Hz) from its tag metadata. Returns 0 if the
	// tag can't be read or the rate is unknown. SEH-guarded: the asset array is game memory.
	DWORD GetSoundTagSampleRateHz(uint32_t tagId)
	{
		if (!IsValidSoundTagId(tagId))
		{
			return 0;
		}

		const uint16_t index = static_cast<uint16_t>(tagId & 0xFFFF);
		__try
		{
			Asset_Generic* base = Helpers::GetAssetArray();
			if (!base || !IsReadableMemory(base, sizeof(Asset_Generic)))
			{
				return 0;
			}

			const Asset_Sound* asset = reinterpret_cast<const Asset_Sound*>(
				reinterpret_cast<const char*>(base) + static_cast<size_t>(index) * sizeof(Asset_Generic));
			if (!IsReadableMemory(asset, sizeof(Asset_Sound)))
			{
				return 0;
			}

			const SoundTagData* data = asset->Data;
			if (!data || !IsReadableMemory(data, sizeof(SoundTagData)))
			{
				return 0;
			}

			switch (data->sampleRate)
			{
			case 0: return 22050;
			case 1: return 44100;
			case 2: return 32000;
			default: return 0;
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return 0;
		}
	}

	int16_t GetSoundTagClass(uint32_t tagId)
	{
		if (!IsValidSoundTagId(tagId))
		{
			return -1;
		}

		const uint16_t index = static_cast<uint16_t>(tagId & 0xFFFF);
		__try
		{
			Asset_Generic* base = Helpers::GetAssetArray();
			if (!base || !IsReadableMemory(base, sizeof(Asset_Generic)))
			{
				return -1;
			}

			const Asset_Sound* asset = reinterpret_cast<const Asset_Sound*>(
				reinterpret_cast<const char*>(base) + static_cast<size_t>(index) * sizeof(Asset_Generic));
			if (!IsReadableMemory(asset, sizeof(Asset_Sound)))
			{
				return -1;
			}

			const SoundTagData* data = asset->Data;
			if (!data || !IsReadableMemory(data, sizeof(SoundTagData)))
			{
				return -1;
			}

			return data->soundClass;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return -1;
		}
	}

	void PushUniqueSize(std::vector<DWORD>& sizes, DWORD value)
	{
		if (value != 0 && std::find(sizes.begin(), sizes.end(), value) == sizes.end())
		{
			sizes.push_back(value);
		}
	}

	// Halo allocates a fixed 3-second streaming ring buffer per voice, so a sound's DS-buffer
	// capacity is bytes = 3 * (2 * channels * rate) = 6 * channels * rate — determined entirely
	// by its audio format. We accept both mono and stereo for the discovered rate.
	void AddReloadSizesForRate(DWORD rate, std::vector<DWORD>& sizes)
	{
		if (rate == 0)
		{
			return;
		}
		PushUniqueSize(sizes, 6u * 1u * rate); // mono
		PushUniqueSize(sizes, 6u * 2u * rate); // stereo
	}

	// Fallback set covering every Halo CE output format, used when a tag's rate can't be read.
	void AddStandardReloadSizes(std::vector<DWORD>& sizes)
	{
		AddReloadSizesForRate(22050, sizes);
		AddReloadSizesForRate(44100, sizes);
		AddReloadSizesForRate(32000, sizes);
	}

	bool IsAllowedReloadBytes(DWORD bytes)
	{
		return std::find(g_reloadAllowedBytes.begin(), g_reloadAllowedBytes.end(), bytes)
			!= g_reloadAllowedBytes.end();
	}

	// Forward decls (DS helpers defined later).
	bool IsValidComObjectPointer(void* ptr);
	void SnapshotPlayingAtMute();
	void ProbeChannelForTrackedBuffer(uintptr_t entry);

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

	bool IsWeaponParentedSound(uintptr_t entry, const HaloID& weaponId)
	{
		if (weaponId.id == 0xffff || weaponId.index == 0xffff)
		{
			return false;
		}

		const uint32_t parentId = *reinterpret_cast<uint32_t*>(entry + kSoundParentOffset);
		return MatchesSoundParent(parentId, weaponId);
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

	// Playback-pool channel this voice occupies, or -1 while idle.
	int GetSoundEntryChannel(uintptr_t entry)
	{
		if (!entry)
		{
			return -1;
		}
		const uint16_t channelIndex = *reinterpret_cast<uint16_t*>(entry + kSoundOrderIdOffset);
		return (channelIndex == kSoundNotPlaying) ? -1 : static_cast<int>(channelIndex);
	}

	// A voice is the same one we saw at capture begin only if tag, slot AND channel match.
	// Halo cannot place a restarted voice on an occupied slot/channel, so a re-reload while
	// the previous SFX is alive always differs here.
	bool IsBaselineVoice(uint32_t tagId, int slot, int channel)
	{
		for (const BaselineVoice& voice : g_baselineWeaponVoices)
		{
			if (voice.tagId == tagId && voice.slot == slot && voice.channel == channel)
			{
				return true;
			}
		}
		return false;
	}

	uintptr_t FindEntryForTagId(uintptr_t manager, uint32_t tagId, int& outSlot)
	{
		outSlot = -1;
		if (!manager || !IsValidSoundTagId(tagId))
		{
			return 0;
		}

		uintptr_t found = 0;
		int foundSlot = -1;
		ForEachActiveSound(manager, [&](int slot, uintptr_t entry)
		{
			if (found || *reinterpret_cast<uint32_t*>(entry + kSoundTagIdOffset) != tagId)
			{
				return;
			}
			found = entry;
			foundSlot = slot;
		});

		outSlot = foundSlot;
		return found;
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

	int CountAliveMutedRetailSounds()
	{
		const uintptr_t manager = GetSoundManagerBase();
		if (!manager)
		{
			return 0;
		}

		int aliveCount = 0;
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
				continue;
			}

			const uint32_t tagId = *reinterpret_cast<uint32_t*>(entry + kSoundTagIdOffset);
			if (tagId != muted.tagId)
			{
				continue;
			}

			aliveCount++;
		}
		return aliveCount;
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

		const int aliveCount = CountAliveMutedRetailSounds();
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
			}
		}

		Logger::log << "[Sound] retail identity alive=" << aliveCount
			<< " finishedWhilePaused=" << finishedCount
			<< " tagMismatch=" << tagMismatchCount
			<< " pending=" << pending
			<< std::endl;

		g_mutedRetailSounds.clear();
		return aliveCount;
	}

	// Runtime reload-sound discovery. Prefer weapon-parented SoundsGlobal entries (the reload);
	// only fall back to the broader local/FP filter when none are present.
	void MuteLocalRetailSounds(WeaponType weaponType)
	{
		// Snapshot identity once at eject. Re-scanning every pause tick would absorb later
		// local one-shots while the magazine is held out.
		if (g_retailIdentityKnown.load(std::memory_order_acquire)
			|| g_reloadIdentityExpired.load(std::memory_order_acquire))
		{
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

		struct DiscoveredSound
		{
			int slot = -1;
			uintptr_t entry = 0;
			uint32_t tagId = 0;
			DWORD rate = 0;
			bool bWeaponParented = false;
		};
		std::vector<DiscoveredSound> weaponSounds;
		std::vector<DiscoveredSound> fallbackSounds;

		ForEachActiveSound(manager, [&](int slot, uintptr_t entry)
		{
			const uint32_t tagId = *reinterpret_cast<uint32_t*>(entry + kSoundTagIdOffset);
			if (!IsValidSoundTagId(tagId))
			{
				return;
			}

			DiscoveredSound discovered;
			discovered.slot = slot;
			discovered.entry = entry;
			discovered.tagId = tagId;
			discovered.rate = GetSoundTagSampleRateHz(tagId);
			discovered.bWeaponParented = IsWeaponParentedSound(entry, weaponId);

			if (discovered.bWeaponParented)
			{
				weaponSounds.push_back(discovered);
				return;
			}

			if (IsLocalPlayerOrWeaponSound(entry, playerId, weaponId))
			{
				fallbackSounds.push_back(discovered);
			}
		});

		const std::vector<DiscoveredSound>& candidates =
			!weaponSounds.empty() ? weaponSounds : fallbackSounds;
		const char* source = !weaponSounds.empty() ? "weapon" : "fallback";

		if (candidates.empty())
		{
			// No local reload voice yet — leave identity unknown so the gate stays off.
			return;
		}

		// Prefer NEW capture-era tags still alive at eject (reload). Baseline leftovers and
		// eject-only companions are excluded / lose on firstSeenMs.
		auto captureFirstSeen = [](uint32_t tagId) -> DWORD
		{
			for (const CaptureTagSighting& sighting : g_captureWeaponTags)
			{
				if (sighting.tagId == tagId)
				{
					return sighting.firstSeenMs;
				}
			}
			return 0xFFFFFFFFu;
		};

		std::vector<DiscoveredSound> preferred;
		for (const DiscoveredSound& candidate : candidates)
		{
			if (captureFirstSeen(candidate.tagId) != 0xFFFFFFFFu)
			{
				preferred.push_back(candidate);
			}
		}

		std::ostringstream allTagsLog;
		for (size_t i = 0; i < candidates.size(); i++)
		{
			if (i > 0)
			{
				allTagsLog << ' ';
			}
			allTagsLog << "0x" << std::hex << candidates[i].tagId << std::dec;
		}

		std::ostringstream captureTagsLog;
		for (size_t i = 0; i < g_captureWeaponTags.size(); i++)
		{
			if (i > 0)
			{
				captureTagsLog << ' ';
			}
			captureTagsLog << "0x" << std::hex << g_captureWeaponTags[i].tagId << std::dec;
		}

		// A weapon_reload voice on the current weapon at eject IS this reload, even when the
		// capture window never saw it start (rapid re-reload restarts the previous voice).
		// Companions are other sound classes, so this cannot latch onto one.
		const char* preferReason = "capture";
		if (preferred.empty())
		{
			for (const DiscoveredSound& candidate : candidates)
			{
				if (candidate.bWeaponParented
					&& GetSoundTagClass(candidate.tagId) == kSoundClassWeaponReload)
				{
					preferred.push_back(candidate);
				}
			}
			preferReason = "reloadClass";
		}

		if (preferred.empty())
		{
			// Nothing but baseline companions — do not lock onto a leftover.
			bool bExpectedMiss = false;
			if (g_loggedRetailThisPause.compare_exchange_strong(bExpectedMiss, true, std::memory_order_acq_rel))
			{
				Logger::log << "[Sound] reload discover skip noReloadVoice"
					<< " active=" << activeCount
					<< " candidates=" << candidates.size()
					<< " captureTags=" << g_captureWeaponTags.size()
					<< " baselineVoices=" << g_baselineWeaponVoices.size()
					<< " source=" << source
					<< " tags=[" << allTagsLog.str() << "]"
					<< " capture=[" << captureTagsLog.str() << "]"
					<< " weaponType=" << static_cast<int>(weaponType)
					<< std::endl;
			}
			return;
		}

		// Prefer weapon_reload class when present (companion SFX are usually other classes).
		std::vector<DiscoveredSound> reloadClass;
		for (const DiscoveredSound& candidate : preferred)
		{
			if (GetSoundTagClass(candidate.tagId) == kSoundClassWeaponReload)
			{
				reloadClass.push_back(candidate);
			}
		}
		const std::vector<DiscoveredSound>& pickFrom = !reloadClass.empty() ? reloadClass : preferred;

		// A fresh voice always beats one that was already ringing out at capture begin; among
		// equals, the one seen earliest in the capture window is the reload.
		auto isLeftover = [](const DiscoveredSound& candidate)
		{
			return IsBaselineVoice(candidate.tagId, candidate.slot, GetSoundEntryChannel(candidate.entry));
		};

		DiscoveredSound chosen = pickFrom.front();
		DWORD bestFirstSeen = captureFirstSeen(chosen.tagId);
		bool bChosenLeftover = isLeftover(chosen);
		for (size_t i = 1; i < pickFrom.size(); i++)
		{
			const DWORD firstSeen = captureFirstSeen(pickFrom[i].tagId);
			const bool bLeftover = isLeftover(pickFrom[i]);
			const bool bBetter = (bChosenLeftover && !bLeftover)
				|| (bChosenLeftover == bLeftover && firstSeen < bestFirstSeen);
			if (bBetter)
			{
				bestFirstSeen = firstSeen;
				bChosenLeftover = bLeftover;
				chosen = pickFrom[i];
			}
		}

		MuteSoundEntry(chosen.entry, chosen.slot, chosen.tagId);
		g_primaryMuteTagId = chosen.tagId;

		std::vector<DWORD> allowedBytes;
		AddReloadSizesForRate(chosen.rate, allowedBytes);

		// If no tag rate could be read, fall back to the standard Halo output-format sizes so
		// the reload is still caught (precision degrades to format-class, never worse).
		if (allowedBytes.empty())
		{
			AddStandardReloadSizes(allowedBytes);
		}

		g_reloadAllowedBytes = allowedBytes;

		bool bExpectedMute = false;
		if (g_loggedRetailThisPause.compare_exchange_strong(bExpectedMute, true, std::memory_order_acq_rel))
		{
			Logger::log << "[Sound] reload discover active=" << activeCount
				<< " candidates=" << candidates.size()
				<< " preferred=" << preferred.size()
				<< " via=" << preferReason
				<< " captureTags=" << g_captureWeaponTags.size()
				<< " baselineVoices=" << g_baselineWeaponVoices.size()
				<< " source=" << source
				<< " tags=[" << allTagsLog.str() << "]"
				<< " capture=[" << captureTagsLog.str() << "]"
				<< " chosen=0x" << std::hex << chosen.tagId << std::dec
				<< " slot=" << chosen.slot
				<< " class=" << GetSoundTagClass(chosen.tagId)
				<< " reloadClassHits=" << reloadClass.size()
				<< " rate=" << chosen.rate
				<< " weaponType=" << static_cast<int>(weaponType)
				<< std::endl;

			std::string sizes;
			for (DWORD b : allowedBytes)
			{
				sizes += std::to_string(b);
				sizes += ' ';
			}
			Logger::log << "[Sound] reload sizes=[ " << sizes << "]"
				<< " fallback=" << (chosen.rate == 0 ? 1 : 0)
				<< std::endl;
		}
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

	void ReleaseCapturePlayStartsLocked()
	{
		for (CapturePlayStart& start : g_capturePlayStarts)
		{
			SafeBufferRelease(start.buffer);
			start.buffer = nullptr;
		}
		g_capturePlayStarts.clear();
	}

	void ReleaseCaptureTagBuffers()
	{
		for (CaptureTagSighting& sighting : g_captureWeaponTags)
		{
			if (sighting.buffer)
			{
				SafeBufferRelease(sighting.buffer);
				sighting.buffer = nullptr;
			}
		}
	}

	const CaptureTagSighting* FindCaptureTagSighting(uint32_t tagId)
	{
		for (const CaptureTagSighting& sighting : g_captureWeaponTags)
		{
			if (sighting.tagId == tagId)
			{
				return &sighting;
			}
		}
		return nullptr;
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

	bool SafeGetBufferFingerprint(IDirectSoundBuffer* buffer, DWORD& outRate, DWORD& outBytes, DWORD* outBytesPerSec = nullptr)
	{
		outRate = 0;
		outBytes = 0;
		if (outBytesPerSec)
		{
			*outBytesPerSec = 0;
		}
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
			if (outBytesPerSec)
			{
				// Prefer reported average; fall back to PCM estimate from channel layout.
				DWORD bps = format.nAvgBytesPerSec;
				if (bps == 0)
				{
					const WORD channels = format.nChannels ? format.nChannels : 1;
					const WORD bits = format.wBitsPerSample ? format.wBitsPerSample : 16;
					bps = format.nSamplesPerSec * channels * (bits / 8);
				}
				*outBytesPerSec = bps;
			}
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	// Attach a fresh DS Play/Seek to the weapon_reload tag sighting (slot→pool is NOT 1:1).
	// First bind wins — later same-sized Play()s are companions/eject SFX and must not steal it.
	void AttachCapturePlayToReloadTagsLocked(IDirectSoundBuffer* buffer, DWORD startTickMs)
	{
		if (!buffer)
		{
			return;
		}

		for (CaptureTagSighting& sighting : g_captureWeaponTags)
		{
			if (sighting.soundClass != kSoundClassWeaponReload)
			{
				continue;
			}

			if (sighting.buffer)
			{
				return;
			}

			const int dt = static_cast<int>(startTickMs) - static_cast<int>(sighting.firstSeenMs);
			// Play may land slightly before the SoundsGlobal row is visible.
			if (dt < -200 || dt > 600)
			{
				continue;
			}

			if (!SafeBufferAddRef(buffer))
			{
				return;
			}

			sighting.buffer = buffer;
			Logger::log << "[Sound] tag bind play tag=0x" << std::hex << sighting.tagId << std::dec
				<< " slot=" << sighting.slot
				<< " buffer=" << buffer
				<< " dtMs=" << dt
				<< std::endl;
			return;
		}
	}

	void NoteCapturePlayStartLocked(IDirectSoundBuffer* buffer)
	{
		if (!buffer || !IsValidComObjectPointer(buffer))
		{
			return;
		}

		DWORD rate = 0;
		DWORD bytes = 0;
		DWORD bytesPerSec = 0;
		if (!SafeGetBufferFingerprint(buffer, rate, bytes, &bytesPerSec) || bytesPerSec == 0)
		{
			return;
		}

		// Reused ring buffers often Play() without seeking to 0 — reject those here so they
		// cannot become the earliest "reload" capturePlay candidate.
		const DWORD cursor = SafeGetPlayCursor(buffer);
		if (cursor == 0xFFFFFFFFu)
		{
			return;
		}
		const DWORD nowMs = GetTickCount();
		const float elapsedSec = (g_captureStartTickMs != 0 && nowMs >= g_captureStartTickMs)
			? static_cast<float>(nowMs - g_captureStartTickMs) / 1000.0f
			: 0.0f;
		const DWORD expected = static_cast<DWORD>(bytesPerSec * elapsedSec + 0.5f);
		const DWORD cursorError = (cursor > expected) ? (cursor - expected) : (expected - cursor);
		const DWORD maxError = (expected > 8000u) ? (expected / 3u) : 8000u;
		const bool bNearStart = cursor <= 4096u;
		if (!bNearStart && cursorError > maxError)
		{
			return;
		}

		// Same buffer restarted (Seek+Play) — refresh start time and rebind to reload tag.
		for (CapturePlayStart& existing : g_capturePlayStarts)
		{
			if (existing.buffer == buffer)
			{
				existing.startTickMs = nowMs;
				AttachCapturePlayToReloadTagsLocked(buffer, nowMs);
				return;
			}
		}

		if (!SafeBufferAddRef(buffer))
		{
			return;
		}

		CapturePlayStart start;
		start.buffer = buffer;
		start.startTickMs = nowMs;
		start.rate = rate;
		start.bytes = bytes;
		g_capturePlayStarts.push_back(start);

		Logger::log << "[Sound] capture play buffer=" << buffer
			<< " cursor=" << cursor
			<< " rate=" << rate
			<< " bytes=" << bytes
			<< " starts=" << g_capturePlayStarts.size()
			<< std::endl;

		AttachCapturePlayToReloadTagsLocked(buffer, nowMs);
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

	int PoolChannelCount(uintptr_t poolBase)
	{
		if (!poolBase || !IsReadableMemory(reinterpret_cast<const void*>(poolBase - 8), 8))
		{
			return kMaxSoundSlots;
		}

		const int count = *reinterpret_cast<const uint16_t*>(poolBase - 8);
		return (count <= 0 || count > 256) ? kMaxSoundSlots : count;
	}

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

	// Authoritative tag -> DS buffer link.
	//
	// A SoundsGlobal entry stores the playback-pool channel it currently occupies at +140
	// (the field we test as "not playing" when it reads 0xFFFF); that channel's +0x670 is the
	// voice's DirectSound buffer. Calibration converged on this offset across reloads.
	//
	// This needs no Play/Seek event, so it also resolves the case that kept failing: Halo
	// restarting the reload voice on a ring buffer that never stopped playing.
	IDirectSoundBuffer* GetBufferForSoundEntry(uintptr_t entry, bool bAlreadyHoldingDsMutex, int& outChannel)
	{
		outChannel = -1;
		if (!entry || !IsReadableMemory(reinterpret_cast<const void*>(entry), kSoundEntrySize))
		{
			return nullptr;
		}

		const uint16_t channelIndex = *reinterpret_cast<const uint16_t*>(entry + kSoundOrderIdOffset);
		if (channelIndex == kSoundNotPlaying)
		{
			return nullptr;
		}

		const uintptr_t poolBase = static_cast<uintptr_t>(Hooks::o.SoundPlaybackPool);
		if (!poolBase || channelIndex >= static_cast<uint16_t>(PoolChannelCount(poolBase)))
		{
			return nullptr;
		}

		const uintptr_t channelBase = poolBase
			+ static_cast<uintptr_t>(channelIndex) * static_cast<uintptr_t>(kPlaybackChannelSize);
		if (!IsReadableMemory(reinterpret_cast<const void*>(channelBase), kPlaybackChannelSize))
		{
			return nullptr;
		}

		const uintptr_t bufferAddr = SafeReadUIntPtr(channelBase + kPlaybackChannelDsBufferOffset);
		if (!bufferAddr)
		{
			return nullptr;
		}

		IDirectSoundBuffer* resolved = ResolveDsBufferCandidate(
			reinterpret_cast<IDirectSoundBuffer*>(bufferAddr),
			bAlreadyHoldingDsMutex);
		if (resolved)
		{
			outChannel = static_cast<int>(channelIndex);
		}
		return resolved;
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
	// Never call COM on unknown values (avoids the old 0x3F800000 crash). Caller holds g_dsMutex.
	void ProbeChannelForTrackedBufferLocked(uintptr_t entry)
	{
		if (!entry || !IsReadableMemory(reinterpret_cast<const void*>(entry), kSoundEntrySize))
		{
			return;
		}

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
	}

	void ProbeChannelForTrackedBuffer(uintptr_t entry)
	{
		std::lock_guard<std::mutex> lock(g_dsMutex);
		ProbeChannelForTrackedBufferLocked(entry);
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

	// The cursor-ranked pool scan is only sane right after eject; later it gates leftovers.
	constexpr DWORD kHeuristicPoolWindowMs = 500;
	DWORD g_firstGateAttemptMs = 0;
	std::string g_lastGateMissReason;

	// The gate runs every tick of the hold; only report a miss when its story changes.
	void LogGateMiss(const std::string& reason)
	{
		if (reason == g_lastGateMissReason)
		{
			return;
		}
		g_lastGateMissReason = reason;
		Logger::log << "[Sound] gate miss " << reason << std::endl;
	}

	DWORD MaxReloadCursorError(DWORD expected)
	{
		// Tight enough to reject wrong same-size leftovers (~err 9k–20k in recent logs).
		return (expected > 8000u) ? (expected / 4u) : 6000u;
	}

	// Validate a candidate DS buffer against discovered reload sizes + expected playhead.
	bool EvaluateReloadGateBuffer(
		IDirectSoundBuffer* buffer,
		float elapsedSec,
		DWORD& outCursor,
		DWORD& outExpected,
		DWORD& outError,
		DWORD& outBytes)
	{
		outCursor = 0;
		outExpected = 0;
		outError = 0;
		outBytes = 0;
		if (!buffer || !IsValidComObjectPointer(buffer) || !IsBufferPlaying(buffer))
		{
			return false;
		}

		DWORD rate = 0;
		DWORD bytes = 0;
		DWORD bytesPerSec = 0;
		if (!SafeGetBufferFingerprint(buffer, rate, bytes, &bytesPerSec) || bytesPerSec == 0
			|| !IsAllowedReloadBytes(bytes))
		{
			return false;
		}

		const DWORD cursor = SafeGetPlayCursor(buffer);
		if (cursor == 0xFFFFFFFFu)
		{
			return false;
		}

		const DWORD expected = static_cast<DWORD>(bytesPerSec * elapsedSec + 0.5f);
		const DWORD err = (cursor > expected) ? (cursor - expected) : (expected - cursor);
		if (err > MaxReloadCursorError(expected))
		{
			return false;
		}

		outCursor = cursor;
		outExpected = expected;
		outError = err;
		outBytes = bytes;
		return true;
	}

	void AcceptReloadGateLocked(
		IDirectSoundBuffer* buffer,
		DWORD cursor,
		DWORD expected,
		DWORD err,
		DWORD bytes,
		float elapsedSec,
		int slot,
		const char* source)
	{
		if (!buffer || !SafeBufferAddRef(buffer))
		{
			return;
		}

		g_reloadBuffers.push_back(buffer);
		g_reloadCursors.push_back(cursor);
		g_reloadListFinalized = true;

		Logger::log << "[Sound] reload gate source=" << source
			<< " slot=" << slot
			<< " buffer=" << buffer
			<< " cursor=" << cursor
			<< " expected=" << expected
			<< " err=" << err
			<< " bytes=" << bytes
			<< " elapsed=" << elapsedSec
			<< " capturePlays=" << g_capturePlayStarts.size()
			<< std::endl;
	}

	// Gate the reload voice ONCE at eject. The SoundsGlobal entry names its playback channel,
	// which owns the DS buffer; capture-window Play/Seek binds are only fallbacks.
	void FinalizeReloadBuffersFromIdentityLocked()
	{
		if (g_reloadListFinalized)
		{
			return;
		}

		// Only gate while a local reload voice actually exists (identity confirmed), so we
		// never mute an unrelated same-sized sound outside a reload.
		if (!g_retailIdentityKnown.load(std::memory_order_acquire))
		{
			return;
		}

		// Nothing to match against (discovery hasn't produced a size set yet).
		if (g_reloadAllowedBytes.empty())
		{
			return;
		}

		const DWORD nowMs = GetTickCount();
		const float elapsedSec = (g_captureStartTickMs != 0 && nowMs >= g_captureStartTickMs)
			? static_cast<float>(nowMs - g_captureStartTickMs) / 1000.0f
			: 0.8f; // ~pauseTicks/30 fallback

		const int mutedSlot = g_mutedRetailSounds.empty() ? -1 : g_mutedRetailSounds.front().slot;
		const uintptr_t mutedEntry = g_mutedRetailSounds.empty() ? 0 : g_mutedRetailSounds.front().entry;

		if (g_firstGateAttemptMs == 0)
		{
			g_firstGateAttemptMs = nowMs;
		}
		const bool bPoolWindowOpen = (nowMs - g_firstGateAttemptMs) <= kHeuristicPoolWindowMs;

		// 1) Deterministic channel link: the reload's SoundsGlobal entry names the playback
		// channel it occupies, and that channel owns the DS buffer. No playhead heuristic —
		// a reused ring buffer has a continuous cursor and would fail any such check.
		{
			int linkChan = -1;
			IDirectSoundBuffer* linked = GetBufferForSoundEntry(mutedEntry, true, linkChan);
			if (linked && !IsReloadBufferLocked(linked))
			{
				DWORD linkRate = 0;
				DWORD linkBytes = 0;
				DWORD linkBytesPerSec = 0;
				SafeGetBufferFingerprint(linked, linkRate, linkBytes, &linkBytesPerSec);

				const DWORD rawCursor = SafeGetPlayCursor(linked);
				const DWORD linkCursor = (rawCursor == 0xFFFFFFFFu) ? 0 : rawCursor;
				const DWORD linkExpected = static_cast<DWORD>(linkBytesPerSec * elapsedSec + 0.5f);
				const DWORD linkErr = (linkCursor > linkExpected)
					? (linkCursor - linkExpected) : (linkExpected - linkCursor);

				Logger::log << "[Sound] channel link chan=" << linkChan
					<< " tag=0x" << std::hex << g_primaryMuteTagId << std::dec
					<< " buffer=" << linked
					<< " playing=" << (IsBufferPlaying(linked) ? 1 : 0)
					<< std::endl;

				AcceptReloadGateLocked(linked, linkCursor, linkExpected, linkErr, linkBytes,
					elapsedSec, mutedSlot, "channelLink");
				return;
			}
		}

		DWORD cursor = 0;
		DWORD expected = 0;
		DWORD err = 0;
		DWORD bytes = 0;

		struct GateCandidate
		{
			IDirectSoundBuffer* buffer = nullptr;
			DWORD cursor = 0;
			DWORD bytes = 0;
			DWORD expected = 0;
			DWORD cursorError = 0;
			DWORD startTickMs = 0;
			const char* source = "none";
		};
		std::vector<GateCandidate> candidates;

		auto consider = [&](IDirectSoundBuffer* buffer, float useElapsed, const char* source, DWORD startTickMs = 0)
		{
			if (!buffer || !EvaluateReloadGateBuffer(buffer, useElapsed, cursor, expected, err, bytes))
			{
				return;
			}
			GateCandidate candidate;
			candidate.buffer = buffer;
			candidate.cursor = cursor;
			candidate.bytes = bytes;
			candidate.expected = expected;
			candidate.cursorError = err;
			candidate.startTickMs = startTickMs;
			candidate.source = source;
			candidates.push_back(candidate);
		};

		// 2) Capture-window Play/Seek starts — expected playhead from each buffer's own start.
		for (const CapturePlayStart& start : g_capturePlayStarts)
		{
			if (!start.buffer || !IsAllowedReloadBytes(start.bytes))
			{
				continue;
			}
			const float playElapsed = (start.startTickMs != 0 && nowMs >= start.startTickMs)
				? static_cast<float>(nowMs - start.startTickMs) / 1000.0f
				: elapsedSec;
			consider(start.buffer, playElapsed, "capturePlay", start.startTickMs);
		}

		// 3) Tag↔Play bind (set when capture Play correlates with weapon_reload tag).
		if (const CaptureTagSighting* sighting = FindCaptureTagSighting(g_primaryMuteTagId))
		{
			const float tagElapsed = (sighting->firstSeenMs != 0 && nowMs >= sighting->firstSeenMs)
				? static_cast<float>(nowMs - sighting->firstSeenMs) / 1000.0f
				: elapsedSec;
			consider(sighting->buffer, tagElapsed, "tagBind", sighting->firstSeenMs);
		}

		// 4) Probe muted SoundsGlobal entry for a tracked DS pointer.
		if (mutedEntry)
		{
			ProbeChannelForTrackedBufferLocked(mutedEntry);
			consider(g_probedReloadBuffer, elapsedSec, "probe");
		}

		// 5) Cursor-ranked pool scan, only while the eject is fresh. Later in the hold every
		// same-sized leftover looks plausible, which is how we used to gate junk at 1.6s.
		const uintptr_t poolBase = static_cast<uintptr_t>(Hooks::o.SoundPlaybackPool);
		if (bPoolWindowOpen && poolBase)
		{
			const int count = PoolChannelCount(poolBase);

			for (int chan = 0; chan < count; chan++)
			{
				const uintptr_t channelBase = poolBase + static_cast<uintptr_t>(chan) * kPlaybackChannelSize;
				if (!IsReadableMemory(reinterpret_cast<const void*>(channelBase), kPlaybackChannelSize))
				{
					continue;
				}
				if (*reinterpret_cast<const uint16_t*>(channelBase) == 0 || ChannelHasName(channelBase))
				{
					continue;
				}

				IDirectSoundBuffer* buffer = reinterpret_cast<IDirectSoundBuffer*>(
					SafeReadUIntPtr(channelBase + kPlaybackChannelDsBufferOffset));
				if (!buffer || IsReloadBufferLocked(buffer))
				{
					continue;
				}

				const CaptureBaseline* baseline = FindBaselineLocked(buffer);
				if (baseline && baseline->bPlaying)
				{
					const DWORD playCursor = SafeGetPlayCursor(buffer);
					const bool bCursorReset = playCursor != 0xFFFFFFFFu
						&& playCursor + 4096u < baseline->playCursor;
					if (!bCursorReset)
					{
						continue;
					}
				}

				consider(buffer, elapsedSec, "pool");
			}
		}

		if (candidates.empty())
		{
			const CaptureTagSighting* sighting = FindCaptureTagSighting(g_primaryMuteTagId);
			std::ostringstream reason;
			reason << "capturePlays=" << g_capturePlayStarts.size()
				<< " tagBound=" << (sighting && sighting->buffer ? 1 : 0)
				<< " poolWindow=" << (bPoolWindowOpen ? 1 : 0)
				<< " slot=" << mutedSlot;
			LogGateMiss(reason.str());
			return;
		}

		std::sort(candidates.begin(), candidates.end(),
			[](const GateCandidate& a, const GateCandidate& b)
			{
				if (a.cursorError != b.cursorError)
				{
					return a.cursorError < b.cursorError;
				}
				// Same playhead fit — earliest capture-window start is the reload voice.
				if (a.startTickMs != 0 && b.startTickMs != 0 && a.startTickMs != b.startTickMs)
				{
					return a.startTickMs < b.startTickMs;
				}
				return false;
			});

		const GateCandidate& best = candidates.front();
		if (best.cursorError > MaxReloadCursorError(best.expected))
		{
			std::ostringstream reason;
			reason << "bestErrK=" << (best.cursorError / 1000u)
				<< " max=" << MaxReloadCursorError(best.expected)
				<< " source=" << best.source;
			LogGateMiss(reason.str());
			return;
		}

		AcceptReloadGateLocked(
			best.buffer, best.cursor, best.expected, best.cursorError, best.bytes,
			elapsedSec, mutedSlot, best.source);
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

		const bool bCapture = g_captureReloadBuffers.load(std::memory_order_acquire);

		const HRESULT result = g_origBufferPlay
			? g_origBufferPlay(self, dwReserved1, dwPriority, dwFlags)
			: DSERR_GENERIC;

		// Record Play() during PlayingEject (including reuse of an already-playing buffer
		// when the playhead is near start — Halo often Seek+Play without a stopped gap).
		if (SUCCEEDED(result) && bCapture)
		{
			std::lock_guard<std::mutex> lock(g_dsMutex);
			NoteCapturePlayStartLocked(self);
		}

		return result;
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

		const HRESULT result = g_origBufferSetCurrentPosition
			? g_origBufferSetCurrentPosition(self, dwNewPosition)
			: DSERR_GENERIC;

		// Halo often reuses a voice with Seek(0) and no fresh stopped→Play edge.
		if (SUCCEEDED(result)
			&& g_captureReloadBuffers.load(std::memory_order_acquire)
			&& dwNewPosition <= 4096u)
		{
			std::lock_guard<std::mutex> lock(g_dsMutex);
			NoteCapturePlayStartLocked(self);
		}

		return result;
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

	// Halo ended the reload voice during the hold. Stop swallowing Stop/Play and kill the
	// gated streaming buffers now so they cannot loop when volume is restored on insert.
	void DiscardGatedReloadBuffersForDeadIdentity()
	{
		std::vector<IDirectSoundBuffer*> toDiscard;
		{
			std::lock_guard<std::mutex> lock(g_dsMutex);
			toDiscard = g_reloadBuffers;
			g_reloadBuffers.clear();
			g_reloadCursors.clear();
			ReleaseBufferListLocked(g_candidateBuffers);
			g_reloadListFinalized = false;
		}

		g_suppressReloadBuffers.store(false, std::memory_order_release);

		for (IDirectSoundBuffer* buffer : toDiscard)
		{
			if (!IsValidComObjectPointer(buffer))
			{
				SafeBufferRelease(buffer);
				continue;
			}

			SafeBufferSetVolume(buffer, DSBVOLUME_MAX);
			SafeBufferStop(buffer);
			Logger::log << "[Sound] ds discard deadIdentity(midHold) buffer=" << buffer << std::endl;
			SafeBufferRelease(buffer);
		}

		g_mutedRetailSounds.clear();
		g_retailIdentityKnown.store(false, std::memory_order_release);
		g_reloadIdentityExpired.store(true, std::memory_order_release);
		g_primaryMuteTagId = 0;
		g_reloadAllowedBytes.clear();
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
			RememberReloadBuffersLocked();
			toResume = g_reloadBuffers;
			toResumeCursors = g_reloadCursors;
			g_reloadBuffers.clear();
			g_reloadCursors.clear();
			ReleaseBufferListLocked(g_candidateBuffers);
			ReleaseCapturePlayStartsLocked();
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

			// Stop first so we don't continue a Halo LOOPING streaming play state (that was
			// the infinite-loop bug after identity died mid-hold). Then seek + one-shot Play.
			SafeBufferStop(buffer);

			const int seekOk = SafeSeekBufferForward(buffer, skipSeconds, pauseCursor, true);
			if (!seekOk)
			{
				Logger::log << "[Sound] ds resume seekPastEnd buffer=" << buffer
					<< " skipSeconds=" << skipSeconds
					<< " pauseCursor=" << pauseCursor
					<< " (playing from current)"
					<< std::endl;
			}

			const int playOk = SafeBufferPlay(buffer);
			if (!playOk)
			{
				Logger::log << "[Sound] ds resume playFail buffer=" << buffer << std::endl;
			}
			else
			{
				resumed++;
			}

			Logger::log << "[Sound] ds resume buffer=" << buffer
				<< " volume=" << (volRead ? volumeAfter : 0x7FFFFFFF)
				<< " seekOk=" << seekOk
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
	g_reloadIdentityExpired.store(false, std::memory_order_release);
	g_suppressReloadBuffers.store(false, std::memory_order_release);
	g_mutedRetailSounds.clear();
	g_primaryMuteTagId = 0;
	g_reloadAllowedBytes.clear();
	ReleaseCaptureTagBuffers();
	g_captureWeaponTags.clear();
	g_baselineWeaponVoices.clear();
	g_captureStartTickMs = GetTickCount();
	g_firstGateAttemptMs = 0;
	g_lastGateMissReason.clear();

	// Tags already playing before TriggerWeaponReload are leftovers (not this reload).
	{
		const uintptr_t manager = GetSoundManager();
		HaloID weaponId{};
		BaseDynamicObject* player = Helpers::GetLocalPlayer();
		if (manager && player && player->weapon.id != 0xffff)
		{
			weaponId = player->weapon;
			ForEachActiveSound(manager, [&](int slot, uintptr_t entry)
			{
				if (!IsWeaponParentedSound(entry, weaponId))
				{
					return;
				}
				const uint32_t tagId = *reinterpret_cast<uint32_t*>(entry + kSoundTagIdOffset);
				if (!IsValidSoundTagId(tagId))
				{
					return;
				}

				BaselineVoice voice;
				voice.tagId = tagId;
				voice.slot = slot;
				voice.channel = GetSoundEntryChannel(entry);
				g_baselineWeaponVoices.push_back(voice);
			});
		}
	}

	{
		std::lock_guard<std::mutex> lock(g_dsMutex);
		ReleaseBufferListLocked(g_candidateBuffers);
		ReleaseBufferListLocked(g_reloadBuffers);
		g_reloadCursors.clear();
		ReleaseBufferListLocked(g_playingAtMute);
		ClearProbedReloadBufferLocked();
		ReleaseCapturePlayStartsLocked();
		// Unconditionally stop prior-resume leftovers before baselines.
		const size_t rememberedCount = g_rememberedReloadBuffers.size();
		for (IDirectSoundBuffer* buffer : g_rememberedReloadBuffers)
		{
			SafeBufferStop(buffer);
			SafeBufferSetVolume(buffer, DSBVOLUME_MAX);
		}
		ReleaseBufferListLocked(g_rememberedReloadBuffers);
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
			<< " baselineVoices=" << g_baselineWeaponVoices.size()
			<< " remembered=" << rememberedCount
			<< std::endl;
	}

	g_captureReloadBuffers.store(true, std::memory_order_release);
}

void Helpers::SnapshotCaptureWeaponTags()
{
	const uintptr_t manager = GetSoundManager();
	if (!manager)
	{
		return;
	}

	HaloID weaponId{};
	BaseDynamicObject* player = Helpers::GetLocalPlayer();
	if (!player || player->weapon.id == 0xffff)
	{
		return;
	}
	weaponId = player->weapon;

	const DWORD nowMs = GetTickCount();
	ForEachActiveSound(manager, [&](int slot, uintptr_t entry)
	{
		if (!IsWeaponParentedSound(entry, weaponId))
		{
			return;
		}

		const uint32_t tagId = *reinterpret_cast<uint32_t*>(entry + kSoundTagIdOffset);
		if (!IsValidSoundTagId(tagId))
		{
			return;
		}

		// Ignore the exact voices that were already active at capture begin.
		if (IsBaselineVoice(tagId, slot, GetSoundEntryChannel(entry)))
		{
			return;
		}

		for (const CaptureTagSighting& sighting : g_captureWeaponTags)
		{
			if (sighting.tagId == tagId)
			{
				return;
			}
		}

		// The entry names its playback channel, and that channel owns the DS buffer. Fall
		// back to the nearest capture Play/Seek only if the channel isn't resolvable yet.
		IDirectSoundBuffer* bound = nullptr;
		int boundChannel = -1;
		{
			std::lock_guard<std::mutex> lock(g_dsMutex);
			IDirectSoundBuffer* linked = GetBufferForSoundEntry(entry, true, boundChannel);
			if (linked && SafeBufferAddRef(linked))
			{
				bound = linked;
			}

			if (!bound)
			{
				DWORD bestDt = 250;
				IDirectSoundBuffer* best = nullptr;
				for (const CapturePlayStart& start : g_capturePlayStarts)
				{
					if (!start.buffer)
					{
						continue;
					}
					const DWORD dt = (nowMs >= start.startTickMs)
						? (nowMs - start.startTickMs) : (start.startTickMs - nowMs);
					if (dt <= bestDt)
					{
						bestDt = dt;
						best = start.buffer;
					}
				}
				if (best && SafeBufferAddRef(best))
				{
					bound = best;
				}
			}
		}

		CaptureTagSighting sighting;
		sighting.tagId = tagId;
		sighting.firstSeenMs = nowMs;
		sighting.slot = slot;
		sighting.soundClass = GetSoundTagClass(tagId);
		sighting.buffer = bound;
		g_captureWeaponTags.push_back(sighting);

		Logger::log << "[Sound] tag start tag=0x" << std::hex << tagId << std::dec
			<< " slot=" << slot
			<< " class=" << sighting.soundClass
			<< " chan=" << boundChannel
			<< " buffer=" << bound
			<< std::endl;

		// If Play arrives after this snapshot, AttachCapturePlayToReloadTags will bind it.
	});
}

void Helpers::ClearRememberedReloadSounds()
{
	g_mutedRetailSounds.clear();
	g_primaryMuteTagId = 0;
	g_pauseWeaponType = WeaponType::Unknown;
	g_reloadAllowedBytes.clear();
	ReleaseCaptureTagBuffers();
	g_captureWeaponTags.clear();
	g_baselineWeaponVoices.clear();
	g_captureStartTickMs = 0;
	g_firstGateAttemptMs = 0;
	g_lastGateMissReason.clear();
	g_reloadFpRate = 0;
	g_reloadFpBytes = 0;
	g_reloadFpValid = false;
	g_retailIdentityKnown.store(false, std::memory_order_release);
	g_reloadIdentityExpired.store(false, std::memory_order_release);
	std::lock_guard<std::mutex> lock(g_dsMutex);
	ReleaseBufferListLocked(g_candidateBuffers);
	ReleaseBufferListLocked(g_reloadBuffers);
	g_reloadCursors.clear();
	ReleaseBufferListLocked(g_rememberedReloadBuffers);
	ReleaseBufferListLocked(g_playingAtMute);
	ClearProbedReloadBufferLocked();
	ReleaseCapturePlayStartsLocked();
	g_captureBaselines.clear();
	g_reloadListFinalized = false;
	g_playingAtMuteCaptured = false;
}

void Helpers::PauseActiveSounds(WeaponType weaponType)
{
	InitSoundHook();
	g_captureReloadBuffers.store(false, std::memory_order_release);
	g_inPause.store(true, std::memory_order_release);
	g_pauseWeaponType = weaponType;
	g_suppressReloadBuffers.store(true, std::memory_order_release);

	MuteLocalRetailSounds(weaponType);

	// Keep the gated DS buffer frozen for the whole hold even if SoundsGlobal marks the
	// channel finished — mid-hold discard was aborting resume after grenade throws.
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
	// Always mid-clip resume the gated buffer if we still hold one. Identity may have died
	// during the hold; Stop+Play (non-loop) in ResumeGatedDsBuffers prevents the old loop.
	const int dsResumed = ResumeGatedDsBuffers(skipSeconds);
	g_retailIdentityKnown.store(false, std::memory_order_release);
	g_reloadIdentityExpired.store(false, std::memory_order_release);
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
	g_reloadIdentityExpired.store(false, std::memory_order_release);
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

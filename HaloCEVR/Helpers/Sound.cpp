#include "Sound.h"
#include "DirectSoundHook.h"
#include "../Hooking/Hooks.h"
#include "../Logger.h"
#include "Objects.h"
#include <cstring>

namespace
{
	constexpr int kSoundEntrySize = 176;
	constexpr int kMaxSoundSlots = 100;
	constexpr int kSoundOrderIdOffset = 140;
	constexpr int kSoundParentOffset = 12;
	constexpr int kSoundScaleOffset = 24;
	constexpr int kSoundGainOffset = 28;
	constexpr int kSoundFirstPersonOffset = 172;
	constexpr uint16_t kSoundNotPlaying = 0xFFFF;

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

		if ((parentId & 0xFFFF) == objectId.index)
		{
			return true;
		}

		return false;
	}

	uintptr_t GetSoundManager(bool bDebugLog)
	{
		if (Helpers::DirectSoundHook::IsActive())
		{
			return 0;
		}

		const uintptr_t globalAddress = static_cast<uintptr_t>(Hooks::o.SoundsGlobal.Address);
		if (!globalAddress)
		{
			if (bDebugLog)
			{
				static bool loggedNullGlobal = false;
				if (!loggedNullGlobal)
				{
					loggedNullGlobal = true;
					Logger::log << "[Sound] SoundsGlobal.Address is null" << std::endl;
				}
			}
			return 0;
		}

		auto tryManager = [&](uintptr_t manager, const char* label) -> uintptr_t
		{
			const uintptr_t soundArray = *reinterpret_cast<uintptr_t*>(manager + 52);
			const uint16_t activeCount = *reinterpret_cast<uint16_t*>(manager + 48);
			if (bDebugLog)
			{
				Logger::log << "[Sound] " << label
					<< " manager=0x" << std::hex << manager
					<< " array=0x" << soundArray
					<< " active=" << std::dec << activeCount << std::endl;
			}

			if (soundArray && activeCount > 0)
			{
				return manager;
			}

			return 0;
		};

		const uintptr_t managerPtr = *reinterpret_cast<uintptr_t*>(globalAddress);
		if (managerPtr)
		{
			if (uintptr_t heap = tryManager(managerPtr, "heap"))
			{
				return heap;
			}
		}

		if (uintptr_t embedded = tryManager(globalAddress, "embedded"))
		{
			return embedded;
		}

		if (bDebugLog)
		{
			static bool loggedEmptyManager = false;
			if (!loggedEmptyManager)
			{
				loggedEmptyManager = true;
				Logger::log << "[Sound] no active sounds via manager global=0x"
					<< std::hex << globalAddress
					<< " deref=0x" << managerPtr << std::dec << std::endl;
			}
		}

		return 0;
	}

	template<typename Fn>
	void ForEachActiveSound(uintptr_t manager, Fn callback)
	{
		const uint16_t activeCount = *reinterpret_cast<uint16_t*>(manager + 48);
		const uintptr_t soundArray = *reinterpret_cast<uintptr_t*>(manager + 52);
		if (!soundArray || activeCount == 0)
		{
			return;
		}

		int seen = 0;
		for (int i = 0; i < kMaxSoundSlots && seen < activeCount; i++)
		{
			const uintptr_t entry = soundArray + i * kSoundEntrySize;
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

	void StopSoundEntry(uintptr_t entry)
	{
		*reinterpret_cast<uint16_t*>(entry + kSoundOrderIdOffset) = kSoundNotPlaying;
		*reinterpret_cast<float*>(entry + kSoundScaleOffset) = 0.0f;
		*reinterpret_cast<float*>(entry + kSoundGainOffset) = 0.0f;
	}

	void ProcessActiveSounds(bool bClearOnly, bool bDebugLog)
	{
		if (Helpers::DirectSoundHook::IsActive())
		{
			return;
		}

		const uintptr_t manager = GetSoundManager(bDebugLog);
		if (!manager)
		{
			return;
		}

		HaloID playerId{};
		if (!Helpers::GetLocalPlayerID(playerId))
		{
			if (bDebugLog)
			{
				Logger::log << "[Sound] no local player id" << std::endl;
			}
			return;
		}

		HaloID weaponId{};
		BaseDynamicObject* player = Helpers::GetLocalPlayer();
		if (player && player->weapon.id != 0xffff)
		{
			weaponId = player->weapon;
		}

		const uint16_t activeCount = *reinterpret_cast<uint16_t*>(manager + 48);
		int matchedCount = 0;
		ForEachActiveSound(manager, [&](int /*slot*/, uintptr_t entry)
		{
			if (IsLocalPlayerOrWeaponSound(entry, playerId, weaponId))
			{
				matchedCount++;
			}
		});

		const bool bStopAllDuringPause = !bClearOnly && matchedCount == 0 && activeCount > 0;
		int stoppedCount = 0;

		ForEachActiveSound(manager, [&](int /*slot*/, uintptr_t entry)
		{
			const bool bMatched = IsLocalPlayerOrWeaponSound(entry, playerId, weaponId);
			if (bClearOnly && !bMatched)
			{
				return;
			}

			if (!bClearOnly && !bMatched && !bStopAllDuringPause)
			{
				return;
			}

			StopSoundEntry(entry);
			stoppedCount++;
		});

		if (bDebugLog)
		{
			Logger::log << "[Sound] retail active=" << activeCount
				<< " matched=" << matchedCount
				<< " stopped=" << stoppedCount
				<< " stopAllFallback=" << bStopAllDuringPause
				<< std::endl;
		}
	}
}

void Helpers::BeginActiveSoundCapture()
{
	Helpers::DirectSoundHook::Init();
	Helpers::DirectSoundHook::BeginCapture();
}

void Helpers::PauseActiveSounds(unsigned int stopDelayMs, bool bDebugLog)
{
	Helpers::DirectSoundHook::SetDebugLogging(bDebugLog);
	Helpers::DirectSoundHook::Init();
	Helpers::DirectSoundHook::EnterPause(stopDelayMs);
	ProcessActiveSounds(false, bDebugLog);
}

void Helpers::ClearActiveSounds(bool bDebugLog)
{
	Helpers::DirectSoundHook::SetDebugLogging(bDebugLog);
	Helpers::DirectSoundHook::ResumeCaptured();
	ProcessActiveSounds(true, bDebugLog);
}

void Helpers::ResumeActiveSounds(bool bStopActiveSources, bool bDebugLog)
{
	Helpers::DirectSoundHook::SetDebugLogging(bDebugLog);
	Helpers::DirectSoundHook::ExitPause(bStopActiveSources);
	if (bDebugLog && bStopActiveSources)
	{
		Logger::log << "[Sound] resume requested stopActive=" << bStopActiveSources << std::endl;
	}
}

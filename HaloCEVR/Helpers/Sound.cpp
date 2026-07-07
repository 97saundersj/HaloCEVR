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
	constexpr int kSoundManagerActiveCountOffset = 48;
	constexpr int kSoundManagerSoundArrayOffset = 52;
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

	uintptr_t GetSoundManager()
	{
		if (Helpers::DirectSoundHook::IsActive())
		{
			return 0;
		}

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
		const uint16_t activeCount = *reinterpret_cast<uint16_t*>(manager + kSoundManagerActiveCountOffset);
		Logger::log << "[Sound] manager=0x" << std::hex << manager
			<< " array=0x" << soundArray
			<< " active=" << std::dec << activeCount << std::endl;

		if (!soundArray || activeCount == 0)
		{
			return 0;
		}

		return manager;
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

	void ProcessActiveSounds(bool bClearOnly)
	{
		if (Helpers::DirectSoundHook::IsActive())
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
			Logger::log << "[Sound] no local player id" << std::endl;
			return;
		}

		HaloID weaponId{};
		BaseDynamicObject* player = Helpers::GetLocalPlayer();
		if (player && player->weapon.id != 0xffff)
		{
			weaponId = player->weapon;
		}

		const uint16_t activeCount = *reinterpret_cast<uint16_t*>(manager + kSoundManagerActiveCountOffset);
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

		Logger::log << "[Sound] retail active=" << activeCount
			<< " matched=" << matchedCount
			<< " stopped=" << stoppedCount
			<< " stopAllFallback=" << bStopAllDuringPause
			<< std::endl;
	}
}

void Helpers::BeginActiveSoundCapture()
{
	Helpers::DirectSoundHook::Init();
	Helpers::DirectSoundHook::BeginCapture();
}

void Helpers::PauseActiveSounds(unsigned int stopDelayMs)
{
	Helpers::DirectSoundHook::Init();
	Helpers::DirectSoundHook::EnterPause(stopDelayMs);
	ProcessActiveSounds(false);
}

void Helpers::ClearActiveSounds()
{
	Helpers::DirectSoundHook::ResumeCaptured();
	ProcessActiveSounds(true);
}

void Helpers::ResumeActiveSounds(bool bStopActiveSources)
{
	Helpers::DirectSoundHook::ExitPause(bStopActiveSources);
	if (bStopActiveSources)
	{
		Logger::log << "[Sound] resume requested stopActive=" << bStopActiveSources << std::endl;
	}
}

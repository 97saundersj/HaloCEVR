#pragma once
#include <../../../ThirdParty/nlohmann/json.hpp>
#include <filesystem>
#include <list>
#include <string>
#include <vector>
#include "WeaponHandler.h"

using json = nlohmann::json;

struct WeaponManualReloadSettings
{
	WeaponType Weapon = WeaponType::Unknown;
	std::string Description;
	int PauseTicks = 10;
	int ResumeTicks = 30;
	std::vector<std::string> MagazineBoneNames;
	std::string PreferredMagazineBone;
	bool ContinuousReload = false;
	bool AutoContinuousReload = false;
	bool FireWhileContinuousReload = false;
	bool UseEmptyReloadAnimOnly = false;
	uint16_t TubeCapacity = 0;
};

class WeaponManualReloadConfigManager
{
public:
	WeaponManualReloadConfigManager();

	void LoadConfig() const;
	const WeaponManualReloadSettings& GetSettings(WeaponType type) const;
	bool IsMagazineBoneName(WeaponType type, const char* name) const;
	int GetMagazineBonePriority(WeaponType type, const char* name) const;

	mutable std::filesystem::file_time_type Version;
	mutable bool ReloadOnChange = true;

protected:
	mutable WeaponManualReloadSettings defaults;
	mutable std::list<WeaponManualReloadSettings> weaponList;
	mutable WeaponManualReloadSettings fallbackSettings;

	WeaponManualReloadSettings ParseSettingsFromJson(const json& entry, const WeaponManualReloadSettings& base) const;
	static bool MatchesBoneName(const char* boneName, const std::string& configName);
};

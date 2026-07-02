#pragma once
#include <../../../ThirdParty/nlohmann/json.hpp>
#include <filesystem>
#include <list>
#include <string>
#include "WeaponHandler.h"

using json = nlohmann::json;

struct WeaponManualReloadSettings
{
	WeaponType Weapon = WeaponType::Unknown;
	std::string Description;
	int PauseTicks = 10;
	int ResumeTicks = 30;
	std::string MagazineBoneName = "frame magazine";
	bool ContinuousReload = false;
	uint16_t MagazineCapacity = 0;
};

class WeaponManualReloadConfigManager
{
public:
	WeaponManualReloadConfigManager();

	void LoadConfig() const;
	const WeaponManualReloadSettings& GetSettings(WeaponType type) const;
	bool IsMagazineBoneName(WeaponType type, const char* name) const;

	mutable std::filesystem::file_time_type Version;
	mutable bool ReloadOnChange = true;

protected:
	mutable WeaponManualReloadSettings defaults;
	mutable std::list<WeaponManualReloadSettings> weaponList;
	mutable WeaponManualReloadSettings fallbackSettings;

	WeaponManualReloadSettings ParseSettingsFromJson(const json& entry, const WeaponManualReloadSettings& base) const;
	static bool MatchesBoneName(const char* boneName, const std::string& configName);
};

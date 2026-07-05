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

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	WeaponManualReloadSettings,
	Weapon,
	Description,
	PauseTicks,
	ResumeTicks,
	MagazineBoneName,
	ContinuousReload,
	MagazineCapacity)

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
	mutable std::list<WeaponManualReloadSettings> weaponList;
	mutable WeaponManualReloadSettings fallbackSettings;

	static bool MatchesBoneName(const char* boneName, const std::string& configName);
};

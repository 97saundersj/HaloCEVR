#include "WeaponManualReloadConfig.h"
#include "Logger.h"
#include <fstream>

namespace fs = std::filesystem;

WeaponManualReloadConfigManager::WeaponManualReloadConfigManager()
{
	fallbackSettings = defaults;
	fallbackSettings.Weapon = WeaponType::Unknown;

	Logger::log << "[WeaponManualReloadConfig] Initializing" << std::endl;
	LoadConfig();
}

bool WeaponManualReloadConfigManager::MatchesBoneName(const char* boneName, const std::string& configName)
{
	if (!boneName || !boneName[0] || configName.empty())
	{
		return false;
	}

	return _stricmp(boneName, configName.c_str()) == 0;
}

WeaponManualReloadSettings WeaponManualReloadConfigManager::ParseSettingsFromJson(
	const json& entry,
	const WeaponManualReloadSettings& base) const
{
	WeaponManualReloadSettings settings = base;

	if (entry.contains("Description"))
	{
		settings.Description = entry["Description"].get<std::string>();
	}

	if (entry.contains("PauseTicks"))
	{
		settings.PauseTicks = entry["PauseTicks"];
	}

	if (entry.contains("ResumeTicks"))
	{
		settings.ResumeTicks = entry["ResumeTicks"];
	}

	if (entry.contains("MagazineBoneName"))
	{
		settings.MagazineBoneName = entry["MagazineBoneName"].get<std::string>();
	}

	if (entry.contains("ContinuousReload"))
	{
		settings.ContinuousReload = entry["ContinuousReload"];
	}

	if (entry.contains("MagazineCapacity"))
	{
		settings.MagazineCapacity = entry["MagazineCapacity"];
	}
	else if (entry.contains("WeaponCapacity"))
	{
		settings.MagazineCapacity = entry["WeaponCapacity"];
	}

	return settings;
}

void WeaponManualReloadConfigManager::LoadConfig() const
{
	const std::string configPath = "VR/OpenVR/manual_reload.json";
	fs::path f{ configPath };

	if (!fs::exists(f))
	{
		Logger::log << "[WeaponManualReloadConfig] Config file " << configPath << " was not found." << std::endl;
		return;
	}

	const fs::file_time_type latestVersion = fs::last_write_time(configPath);
	const bool reload = ReloadOnChange && (Version < latestVersion);

	if (!reload)
	{
		return;
	}

	Logger::log << "[WeaponManualReloadConfig] Loading config" << std::endl;

	weaponList = {};
	Version = latestVersion;

	std::ifstream ifs(configPath);
	json jf = json::parse(ifs);

	defaults = WeaponManualReloadSettings{};

	try
	{
		if (jf.contains("Defaults"))
		{
			defaults = ParseSettingsFromJson(jf["Defaults"], defaults);
		}
	}
	catch (...)
	{
		Logger::log << "[WeaponManualReloadConfig] There was an issue loading Defaults" << std::endl;
	}

	fallbackSettings = defaults;
	fallbackSettings.Weapon = WeaponType::Unknown;

	try
	{
		ReloadOnChange = jf["ReloadOnChange"];
	}
	catch (...)
	{
	}

	try
	{
		json weapons = jf["Weapons"];
		for (const auto& item : weapons)
		{
			try
			{
				WeaponManualReloadSettings settings = defaults;
				settings.Weapon = item["Weapon"];
				settings = ParseSettingsFromJson(item, settings);
				weaponList.push_back(settings);
			}
			catch (...)
			{
				Logger::log << "[WeaponManualReloadConfig] There was an issue parsing weapon entry" << std::endl;
			}
		}
	}
	catch (...)
	{
		Logger::log << "[WeaponManualReloadConfig] There was an issue loading Weapons" << std::endl;
	}
}

const WeaponManualReloadSettings& WeaponManualReloadConfigManager::GetSettings(WeaponType type) const
{
	LoadConfig();

	for (const WeaponManualReloadSettings& settings : weaponList)
	{
		if (settings.Weapon == type)
		{
			return settings;
		}
	}

	return fallbackSettings;
}

bool WeaponManualReloadConfigManager::IsMagazineBoneName(WeaponType type, const char* name) const
{
	if (!name || !name[0])
	{
		return false;
	}

	const WeaponManualReloadSettings& settings = GetSettings(type);
	return MatchesBoneName(name, settings.MagazineBoneName);
}

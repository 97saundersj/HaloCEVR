#include "WeaponManualReloadConfig.h"
#include "Logger.h"
#include <fstream>

namespace fs = std::filesystem;

WeaponManualReloadConfigManager::WeaponManualReloadConfigManager()
{
	fallbackSettings = WeaponManualReloadSettings{};
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

	fallbackSettings = WeaponManualReloadSettings{};
	fallbackSettings.Weapon = WeaponType::Unknown;

	try
	{
		ReloadOnChange = jf.value("ReloadOnChange", true);
	}
	catch (...)
	{
	}

	const json base = WeaponManualReloadSettings{};

	try
	{
		for (const auto& item : jf.value("Weapons", json::array()))
		{
			try
			{
				json merged = base;
				merged.update(item);
				WeaponManualReloadSettings settings = merged.get<WeaponManualReloadSettings>();

				if (const auto it = item.find("MagazinePouchRotation"); it != item.end() && it->is_object())
				{
					settings.MagazinePouchRotation.x = it->value("x", 0.0f);
					settings.MagazinePouchRotation.y = it->value("y", 0.0f);
					settings.MagazinePouchRotation.z = it->value("z", 0.0f);
				}

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

#include "Assets.h"
#include "../Hooking/Hooks.h"

Asset_Generic* Helpers::GetAssetArray()
{
    return *reinterpret_cast<Asset_Generic**>(Hooks::o.AssetsArray);
}

const char* Helpers::GetAssetPath(HaloID ID)
{
	if (ID.id == static_cast<uint16_t>(-1) || ID.index == static_cast<uint16_t>(-1))
	{
		return nullptr;
	}

	Asset_PathTagged* asset = reinterpret_cast<Asset_PathTagged*>(&GetAssetArray()[ID.index]);
	return asset->AssetPath;
}

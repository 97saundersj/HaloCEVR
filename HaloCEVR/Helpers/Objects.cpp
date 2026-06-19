#include "Objects.h"
#include "../Hooking/Hooks.h"

ObjectTable& Helpers::GetObjectTable()
{
	return **reinterpret_cast<ObjectTable**>(Hooks::o.ObjectTable);
}

PlayerTable& Helpers::GetPlayerTable()
{
	return **reinterpret_cast<PlayerTable**>(Hooks::o.PlayerTable);
}

BaseDynamicObject* Helpers::GetDynamicObject(HaloID& id)
{
	ObjectTable& table = GetObjectTable();

	if (id.index > table.currentSize)
	{
		return nullptr;
	}

	return table.elements[id.index].dynamicObject;
}

bool Helpers::GetLocalPlayerID(HaloID& outID)
{
	PlayerTable& table = GetPlayerTable();

	if (table.currentSize == 0)
	{
		return false;
	}

	outID = table.elements->objectID;
	return true;
}

BaseDynamicObject* Helpers::GetLocalPlayer()
{
	// TODO: Test this in MP, it may not be that the first player is the local one

	HaloID playerID;
	if (!GetLocalPlayerID(playerID))
	{
		return nullptr;
	}

	return GetDynamicObject(playerID);
}

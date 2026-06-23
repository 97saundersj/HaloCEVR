#include "FirstPersonAnim.h"
#include "../Hooking/Hooks.h"

namespace Helpers
{
	uintptr_t GetFirstPersonAnimBase()
	{
		return static_cast<uintptr_t>(Hooks::o.FirstPersonAnimBase);
	}

	bool HasFirstPersonAnimBase()
	{
		return GetFirstPersonAnimBase() != 0;
	}

	uint16_t GetFirstPersonBaseAnimId()
	{
		const uintptr_t base = GetFirstPersonAnimBase();
		if (!base)
		{
			return 0xFFFF;
		}

		return *reinterpret_cast<uint16_t*>(base + FirstPersonAnimBaseAnimIdOffset);
	}

	uint16_t GetFirstPersonBaseAnimFrame()
	{
		const uintptr_t base = GetFirstPersonAnimBase();
		if (!base)
		{
			return 0;
		}

		return *reinterpret_cast<uint16_t*>(base + FirstPersonAnimBaseAnimFrameOffset);
	}

	void SetFirstPersonBaseAnimId(uint16_t animId)
	{
		const uintptr_t base = GetFirstPersonAnimBase();
		if (!base)
		{
			return;
		}

		*reinterpret_cast<uint16_t*>(base + FirstPersonAnimBaseAnimIdOffset) = animId;
	}

	void SetFirstPersonBaseAnimFrame(uint16_t frame)
	{
		const uintptr_t base = GetFirstPersonAnimBase();
		if (!base)
		{
			return;
		}

		*reinterpret_cast<uint16_t*>(base + FirstPersonAnimBaseAnimFrameOffset) = frame;
	}
}

#include "FirstPersonAnim.h"
#include "../Logger.h"

namespace
{
	uintptr_t g_firstPersonAnimBase = 0;
}

namespace Helpers
{
	void InitFirstPersonAnimBase(uintptr_t drawViewModelAddress)
	{
		if (!drawViewModelAddress)
		{
			Logger::err << "[FirstPersonAnim] DrawViewModel address is null" << std::endl;
			return;
		}

		const uint8_t* instr = reinterpret_cast<uint8_t*>(drawViewModelAddress);
		for (int offset = 0; offset < 48; offset++)
		{
			if (instr[offset] == 0x66 && instr[offset + 1] == 0x8B && instr[offset + 2] == 0x0D)
			{
				const uint32_t baseAnimIdAddress = *reinterpret_cast<const uint32_t*>(instr + offset + 3);
				g_firstPersonAnimBase = static_cast<uintptr_t>(baseAnimIdAddress - FirstPersonAnimBaseAnimIdOffset);
				Logger::log << "[FirstPersonAnim] base=0x" << std::hex << g_firstPersonAnimBase
					<< " (DrawViewModel+" << std::dec << offset << ")" << std::endl;
				return;
			}
		}

		Logger::err << "[FirstPersonAnim] DrawViewModel does not contain expected mov cx, [global]" << std::endl;
	}

	uintptr_t GetFirstPersonAnimBase()
	{
		return g_firstPersonAnimBase;
	}

	bool HasFirstPersonAnimBase()
	{
		return g_firstPersonAnimBase != 0;
	}

	uint16_t GetFirstPersonBaseAnimId()
	{
		if (!g_firstPersonAnimBase)
		{
			return 0xFFFF;
		}

		return *reinterpret_cast<uint16_t*>(g_firstPersonAnimBase + FirstPersonAnimBaseAnimIdOffset);
	}

	uint16_t GetFirstPersonBaseAnimFrame()
	{
		if (!g_firstPersonAnimBase)
		{
			return 0;
		}

		return *reinterpret_cast<uint16_t*>(g_firstPersonAnimBase + FirstPersonAnimBaseAnimFrameOffset);
	}

	void SetFirstPersonBaseAnimId(uint16_t animId)
	{
		if (!g_firstPersonAnimBase)
		{
			return;
		}

		*reinterpret_cast<uint16_t*>(g_firstPersonAnimBase + FirstPersonAnimBaseAnimIdOffset) = animId;
	}

	void SetFirstPersonBaseAnimFrame(uint16_t frame)
	{
		if (!g_firstPersonAnimBase)
		{
			return;
		}

		*reinterpret_cast<uint16_t*>(g_firstPersonAnimBase + FirstPersonAnimBaseAnimFrameOffset) = frame;
	}
}

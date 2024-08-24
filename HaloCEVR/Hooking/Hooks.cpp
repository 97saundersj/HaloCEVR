#include "Hooks.h"
#include "../Helpers/Player.h"
#include "../Helpers/Renderer.h"
#include "../Helpers/DX9.h"
#include "../Game.h"
#include "../Helpers/Menus.h"
#include "../Helpers/Maths.h"

#include <tlhelp32.h>
#include "../DirectXWrappers/IDirect3DDevice9ExWrapper.h"

#define CREATEHOOK(Func) Func##.CreateHook(#Func, o.##Func##, &H_##Func##)
#define RESOLVEINDIRECT(Name) ResolveIndirect(o.I_##Name, o.##Name)

bool bPotentiallyFoundChimera = false;

void Hooks::InitHooks()
{
	RESOLVEINDIRECT(AssetsArray);
	RESOLVEINDIRECT(Controls);
	RESOLVEINDIRECT(DirectX9);
	RESOLVEINDIRECT(DirectX9Device);
	RESOLVEINDIRECT(HideMouse);
	RESOLVEINDIRECT(ObjectTable);
	RESOLVEINDIRECT(PlayerTable);
	RESOLVEINDIRECT(LocalPlayer);
	RESOLVEINDIRECT(WindowRect);
	RESOLVEINDIRECT(RenderTargets);
	RESOLVEINDIRECT(CameraMatrices);
	RESOLVEINDIRECT(InputData);
	RESOLVEINDIRECT(CurrentRect);
	RESOLVEINDIRECT(LoadingState);

	CREATEHOOK(InitDirectX);
	CREATEHOOK(DrawFrame);
	CREATEHOOK(DrawHUD);
	CREATEHOOK(DrawMenu);
	CREATEHOOK(DrawLoadingScreen);
	CREATEHOOK(DrawCrosshair);
	CREATEHOOK(DrawImage);
	CREATEHOOK(SetViewModelPosition);
	CREATEHOOK(HandleInputs);
	CREATEHOOK(UpdatePitchYaw);
	CREATEHOOK(SetViewportScale);
	CREATEHOOK(SetMousePosition);
	CREATEHOOK(UpdateMouseInfo);
	CREATEHOOK(FireWeapon);
	CREATEHOOK(ThrowGrenade);
	CREATEHOOK(DrawLoadingScreen2);

	// These are handled with a direct patch, so manually scan them
	bPotentiallyFoundChimera |= SigScanner::UpdateOffset(o.TabOutVideo) < 0;
	bPotentiallyFoundChimera |= SigScanner::UpdateOffset(o.TabOutVideo2) < 0;
	bPotentiallyFoundChimera |= SigScanner::UpdateOffset(o.TabOutVideo3) < 0;
	SigScanner::UpdateOffset(o.CutsceneFPSCap);
	SigScanner::UpdateOffset(o.CreateMouseDevice);
	SigScanner::UpdateOffset(o.SetViewModelVisible);
	SigScanner::UpdateOffset(o.TextureAlphaWrite);
	SigScanner::UpdateOffset(o.TextAlphaWrite);

	SigScanner::UpdateOffset(o.SetCameraMatrices);
	oSetCameraMatrices = reinterpret_cast<Func_SetCameraMatrices>(o.SetCameraMatrices.Address);
}

#undef CREATEHOOK
#undef RESOLVEINDIRECT

void Hooks::EnableAllHooks()
{
	InitDirectX.EnableHook();
	DrawFrame.EnableHook();
	DrawHUD.EnableHook();
	DrawMenu.EnableHook();
	DrawLoadingScreen.EnableHook();
	DrawCrosshair.EnableHook();
	//DrawImage.EnableHook();
	SetViewModelPosition.EnableHook();
	HandleInputs.EnableHook();
	UpdatePitchYaw.EnableHook();
	SetViewportScale.EnableHook();
	SetMousePosition.EnableHook();
	UpdateMouseInfo.EnableHook();
	FireWeapon.EnableHook();
	ThrowGrenade.EnableHook();
	DrawLoadingScreen2.EnableHook();

	Freeze();

	P_RemoveCutsceneFPSCap();
	P_KeepViewModelVisible(false);
	P_EnableUIAlphaWrite();
	P_DontStealMouse();

	// If we think the user has chimera installed, don't try to patch their patches
	if (!bPotentiallyFoundChimera)
	{
		P_FixTabOut();
	}

	Unfreeze();
}

//===============================//Helpers//===================================//

void Hooks::SetByte(long long Address, byte Byte)
{
	byte bytes[1]{ Byte };
	SetBytes(Address, 1, bytes);
}

void Hooks::SetBytes(long long Address, int Length, byte* Bytes)
{
	LPVOID Addr = reinterpret_cast<LPVOID>(Address);
	DWORD OldProt;
	VirtualProtect(Addr, Length, PAGE_EXECUTE_READWRITE, &OldProt);
	for (int i = 0; i < Length; i++)
	{
		*reinterpret_cast<char*>(Address + i) = Bytes[i];
	}
	if (OldProt != PAGE_EXECUTE_READWRITE)
	{
		DWORD NewProt;
		VirtualProtect(Addr, Length, OldProt, &NewProt);
	}
}

void Hooks::NOPInstructions(long long Address, int Length)
{
	LPVOID Addr = reinterpret_cast<LPVOID>(Address);
	DWORD OldProt;
	VirtualProtect(Addr, Length, PAGE_EXECUTE_READWRITE, &OldProt);
	for (int i = 0; i < Length; i++)
	{
		*reinterpret_cast<byte*>(Address + i) = 0x90;
	}
	if (OldProt != PAGE_EXECUTE_READWRITE)
	{
		DWORD NewProt;
		VirtualProtect(Addr, Length, OldProt, &NewProt);
	}
}

// Below is borrowed from minhook to freeze threads while applying patches
#define THREAD_ACCESS \
    (THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION | THREAD_SET_CONTEXT)
#define INITIAL_THREAD_CAPACITY 128

HANDLE g_hHeap = NULL;

typedef struct _FROZEN_THREADS
{
	LPDWORD pItems;         // Data heap
	UINT    capacity;       // Size of allocated data heap, items
	UINT    size;           // Actual number of data items
} FROZEN_THREADS, * PFROZEN_THREADS;

static BOOL EnumerateThreads(PFROZEN_THREADS pThreads)
{
	LPDWORD p;
	BOOL succeeded = FALSE;

	HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	if (hSnapshot != INVALID_HANDLE_VALUE)
	{
		THREADENTRY32 te;
		te.dwSize = sizeof(THREADENTRY32);
		if (Thread32First(hSnapshot, &te))
		{
			succeeded = TRUE;
			do
			{
				if (te.dwSize >= (FIELD_OFFSET(THREADENTRY32, th32OwnerProcessID) + sizeof(DWORD))
					&& te.th32OwnerProcessID == GetCurrentProcessId()
					&& te.th32ThreadID != GetCurrentThreadId())
				{
					if (pThreads->pItems == NULL)
					{
						pThreads->capacity = INITIAL_THREAD_CAPACITY;
						pThreads->pItems
							= (LPDWORD)HeapAlloc(g_hHeap, 0, pThreads->capacity * sizeof(DWORD));
						if (pThreads->pItems == NULL)
						{
							succeeded = FALSE;
							break;
						}
					}
					else if (pThreads->size >= pThreads->capacity)
					{
						pThreads->capacity *= 2;
						p = (LPDWORD)HeapReAlloc(
							g_hHeap, 0, pThreads->pItems, pThreads->capacity * sizeof(DWORD));
						if (p == NULL)
						{
							succeeded = FALSE;
							break;
						}

						pThreads->pItems = p;
					}
					pThreads->pItems[pThreads->size++] = te.th32ThreadID;
				}

				te.dwSize = sizeof(THREADENTRY32);
			} while (Thread32Next(hSnapshot, &te));

			if (succeeded && GetLastError() != ERROR_NO_MORE_FILES)
				succeeded = FALSE;

			if (!succeeded && pThreads->pItems != NULL)
			{
				HeapFree(g_hHeap, 0, pThreads->pItems);
				pThreads->pItems = NULL;
			}
		}
		CloseHandle(hSnapshot);
	}

	return succeeded;
}

//-------------------------------------------------------------------------
FROZEN_THREADS Threads;

bool Hooks::Freeze()
{
	Threads.pItems = NULL;
	Threads.capacity = 0;
	Threads.size = 0;
	g_hHeap = HeapCreate(0, 0, 0);
	if (!EnumerateThreads(&Threads))
	{
		return false;
	}

	if (Threads.pItems != NULL)
	{
		UINT i;
		for (i = 0; i < Threads.size; ++i)
		{
			HANDLE hThread = OpenThread(THREAD_ACCESS, FALSE, Threads.pItems[i]);
			if (hThread != NULL)
			{
				SuspendThread(hThread);
				CloseHandle(hThread);
			}
		}
	}

	return true;
}

//-------------------------------------------------------------------------
void Hooks::Unfreeze()
{
	if (Threads.pItems != NULL)
	{
		UINT i;
		for (i = 0; i < Threads.size; ++i)
		{
			HANDLE hThread = OpenThread(THREAD_ACCESS, FALSE, Threads.pItems[i]);
			if (hThread != NULL)
			{
				ResumeThread(hThread);
				CloseHandle(hThread);
			}
		}

		HeapFree(g_hHeap, 0, Threads.pItems);
	}
	HeapDestroy(g_hHeap);
}

//-------------------------------------------------------------------------
void Hooks::ResolveIndirect(Offset& offset, long long& Address)
{
	SigScanner::UpdateOffset(offset);
	void* pointer = *reinterpret_cast<void**>(offset.Address + Address);
	Address = reinterpret_cast<long long>(pointer);
	Logger::log << "Calculated indirect address: 0x" << std::hex << Address << std::dec << std::endl;
}

//===============================//Hooks//===================================//

bool Hooks::H_InitDirectX()
{
	bool bSuccess = InitDirectX.Original();

	Game::instance.OnInitDirectX();

	return bSuccess;
}

void Hooks::H_DrawFrame(Renderer* param1, short param2, short* param3, float tickProgress, float deltaTime)
{
	/*
	In order to get each perspective (left eye, right eye, PiP scope, mirror [todo: just blit an eye for the mirror])
	we need to call the draw function multiple times, but should take care to avoid creating multiple d3d scenes as
	according to the docs you should only call this once per frame.
	The shim we use to upgrade D3D9 to D3D9Ex has some functionality added to skip these calls when necessary
	*/

	bool bDrawMirror = Game::instance.GetDrawMirror();

	Game::instance.PreDrawFrame(param1, deltaTime);
	reinterpret_cast<IDirect3DDevice9ExWrapper*>(Helpers::GetDirect3DDevice9())->bIgnoreNextEnd = true;

	// Draw scope if necessary
	if (Game::instance.PreDrawScope(param1, deltaTime))
	{
		DrawFrame.Original(param1, param2, param3, tickProgress, deltaTime);
		Game::instance.PostDrawScope(param1, deltaTime);

		reinterpret_cast<IDirect3DDevice9ExWrapper*>(Helpers::GetDirect3DDevice9())->bIgnoreNextStart = true;
		reinterpret_cast<IDirect3DDevice9ExWrapper*>(Helpers::GetDirect3DDevice9())->bIgnoreNextEnd = true;
	}

	// Draw left eye
	Game::instance.PreDrawEye(param1, deltaTime, 0);
	DrawFrame.Original(param1, param2, param3, tickProgress, deltaTime);
	Game::instance.PostDrawEye(param1, deltaTime, 0);

	reinterpret_cast<IDirect3DDevice9ExWrapper*>(Helpers::GetDirect3DDevice9())->bIgnoreNextStart = true;
	reinterpret_cast<IDirect3DDevice9ExWrapper*>(Helpers::GetDirect3DDevice9())->bIgnoreNextEnd = bDrawMirror;

	// Draw right eye
	Game::instance.PreDrawEye(param1, deltaTime, 1);
	DrawFrame.Original(param1, param2, param3, tickProgress, deltaTime);
	Game::instance.PostDrawEye(param1, deltaTime, 1);
	// Draw Mirror view, should be replaced at some point as it is wasteful to draw an entirely new viewport
	if (bDrawMirror)
	{
		reinterpret_cast<IDirect3DDevice9ExWrapper*>(Helpers::GetDirect3DDevice9())->bIgnoreNextStart = true;

		Game::instance.PreDrawMirror(param1, deltaTime);
		DrawFrame.Original(param1, param2, param3, tickProgress, deltaTime);
		Game::instance.PostDrawMirror(param1, deltaTime);
	}
	Game::instance.PostDrawFrame(param1, deltaTime);
}

void Hooks::H_DrawHUD()
{
	if (Game::instance.PreDrawHUD())
	{
		DrawHUD.Original();
		Game::instance.PostDrawHUD();
	}
}

void __declspec(naked) Hooks::H_DrawMenu()
{
	// Original function uses AX register as param_1
	_asm
	{
		pushad
	}

	if (Game::instance.PreDrawMenu())
	{
		DrawMenu.Original();
		Game::instance.PostDrawMenu();
	}

	_asm
	{
		popad
		ret
	}
}

void __declspec(naked) Hooks::H_DrawLoadingScreen()
{
	// Function has two params
	// int param1 : EAX
	// Renderer* renderer : Stack[0x4]

	static int param1;
	static Renderer* param2;

	_asm
	{
		// Grab param1
		mov param1, eax
		// Grab param2
		mov eax, [esp + 0x4]
		mov param2, eax

		pushad
	}

	if (Game::instance.PreDrawLoading(param1, param2))
	{

		_asm
		{
			popad

			// Re-push parameters
			push eax
			mov eax, param1
		}

		DrawLoadingScreen.Original();

		_asm
		{
			pushad
		}

		Game::instance.PostDrawLoading(param1, param2);

		_asm
		{
			popad

			add esp, 0x4
			ret
		}
	}
	else
	{
		_asm
		{
			popad

			ret
		}
	}
}

void __declspec(naked) Hooks::H_DrawCrosshair()
{
	static void* retAddress;
	static float* param1; // We don't actually need this param, but we steal its register for moving some memory around
	static short* param6;

	_asm
	{
		mov param1, eax
		mov eax, [esp + 0xc]
		mov param6, eax
		mov eax, param1
		pop retAddress
		pushad
	}

	Game::instance.PreDrawCrosshair(param6);

	_asm
	{
		popad
	}

	DrawCrosshair.Original();

	_asm
	{
		pushad
	}

	Game::instance.PostDrawCrosshair();

	_asm
	{
		popad
		push retAddress
		ret
	}
}

void __declspec(naked) Hooks::H_DrawImage()
{
	static void* param1;
	static void* param2;

	_asm
	{
		mov param1, eax
		mov eax, [esp + 0x4]
		mov param2, eax
		pushad
	}

	Game::instance.PreDrawImage(param1, param2);

	_asm
	{
		popad
		mov eax, param2
		push eax
		mov eax, param1
	}

	DrawImage.Original();

	_asm
	{
		pushad
	}

	Game::instance.PostDrawImage(param1, param2);

	_asm
	{
		popad
		add esp, 0x4
		ret
	}
}

void __declspec(naked) Hooks::H_SetViewModelPosition()
{
	static Vector3* pos;
	static Vector3* facing;
	static Vector3* up;
	static HaloID id;
	static TransformQuat* quatTrans;
	static Transform* trans;

	_asm
	{
		mov id, eax // Get ID from EAX param
		mov pos, ecx // Get Position from ECX param
		mov ecx, [esp + 0x10] // Get UpVector from 0x10 param
		mov up, ecx
		mov ecx, [esp + 0xc] // Get FacingVector from 0xc param
		mov facing, ecx
		mov ecx, [esp + 0x8] // Get QuatTransforms from 0x8 param
		mov quatTrans, ecx
		mov ecx, [esp + 0x4] // Get Transforms from 0x4 param
		mov trans, ecx
		mov ecx, pos // Restore Position in ECX register
	}

	// The original function was simple enough + required enough patches for VR
	// that it is easier to just fully reimplement it in the mod
	Game::instance.UpdateViewModel(id, pos, facing, up, quatTrans, trans);

	_asm
	{
		ret
	}
}

void Hooks::H_HandleInputs()
{
	HandleInputs.Original();

	Game::instance.UpdateInputs();
}

void __declspec(naked) Hooks::H_UpdatePitchYaw()
{
	static short playerId;
	static float yaw;
	static float pitch;

	// Extract parameters
	_asm
	{
		mov playerId, ax
		mov eax, [esp + 0x4]
		mov yaw, eax
		mov eax, [esp + 0x8]
		mov pitch, eax
		movsx eax, playerId
	}

	/*
	Original stack:
	0x0: ret address
	0x4: yaw
	0x8: pitch
	
	Post-hook stack:
	0x0: hook ret address
	0x4: ret address
	0x8: yaw
	0xc: pitch

	Fixed stack:
	0x0: hook ret address
	0x4: yaw
	0x8: pitch
	0xc: ret address
	0x10: yaw
	0x14: pitch
	*/

	_asm
	{
		pushad
	}

	Game::instance.UpdateCamera(yaw, pitch);

	_asm
	{
		popad
	}

	// Re-push parameters to fix stack for original call
	_asm
	{
		push pitch
		push yaw
	}

	UpdatePitchYaw.Original();

	// Cleanup and return
	_asm
	{
		add esp, 0x8
		ret
	}
}


void __declspec(naked) Hooks::H_SetViewportScale()
{
	static Viewport* view;

	_asm
	{
		mov view, ecx
	}

	SetViewportScale.Original();

	_asm
	{
		pushad
	}

	Game::instance.SetViewportScale(view);

	_asm
	{
		popad
	}

	_asm
	{
		ret
	}
}

void __declspec(naked) Hooks::H_SetMousePosition()
{
	static int x;
	static int y;

	_asm
	{
		mov x, eax
		mov y, ecx
	}

	_asm
	{
		pushad
	}

	Game::instance.SetMousePosition(x, y);

	_asm
	{
		popad
	}

	_asm
	{
		mov eax, x
		mov ecx, y
	}

	SetMousePosition.Original();

	_asm
	{
		ret
	}
}

void __declspec(naked) Hooks::H_UpdateMouseInfo()
{
	static MouseInfo* mouseInfo;

	_asm
	{
		push eax
		mov eax, [esp + 0x8]
		mov mouseInfo, eax
		pop eax
	}

	_asm
	{
		push mouseInfo
	}

	UpdateMouseInfo.Original();

	_asm
	{
		pushad
	}

	Game::instance.UpdateMouseInfo(mouseInfo);

	_asm
	{
		popad
	}

	_asm
	{
		add esp, 0x4
		ret
	}
}

void Hooks::H_FireWeapon(HaloID param1, short param2)
{
	Game::instance.PreFireWeapon(param1, param2);

	FireWeapon.Original(param1, param2);

	Game::instance.PostFireWeapon(param1, param2);

}


void Hooks::H_ThrowGrenade(HaloID param1, bool param2)
{
	Game::instance.PreThrowGrenade(param1);

	ThrowGrenade.Original(param1, param2);

	Game::instance.PostThrowGrenade(param1);
}

void Hooks::H_DrawLoadingScreen2(void* param1)
{
	D3DVIEWPORT9 currentViewport;
	D3DVIEWPORT9 desiredViewport
	{
		0,
		0,
		static_cast<DWORD>(Game::instance.c_UIOverlayWidth->Value()),
		static_cast<DWORD>(Game::instance.c_UIOverlayHeight->Value()),
		0.0f,
		1.0f
	};

	Helpers::GetDirect3DDevice9()->GetViewport(&currentViewport);
	Helpers::GetDirect3DDevice9()->SetViewport(&desiredViewport);

	DrawLoadingScreen2.Original(param1);

	Helpers::GetDirect3DDevice9()->SetViewport(&currentViewport);
}

//================================//Patches//================================//

void Hooks::P_FixTabOut()
{	
	// There are 3 checks in the main loop for focus
	// Attempting to patch this out with minhook would be painful
	
	// Always jump past the first check
	SetByte(o.TabOutVideo.Address + 6, 0xEB);
	// NOP a JNZ in the second check
	NOPInstructions(o.TabOutVideo2.Address, 2);
	// Always jump past the final check
	SetByte(o.TabOutVideo3.Address + 6, 0xEB);

	// TODO: There's a more complex patch I think to do with fullscreen
}

void Hooks::P_RemoveCutsceneFPSCap()
{
	// Instead of setting the flag for capping the fps when we are in a cutscene, just clear it
	byte bytes[2]{ 0x32, 0xdb };
	SetBytes(o.CutsceneFPSCap.Address, 2, bytes);
}

void Hooks::P_KeepViewModelVisible(bool bAlwaysShow)
{
	// Replace "bShowViewModel = false" with "bShowViewModel = true"
	SetByte(o.SetViewModelVisible.Address + 0x55, bAlwaysShow ? 0x0 : 0x1);
}

void Hooks::P_EnableUIAlphaWrite()
{
	// By default the UI doesn't write to the alpha channel (since its being drawn last + straight onto the main image)
	// We need the alpha value in order to have transparency in floating UI elements
	SetByte(o.TextureAlphaWrite.Address + 0x4a, 0xf);
	SetByte(o.TextAlphaWrite.Address + 0x72, 0xf);
}

void Hooks::P_DontStealMouse()
{
	// Halo claims exclusive rights over the mouse which causes all sorts of chaos when trying to pause the game with a debugger
	// Changing it to non-exclusive is fine for our purposes (VR/testing),
	// but does cause the mouse to stop being locked to the window in game
	SetByte(o.CreateMouseDevice.Address + 0x5B, 6);
}

void Hooks::SetCameraMatrices(struct Viewport* viewport, struct CameraFrustum* frustum, struct CameraRenderMatrices* crm, bool bDoProjection)
{
	// Halo loves its non-standard calling conventions...
	_asm
	{
		push eax
		push ecx
		push esi
		movzx eax, bDoProjection
		push eax
		mov eax, viewport
		mov ecx, frustum
		mov esi, crm
	}
	
	oSetCameraMatrices();

	_asm
	{
		pop esi
		pop ecx
		pop eax
		add esp, 0x4
	}
}

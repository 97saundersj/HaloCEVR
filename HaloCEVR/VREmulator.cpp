#include <d3d9.h>
#include "VREmulator.h"
#include "Helpers/DX9.h"
#include "Helpers/RenderTarget.h"
#include "Helpers/Camera.h"
#include "Helpers/Renderer.h"
#include "Logger.h"
#include "DirectXWrappers/IDirect3DDevice9ExWrapper.h"

LRESULT CALLBACK WindowProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_DESTROY:
	{
		PostQuitMessage(0);
		return 0;
	} break;
	}

	return DefWindowProc(hWnd, message, wParam, lParam);
}

void VREmulator::Init()
{
	HWND hWnd;
	WNDCLASSEX wc;
	HINSTANCE hInstance = GetModuleHandle(NULL);

	ZeroMemory(&wc, sizeof(WNDCLASSEX));

	wc.cbSize = sizeof(WNDCLASSEX);
	wc.style = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc = WindowProc;
	wc.hInstance = hInstance;
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)COLOR_WINDOW;
	wc.lpszClassName = L"WindowClass";

	RegisterClassEx(&wc);

	hWnd = CreateWindowEx(NULL, L"WindowClass", L"Mirror Window", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1200, 600, NULL, NULL, hInstance, NULL);

	ShowWindow(hWnd, SW_SHOW);

	if (!Helpers::GetDirect3D9())
	{
		Logger::log << "GetDirect3D9 returned null, aborting" << std::endl;
		return;
	}

	IDirect3DSurface9* GameSurface = nullptr;

	Helpers::GetDirect3DDevice9()->GetRenderTarget(0, &GameSurface);

	D3DSURFACE_DESC Desc;

	GameSurface->GetDesc(&Desc);
	GameSurface->Release();

	D3DPRESENT_PARAMETERS present;
	ZeroMemory(&present, sizeof(present));
	present.Windowed = TRUE;
	present.SwapEffect = D3DSWAPEFFECT_DISCARD;
	present.hDeviceWindow = hWnd;
	present.BackBufferWidth = 1200;
	present.BackBufferHeight = 600;
	present.BackBufferFormat = Desc.Format;
	present.MultiSampleQuality = Desc.MultiSampleQuality;
	present.MultiSampleType = Desc.MultiSampleType;

	HRESULT result = S_OK;

	result = Helpers::GetDirect3D9()->CreateDevice(0, D3DDEVTYPE_HAL, hWnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &present, &MirrorDevice);

	if (FAILED(result))
	{
		Logger::log << "IDirect3DDevice9::CreateDevice failed: " << result << std::endl;
		return;
	}

	CreateSharedTarget();

	Logger::log << "Created Mirror D3D window" << std::endl;
}

void VREmulator::PreDrawFrame(struct Renderer* renderer, float deltaTime)
{
	if (!MirrorDevice)
	{
		Logger::log << "No Mirror Device, can't draw" << std::endl;
		return;
	}

	HRESULT result;

	result = MirrorDevice->BeginScene();
	if (FAILED(result))
	{
		Logger::log << "Failed to call mirror begin scene: " << result << std::endl;
	}
}

void VREmulator::PreDrawEye(struct Renderer* renderer, float deltaTime, int eye)
{
	// Set camera here

	if (eye == 0)
	{
		reinterpret_cast<IDirect3DDevice9ExWrapper*>(Helpers::GetDirect3DDevice9())->bIgnoreNextEnd = true;
	}
	else
	{
		reinterpret_cast<IDirect3DDevice9ExWrapper*>(Helpers::GetDirect3DDevice9())->bIgnoreNextStart = true;
	}
	
	// I read somewhere a halo unit is 3.048 metres
	const float DIST = (64.0f / 1000.0f) / 3.048f;

	Vector3 rightVec = renderer->frustum.facingDirection.Cross(renderer->frustum.upDirection);

	renderer->frustum.position += rightVec * DIST * (float)(3 * eye - 1);
	renderer->frustum2.position += rightVec * DIST * (float)(3 * eye - 1);

	for (CameraFrustum* f : { &renderer->frustum, &renderer->frustum2 })
	{
		f->Viewport.left = 0;
		f->Viewport.top = 0;
		f->Viewport.right = 600;
		f->Viewport.bottom = 600;
		f->oViewport.left = 0;
		f->oViewport.top = 0;
		f->oViewport.right = 600;
		f->oViewport.bottom = 600;
	}

	RenderTarget* primaryRenderTarget = Helpers::GetRenderTargets();

	for (int i = 0; i < 1; i++)
	{
		//primaryRenderTarget->renderSurface = eye == 0 ? SharedTarget_L : SharedTarget_R;
		primaryRenderTarget[i].renderSurface = EyeSurface_Game[eye][i];
		//primaryRenderTarget->renderTexture = eye == 0 ? LeftEyeTexture : RightEyeTexture;
		primaryRenderTarget[i].renderTexture = EyeTexture_Game[eye][i];
		primaryRenderTarget[i].width = 600;
		primaryRenderTarget[i].height = 600;
	}
}

void VREmulator::PostDrawEye(struct Renderer* renderer, float deltaTime, int eye)
{
	IDirect3DSurface9* MirrorSurface = nullptr;
	HRESULT result = MirrorDevice->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &MirrorSurface);

	if (FAILED(result))
	{
		Logger::log << "Failed to get mirror back buffer: " << result << std::endl;
	}

	D3DSURFACE_DESC mirrorDesc;

	MirrorSurface->GetDesc(&mirrorDesc);

	UINT HalfWidth = mirrorDesc.Width / 2;

	RECT rcDest;
	rcDest.left = eye * HalfWidth;
	rcDest.top = 0;
	rcDest.right = HalfWidth + eye * HalfWidth;
	rcDest.bottom = mirrorDesc.Height;

	// Flip eyes for cross eyed
	result = MirrorDevice->StretchRect(EyeSurface_VR[1 - eye][0], nullptr, MirrorSurface, &rcDest, D3DTEXF_NONE);
	
	if (FAILED(result))
	{
		Logger::log << "Failed to copy from shared texture (" << eye << "): " << result << std::endl;
	}

	MirrorSurface->Release();
}

void VREmulator::PostDrawFrame(struct Renderer* renderer, float deltaTime)
{
	IDirect3DQuery9* pEventQuery = nullptr;
	Helpers::GetDirect3DDevice9()->CreateQuery(D3DQUERYTYPE_EVENT, &pEventQuery);
	if (pEventQuery != nullptr)
	{
		pEventQuery->Issue(D3DISSUE_END);
		while (pEventQuery->GetData(nullptr, 0, D3DGETDATA_FLUSH) != S_OK);
		pEventQuery->Release();
	}

	PostDrawEye(renderer, deltaTime, 0);
	PostDrawEye(renderer, deltaTime, 1);

	HRESULT result = MirrorDevice->EndScene();
	if (FAILED(result))
	{
		Logger::log << "Failed to end scene: " << result << std::endl;
	}

	result = MirrorDevice->Present(nullptr, nullptr, nullptr, nullptr);
	if (FAILED(result))
	{
		Logger::log << "Failed to present mirror view: " << result << std::endl;
	}
}

void VREmulator::CreateSharedTarget()
{

	D3DSURFACE_DESC Desc;

	Helpers::GetRenderTargets()[0].renderSurface->GetDesc(&Desc);
		
	D3DSURFACE_DESC Desc2;

	Helpers::GetRenderTargets()[1].renderSurface->GetDesc(&Desc2);

	Logger::log << (Desc.Format) << ", " << Desc2.Format << std::endl;

	/*
	* Old way, creating separate render targets and textures
	
	HRESULT result = Helpers::GetDirect3DDevice9()->CreateTexture(600, 600, 1, 1, Desc.Format, D3DPOOL_DEFAULT, &LeftEyeTexture, 0);

	if (FAILED(result))
	{
		Logger::log << "Failed to create left eye texture: " << result << std::endl;
	}

	result = Helpers::GetDirect3DDevice9()->CreateTexture(600, 600, 1, 1, Desc.Format, D3DPOOL_DEFAULT, &RightEyeTexture, 0);

	if (FAILED(result))
	{
		Logger::log << "Failed to create right eye texture: " << result << std::endl;
	}

	result = Helpers::GetDirect3DDevice9()->CreateRenderTarget(600, 600, Desc.Format, Desc.MultiSampleType, Desc.MultiSampleQuality, FALSE, &SharedTarget_L, &SharedHandleL);

	if (FAILED(result))
	{
		Logger::log << "Failed to create shared render target: " << result << std::endl;
	}

	result = MirrorDevice->CreateRenderTarget(600, 600, Desc.Format, Desc.MultiSampleType, Desc.MultiSampleQuality, FALSE, &SharedTarget_L_M, &SharedHandleL);

	if (FAILED(result))
	{
		Logger::log << "Failed to create copy of render target: " << result << std::endl;
	}

	result = Helpers::GetDirect3DDevice9()->CreateRenderTarget(600, 600, Desc.Format, Desc.MultiSampleType, Desc.MultiSampleQuality, FALSE, &SharedTarget_R, &SharedHandleR);

	if (FAILED(result))
	{
		Logger::log << "Failed to create shared render target: " << result << std::endl;
	}

	result = MirrorDevice->CreateRenderTarget(600, 600, Desc.Format, Desc.MultiSampleType, Desc.MultiSampleQuality, FALSE, &SharedTarget_R_M, &SharedHandleR);

	if (FAILED(result))
	{
		Logger::log << "Failed to create copy of render target: " << result << std::endl;
	}
	*/

	/*
	* Newer way, creating shared textures + extracting surfaces

	HRESULT result = Helpers::GetDirect3DDevice9()->CreateTexture(600, 600, 1, 1, Desc.Format, D3DPOOL_DEFAULT, &LeftEyeTexture, &SharedHandleL);

	if (FAILED(result))
	{
		Logger::log << "Failed to create left eye texture: " << result << std::endl;
	}

	result = Helpers::GetDirect3DDevice9()->CreateTexture(600, 600, 1, 1, Desc.Format, D3DPOOL_DEFAULT, &RightEyeTexture, &SharedHandleR);

	if (FAILED(result))
	{
		Logger::log << "Failed to create right eye texture: " << result << std::endl;
	}

	result = MirrorDevice->CreateTexture(600, 600, 1, 1, Desc.Format, D3DPOOL_DEFAULT, &LeftEyeTexture_M, &SharedHandleL);

	if (FAILED(result))
	{
		Logger::log << "Failed to create mirror left eye texture: " << result << std::endl;
	}

	result = MirrorDevice->CreateTexture(600, 600, 1, 1, Desc.Format, D3DPOOL_DEFAULT, &RightEyeTexture_M, &SharedHandleR);

	if (FAILED(result))
	{
		Logger::log << "Failed to create mirror right eye texture: " << result << std::endl;
	}

	result = LeftEyeTexture->GetSurfaceLevel(0, &SharedTarget_L);

	if (FAILED(result))
	{
		Logger::log << "Failed to get render surface from left texture: " << result << std::endl;
	}

	result = LeftEyeTexture_M->GetSurfaceLevel(0, &SharedTarget_L_M);

	if (FAILED(result))
	{
		Logger::log << "Failed to get render surface from left mirror texture: " << result << std::endl;
	}

	result = RightEyeTexture->GetSurfaceLevel(0, &SharedTarget_R);

	if (FAILED(result))
	{
		Logger::log << "Failed to get render surface from right texture: " << result << std::endl;
	}

	result = RightEyeTexture_M->GetSurfaceLevel(0, &SharedTarget_R_M);

	if (FAILED(result))
	{
		Logger::log << "Failed to get render surface from right mirror texture: " << result << std::endl;
	}
	*/


	const UINT WIDTH = 600;
	const UINT HEIGHT = 600;

	CreateTexAndSurface(0, WIDTH, HEIGHT, Desc.Usage, Desc.Format);
	CreateTexAndSurface(1, WIDTH, HEIGHT, Desc2.Usage, Desc2.Format);
}


void VREmulator::CreateTexAndSurface(int index, UINT Width, UINT Height, DWORD Usage, D3DFORMAT Format)
{
	for (int i = 0; i < 2; i++)
	{
		HANDLE SharedHandle = nullptr;

		// Create texture on game

		HRESULT result = Helpers::GetDirect3DDevice9()->CreateTexture(Width, Height, 1, Usage, Format, D3DPOOL_DEFAULT, &EyeTexture_Game[i][index], &SharedHandle);
		if (FAILED(result))
		{
			Logger::log << "Failed to create game " << index << " texture: " << result << std::endl;
		}

		// Created shared texture on vr
		result = MirrorDevice->CreateTexture(Width, Height, 1, Usage, Format, D3DPOOL_DEFAULT, &EyeTexture_VR[i][index], &SharedHandle);
		if (FAILED(result))
		{
			Logger::log << "Failed to create vr " << index << " texture: " << result << std::endl;
		}

		// Extract game surface
		result = EyeTexture_Game[i][index]->GetSurfaceLevel(0, &EyeSurface_Game[i][index]);

		if (FAILED(result))
		{
			Logger::log << "Failed to get game render surface from " << index << ": " << result << std::endl;
		}

		// Extract vr surface
		result = EyeTexture_VR[i][index]->GetSurfaceLevel(0, &EyeSurface_VR[i][index]);

		if (FAILED(result))
		{
			Logger::log << "Failed to get vr render surface from " << index << ": " << result << std::endl;
		}
	}
}

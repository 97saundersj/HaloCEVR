#include <d3d11.h>
#include <d3d9.h>
#include <algorithm>
#include "OpenVR.h"
#include "../Logger.h"
#include "../Game.h"
#include "../Helpers/DX9.h"
#include "../Helpers/Renderer.h"
#include "../Helpers/RenderTarget.h"

#pragma comment(lib, "openvr_api.lib")
#pragma comment(lib, "d3d11.lib")

void OpenVR::Init()
{
	Logger::log << "[OpenVR] Initialising OpenVR" << std::endl;

	vr::EVRInitError initError = vr::VRInitError_None;
	VRSystem = vr::VR_Init(&initError, vr::VRApplication_Scene);

	if (initError != vr::VRInitError_None)
	{
		Logger::log << "[OpenVR] VR_Init failed: " << vr::VR_GetVRInitErrorAsEnglishDescription(initError) << std::endl;
		return;
	}

	VRCompositor = vr::VRCompositor();

	if (!VRCompositor)
	{
		Logger::log << "[OpenVR] VRCompositor failed" << std::endl;
		return;
	}

	VRSystem->GetRecommendedRenderTargetSize(&RecommendedWidth, &RecommendedHeight);

	// Voodoo magic to convert normal view frustums into asymmetric ones through selective cropping

	float l_left = 0.0f, l_right = 0.0f, l_top = 0.0f, l_bottom = 0.0f;
	VRSystem->GetProjectionRaw(vr::EVREye::Eye_Left, &l_left, &l_right, &l_top, &l_bottom);

	float r_left = 0.0f, r_right = 0.0f, r_top = 0.0f, r_bottom = 0.0f;
	VRSystem->GetProjectionRaw(vr::EVREye::Eye_Right, &r_left, &r_right, &r_top, &r_bottom);

	float tanHalfFov[2];

	tanHalfFov[0] = (std::max)({ -l_left, l_right, -r_left, r_right });
	tanHalfFov[1] = (std::max)({ -l_top, l_bottom, -r_top, r_bottom });

	TextureBounds[0].uMin = 0.5f + 0.5f * l_left / tanHalfFov[0];
	TextureBounds[0].uMax = 0.5f + 0.5f * l_right / tanHalfFov[0];
	TextureBounds[0].vMin = 0.5f - 0.5f * l_bottom / tanHalfFov[1];
	TextureBounds[0].vMax = 0.5f - 0.5f * l_top / tanHalfFov[1];

	TextureBounds[1].uMin = 0.5f + 0.5f * r_left / tanHalfFov[0];
	TextureBounds[1].uMax = 0.5f + 0.5f * r_right / tanHalfFov[0];
	TextureBounds[1].vMin = 0.5f - 0.5f * r_bottom / tanHalfFov[1];
	TextureBounds[1].vMax = 0.5f - 0.5f * r_top / tanHalfFov[1];

	Aspect = tanHalfFov[0] / tanHalfFov[1];
	FOV = 2.0f * atan(tanHalfFov[0]);

	RecommendedWidth = static_cast<uint32_t>(RecommendedWidth / (std::max)(TextureBounds[0].uMax - TextureBounds[0].uMin, TextureBounds[1].uMax - TextureBounds[1].uMin));
	RecommendedHeight = static_cast<uint32_t>(RecommendedHeight / (std::max)(TextureBounds[0].vMax - TextureBounds[0].vMin, TextureBounds[1].vMax - TextureBounds[1].vMin));

	Logger::log << "[OpenVR] VR systems created successfully" << std::endl;
}

void OpenVR::OnGameFinishInit()
{
	HRESULT result = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, NULL, D3D11_SDK_VERSION, &D3DDevice, NULL, NULL);

	if (FAILED(result))
	{
		Logger::log << "[OpenVR] Could not initialise DirectX11: " << result << std::endl;
	}
	
	D3DSURFACE_DESC Desc, Desc2;
	Helpers::GetRenderTargets()[0].renderSurface->GetDesc(&Desc);
	Helpers::GetRenderTargets()[1].renderSurface->GetDesc(&Desc2);

	// Create shared textures
	// Eyes
	CreateTexAndSurface(0, RecommendedWidth, RecommendedHeight, Desc.Usage, Desc.Format);
	CreateTexAndSurface(1, RecommendedWidth, RecommendedHeight, Desc.Usage, Desc.Format);
	// UI Layer
	CreateTexAndSurface(2, RecommendedWidth, RecommendedHeight, Desc2.Usage, Desc2.Format);

	Logger::log << "[OpenVR] Finished Initialisation" << std::endl;
}


void OpenVR::CreateTexAndSurface(int index, UINT Width, UINT Height, DWORD Usage, D3DFORMAT Format)
{
	HANDLE SharedHandle = nullptr;

	// Create texture on game
	HRESULT result = Helpers::GetDirect3DDevice9()->CreateTexture(Width, Height, 1, Usage, Format, D3DPOOL_DEFAULT, &GameRenderTexture[index], &SharedHandle);
	if (FAILED(result))
	{
		Logger::log << "[OpenVR] Failed to create game " << index << " texture: " << result << std::endl;
		return;
	}

	result = GameRenderTexture[index]->GetSurfaceLevel(0, &GameRenderSurface[index]);
	if (FAILED(result))
	{
		Logger::log << "[OpenVR] Failed to retrieve game " << index << " surface: " << result << std::endl;
		return;
	}

	ID3D11Resource* tempResource = nullptr;

	// Open shared texture on vr
	result = D3DDevice->OpenSharedResource(SharedHandle, __uuidof(ID3D11Resource), (void**)&tempResource);

	if (FAILED(result))
	{
		Logger::log << "[OpenVR] Failed to open shared resource " << index << ": " << result << std::endl;
		return;
	}

	result = tempResource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&VRRenderTexture[index]);
	tempResource->Release();

	if (FAILED(result))
	{
		Logger::log << "[OpenVR] Failed to query texture interface " << index << ": " << result << std::endl;
		return;
	}

	Logger::log << "[OpenVR] Created shared texture " << index << std::endl;
}


void OpenVR::UpdatePoses()
{
	if (!VRCompositor)
	{
		return;
	}

	VRCompositor->WaitGetPoses(RenderPoses, vr::k_unMaxTrackedDeviceCount, GamePoses, vr::k_unMaxTrackedDeviceCount);
}

void OpenVR::PreDrawFrame(Renderer* renderer, float deltaTime)
{
}

void OpenVR::PostDrawFrame(Renderer* renderer, float deltaTime)
{
	if (!VRCompositor)
	{
		return;
	}

	// Wait for frame to finish rendering
	IDirect3DQuery9* pEventQuery = nullptr;
	Helpers::GetDirect3DDevice9()->CreateQuery(D3DQUERYTYPE_EVENT, &pEventQuery);
	if (pEventQuery != nullptr)
	{
		pEventQuery->Issue(D3DISSUE_END);
		while (pEventQuery->GetData(nullptr, 0, D3DGETDATA_FLUSH) != S_OK);
		pEventQuery->Release();
	}

	vr::Texture_t leftEye { (void*)VRRenderTexture[0], vr::TextureType_DirectX, vr::ColorSpace_Auto};
	vr::EVRCompositorError error = VRCompositor->Submit(vr::Eye_Left, &leftEye, &TextureBounds[0], vr::Submit_Default);

	if (error != vr::VRCompositorError_None)
	{
		Logger::log << "[OpenVR] Could not submit left eye texture: " << error << std::endl;
	}

	vr::Texture_t rightEye{ (void*)VRRenderTexture[1], vr::TextureType_DirectX, vr::ColorSpace_Auto };
	error = VRCompositor->Submit(vr::Eye_Right, &rightEye, &TextureBounds[1], vr::Submit_Default);

	if (error != vr::VRCompositorError_None)
	{
		Logger::log << "[OpenVR] Could not submit right eye texture: " << error << std::endl;
	}

	// TODO: UI via an overlay here

	VRCompositor->PostPresentHandoff();
}

void OpenVR::UpdateCameraFrustum(CameraFrustum* frustum, int eye)
{
	const float WORLD_SCALE = 1.0f;

	frustum->fov = FOV;

	Matrix4 eyeMatrix = GetHMDMatrixPoseEye((vr::Hmd_Eye) eye);

	Matrix4 headMatrix = ConvertSteamVRMatrixToMatrix4(RenderPoses[vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking);

	Matrix4 viewMatrix = (headMatrix * eyeMatrix.invert()).scale(Game::MetresToWorld(WORLD_SCALE));

	Matrix3 rotationMatrix = GetRotationMatrix(headMatrix);

	// Ignore all existing rotation for now
	frustum->facingDirection = Vector3(1.0f, 0.0f, 0.0f);
	frustum->upDirection = Vector3(0.0f, 0.0f, 1.0f);

	frustum->facingDirection = (rotationMatrix * frustum->facingDirection).normalize();
	frustum->upDirection = (rotationMatrix * frustum->upDirection).normalize();

	Vector4 NewPos = viewMatrix * Vector4(0.0f, 0.0f, 0.0f, 1.0f);

	frustum->position = frustum->position + GetVector3(NewPos);
}

int OpenVR::GetViewWidth()
{
    return RecommendedWidth;
}

int OpenVR::GetViewHeight()
{
    return RecommendedHeight;
}

float OpenVR::GetAspect()
{
	return Aspect;
}

IDirect3DSurface9* OpenVR::GetRenderSurface(int eye)
{
    return GameRenderSurface[eye];
}

IDirect3DTexture9* OpenVR::GetRenderTexture(int eye)
{
    return GameRenderTexture[eye];
}

IDirect3DSurface9* OpenVR::GetUISurface()
{
    return GameRenderSurface[2];
}

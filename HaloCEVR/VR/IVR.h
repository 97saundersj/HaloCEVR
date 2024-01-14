#pragma once
#include "../Maths/Matrices.h"

class IVR
{
public:
	virtual void Init() = 0;
	virtual void OnGameFinishInit() = 0;
	virtual void UpdatePoses() = 0;
	virtual void PreDrawFrame(struct Renderer* renderer, float deltaTime) = 0;
	virtual void PostDrawFrame(struct Renderer* renderer, float deltaTime) = 0;

	virtual void UpdateCameraFrustum(struct CameraFrustum* frustum, int eye) = 0;

	virtual int GetViewWidth() = 0;
	virtual int GetViewHeight() = 0;
	virtual float GetAspect() = 0;
	virtual Matrix4 GetHMDTransform(bool bRenderPose = false) = 0;
	virtual struct IDirect3DSurface9* GetRenderSurface(int eye) = 0;
	virtual struct IDirect3DTexture9* GetRenderTexture(int eye) = 0;
	virtual struct IDirect3DSurface9* GetUISurface() = 0;
};

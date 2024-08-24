#pragma once
#include "VR/IVR.h"

class InputHandler
{
public:
	void RegisterInputs();
	void UpdateInputs();
	void UpdateCamera(float& yaw, float& pitch);
	void SetMousePosition(int& x, int& y);
	void UpdateMouseInfo(struct MouseInfo* mouseInfo);

protected:

	unsigned char UpdateFlashlight();
	unsigned char UpdateMelee();
	unsigned char UpdateCrouch();

	char lastSnapState = 0;
	unsigned char mouseDownState = 0;

	InputBindingID Jump = 0;
	InputBindingID SwitchGrenades = 0;
	InputBindingID Interact = 0;
	InputBindingID SwitchWeapons = 0;
	InputBindingID Melee = 0;
	InputBindingID Flashlight = 0;
	InputBindingID Grenade = 0;
	InputBindingID Fire = 0;
	InputBindingID MenuForward = 0;
	InputBindingID MenuBack = 0;
	InputBindingID Crouch = 0;
	InputBindingID Zoom = 0;
	InputBindingID Reload = 0;
	InputBindingID Move = 0;
	InputBindingID Look = 0;
	
	InputBindingID Recentre = 0;
};


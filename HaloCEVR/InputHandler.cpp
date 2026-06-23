#include "InputHandler.h"
#include "Game.h"
#include "Helpers/Controls.h"
#include "Helpers/Camera.h"
#include "Helpers/Menus.h"
#include "Helpers/Maths.h"
#include "Helpers/Objects.h"
#include "Helpers/FirstPersonAnim.h"
#include "Helpers/Sound.h"
#include "Logger.h"


#define RegisterBoolInput(set, x) x = vr->RegisterBoolInput(set, #x);
#define RegisterVector2Input(set, x) x = vr->RegisterVector2Input(set, #x);
#define ApplyBoolInput(x) controls.##x = vr->GetBoolInput(x) ? 127 : 0;
#define ApplyImpulseBoolInput(x) controls.##x = vr->GetBoolInput(x, bHasChanged) && bHasChanged ? 127 : 0;

static void SuppressVanillaReloadControl(unsigned char& reloadControl)
{
	if (!Game::instance.c_DisableEmptyMagazineAutoReload->Value())
	{
		return;
	}

	if (Game::instance.bUse3DOFAiming || !Game::instance.SupportsPhysicalMagazineReload())
	{
		return;
	}

	// Always zero — physical reload is started via BeginPhysicalReload on VR edge input.
	reloadControl = 0;
}

bool InputHandler::ShouldBlockAutoReloadStart() const
{
	if (!Game::instance.c_DisableEmptyMagazineAutoReload->Value())
	{
		return false;
	}

	if (Game::instance.bUse3DOFAiming)
	{
		IVR* vr = Game::instance.GetVR();
		return !vr->GetBoolInput(Reload);
	}

	if (!Game::instance.SupportsPhysicalMagazineReload())
	{
		IVR* vr = Game::instance.GetVR();
		return !vr->GetBoolInput(Reload);
	}

	return !Game::instance.bManualPhysicalReloadPending;
}

void InputHandler::RegisterInputs()
{
	IVR* vr = Game::instance.GetVR();

	// These bindings will always be the same and won't change depending on action sets
	RegisterBoolInput("left handed", SwapWeaponHand);
	OffhandSwapWeaponHand = SwapWeaponHand;
	RegisterBoolInput("default", SwapWeaponHand);

	UpdateRegisteredInputs();
}

void InputHandler::UpdateRegisteredInputs()
{
	IVR* vr = Game::instance.GetVR();

	const char* actionSet = Game::instance.bLeftHanded ? "left handed" : "default";
	
	// These bindings will change depending on action sets
	RegisterBoolInput(actionSet, Jump);
	RegisterBoolInput(actionSet, SwitchGrenades);
	RegisterBoolInput(actionSet, Interact);
	RegisterBoolInput(actionSet, SwitchWeapons);
	RegisterBoolInput(actionSet, Melee);
	RegisterBoolInput(actionSet, Flashlight);
	RegisterBoolInput(actionSet, Grenade);
	RegisterBoolInput(actionSet, Fire);
	RegisterBoolInput(actionSet, MenuForward);
	RegisterBoolInput(actionSet, MenuBack);
	RegisterBoolInput(actionSet, Crouch);
	RegisterBoolInput(actionSet, Zoom);
	RegisterBoolInput(actionSet, Reload);
	RegisterBoolInput(actionSet, TwoHandGrip);

	RegisterVector2Input(actionSet, Move);
	RegisterVector2Input(actionSet, Look);
}

float AngleBetweenVector2(const Vector2& v1, const Vector2& v2)
{
	const float dot = v1.dot(v2);
	const float determinant = v1.x * v2.y - v1.y * v2.x;
	const float angle = atan2(determinant, dot);
	return angle;
}

Vector2 RotateVector2(const Vector2& v, float angle)
{
	Vector2 rotated;
	rotated.x = v.x * cos(angle) - v.y * sin(angle);
	rotated.y = v.x * sin(angle) + v.y * cos(angle);
	return rotated;
}

#define DRAW_DEBUG_MOVE 0

void InputHandler::UpdateInputs(bool bInVehicle)
{
	IVR* vr = Game::instance.GetVR();

	vr->UpdateInputs();

	static bool bHasChanged = false;

	if (Game::instance.bIsCustom)
	{
		Controls_Custom& controls = Helpers::GetControlsCustom();

		ApplyBoolInput(Jump);
		ApplyImpulseBoolInput(SwitchGrenades);
		ApplyBoolInput(Interact);
		ApplyBoolInput(Melee);
		ApplyBoolInput(Flashlight);
		ApplyBoolInput(Grenade);
		ApplyBoolInput(Fire);
		ApplyBoolInput(MenuForward);
		ApplyBoolInput(MenuBack);
		ApplyBoolInput(Crouch);
		ApplyImpulseBoolInput(Zoom);
		ApplyBoolInput(Reload);
		SuppressVanillaReloadControl(controls.Reload);

		Game::instance.bIsFiring = controls.Fire;
	}
	else
	{
		Controls& controls = Helpers::GetControls();

		ApplyBoolInput(Jump);
		ApplyImpulseBoolInput(SwitchGrenades);
		ApplyBoolInput(Interact);
		ApplyBoolInput(Melee);
		ApplyBoolInput(Flashlight);
		ApplyBoolInput(Grenade);
		ApplyBoolInput(Fire);
		ApplyBoolInput(MenuForward);
		ApplyBoolInput(MenuBack);
		ApplyBoolInput(Crouch);
		ApplyImpulseBoolInput(Zoom);
		ApplyBoolInput(Reload);
		SuppressVanillaReloadControl(controls.Reload);

		Game::instance.bIsFiring = controls.Fire;
	}


	unsigned char MotionControlFlashlight = UpdateFlashlight();
	if (MotionControlFlashlight > 0)
	{
		if (Game::instance.bIsCustom)
		{
			Helpers::GetControlsCustom().Flashlight = MotionControlFlashlight;
		}
		else
		{
			Helpers::GetControls().Flashlight = MotionControlFlashlight;
		}
	}

	if (Game::instance.c_EnableWeaponHolsters->Value())
	{
		unsigned char HolsterSwitchWeapons = UpdateHolsterSwitchWeapons();
		bool bSwitchWeaponsPressed = vr->GetBoolInput(SwitchWeapons);

		if (HolsterSwitchWeapons > 0 && bSwitchWeaponsPressed)
		{
			if (Game::instance.bIsCustom)
			{
				Controls_Custom& controls = Helpers::GetControlsCustom();
				ApplyImpulseBoolInput(SwitchWeapons);
			}
			else
			{
				Controls& controls = Helpers::GetControls();
				ApplyImpulseBoolInput(SwitchWeapons);
			}
		}
	}
	else
	{
		if (Game::instance.bIsCustom)
		{
			Controls_Custom& controls = Helpers::GetControlsCustom();
			ApplyImpulseBoolInput(SwitchWeapons);
		}
		else
		{
			Controls& controls = Helpers::GetControls();
			ApplyImpulseBoolInput(SwitchWeapons);
		}
	}

	unsigned char MotionControlMelee = UpdateMelee();
	if (MotionControlMelee > 0)
	{
		if (Game::instance.bIsCustom)
		{
			Helpers::GetControlsCustom().Melee = MotionControlMelee;
		}
		else
		{
			Helpers::GetControls().Melee = MotionControlMelee;
		}
	}

	unsigned char MotionControlCrouch = UpdateCrouch();
	if (MotionControlCrouch > 0)
	{
		if (Game::instance.bIsCustom)
		{
			Helpers::GetControlsCustom().Crouch = MotionControlCrouch;
		}
		else
		{
			Helpers::GetControls().Crouch = MotionControlCrouch;
		}
	}

	const float holdToRecentreTime = 1000.0f;

	bool bMenuChanged;
	bool bMenuPressed = vr->GetBoolInput(MenuBack, bMenuChanged);

	if (bMenuPressed)
	{
		if (bMenuChanged)
		{
			menuHeldTime = std::chrono::high_resolution_clock::now();
		}

		float heldTime = std::chrono::duration<float, std::milli>(std::chrono::high_resolution_clock::now() - menuHeldTime).count();

		if (heldTime > 100.0f)
		{
			float progress = std::min(heldTime, holdToRecentreTime) / holdToRecentreTime;

			Matrix4 handTrans = vr->GetControllerTransform(ControllerRole::Left, true);

			Vector3 handPos = (handTrans * Vector3(0.0f, 0.0f, 0.0f)) * Game::instance.MetresToWorld(1.0f) + Helpers::GetCamera().position;

			Vector3 centre = handPos + Vector3(0.0f, 0.0f, Game::instance.MetresToWorld(0.1f));
			Vector3 facing = -handTrans.getLeftAxis();
			Vector3 upVector = handTrans.getForwardAxis();

			Game::instance.inGameRenderer.DrawPolygon(centre, facing, upVector, 16, Game::instance.MetresToWorld(0.01f), D3DCOLOR_XRGB(255, 0, 0), false, progress);
		}
	}
	else if (bMenuChanged)
	{
		float heldTime = std::chrono::duration<float, std::milli>(std::chrono::high_resolution_clock::now() - menuHeldTime).count();

		if (heldTime > holdToRecentreTime)
		{
			Game::instance.bNeedsRecentre = true;
		}
		else
		{
			INPUT input{};
			input.type = INPUT_KEYBOARD;
			input.ki.dwFlags = KEYEVENTF_SCANCODE; // DirectInput only detects scancodes
			input.ki.wScan = 01; // Escape
			SendInput(1, &input, sizeof(INPUT));

			bHoldingMenu = true;
		}
	}
	else if (bHoldingMenu)
	{
		bHoldingMenu = false;

		INPUT input{};
		input.type = INPUT_KEYBOARD;
		input.ki.dwFlags = KEYEVENTF_SCANCODE; // DirectInput only detects scancodes
		input.ki.wScan = 01; // Escape
		input.ki.dwFlags |= KEYEVENTF_KEYUP;
		SendInput(1, &input, sizeof(INPUT));
	}

	UpdatePhysicalMagazineReload();

	UpdateHandsProximity();

	Vector2 MoveInput = vr->GetVector2Input(Move);

	if (!bInVehicle && (Game::instance.c_HandRelativeMovement->Value() != 0))
	{
		Camera& cam = Helpers::GetCamera();
		Vector3 camForward = cam.lookDir;

		const bool leftHand = (Game::instance.c_HandRelativeMovement->Value() == 1);
		const ControllerRole role = leftHand ? ControllerRole::Left : ControllerRole::Right;
		Matrix4 controllerTransform = vr->GetControllerTransform(role); // TODO: Not sure about bRenderPose bool param here. Doesn't seem to matter.
		const float offset = Game::instance.c_HandRelativeOffsetRotation->Value();
		controllerTransform.rotateZ(leftHand ? offset: -offset);
		Vector3 handForward = controllerTransform.getLeftAxis();

#if DRAW_DEBUG_MOVE
		Vector3 start = cam.position + cam.lookDirUp * -Game::instance.MetresToWorld(0.1f);
		Game::instance.inGameRenderer.DrawLine3D(start, start + camForward * Game::instance.MetresToWorld(2.0f), D3DCOLOR_ARGB(255, 255, 0, 0), false, 0.5f);
		Game::instance.inGameRenderer.DrawLine3D(start, start + handForward * Game::instance.MetresToWorld(2.0f), D3DCOLOR_ARGB(255, 0, 0, 255), false, 0.5f);
		handForward.z = camForward.z = 0.0f;
		Game::instance.inGameRenderer.DrawLine3D(start, start + camForward * Game::instance.MetresToWorld(2.0f), D3DCOLOR_ARGB(255, 255, 75, 0), false, 0.5f);
		Game::instance.inGameRenderer.DrawLine3D(start, start + handForward * Game::instance.MetresToWorld(2.0f), D3DCOLOR_ARGB(255, 100, 0, 255), false, 0.5f);

		start = cam.position + cam.lookDir * Game::instance.MetresToWorld(2.00f);
		const Vector3 camRight = cam.lookDir.cross(cam.lookDirUp);
		Vector3 end = start + cam.lookDirUp * MoveInput.y * Game::instance.MetresToWorld(1.0f) + camRight * MoveInput.x * Game::instance.MetresToWorld(1.0f);
		Game::instance.inGameRenderer.DrawLine3D(start, end, D3DCOLOR_ARGB(255, 0, 255, 0), false, 0.5f);
#endif

		const float angle = AngleBetweenVector2(Vector2(camForward.x, camForward.y), Vector2(handForward.x, handForward.y));
		MoveInput = RotateVector2(MoveInput, angle);

#if DRAW_DEBUG_MOVE
		end = start + cam.lookDirUp * MoveInput.y * Game::instance.MetresToWorld(1.0f) + camRight * MoveInput.x * Game::instance.MetresToWorld(1.0f);
		Game::instance.inGameRenderer.DrawLine3D(start, end, D3DCOLOR_ARGB(255, 255, 255, 0), false, 0.5f);
#endif
	}

	if (Game::instance.bIsCustom)
	{
		Controls_Custom& controls = Helpers::GetControlsCustom();

		controls.Left = -MoveInput.x;
		controls.Forwards = MoveInput.y;
	}
	else
	{
		Controls& controls = Helpers::GetControls();

		controls.Left = -MoveInput.x;
		controls.Forwards = MoveInput.y;
	}
}

void InputHandler::UpdateCamera(float& yaw, float& pitch)
{
	IVR* vr = Game::instance.GetVR();

	Vector2 lookInput = vr->GetVector2Input(Look);

	float yawOffset = vr->GetYawOffset();

	if (Game::instance.c_SnapTurn->Value())
	{
		if (lastSnapState == 1)
		{
			if (lookInput.x < 0.4f)
			{
				lastSnapState = 0;
			}
		}
		else if (lastSnapState == -1)
		{
			if (lookInput.x > -0.4f)
			{
				lastSnapState = 0;
			}
		}
		else
		{
			if (lookInput.x > 0.6f)
			{
				lastSnapState = 1;
				yawOffset += Game::instance.c_SnapTurnAmount->Value();
			}
			else if (lookInput.x < -0.6f)
			{
				lastSnapState = -1;
				yawOffset -= Game::instance.c_SnapTurnAmount->Value();
			}
		}
	}
	else
	{
		yawOffset += lookInput.x * Game::instance.c_SmoothTurnAmount->Value() * Game::instance.lastDeltaTime;
	}

	vr->SetYawOffset(yawOffset);

	Vector3 lookHMD = vr->GetHMDTransform().getLeftAxis();
	// Get current camera angle
	Vector3 lookGame = Game::instance.bDetectedChimera ? Game::instance.LastLookDir : Helpers::GetCamera().lookDir;
	// Apply deltas
	float yawHMD = atan2(lookHMD.y, lookHMD.x);
	float yawGame = atan2(lookGame.y, lookGame.x);
	yaw = (yawHMD - yawGame);

	float pitchHMD = atan2(lookHMD.z, sqrt(lookHMD.x * lookHMD.x + lookHMD.y * lookHMD.y));
	float pitchGame = atan2(lookGame.z, sqrt(lookGame.x * lookGame.x + lookGame.y * lookGame.y));
	pitch = (pitchHMD - pitchGame);
}

void InputHandler::UpdateCameraForVehicles(float& yaw, float& pitch)
{
	IVR* vr = Game::instance.GetVR();

	Vector2 lookInput = vr->GetVector2Input(Look);

	const float DegToRad = 3.141593f / 180.0f;

	const float YawDelta = lookInput.x * Game::instance.c_HorizontalVehicleTurnAmount->Value() * Game::instance.lastDeltaTime;
	const float PitchDelta = lookInput.y * Game::instance.c_VerticalVehicleTurnAmount->Value() * Game::instance.lastDeltaTime;

	float yawOffset = vr->GetYawOffset();
	
	yawOffset += YawDelta;

	vr->SetYawOffset(yawOffset);

	yaw = -DegToRad * YawDelta;
	pitch = DegToRad * PitchDelta;
}

unsigned char InputHandler::UpdateFlashlight()
{
	IVR* vr = Game::instance.GetVR();

	Vector3 headPos = vr->GetHMDTransform() * Vector3(-0.1f, 0.0f, 0.0f);

	float leftDistance = Game::instance.c_LeftHandFlashlightDistance->Value();
	float rightDistance = Game::instance.c_RightHandFlashlightDistance->Value();

	bool offhandFlashlightEnabled = Game::instance.c_OffhandHandFlashlight->Value();

	bool checkLeftHand = !offhandFlashlightEnabled || !Game::instance.bLeftHanded; 
	if (checkLeftHand && leftDistance > 0.0f)
	{
		Vector3 handPos = vr->GetRawControllerTransform(ControllerRole::Left) * Vector3(0.0f, 0.0f, 0.0f);

		if ((headPos - handPos).lengthSqr() < leftDistance * leftDistance)
		{
			return 127;
		}
	}

	bool checkRightHand = !offhandFlashlightEnabled || Game::instance.bLeftHanded; 
	if (checkRightHand && rightDistance > 0.0f)
	{
		Vector3 handPos = vr->GetRawControllerTransform(ControllerRole::Right) * Vector3(0.0f, 0.0f, 0.0f);

		if ((headPos - handPos).lengthSqr() < rightDistance * rightDistance)
		{
			return 127;
		}
	}

	return 0;
}

unsigned char InputHandler::UpdateHolsterSwitchWeapons()
{
	IVR* vr = Game::instance.GetVR();

	Matrix4 headTransform = vr->GetHMDTransform();

	// Calculate shoulder holster positions with the correct offset
	Vector3 leftShoulderPos = headTransform * Game::instance.c_LeftShoulderHolsterOffset->Value();
	Vector3 rightShoulderPos = headTransform * Game::instance.c_RightShoulderHolsterOffset->Value();

	Vector3 handPos;
	if (Game::instance.bLeftHanded)
	{
		handPos = vr->GetRawControllerTransform(ControllerRole::Left) * Vector3(0.0f, 0.0f, 0.0f);
	}
	else
	{
		handPos = vr->GetRawControllerTransform(ControllerRole::Right) * Vector3(0.0f, 0.0f, 0.0f);
	}

	if (InputHandler::IsHandInHolster(handPos, leftShoulderPos, Game::instance.c_LeftShoulderHolsterActivationDistance->Value()) 
		|| InputHandler::IsHandInHolster(handPos, rightShoulderPos, Game::instance.c_RightShoulderHolsterActivationDistance->Value()))
	{
		return 127;
	}

	return 0;
}

// Helper function to check if a hand is in a holster
bool InputHandler::IsHandInHolster(const Vector3& handPos, const Vector3& holsterPos, const float& holsterActivationDistance)
{
	return (holsterPos - handPos).lengthSqr() < holsterActivationDistance * holsterActivationDistance;
}

unsigned char InputHandler::UpdateMelee()
{
	IVR* vr = Game::instance.GetVR();

	Vector3 handVel = vr->GetControllerVelocity(ControllerRole::Left);

	handVel *= Game::instance.WorldToMetres(1.0f);

	if (Game::instance.c_LeftHandMeleeSwingSpeed->Value() > 0.0f && abs(handVel.z) > Game::instance.c_LeftHandMeleeSwingSpeed->Value())
	{
		return 127;
	}

	handVel = vr->GetControllerVelocity(ControllerRole::Right);

	handVel *= Game::instance.WorldToMetres(1.0f);

	if (Game::instance.c_RightHandMeleeSwingSpeed->Value() > 0.0f && abs(handVel.z) > Game::instance.c_RightHandMeleeSwingSpeed->Value())
	{
		return 127;
	}

    return 0;
}

unsigned char InputHandler::UpdateCrouch()
{
	IVR* vr = Game::instance.GetVR();

	Vector3 headPos = vr->GetHMDTransform() * Vector3(0.0f, 0.0f, 0.0f);

	float crouchHeight = Game::instance.c_CrouchHeight->Value();

	if (crouchHeight < 0)
	{
		return 0;
	}

	if (headPos.z < -crouchHeight)
	{
		return 127;
	}

	return 0;
}

void InputHandler::ResetPhysicalReloadState()
{
	Game::instance.ResetPhysicalReloadState();
}

WeaponDynamicObject* InputHandler::GetLocalWeaponObject()
{
	BaseDynamicObject* player = Helpers::GetLocalPlayer();
	if (!player || player->weapon.id == 0xffff)
	{
		return nullptr;
	}

	return static_cast<WeaponDynamicObject*>(Helpers::GetDynamicObject(player->weapon));
}

static bool ShouldPausePhysicalReload(
	uint16_t fpAnimId,
	uint16_t fpAnimFrame,
	int reloadEmptyAnimIndex,
	int ejectFrame,
	uint16_t initialRemaining,
	uint16_t currentRemaining,
	int ejectTicksFromStart)
{
	if (initialRemaining > currentRemaining && ejectTicksFromStart > 0)
	{
		const uint16_t elapsed = initialRemaining - currentRemaining;
		if (elapsed >= static_cast<uint16_t>(ejectTicksFromStart))
		{
			return true;
		}
	}

	if (ejectFrame < 0)
	{
		return false;
	}

	if (reloadEmptyAnimIndex >= 0)
	{
		return fpAnimId == static_cast<uint16_t>(reloadEmptyAnimIndex)
			&& fpAnimFrame >= static_cast<uint16_t>(ejectFrame);
	}

	return fpAnimFrame >= static_cast<uint16_t>(ejectFrame);
}

void InputHandler::ApplyPhysicalReloadAnimPin()
{
	if (Game::instance.physicalReloadPhase != EPhysicalReloadPhase::PausedAtEject)
	{
		return;
	}

	WeaponDynamicObject* weaponObject = GetLocalWeaponObject();
	if (!weaponObject)
	{
		return;
	}

	Weapon& weapon = weaponObject->weaponData[0];

	if (Game::instance.frozenReloadRemaining > 0)
	{
		weapon.reloadRemaining = Game::instance.frozenReloadRemaining;
	}
	else
	{
		weapon.reloadRemaining = 9999;
		Game::instance.frozenReloadRemaining = 9999;
	}

	weapon.reloadState = 1;

	const uint16_t pinnedAnim = Game::instance.pausedReloadAnimIndex != 0
		? Game::instance.pausedReloadAnimIndex
		: static_cast<uint16_t>(Game::instance.GetActiveReloadAnimIndex());
	const uint16_t pinnedFrame = Game::instance.pausedReloadAnimFrame;

	if (pinnedAnim != 0xFFFF)
	{
		weaponObject->animation = pinnedAnim;
	}

	weaponObject->animFrame = pinnedFrame;

	if (Helpers::HasFirstPersonAnimBase())
	{
		if (pinnedAnim != 0xFFFF)
		{
			Helpers::SetFirstPersonBaseAnimId(pinnedAnim);
		}

		Helpers::SetFirstPersonBaseAnimFrame(pinnedFrame);
	}
}

void InputHandler::UpdateReloadAnimationPause()
{
	if (Game::instance.physicalReloadPhase != EPhysicalReloadPhase::PlayingEject
		&& Game::instance.physicalReloadPhase != EPhysicalReloadPhase::PausedAtEject
		&& Game::instance.physicalReloadPhase != EPhysicalReloadPhase::PlayingFinish)
	{
		return;
	}

	WeaponDynamicObject* weaponObject = GetLocalWeaponObject();
	if (!weaponObject)
	{
		ResetPhysicalReloadState();
		return;
	}

	const uint16_t fpAnimId = Helpers::GetFirstPersonBaseAnimId();
	const uint16_t fpAnimFrame = Helpers::GetFirstPersonBaseAnimFrame();

	static EPhysicalReloadPhase lastLoggedPhase = EPhysicalReloadPhase::Idle;
	static int logFrameCounter = 0;
	const EPhysicalReloadPhase currentPhase = Game::instance.physicalReloadPhase;
	const bool phaseChanged = currentPhase != lastLoggedPhase;
	if (phaseChanged)
	{
		logFrameCounter = 0;
		lastLoggedPhase = currentPhase;
	}
	else
	{
		logFrameCounter++;
	}

	if (Game::instance.c_LogPhysicalReloadFrames->Value()
		&& (Game::instance.bIsReloading || currentPhase != EPhysicalReloadPhase::Idle)
		&& (phaseChanged
			|| currentPhase == EPhysicalReloadPhase::PlayingEject
			|| (currentPhase == EPhysicalReloadPhase::PausedAtEject && logFrameCounter % 30 == 0)
			|| currentPhase == EPhysicalReloadPhase::PlayingFinish))
	{
		Logger::log << "[PhysicalReload] weapon=" << static_cast<int>(Game::instance.GetCachedWeaponType())
			<< " phase=" << static_cast<int>(currentPhase)
			<< " fpAnim=" << fpAnimId
			<< " fpFrame=" << fpAnimFrame
			<< " reloadEmptyIdx=" << Game::instance.GetActiveReloadAnimIndex()
			<< " reloadState=" << weaponObject->weaponData[0].reloadState
			<< " reloadRemaining=" << weaponObject->weaponData[0].reloadRemaining
			<< " initialRemaining=" << Game::instance.initialReloadRemaining
			<< " ammo=" << weaponObject->weaponData[0].ammo
			<< std::endl;
	}

	if (Game::instance.physicalReloadPhase == EPhysicalReloadPhase::PlayingEject)
	{
		if (!Game::instance.bIsReloading)
		{
			ResetPhysicalReloadState();
			return;
		}

		if (Game::instance.initialReloadRemaining == 0)
		{
			Game::instance.initialReloadRemaining = weaponObject->weaponData[0].reloadRemaining;
		}

		const int ejectFrame = Game::instance.GetPhysicalReloadEjectFrame();
		const int reloadAnimIndex = Game::instance.GetActiveReloadAnimIndex();
		const int ejectTicksFromStart = Game::instance.c_PhysicalReloadEjectTicksFromStart->Value();

		if (ShouldPausePhysicalReload(
			fpAnimId,
			fpAnimFrame,
			reloadAnimIndex,
			ejectFrame,
			Game::instance.initialReloadRemaining,
			weaponObject->weaponData[0].reloadRemaining,
			ejectTicksFromStart))
		{
			const int configuredEjectFrame = ejectFrame >= 0 ? ejectFrame : 0;
			Game::instance.pausedReloadAnimIndex = reloadAnimIndex >= 0
				? static_cast<uint16_t>(reloadAnimIndex)
				: fpAnimId;
			Game::instance.pausedReloadAnimFrame = static_cast<uint16_t>(configuredEjectFrame);
			Game::instance.frozenReloadRemaining = weaponObject->weaponData[0].reloadRemaining;
			Game::instance.physicalReloadPhase = EPhysicalReloadPhase::PausedAtEject;
			Game::instance.bMagazineEjected = true;
			Game::instance.ClearPhysicalReloadBoneSnapshot();
			if (Game::instance.c_LogPhysicalReloadDebug->Value())
			{
				const Vector3 beltPos = Game::instance.GetBeltMagazineWorldPosition();
				Logger::log << "[PhysicalReload] paused at eject beltPos=("
					<< beltPos.x << "," << beltPos.y << "," << beltPos.z << ")"
					<< std::endl;
			}
			Helpers::PausePhysicalReloadSounds();
		}
	}
	else if (Game::instance.physicalReloadPhase == EPhysicalReloadPhase::PausedAtEject)
	{
		ApplyPhysicalReloadAnimPin();
		Helpers::PausePhysicalReloadSounds();

		if (!Game::instance.bIsReloading)
		{
			ResetPhysicalReloadState();
		}
	}
	else if (Game::instance.physicalReloadPhase == EPhysicalReloadPhase::PlayingFinish)
	{
		Weapon& weapon = weaponObject->weaponData[0];

		static int finishFrameCounter = 0;
		if (phaseChanged)
		{
			finishFrameCounter = 0;
		}
		else
		{
			finishFrameCounter++;
		}

		// Pausing mid state-1 skips the normal transition that refills ammo via ReloadEnd.
		constexpr int kFinishReloadTicks = 15;
		const bool bTimerDone = weapon.reloadRemaining == 0 || weapon.reloadState == 0;
		const bool bFinishTimedOut = finishFrameCounter >= kFinishReloadTicks + 5;

		if (weapon.ammo == 0 && (bTimerDone || bFinishTimedOut))
		{
			Game::instance.TriggerWeaponReloadEnd();
		}
		else if (!Game::instance.bIsReloading)
		{
			ResetPhysicalReloadState();
		}
	}
}

void InputHandler::BeginPhysicalReload()
{
	Game::instance.bPhysicalReloadFromEmpty = Game::instance.IsLocalMagazineEmpty();
	Game::instance.bManualPhysicalReloadPending = true;
	Game::instance.physicalReloadPhase = EPhysicalReloadPhase::PlayingEject;
	Game::instance.initialReloadRemaining = 0;
	// Capture the reload SFX buffer(s) so we can silence only those during the eject pause.
	Helpers::BeginPhysicalReloadSoundCapture();
	Game::instance.TriggerWeaponReload();
	Game::instance.bManualPhysicalReloadPending = false;

	if (!Game::instance.bIsReloading)
	{
		ResetPhysicalReloadState();
		return;
	}

	WeaponDynamicObject* weaponObject = GetLocalWeaponObject();
	if (weaponObject)
	{
		Game::instance.initialReloadRemaining = weaponObject->weaponData[0].reloadRemaining;
	}
}

void InputHandler::ResumePhysicalReloadAnimation()
{
	WeaponDynamicObject* weaponObject = GetLocalWeaponObject();
	if (!weaponObject)
	{
		ResetPhysicalReloadState();
		return;
	}

	const int resumeFrame = Game::instance.GetPhysicalReloadResumeFrame();
	const int exitAnimIndex = Game::instance.GetActiveReloadExitAnimIndex();
	const int reloadAnimIndex = Game::instance.GetActiveReloadAnimIndex();

	if (Helpers::HasFirstPersonAnimBase())
	{
		if (exitAnimIndex >= 0)
		{
			Helpers::SetFirstPersonBaseAnimId(static_cast<uint16_t>(exitAnimIndex));
			Helpers::SetFirstPersonBaseAnimFrame(0);
		}
		else if (reloadAnimIndex >= 0)
		{
			Helpers::SetFirstPersonBaseAnimId(static_cast<uint16_t>(reloadAnimIndex));
			if (resumeFrame >= 0)
			{
				Helpers::SetFirstPersonBaseAnimFrame(static_cast<uint16_t>(resumeFrame));
			}
		}
		else if (resumeFrame >= 0)
		{
			Helpers::SetFirstPersonBaseAnimFrame(static_cast<uint16_t>(resumeFrame));
		}
	}

	constexpr uint16_t kFinishReloadTicks = 15;
	weaponObject->weaponData[0].reloadRemaining = kFinishReloadTicks;
	weaponObject->weaponData[0].reloadState = 2;

	Helpers::ClearPhysicalReloadSounds();

	Game::instance.physicalReloadPhase = EPhysicalReloadPhase::PlayingFinish;
	Game::instance.bMagazineGrabbed = false;
	Game::instance.bMagazineEjected = false;
	Game::instance.frozenReloadRemaining = 0;
	Game::instance.ClearPhysicalReloadBoneSnapshot();
}

void InputHandler::HandlePhysicalMagazineGrabInsert()
{
	IVR* vr = Game::instance.GetVR();
	const ControllerRole offHand = Game::instance.bLeftHanded ? ControllerRole::Right : ControllerRole::Left;
	Matrix4 offHandTransform = vr->GetControllerTransform(offHand, true);
	Vector3 offHandPos = offHandTransform * Vector3(0.0f, 0.0f, 0.0f);
	offHandPos *= Game::instance.MetresToWorld(1.0f);
	offHandPos += Helpers::GetCamera().position;

	const Vector3 beltPos = Game::instance.GetBeltMagazineWorldPosition();
	const Vector3 socketPos = Game::instance.GetMagazineSocketWorldPosition();
	const float grabDistance = Game::instance.c_BeltMagazineGrabDistance->Value();
	const float insertDistance = Game::instance.c_BeltMagazineInsertDistance->Value();
	const float grabDistanceSqr = grabDistance * grabDistance;
	const float insertDistanceSqr = insertDistance * insertDistance;

	const bool offHandNearBelt = (offHandPos - beltPos).lengthSqr() < grabDistanceSqr;
	const bool offHandNearSocket = (offHandPos - socketPos).lengthSqr() < insertDistanceSqr;
	const bool gripHeld = vr->GetBoolInput(TwoHandGrip);

	if (!Game::instance.bMagazineGrabbed)
	{
		if (gripHeld && offHandNearBelt)
		{
			Game::instance.bMagazineGrabbed = true;
			bSuppressSwapUntilGripRelease = true;
		}
	}
	else if (offHandNearSocket)
	{
		ResumePhysicalReloadAnimation();
	}
	else if (!gripHeld)
	{
		Game::instance.bMagazineGrabbed = false;
	}
}

void InputHandler::UpdatePhysicalMagazineReload()
{
	if (!Game::instance.c_DisableEmptyMagazineAutoReload->Value()
		|| Game::instance.bUse3DOFAiming
		|| !Game::instance.HasMagazineBones())
	{
		if (Game::instance.physicalReloadPhase != EPhysicalReloadPhase::Idle)
		{
			ResetPhysicalReloadState();
		}
		else
		{
			Game::instance.bMagazineEjected = false;
			Game::instance.bMagazineGrabbed = false;
		}
		return;
	}

	UpdateReloadAnimationPause();

	switch (Game::instance.physicalReloadPhase)
	{
	case EPhysicalReloadPhase::Idle:
	{
		IVR* vr = Game::instance.GetVR();
		bool bReloadChanged = false;
		const bool reloadPressed = vr->GetBoolInput(Reload, bReloadChanged);

		if (reloadPressed && bReloadChanged)
		{
			BeginPhysicalReload();
		}
		break;
	}
	case EPhysicalReloadPhase::PausedAtEject:
		HandlePhysicalMagazineGrabInsert();
		break;
	default:
		break;
	}
}

void InputHandler::SetMousePosition(int& x, int& y)
{
	Vector2 mousePos = Game::instance.GetVR()->GetMousePos();
	// Best I can tell the mouse is always scaled to a 640x480 canvas
	x = static_cast<int>(mousePos.x * 640);
	y = static_cast<int>(mousePos.y * 480);
}

void InputHandler::UpdateMouseInfo(MouseInfo* mouseInfo)
{
	if (Game::instance.GetVR()->GetMouseDown())
	{
		if (mouseDownState < 255)
		{
			mouseDownState++;
		}
	}
	else
	{
		mouseDownState = 0;
	}

	mouseInfo->buttonState[0] = mouseDownState;
}

bool InputHandler::GetCalculatedHandPositions(Matrix4& controllerTransform, Vector3& dominantHandPos, Vector3& offHand)
{
	ControllerRole dominant = Game::instance.bLeftHanded ? ControllerRole::Left : ControllerRole::Right;
	ControllerRole nonDominant = Game::instance.bLeftHanded ? ControllerRole::Right : ControllerRole::Left;

	controllerTransform = Game::instance.GetVR()->GetControllerTransform(dominant, true);

	Vector3 poseDirection;
	bool bHasPoseData = Game::instance.GetVR()->TryGetControllerFacing(dominant, poseDirection);

	// When 2h aiming point the main hand at the offhand 
	if (Game::instance.bUseTwoHandAim || bHasPoseData)
	{
		Matrix4 aimingTransform = Game::instance.GetVR()->GetRawControllerTransform(dominant, true);
		Matrix4 offHandTransform = Game::instance.GetVR()->GetRawControllerTransform(nonDominant, true);

		const Vector3 actualControllerPos = controllerTransform * Vector3(0.0f, 0.0f, 0.0f);
		const Vector3 mainHandPos = aimingTransform * Vector3(0.0f, 0.0f, 0.0f);
		const Vector3 offHandPos = Game::instance.bUseTwoHandAim ? offHandTransform * Vector3(0.0f, 0.0f, 0.0f) : mainHandPos + poseDirection;
		Vector3 toOffHand = (offHandPos - mainHandPos);

		// Avoid NaN errors
		if (toOffHand.lengthSqr() < 1e-8)
		{
			return false;
		}

		toOffHand.normalize();
		dominantHandPos = actualControllerPos; 
		offHand = toOffHand; 

		return true; 
	}

	return false; 
}

void InputHandler::CalculateSmoothedInput()
{
	Matrix4 controllerTransform;
	Vector3 actualControllerPos;
	Vector3 toOffHand;

	if (!GetCalculatedHandPositions(controllerTransform, actualControllerPos, toOffHand))
	{
		return;
	}

	if (!bLastYawInitialized)
	{
		lastSmoothingYawOffset = Game::instance.GetVR()->GetYawOffset();
		bLastYawInitialized = true;
	}

	// Detect snap turns and rotate smoothed position to prevent lerping artifact
	float currentYawOffset = Game::instance.GetVR()->GetYawOffset();
	float yawDelta = currentYawOffset - lastSmoothingYawOffset;

	// Detect snap turns and rotate smoothed position
	Helpers::RotateForSnapTurn(smoothedPosition, yawDelta, Game::instance.c_SnapTurnAmount->Value());

	// Update tracked yaw offset for next frame
	lastSmoothingYawOffset = currentYawOffset;

	float userInput = 0.0f;
	short zoom = Helpers::GetInputData().zoomLevel;

	if (zoom == -1)
	{
		userInput = Game::instance.c_WeaponSmoothingAmountNoZoom->Value();
	}
	else if (zoom == 0)
	{
		userInput = Game::instance.c_WeaponSmoothingAmountOneZoom->Value();
	}
	else if (zoom == 1)
	{
		userInput = Game::instance.c_WeaponSmoothingAmountTwoZoom->Value();
	}

	float clampedValue = std::clamp(userInput, 0.0f, 2.0f);
	if (clampedValue == 0.0f)
	{
		smoothedPosition = actualControllerPos + toOffHand;
	}
	else
	{
		const float scaleFactor = (-20.0f / 9.0f);

		float h = 90.0f * log2(1.0f - exp(clampedValue * scaleFactor));

		float t = 1.0f - pow(2.0f, Game::instance.lastDeltaTime * h);

		smoothedPosition = Helpers::Lerp(smoothedPosition, actualControllerPos + toOffHand, t);
	}
}

void InputHandler::UpdateHandsProximity()
{
	// Once a physical reload uses the off-hand grip, don't allow a weapon-hand swap until that
	// grip is released - otherwise the still-held insert grip is misread as a swap when the
	// reload finishes and the magazine flags clear.
	if (bSuppressSwapUntilGripRelease)
	{
		IVR* swapVr = Game::instance.GetVR();
		if (!swapVr->GetBoolInput(SwapWeaponHand) && !swapVr->GetBoolInput(OffhandSwapWeaponHand))
		{
			bSuppressSwapUntilGripRelease = false;
		}
	}

	float swapHandDistance = Game::instance.c_SwapHandDistance->Value();
	
	const Vector3 leftPos = Game::instance.GetVR()->GetControllerTransform(ControllerRole::Left, true) * Vector3(0.0f, 0.0f, 0.0f);
	const Vector3 rightPos = Game::instance.GetVR()->GetControllerTransform(ControllerRole::Right, true) * Vector3(0.0f, 0.0f, 0.0f);
	float handDistance = (rightPos - leftPos).lengthSqr();

	bool handsWithinSwapWeaponDistance = false;
	if (swapHandDistance >= 0.0f && handDistance < swapHandDistance * swapHandDistance)
	{
		handsWithinSwapWeaponDistance = true;
		CheckSwapWeaponHand();
	}

	UpdateTwoHandedHold(handDistance, handsWithinSwapWeaponDistance);
}

void InputHandler::CheckSwapWeaponHand()
{
	// Suppressed until the magazine-grab/insert grip is released (cleared in UpdateHandsProximity).
	if (bSuppressSwapUntilGripRelease)
	{
		return;
	}

	// Off-hand grip is shared with magazine grab; don't swap weapon hands during physical reload.
	// Cover the whole sequence (incl. PlayingFinish after insert) so the still-held grip used to
	// insert the magazine isn't interpreted as a hand swap once the mag flags are cleared.
	if (Game::instance.c_DisableEmptyMagazineAutoReload->Value()
		&& (Game::instance.physicalReloadPhase != EPhysicalReloadPhase::Idle
			|| Game::instance.bMagazineEjected
			|| Game::instance.bMagazineGrabbed
			|| Game::instance.bIsReloading))
	{
		return;
	}

	IVR* vr = Game::instance.GetVR();

	bool bWeaponHandChanged;
	bool bOffhandWeaponHandChanged;
	bool bIsSwitchHandsPressed = vr->GetBoolInput(SwapWeaponHand, bWeaponHandChanged);
	bool bIsOffhandSwitchHandsPressed = vr->GetBoolInput(OffhandSwapWeaponHand, bOffhandWeaponHandChanged);

	bool offHandGrabbedWeapon = false;
	bool dominantHandReleasedWeapon = false;

    if (!Game::instance.bLeftHanded)
    {
		offHandGrabbedWeapon = bIsSwitchHandsPressed && bWeaponHandChanged && !bIsOffhandSwitchHandsPressed;
		dominantHandReleasedWeapon = bIsSwitchHandsPressed && !bIsOffhandSwitchHandsPressed && bOffhandWeaponHandChanged;
    }
    else
    {
        offHandGrabbedWeapon = bIsOffhandSwitchHandsPressed && bOffhandWeaponHandChanged && !bIsSwitchHandsPressed;
		dominantHandReleasedWeapon = bIsOffhandSwitchHandsPressed && !bIsSwitchHandsPressed && bWeaponHandChanged;
    }

	if (offHandGrabbedWeapon || dominantHandReleasedWeapon)
    {
		// Enable left handed and update the bindings to use the relevant action set
        Game::instance.bLeftHanded = !Game::instance.bLeftHanded;
		
		UpdateRegisteredInputs();
    }
}

void InputHandler::UpdateTwoHandedHold(float handDistance, bool handsWithinSwapWeaponDistance)
{
	// Two hand aim is disabled when 3DOF is enabled.
	if (Game::instance.bUse3DOFAiming) {
		Game::instance.bUseTwoHandAim = false;
		return;
	}

	// Off-hand grip is used to grab the ejected magazine during physical reload.
	if (Game::instance.c_DisableEmptyMagazineAutoReload->Value()
		&& (Game::instance.bMagazineEjected || Game::instance.bMagazineGrabbed))
	{
		Game::instance.bUseTwoHandAim = false;
		return;
	}

	IVR* vr = Game::instance.GetVR();

	bool bGripChanged;
	bool bIsGripping = vr->GetBoolInput(TwoHandGrip, bGripChanged);

	if (handsWithinSwapWeaponDistance)
	{
		if (!bIsGripping) {
	        Game::instance.bUseTwoHandAim = false;
	    }
		return;
	}

	if (Game::instance.c_ToggleGrip->Value())
	{
	    if (bGripChanged && bIsGripping)
	    {
	        bWasGripping ^= true;
	    }
	    bIsGripping = bWasGripping;
	}

	float twoHandDistance = Game::instance.c_TwoHandDistance->Value();
	if (twoHandDistance >= 0.0f)
	{
	    if (bGripChanged)
	    {
	        if (bIsGripping)
	        {
				if (handDistance < twoHandDistance * twoHandDistance)
	            {
	                Game::instance.bUseTwoHandAim = true;
	            }
	        }
	        else
	        {
	            Game::instance.bUseTwoHandAim = false;
	        }
	    }
	}
	else
	{
	    Game::instance.bUseTwoHandAim = bIsGripping;
	}
}

#undef ApplyBoolInput
#undef ApplyImpulseBoolInput
#undef RegisterBoolInput
#undef RegisterVector2Input
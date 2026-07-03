#include "PhysicalReload.h"
#include "../InputHandler.h"
#include "../Game.h"
#include "Camera.h"
#include "FirstPersonAnim.h"
#include "Objects.h"
#include "Sound.h"
#include "../Logger.h"
#include "../WeaponManualReloadConfig.h"
#include "../WeaponHapticsConfig.h"
#include "../Hooking/Hooks.h"

namespace
{
Game& G()
{
	return Game::instance;
}

WeaponHandler& WH()
{
	return Game::instance.GetWeaponHandler();
}

const WeaponManualReloadSettings& ReloadSettings()
{
	return Game::instance.weaponManualReloadConfig.GetSettings(WH().GetCachedWeaponType());
}
}

static WeaponDynamicObject* GetLocalWeaponObject()
{
	BaseDynamicObject* player = Helpers::GetLocalPlayer();
	if (!player || player->weapon.id == 0xffff)
	{
		return nullptr;
	}

	return static_cast<WeaponDynamicObject*>(Helpers::GetDynamicObject(player->weapon));
}

static bool ShouldPausePhysicalReload(
	uint16_t initialRemaining,
	uint16_t currentRemaining,
	int pauseTicks)
{
	if (pauseTicks <= 0 || initialRemaining <= currentRemaining)
	{
		return false;
	}

	return (initialRemaining - currentRemaining) >= static_cast<uint16_t>(pauseTicks);
}

PhysicalReloadController::PhysicalReloadController(InputHandler& inputHandler)
	: input(inputHandler)
{
}

void PhysicalReloadController::SuppressVanillaReloadControl(unsigned char& reloadControl) const
{
	if (!Game::instance.c_DisableEmptyMagazineAutoReload->Value())
	{
		return;
	}

	if (Game::instance.bUse3DOFAiming || !WH().SupportsPhysicalMagazineReload())
	{
		return;
	}

	// Always zero — physical reload is started via BeginPhysicalReload on VR edge input.
	reloadControl = 0;
}

bool PhysicalReloadController::ShouldBlockAutoReloadStart() const
{
	if (!Game::instance.c_DisableEmptyMagazineAutoReload->Value())
	{
		return false;
	}

	if (Game::instance.bUse3DOFAiming)
	{
		IVR* vr = Game::instance.GetVR();
		return !vr->GetBoolInput(input.Reload);
	}

	if (!WH().SupportsPhysicalMagazineReload())
	{
		IVR* vr = Game::instance.GetVR();
		return !vr->GetBoolInput(input.Reload);
	}

	return !Game::instance.bManualPhysicalReloadPending;
}

void PhysicalReloadController::ResetCycleCore()
{
	Game& game = Game::instance;
	Helpers::ResumePhysicalReloadSounds(false);
	game.physicalReloadPhase = EPhysicalReloadPhase::Idle;
	game.bManualPhysicalReloadPending = false;
	game.bPhysicalReloadFromEmpty = false;
	game.bMagazineEjected = false;
	game.bMagazineGrabbed = false;
	game.pausedReloadAnimIndex = 0;
	game.pausedReloadAnimFrame = 0;
	game.frozenReloadRemaining = 0;
	game.initialReloadRemaining = 0;
	WH().ResetPhysicalReloadBonePinState();
	if (!game.bShotgunShellSessionActive)
	{
		WH().ClearReloadStartInsertSocket();
	}
}

bool PhysicalReloadController::ShouldContinueContinuousReloadSession() const
{
	const WeaponManualReloadSettings& settings = ReloadSettings();
	return Game::instance.bShotgunShellSessionActive
		&& settings.ContinuousReload
		&& WH().IsShellByShellReloadWeapon()
		&& WH().CanLoadAnotherShell();
}

bool PhysicalReloadController::ShouldAutoStartContinuousReloadSession() const
{
	const WeaponManualReloadSettings& settings = ReloadSettings();
	return settings.ContinuousReload
		&& !Game::instance.bShotgunShellSessionUserCancelled
		&& WH().IsShellByShellReloadWeapon()
		&& WH().CanLoadAnotherShell()
		&& Game::instance.physicalReloadPhase == EPhysicalReloadPhase::Idle
		&& !Game::instance.bIsReloading;
}

void PhysicalReloadController::TriggerWeaponReload()
{
	BaseDynamicObject* player = Helpers::GetLocalPlayer();
	if (!player || player->weapon.id == 0xffff)
	{
		return;
	}

	Hooks::CallReloadStart(player->weapon, 0, true);
	Game::instance.ReloadStart(player->weapon, 0, true);
}

void PhysicalReloadController::TriggerWeaponReloadEnd()
{
	BaseDynamicObject* player = Helpers::GetLocalPlayer();
	if (!player || player->weapon.id == 0xffff)
	{
		return;
	}

	Hooks::CallReloadEnd(0, player->weapon);
}

void PhysicalReloadController::HandleReloadEnd()
{
	const bool bPreserveChainedReload = ShouldContinueContinuousReloadSession()
		&& Game::instance.physicalReloadPhase != EPhysicalReloadPhase::Idle
		&& Game::instance.physicalReloadPhase != EPhysicalReloadPhase::PlayingFinish;

	if (Game::instance.physicalReloadPhase == EPhysicalReloadPhase::PlayingFinish
		&& ShouldContinueContinuousReloadSession())
	{
		Game::instance.bIsReloading = false;
		return;
	}

	if (bPreserveChainedReload)
	{
		return;
	}

	Game::instance.bIsReloading = false;

	if (ShouldContinueContinuousReloadSession()
		&& Game::instance.physicalReloadPhase == EPhysicalReloadPhase::Idle)
	{
		ResetCycle();
		return;
	}

	ResetState();
}

void PhysicalReloadController::ResetState()
{
	bBeltGripStartedReload = false;
	Game::instance.bShotgunShellSessionActive = false;
	ResetCycleCore();
}

void PhysicalReloadController::ResetCycle()
{
	bBeltGripStartedReload = false;
	ResetCycleCore();
}

void PhysicalReloadController::EndShotgunShellSession()
{
	Game::instance.bShotgunShellSessionActive = false;
	ResetCycleCore();
}

bool PhysicalReloadController::IsOffHandNearBeltMagazine() const
{
	IVR* vr = Game::instance.GetVR();
	const ControllerRole offHand = Game::instance.bLeftHanded ? ControllerRole::Right : ControllerRole::Left;
	Matrix4 offHandTransform = vr->GetControllerTransform(offHand, true);
	Vector3 offHandPos = offHandTransform * Vector3(0.0f, 0.0f, 0.0f);
	offHandPos *= Game::instance.MetresToWorld(1.0f);
	offHandPos += Helpers::GetCamera().position;

	const Vector3 beltPos = WH().GetBeltMagazineWorldPosition();
	const float grabDistance = Game::instance.c_BeltMagazineGrabDistance->Value();
	const float grabDistanceSqr = grabDistance * grabDistance;
	return (offHandPos - beltPos).lengthSqr() < grabDistanceSqr;
}

bool PhysicalReloadController::ShouldSuppressTwoHandAim() const
{
	if (!Game::instance.c_DisableEmptyMagazineAutoReload->Value())
	{
		return false;
	}

	if (Game::instance.bMagazineEjected
		|| Game::instance.bMagazineGrabbed
		|| Game::instance.physicalReloadPhase != EPhysicalReloadPhase::Idle)
	{
		return true;
	}

	return WH().ShouldShowBeltMagazine() && IsOffHandNearBeltMagazine();
}

bool PhysicalReloadController::IsBlockingWeaponHandSwap() const
{
	return bSuppressSwapUntilGripRelease;
}

void PhysicalReloadController::TickSwapSuppression()
{
	if (!bSuppressSwapUntilGripRelease)
	{
		return;
	}

	IVR* swapVr = Game::instance.GetVR();
	if (!swapVr->GetBoolInput(input.SwapWeaponHand) && !swapVr->GetBoolInput(input.OffhandSwapWeaponHand))
	{
		bSuppressSwapUntilGripRelease = false;
	}
}

bool PhysicalReloadController::ShouldSkipWeaponHandSwapDuringReload() const
{
	if (bSuppressSwapUntilGripRelease)
	{
		return true;
	}

	if (!Game::instance.c_DisableEmptyMagazineAutoReload->Value())
	{
		return false;
	}

	return Game::instance.physicalReloadPhase != EPhysicalReloadPhase::Idle
		|| Game::instance.bMagazineEjected
		|| Game::instance.bMagazineGrabbed
		|| Game::instance.bIsReloading;
}

bool PhysicalReloadController::ShouldSuspendShotgunActiveReloadForFire() const
{
	const WeaponManualReloadSettings& settings = ReloadSettings();
	if (!settings.ContinuousReload
		|| !Game::instance.bShotgunShellSessionActive
		|| Game::instance.physicalReloadPhase != EPhysicalReloadPhase::PausedAtEject)
	{
		return false;
	}

	WeaponDynamicObject* weaponObject = GetLocalWeaponObject();
	if (!weaponObject || weaponObject->weaponData[0].ammo == 0)
	{
		return false;
	}

	return Game::instance.GetVR()->GetBoolInput(input.Fire);
}

void PhysicalReloadController::SuspendShotgunActiveReloadForFire()
{
	ResetCycleCore();

	WeaponDynamicObject* weaponObject = GetLocalWeaponObject();
	if (weaponObject)
	{
		weaponObject->weaponData[0].reloadState = 0;
	}

	Game::instance.bIsReloading = false;
	Helpers::ResumePhysicalReloadSounds(false);
}

void PhysicalReloadController::PrepareShotgunFireDuringReload()
{
	if (!ShouldSuspendShotgunActiveReloadForFire())
	{
		return;
	}

	SuspendShotgunActiveReloadForFire();
}

void PhysicalReloadController::TryBeginShotgunLoadFromBelt()
{
	const WeaponManualReloadSettings& settings = ReloadSettings();
	if (!settings.ContinuousReload
		|| !Game::instance.bShotgunShellSessionActive
		|| Game::instance.physicalReloadPhase != EPhysicalReloadPhase::Idle
		|| !ShouldContinueContinuousReloadSession())
	{
		return;
	}

	IVR* vr = Game::instance.GetVR();
	const ControllerRole offHand = Game::instance.bLeftHanded ? ControllerRole::Right : ControllerRole::Left;
	Matrix4 offHandTransform = vr->GetControllerTransform(offHand, true);
	Vector3 offHandPos = offHandTransform * Vector3(0.0f, 0.0f, 0.0f);
	offHandPos *= Game::instance.MetresToWorld(1.0f);
	offHandPos += Helpers::GetCamera().position;

	const Vector3 beltPos = WH().GetBeltMagazineWorldPosition();
	const float grabDistance = Game::instance.c_BeltMagazineGrabDistance->Value();
	const float grabDistanceSqr = grabDistance * grabDistance;
	const bool offHandNearBelt = (offHandPos - beltPos).lengthSqr() < grabDistanceSqr;
	bool gripChanged = false;
	const bool gripHeld = vr->GetBoolInput(input.TwoHandGrip, gripChanged);

	if (gripChanged && gripHeld && offHandNearBelt)
	{
		bBeltGripStartedReload = true;
		BeginChainedShellReload();
	}
}

void PhysicalReloadController::ApplyAnimPin()
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
		: static_cast<uint16_t>(WH().GetActiveReloadAnimIndex());
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

void PhysicalReloadController::UpdateReloadAnimationPause()
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
		ResetState();
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
		const uint16_t reloadElapsed = Game::instance.initialReloadRemaining > weaponObject->weaponData[0].reloadRemaining
			? Game::instance.initialReloadRemaining - weaponObject->weaponData[0].reloadRemaining
			: 0;
		Logger::log << "[PhysicalReload] weapon=" << static_cast<int>(WH().GetCachedWeaponType())
			<< " phase=" << static_cast<int>(currentPhase)
			<< " pauseTicks=" << ReloadSettings().PauseTicks
			<< " resumeTicks=" << ReloadSettings().ResumeTicks
			<< " reloadElapsed=" << reloadElapsed
			<< " fpAnim=" << fpAnimId
			<< " fpFrame=" << fpAnimFrame
			<< " reloadEmptyIdx=" << WH().GetActiveReloadAnimIndex()
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
			EndShotgunShellSession();
			return;
		}

		if (Game::instance.initialReloadRemaining == 0)
		{
			Game::instance.initialReloadRemaining = weaponObject->weaponData[0].reloadRemaining;
		}

		const int pauseTicks = ReloadSettings().PauseTicks;
		const int reloadAnimIndex = WH().GetActiveReloadAnimIndex();

		if (ShouldPausePhysicalReload(
			Game::instance.initialReloadRemaining,
			weaponObject->weaponData[0].reloadRemaining,
			pauseTicks))
		{
			Game::instance.pausedReloadAnimIndex = reloadAnimIndex >= 0
				? static_cast<uint16_t>(reloadAnimIndex)
				: fpAnimId;
			Game::instance.pausedReloadAnimFrame = static_cast<uint16_t>(pauseTicks);
			Game::instance.frozenReloadRemaining = weaponObject->weaponData[0].reloadRemaining;
			Game::instance.physicalReloadPhase = EPhysicalReloadPhase::PausedAtEject;
			Game::instance.bMagazineEjected = true;
			WH().ResetPhysicalReloadBonePinState();

			if (bBeltGripStartedReload)
			{
				bBeltGripStartedReload = false;
				IVR* pauseVr = Game::instance.GetVR();
				if (pauseVr->GetBoolInput(input.TwoHandGrip) && IsOffHandNearBeltMagazine())
				{
					Game::instance.bMagazineGrabbed = true;
					bSuppressSwapUntilGripRelease = true;
				}
			}

			if (Game::instance.c_LogPhysicalReloadDebug->Value())
			{
				const Vector3 beltPos = WH().GetBeltMagazineWorldPosition();
				Logger::log << "[PhysicalReload] paused at eject pauseTicks=" << pauseTicks
					<< " beltPos=("
					<< beltPos.x << "," << beltPos.y << "," << beltPos.z << ")"
					<< std::endl;
			}
			Helpers::PausePhysicalReloadSounds();
		}
	}
	else if (Game::instance.physicalReloadPhase == EPhysicalReloadPhase::PausedAtEject)
	{
		ApplyAnimPin();
		Helpers::PausePhysicalReloadSounds();
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

		const int safetyFrames = static_cast<int>(Game::instance.frozenReloadRemaining) * 6 + 60;
		const bool bTimerDone = weapon.reloadRemaining == 0 || weapon.reloadState == 0;
		const bool bFinishTimedOut = finishFrameCounter >= safetyFrames;

		const bool bHasReplay = WH().HasReloadReplay();
		const bool bVisualDone = !bHasReplay || WH().IsReloadReplayComplete();

		if (weapon.ammo == 0 && ((bTimerDone && bVisualDone) || bFinishTimedOut))
		{
			TriggerWeaponReloadEnd();
		}
		else if (!Game::instance.bIsReloading)
		{
			if (ShouldContinueContinuousReloadSession())
			{
				ResetCycle();
			}
			else
			{
				Game::instance.bShotgunShellSessionUserCancelled = false;
				EndShotgunShellSession();
			}
		}
	}
}

void PhysicalReloadController::BeginPhysicalReload()
{
	const WeaponManualReloadSettings& settings = ReloadSettings();
	if (settings.ContinuousReload)
	{
		Game::instance.bShotgunShellSessionActive = true;
	}

	BeginChainedShellReload();
}

void PhysicalReloadController::BeginChainedShellReload()
{
	if (Game::instance.physicalReloadPhase != EPhysicalReloadPhase::Idle)
	{
		return;
	}

	Game::instance.bPhysicalReloadFromEmpty = WH().IsLocalMagazineEmpty();
	Game::instance.bManualPhysicalReloadPending = true;
	Game::instance.physicalReloadPhase = EPhysicalReloadPhase::PlayingEject;
	Game::instance.initialReloadRemaining = 0;
	Helpers::BeginPhysicalReloadSoundCapture();
	TriggerWeaponReload();
	Game::instance.bManualPhysicalReloadPending = false;

	if (!Game::instance.bIsReloading)
	{
		EndShotgunShellSession();
		return;
	}

	WeaponDynamicObject* weaponObject = GetLocalWeaponObject();
	if (!weaponObject)
	{
		EndShotgunShellSession();
		return;
	}

	Game::instance.initialReloadRemaining = weaponObject->weaponData[0].reloadRemaining;
}

void PhysicalReloadController::ResumePhysicalReloadAnimation()
{
	WeaponDynamicObject* weaponObject = GetLocalWeaponObject();
	if (!weaponObject)
	{
		ResetState();
		return;
	}

	const ControllerRole offHand = Game::instance.bLeftHanded ? ControllerRole::Right : ControllerRole::Left;
	WeaponHapticsConfigManager& hapticsConfig = Game::instance.weaponHapticsConfig;
	hapticsConfig.LoadConfig();
	hapticsConfig.HandleWeaponHaptics(Game::instance.GetVR(), offHand, hapticsConfig.physicalReloadInsert);

	const int pauseTicks = ReloadSettings().PauseTicks;
	const int resumeTicks = ReloadSettings().ResumeTicks;
	const int skipTicks = resumeTicks > pauseTicks ? resumeTicks - pauseTicks : 0;
	const float skipSeconds = static_cast<float>(skipTicks) / 30.0f;
	WH().SetReloadReplaySkipSeconds(skipSeconds);

	const uint16_t realRemaining = Game::instance.frozenReloadRemaining;
	if (realRemaining > 0)
	{
		const uint16_t shortenedRemaining = realRemaining > static_cast<uint16_t>(skipTicks)
			? static_cast<uint16_t>(realRemaining - skipTicks)
			: 1;
		weaponObject->weaponData[0].reloadRemaining = shortenedRemaining;
		Game::instance.frozenReloadRemaining = shortenedRemaining;
	}

	Helpers::ClearPhysicalReloadSounds();

	Game::instance.physicalReloadPhase = EPhysicalReloadPhase::PlayingFinish;
	Game::instance.bMagazineGrabbed = false;
	Game::instance.bMagazineEjected = false;
	WH().ClearPhysicalReloadBoneSnapshot();

	if (Game::instance.c_LogPhysicalReloadDebug && Game::instance.c_LogPhysicalReloadDebug->Value())
	{
		Logger::log << "[PhysicalReload] resume skipTicks=" << skipTicks
			<< " replay=" << (WH().HasReloadReplay() ? "yes" : "no")
			<< std::endl;
	}
}

void PhysicalReloadController::HandlePhysicalMagazineGrabInsert()
{
	IVR* vr = Game::instance.GetVR();
	const ControllerRole offHand = Game::instance.bLeftHanded ? ControllerRole::Right : ControllerRole::Left;
	Matrix4 offHandTransform = vr->GetControllerTransform(offHand, true);
	Vector3 offHandPos = offHandTransform * Vector3(0.0f, 0.0f, 0.0f);
	offHandPos *= Game::instance.MetresToWorld(1.0f);
	offHandPos += Helpers::GetCamera().position;

	const Vector3 beltPos = WH().GetBeltMagazineWorldPosition();
	const Vector3 socketPos = WH().GetMagazineSocketWorldPosition();
	const float grabDistance = Game::instance.c_BeltMagazineGrabDistance->Value();
	const float insertDistance = Game::instance.c_BeltMagazineInsertDistance->Value();
	const float grabDistanceSqr = grabDistance * grabDistance;
	const float insertDistanceSqr = insertDistance * insertDistance;

	const bool offHandNearBelt = (offHandPos - beltPos).lengthSqr() < grabDistanceSqr;
	const bool offHandNearSocket = (offHandPos - socketPos).lengthSqr() < insertDistanceSqr;
	bool gripChanged = false;
	const bool gripHeld = vr->GetBoolInput(input.TwoHandGrip, gripChanged);

	if (!Game::instance.bMagazineGrabbed)
	{
		if (gripChanged && gripHeld && offHandNearBelt)
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

void PhysicalReloadController::Update()
{
	if (!Game::instance.c_DisableEmptyMagazineAutoReload->Value()
		|| Game::instance.bUse3DOFAiming
		|| !WH().HasMagazineBones())
	{
		if (Game::instance.physicalReloadPhase != EPhysicalReloadPhase::Idle)
		{
			ResetState();
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
		const bool reloadPressed = vr->GetBoolInput(input.Reload, bReloadChanged);

		if (reloadPressed && bReloadChanged)
		{
			const WeaponManualReloadSettings& settings = ReloadSettings();
			if (Game::instance.bShotgunShellSessionActive)
			{
				Game::instance.bShotgunShellSessionUserCancelled = true;
				EndShotgunShellSession();
			}
			else if (settings.ContinuousReload)
			{
				Game::instance.bShotgunShellSessionUserCancelled = false;
				BeginPhysicalReload();
			}
			else
			{
				BeginPhysicalReload();
			}
			break;
		}

		if (ShouldAutoStartContinuousReloadSession())
		{
			Game::instance.bShotgunShellSessionActive = true;
		}

		TryBeginShotgunLoadFromBelt();
		break;
	}
	case EPhysicalReloadPhase::PausedAtEject:
	{
		IVR* vr = Game::instance.GetVR();
		bool bReloadChanged = false;
		const bool reloadPressed = vr->GetBoolInput(input.Reload, bReloadChanged);

		if (reloadPressed && bReloadChanged && Game::instance.bShotgunShellSessionActive)
		{
			Game::instance.bShotgunShellSessionUserCancelled = true;
			EndShotgunShellSession();
			break;
		}

		HandlePhysicalMagazineGrabInsert();
		break;
	}
	default:
		break;
	}
}

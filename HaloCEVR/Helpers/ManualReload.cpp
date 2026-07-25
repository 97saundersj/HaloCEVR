#include "ManualReload.h"
#include "../InputHandler.h"
#include "../Game.h"
#include "Camera.h"
#include "SkeletonAnim.h"
#include "FirstPersonAnim.h"
#include "Objects.h"
#include "Assets.h"
#include "Maths.h"
#include "Sound.h"
#include "../Logger.h"
#include "../WeaponManualReloadConfig.h"
#include "../WeaponHapticsConfig.h"
#include "../Hooking/Hooks.h"
#include <algorithm>
#include <chrono>
#include <cstring>

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

const WeaponManualReloadSettings& ReloadSettings(WeaponType weaponType)
{
	return Game::instance.weaponManualReloadConfig.GetSettings(weaponType);
}

WeaponDynamicObject* GetLocalWeaponObject()
{
	BaseDynamicObject* player = Helpers::GetLocalPlayer();
	if (!player || player->weapon.id == 0xffff)
	{
		return nullptr;
	}

	return static_cast<WeaponDynamicObject*>(Helpers::GetDynamicObject(player->weapon));
}

bool ShouldPauseManualReload(uint16_t initialRemaining, uint16_t currentRemaining, int pauseTicks)
{
	if (pauseTicks <= 0 || initialRemaining <= currentRemaining)
	{
		return false;
	}

	return (initialRemaining - currentRemaining) >= static_cast<uint16_t>(pauseTicks);
}

double GetClockSeconds()
{
	using namespace std::chrono;
	return duration<double>(steady_clock::now().time_since_epoch()).count();
}

const char* WeaponTypeName(WeaponType type)
{
	switch (type)
	{
	case WeaponType::Pistol: return "Pistol";
	case WeaponType::AssaultRifle: return "AssaultRifle";
	case WeaponType::Shotgun: return "Shotgun";
	case WeaponType::RocketLauncher: return "RocketLauncher";
	case WeaponType::Sniper: return "Sniper";
	case WeaponType::Flamethrower: return "Flamethrower";
	case WeaponType::PlasmaPistol: return "PlasmaPistol";
	case WeaponType::PlasmaRifle: return "PlasmaRifle";
	case WeaponType::PlasmaCannon: return "PlasmaCannon";
	case WeaponType::Needler: return "Needler";
	case WeaponType::FuelRod: return "FuelRod";
	default: return "Unknown";
	}
}
}

ManualReloadController::ManualReloadController(InputHandler& inputHandler)
	: input(inputHandler)
{
	gripFromWristLocal.identity();
}

void ManualReloadController::BeginSoundCapture()
{
	const WeaponManualReloadSettings& settings = ReloadSettings(cachedWeaponType);
	Logger::log << "[ManualReload] sound capture begin"
		<< " weapon=" << WeaponTypeName(cachedWeaponType)
		<< " (" << static_cast<int>(cachedWeaponType) << ")"
		<< " pauseTicks=" << settings.PauseTicks
		<< " resumeTicks=" << settings.ResumeTicks
		<< std::endl;
	Helpers::BeginActiveSoundCapture();
}

void ManualReloadController::PauseSounds()
{
	Helpers::PauseActiveSounds(cachedWeaponType);
}

void ManualReloadController::ClearSounds(float skipSeconds)
{
	Logger::log << "[ManualReload] sound clear/resume"
		<< " weapon=" << WeaponTypeName(cachedWeaponType)
		<< " skipSeconds=" << skipSeconds
		<< std::endl;
	Helpers::ClearActiveSounds(skipSeconds);
}

void ManualReloadController::ResumeSounds(bool bStopActiveSources)
{
	Helpers::ResumeActiveSounds(bStopActiveSources);
}

void ManualReloadController::SuppressVanillaReloadControl(unsigned char& reloadControl) const
{
	if (!G().c_DisableEmptyMagazineAutoReload->Value())
	{
		return;
	}

	if (G().bUse3DOFAiming || !HasMagazineBones())
	{
		return;
	}

	reloadControl = 0;
}

bool ManualReloadController::ShouldBlockAutoReloadStart() const
{
	if (!G().c_DisableEmptyMagazineAutoReload->Value())
	{
		return false;
	}

	IVR* vr = G().GetVR();
	if (G().bUse3DOFAiming || !HasMagazineBones())
	{
		return !vr->GetBoolInput(input.GetReloadInput());
	}

	return !bManualReloadPending;
}

void ManualReloadController::ClearBoneSnapshot()
{
	boneReplay.ClearSnapshot();
}

void ManualReloadController::ResetBonePinState()
{
	boneReplay.Reset();
	lastBonePinPhase = 0;
	bHasCapturedGrip = false;
	gripFromWristLocal.identity();
}

void ManualReloadController::ClearReloadStartInsertSocket()
{
	bHasReloadStartMagSocket = false;
	reloadStartMagLocalOffset = Vector3(0.0f, 0.0f, 0.0f);
	magazineSocketPosition = Vector3(0.0f, 0.0f, 0.0f);
	bHasCapturedGrip = false;
	gripFromWristLocal.identity();
}

void ManualReloadController::ResetCycleCore()
{
	ResumeSounds(false);
	phase = EManualReloadPhase::Idle;
	bManualReloadPending = false;
	bManualReloadFromEmpty = false;
	bMagazineEjected = false;
	bMagazineGrabbed = false;
	pausedReloadAnimIndex = 0;
	pausedReloadAnimFrame = 0;
	frozenReloadRemaining = 0;
	initialReloadRemaining = 0;
	ResetBonePinState();
	if (!bShotgunShellSessionActive)
	{
		ClearReloadStartInsertSocket();
	}
}

void ManualReloadController::ResetState()
{
	bBeltGripStartedReload = false;
	bShotgunShellSessionActive = false;
	ResetCycleCore();
}

void ManualReloadController::ResetCycle()
{
	bBeltGripStartedReload = false;
	ResetCycleCore();
}

void ManualReloadController::EndShotgunShellSession()
{
	bShotgunShellSessionActive = false;
	ResetCycleCore();
}

void ManualReloadController::ClearReloadMetadata()
{
	cachedViewModelAsset = HaloID{ 0, 0 };
	cachedWeaponType = WeaponType::Unknown;
	bHasMagazineBones = false;
	magazineRootBoneIndex = -1;
	reloadEmptyAnimIndex = -1;
	reloadExitEmptyAnimIndex = -1;
	reloadFullAnimIndex = -1;
	reloadExitFullAnimIndex = -1;
	magazineCapacity = 0;
	memset(magazineHideBones, 0, sizeof(magazineHideBones));
}

void ManualReloadController::MarkMagazineBone(int boneIndex)
{
	if (boneIndex >= 0 && boneIndex < 64)
	{
		magazineHideBones[boneIndex] = true;
	}
}

void ManualReloadController::MarkMagazineDescendants(Bone* boneArray, int numBones, int rootIndex)
{
	if (!boneArray || rootIndex < 0 || rootIndex >= numBones)
	{
		return;
	}

	MarkMagazineBone(rootIndex);

	for (int i = 0; i < numBones && i < 64; i++)
	{
		if (i == rootIndex)
		{
			continue;
		}

		int parentIndex = boneArray[i].Parent;
		int guard = 0;
		while (parentIndex >= 0 && parentIndex < numBones && guard++ < numBones)
		{
			if (parentIndex == rootIndex)
			{
				MarkMagazineBone(i);
				break;
			}

			parentIndex = boneArray[parentIndex].Parent;
		}
	}
}

void ManualReloadController::CacheReloadMetadata(
	const HaloID& id,
	AssetData_ModelAnimations* animationData,
	WeaponType weaponType)
{
	if (cachedWeaponType != weaponType)
	{
		Helpers::ClearRememberedReloadSounds();
	}

	ClearReloadMetadata();
	cachedViewModelAsset = id;
	cachedWeaponType = weaponType;
	magazineCapacity = ReloadSettings(cachedWeaponType).MagazineCapacity;

	if (!animationData || !animationData->BoneArray || animationData->NumBones <= 0)
	{
		return;
	}

	Bone* boneArray = animationData->BoneArray;

	for (int i = 0; i < animationData->NumAnimations; i++)
	{
		const char* animName = animationData->AnimationArray[i].N00000429;
		if (!animName || !animName[0])
		{
			continue;
		}

		if (reloadEmptyAnimIndex < 0 && strstr(animName, "reload-empty"))
		{
			reloadEmptyAnimIndex = i;
		}

		if (reloadExitEmptyAnimIndex < 0
			&& (strstr(animName, "exit-empty") || strstr(animName, "exit empty")
				|| strstr(animName, "exit_empty") || strstr(animName, "reload-exit-empty")
				|| strstr(animName, "reload-exit")))
		{
			reloadExitEmptyAnimIndex = i;
		}

		if (reloadFullAnimIndex < 0
			&& (strstr(animName, "reload-full") || strstr(animName, "reload full")))
		{
			reloadFullAnimIndex = i;
		}

		if (reloadExitFullAnimIndex < 0
			&& (strstr(animName, "exit-full") || strstr(animName, "exit full")
				|| strstr(animName, "exit_full") || strstr(animName, "reload-exit-full")))
		{
			reloadExitFullAnimIndex = i;
		}
	}

	if (reloadEmptyAnimIndex >= 0 || reloadExitEmptyAnimIndex >= 0
		|| reloadFullAnimIndex >= 0 || reloadExitFullAnimIndex >= 0)
	{
		Logger::log << "[ManualReload] Reload anim indices: empty=" << reloadEmptyAnimIndex
			<< " exitEmpty=" << reloadExitEmptyAnimIndex
			<< " full=" << reloadFullAnimIndex
			<< " exitFull=" << reloadExitFullAnimIndex << std::endl;
	}

	for (int i = 0; i < animationData->NumBones && i < 64; i++)
	{
		if (!G().weaponManualReloadConfig.IsMagazineBoneName(weaponType, boneArray[i].BoneName))
		{
			continue;
		}

		magazineRootBoneIndex = i;
		break;
	}

	if (magazineRootBoneIndex < 0)
	{
		return;
	}

	bHasMagazineBones = true;
	MarkMagazineBone(magazineRootBoneIndex);

	const char* rootName = boneArray[magazineRootBoneIndex].BoneName;
	const WeaponManualReloadSettings& reloadSettings = ReloadSettings(cachedWeaponType);
	const bool bShellOnlyRoot = _stricmp(rootName, reloadSettings.MagazineBoneName.c_str()) == 0
		&& reloadSettings.ContinuousReload;

	if (!bShellOnlyRoot)
	{
		MarkMagazineDescendants(boneArray, animationData->NumBones, magazineRootBoneIndex);

		bool bMarkChanged = true;
		while (bMarkChanged)
		{
			bMarkChanged = false;
			for (int i = 0; i < animationData->NumBones && i < 64; i++)
			{
				if (magazineHideBones[i])
				{
					continue;
				}

				const int parentIndex = boneArray[i].Parent;
				if (parentIndex >= 0 && parentIndex < 64 && magazineHideBones[parentIndex])
				{
					MarkMagazineBone(i);
					bMarkChanged = true;
				}
			}
		}
	}

	int markedBoneCount = 0;
	for (int i = 0; i < 64; i++)
	{
		if (magazineHideBones[i])
		{
			markedBoneCount++;
		}
	}

	Logger::log << "[ManualReload] Magazine bone cached: index "
		<< magazineRootBoneIndex << " (\"" << rootName << "\")"
		<< " markedBones=" << markedBoneCount << std::endl;

	WeaponDynamicObject* weaponObject = GetLocalWeaponObject();
	if (weaponObject)
	{
		const Weapon& liveWeapon = weaponObject->weaponData[0];
		Logger::log << "[ManualReload] magazineCapacity=" << magazineCapacity
			<< " weaponType=" << static_cast<int>(cachedWeaponType)
			<< " ammo=" << liveWeapon.ammo
			<< " reserveAmmo=" << liveWeapon.reserveAmmo
			<< std::endl;
	}
}

void ManualReloadController::OnViewModelCached(
	const HaloID& id,
	AssetData_ModelAnimations* animationData,
	WeaponType weaponType)
{
	bShotgunShellSessionUserCancelled = false;
	ResetState();
	CacheReloadMetadata(id, animationData, weaponType);
}

bool ManualReloadController::IsMagazineBone(int boneIndex) const
{
	return boneIndex >= 0 && boneIndex < 64 && magazineHideBones[boneIndex];
}

int ManualReloadController::GetMagazineRootBoneIndex() const
{
	if (magazineRootBoneIndex >= 0)
	{
		return magazineRootBoneIndex;
	}

	for (int i = 0; i < 64; i++)
	{
		if (magazineHideBones[i])
		{
			return i;
		}
	}

	return -1;
}

bool ManualReloadController::IsLocalMagazineEmpty() const
{
	WeaponDynamicObject* weaponObject = GetLocalWeaponObject();
	return weaponObject && weaponObject->weaponData[0].ammo == 0;
}

bool ManualReloadController::IsShellByShellReloadWeapon() const
{
	return ReloadSettings(cachedWeaponType).ContinuousReload;
}

bool ManualReloadController::CanLoadAnotherShell() const
{
	if (!IsShellByShellReloadWeapon() || magazineCapacity == 0)
	{
		return false;
	}

	WeaponDynamicObject* weaponObject = GetLocalWeaponObject();
	if (!weaponObject)
	{
		return false;
	}

	const Weapon& weapon = weaponObject->weaponData[0];
	return weapon.reserveAmmo > 0 && weapon.ammo < magazineCapacity;
}

bool ManualReloadController::IsLocalViewModel(const HaloID& id) const
{
	return cachedViewModelAsset.id == id.id
		&& cachedViewModelAsset.index == id.index
		&& WH().GetRightWristIndex() >= 0;
}

bool ManualReloadController::ShouldContinueContinuousReloadSession() const
{
	const WeaponManualReloadSettings& settings = ReloadSettings(cachedWeaponType);
	return bShotgunShellSessionActive
		&& settings.ContinuousReload
		&& IsShellByShellReloadWeapon()
		&& CanLoadAnotherShell();
}

bool ManualReloadController::ShouldAutoStartContinuousReloadSession() const
{
	const WeaponManualReloadSettings& settings = ReloadSettings(cachedWeaponType);
	return settings.ContinuousReload
		&& !bShotgunShellSessionUserCancelled
		&& IsShellByShellReloadWeapon()
		&& CanLoadAnotherShell()
		&& phase == EManualReloadPhase::Idle
		&& !G().bIsReloading;
}

bool ManualReloadController::ShouldShowBeltMagazine() const
{
	if (!HasMagazineBones() || G().bUse3DOFAiming)
	{
		return false;
	}

	if (IsShellByShellReloadWeapon()
		&& bShotgunShellSessionActive
		&& phase == EManualReloadPhase::Idle
		&& CanLoadAnotherShell())
	{
		return true;
	}

	return bMagazineEjected;
}

int ManualReloadController::GetActiveReloadAnimIndex() const
{
	const WeaponManualReloadSettings& settings = ReloadSettings(cachedWeaponType);
	if (bManualReloadFromEmpty || settings.ContinuousReload)
	{
		return reloadEmptyAnimIndex >= 0 ? reloadEmptyAnimIndex : reloadExitEmptyAnimIndex;
	}

	return reloadFullAnimIndex >= 0 ? reloadFullAnimIndex : reloadExitFullAnimIndex;
}

void ManualReloadController::TriggerWeaponReload()
{
	BaseDynamicObject* player = Helpers::GetLocalPlayer();
	if (!player || player->weapon.id == 0xffff)
	{
		return;
	}

	Hooks::CallReloadStart(player->weapon, 0, true);
	G().ReloadStart(player->weapon, 0, true);
}

void ManualReloadController::TriggerWeaponReloadEnd()
{
	BaseDynamicObject* player = Helpers::GetLocalPlayer();
	if (!player || player->weapon.id == 0xffff)
	{
		return;
	}

	Hooks::CallReloadEnd(0, player->weapon);
}

void ManualReloadController::OnReloadEnd()
{
	const bool bPreserveChainedReload = ShouldContinueContinuousReloadSession()
		&& phase != EManualReloadPhase::Idle
		&& phase != EManualReloadPhase::PlayingFinish;

	if (phase == EManualReloadPhase::PlayingFinish && ShouldContinueContinuousReloadSession())
	{
		G().bIsReloading = false;
		return;
	}

	if (bPreserveChainedReload)
	{
		return;
	}

	G().bIsReloading = false;

	if (ShouldContinueContinuousReloadSession() && phase == EManualReloadPhase::Idle)
	{
		ResetCycle();
		return;
	}

	ResetState();
}

Vector3 ManualReloadController::GetBeltMagazineWorldPosition() const
{
	Vector3 beltPos = Helpers::GetCamera().position;
	beltPos.z -= G().c_BeltMagazineHipDrop->Value();

	Matrix4 headTransform = G().GetVR()->GetHMDTransform(true);
	Vector3 forward;
	Vector3 left;
	SkeletonAnim::GetLeveledBasisFromForward(
		SkeletonAnim::FlattenForwardOnXY(headTransform.getForwardAxis()),
		forward,
		left);

	const Vector3 offset = G().c_BeltMagazineOffset->Value();
	const float scale = G().MetresToWorld(1.0f);
	beltPos += forward * (offset.x * scale);
	beltPos += left * (offset.y * scale);
	return beltPos;
}

Vector3 ManualReloadController::GetOffHandWorldPosition() const
{
	const ControllerRole offHand = G().bLeftHanded ? ControllerRole::Right : ControllerRole::Left;
	Matrix4 offHandTransform = G().GetVR()->GetControllerTransform(offHand, true);
	Vector3 handPos = offHandTransform * Vector3(0.0f, 0.0f, 0.0f);
	handPos *= G().MetresToWorld(1.0f);
	handPos += Helpers::GetCamera().position;
	return handPos;
}

Vector3 ManualReloadController::GetMagazineGripWorldOffset() const
{
	const ControllerRole offHand = G().bLeftHanded ? ControllerRole::Right : ControllerRole::Left;
	Matrix4 offHandTransform = G().GetVR()->GetControllerTransform(offHand, true);
	Vector3 worldOffset = SkeletonAnim::GetRotationMatrix(offHandTransform)
		* G().c_MagazineGripControllerOffset->Value();
	worldOffset *= G().MetresToWorld(1.0f);
	return worldOffset;
}

bool ManualReloadController::GetOffHandNearBelt(bool& gripHeld, bool& gripChanged) const
{
	IVR* vr = G().GetVR();
	const Vector3 offHandPos = GetOffHandWorldPosition();
	const Vector3 beltPos = GetBeltMagazineWorldPosition();
	const float grabDistance = G().c_BeltMagazineGrabDistance->Value();
	const float grabDistanceSqr = grabDistance * grabDistance;

	gripChanged = false;
	gripHeld = vr->GetBoolInput(input.GetTwoHandGripInput(), gripChanged);
	return (offHandPos - beltPos).lengthSqr() < grabDistanceSqr;
}

bool ManualReloadController::ShouldSuppressTwoHandAim() const
{
	if (!G().c_DisableEmptyMagazineAutoReload->Value())
	{
		return false;
	}

	if (bMagazineEjected || bMagazineGrabbed || phase != EManualReloadPhase::Idle)
	{
		return true;
	}

	bool gripHeld = false;
	bool gripChanged = false;
	return ShouldShowBeltMagazine() && GetOffHandNearBelt(gripHeld, gripChanged);
}

bool ManualReloadController::ShouldSkipWeaponHandSwap() const
{
	if (bSuppressSwapUntilGripRelease)
	{
		return true;
	}

	if (!G().c_DisableEmptyMagazineAutoReload->Value())
	{
		return false;
	}

	return phase != EManualReloadPhase::Idle
		|| bMagazineEjected
		|| bMagazineGrabbed
		|| G().bIsReloading;
}

void ManualReloadController::TickSwapSuppression()
{
	if (!bSuppressSwapUntilGripRelease)
	{
		return;
	}

	IVR* vr = G().GetVR();
	if (!vr->GetBoolInput(input.GetSwapWeaponHandInput())
		&& !vr->GetBoolInput(input.GetOffhandSwapWeaponHandInput()))
	{
		bSuppressSwapUntilGripRelease = false;
	}
}

bool ManualReloadController::ShouldSuspendShotgunActiveReloadForFire() const
{
	const WeaponManualReloadSettings& settings = ReloadSettings(cachedWeaponType);
	if (!settings.ContinuousReload
		|| !bShotgunShellSessionActive
		|| phase != EManualReloadPhase::PausedAtEject)
	{
		return false;
	}

	WeaponDynamicObject* weaponObject = GetLocalWeaponObject();
	if (!weaponObject || weaponObject->weaponData[0].ammo == 0)
	{
		return false;
	}

	return G().GetVR()->GetBoolInput(input.GetFireInput());
}

void ManualReloadController::SuspendShotgunActiveReloadForFire()
{
	ResetCycleCore();

	WeaponDynamicObject* weaponObject = GetLocalWeaponObject();
	if (weaponObject)
	{
		weaponObject->weaponData[0].reloadState = 0;
	}

	G().bIsReloading = false;
	ResumeSounds(false);
}

void ManualReloadController::OnPreHandleInputs()
{
	if (!ShouldSuspendShotgunActiveReloadForFire())
	{
		return;
	}

	SuspendShotgunActiveReloadForFire();
}

void ManualReloadController::TryBeginShotgunLoadFromBelt()
{
	const WeaponManualReloadSettings& settings = ReloadSettings(cachedWeaponType);
	if (!settings.ContinuousReload
		|| !bShotgunShellSessionActive
		|| phase != EManualReloadPhase::Idle
		|| !ShouldContinueContinuousReloadSession())
	{
		return;
	}

	bool gripHeld = false;
	bool gripChanged = false;
	if (GetOffHandNearBelt(gripHeld, gripChanged) && gripChanged && gripHeld)
	{
		bBeltGripStartedReload = true;
		BeginChainedShellReload();
	}
}

void ManualReloadController::ApplyAnimPin()
{
	if (phase != EManualReloadPhase::PausedAtEject)
	{
		return;
	}

	WeaponDynamicObject* weaponObject = GetLocalWeaponObject();
	if (!weaponObject)
	{
		return;
	}

	Weapon& weapon = weaponObject->weaponData[0];

	if (frozenReloadRemaining > 0)
	{
		weapon.reloadRemaining = frozenReloadRemaining;
	}
	else
	{
		weapon.reloadRemaining = 9999;
		frozenReloadRemaining = 9999;
	}

	weapon.reloadState = 1;

	const uint16_t pinnedAnim = pausedReloadAnimIndex != 0
		? pausedReloadAnimIndex
		: static_cast<uint16_t>(GetActiveReloadAnimIndex());
	const uint16_t pinnedFrame = pausedReloadAnimFrame;

	SkeletonAnim::PinWeaponFirstPersonAnimation(weaponObject, pinnedAnim, pinnedFrame);
}

void ManualReloadController::UpdateReloadAnimationPause()
{
	if (phase != EManualReloadPhase::PlayingEject
		&& phase != EManualReloadPhase::PausedAtEject
		&& phase != EManualReloadPhase::PlayingFinish)
	{
		return;
	}

	WeaponDynamicObject* weaponObject = GetLocalWeaponObject();
	if (!weaponObject)
	{
		ResetState();
		return;
	}

	if (phase == EManualReloadPhase::PlayingEject)
	{
		if (!G().bIsReloading)
		{
			EndShotgunShellSession();
			return;
		}

		if (initialReloadRemaining == 0)
		{
			initialReloadRemaining = weaponObject->weaponData[0].reloadRemaining;
		}

		const int pauseTicks = ReloadSettings(cachedWeaponType).PauseTicks;
		const int reloadAnimIndex = GetActiveReloadAnimIndex();

		if (ShouldPauseManualReload(initialReloadRemaining, weaponObject->weaponData[0].reloadRemaining, pauseTicks))
		{
			pausedReloadAnimIndex = reloadAnimIndex >= 0
				? static_cast<uint16_t>(reloadAnimIndex)
				: Helpers::GetFirstPersonBaseAnimId();
			pausedReloadAnimFrame = static_cast<uint16_t>(pauseTicks);
			frozenReloadRemaining = weaponObject->weaponData[0].reloadRemaining;
			phase = EManualReloadPhase::PausedAtEject;
			bMagazineEjected = true;
			ResetBonePinState();

			if (bBeltGripStartedReload)
			{
				bBeltGripStartedReload = false;
				bool gripHeld = false;
				bool gripChanged = false;
				if (GetOffHandNearBelt(gripHeld, gripChanged) && gripHeld)
				{
					bMagazineGrabbed = true;
					bSuppressSwapUntilGripRelease = true;
				}
			}

			const Weapon& weapon = weaponObject->weaponData[0];
			Logger::log << "[ManualReload] magazine ejected"
				<< " weapon=" << WeaponTypeName(cachedWeaponType)
				<< " (" << static_cast<int>(cachedWeaponType) << ")"
				<< " pauseTicks=" << pauseTicks
				<< " reloadRemaining=" << frozenReloadRemaining
				<< " ammo=" << weapon.ammo
				<< " initialRemaining=" << initialReloadRemaining
				<< std::endl;
			PauseSounds();
		}
	}
	else if (phase == EManualReloadPhase::PausedAtEject)
	{
		ApplyAnimPin();
		PauseSounds();
	}
	else if (phase == EManualReloadPhase::PlayingFinish)
	{
		Weapon& weapon = weaponObject->weaponData[0];
		finishFrameCounter++;

		const int safetyFrames = static_cast<int>(frozenReloadRemaining) * 6 + 60;
		const bool bTimerDone = weapon.reloadRemaining == 0 || weapon.reloadState == 0;
		const bool bFinishTimedOut = finishFrameCounter >= safetyFrames;
		const bool bHasReplay = boneReplay.HasMultipleSamples();
		const bool bVisualDone = !bHasReplay || boneReplay.IsReplayComplete();

		if (weapon.ammo == 0 && ((bTimerDone && bVisualDone) || bFinishTimedOut))
		{
			TriggerWeaponReloadEnd();
		}
		else if (!G().bIsReloading)
		{
			if (ShouldContinueContinuousReloadSession())
			{
				ResetCycle();
			}
			else
			{
				bShotgunShellSessionUserCancelled = false;
				EndShotgunShellSession();
			}
		}
	}
}

void ManualReloadController::BeginManualReload()
{
	const WeaponManualReloadSettings& settings = ReloadSettings(cachedWeaponType);
	if (settings.ContinuousReload)
	{
		bShotgunShellSessionActive = true;
	}

	BeginChainedShellReload();
}

void ManualReloadController::BeginChainedShellReload()
{
	if (phase != EManualReloadPhase::Idle)
	{
		return;
	}

	bManualReloadFromEmpty = IsLocalMagazineEmpty();
	bManualReloadPending = true;
	phase = EManualReloadPhase::PlayingEject;
	initialReloadRemaining = 0;
	BeginSoundCapture();
	TriggerWeaponReload();
	bManualReloadPending = false;

	if (!G().bIsReloading)
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

	initialReloadRemaining = weaponObject->weaponData[0].reloadRemaining;
}

void ManualReloadController::ResumeManualReloadAnimation()
{
	WeaponDynamicObject* weaponObject = GetLocalWeaponObject();
	if (!weaponObject)
	{
		ResetState();
		return;
	}

	const ControllerRole offHand = G().bLeftHanded ? ControllerRole::Right : ControllerRole::Left;
	WeaponHapticsConfigManager& hapticsConfig = G().weaponHapticsConfig;
	hapticsConfig.LoadConfig();
	hapticsConfig.HandleWeaponHaptics(G().GetVR(), offHand, hapticsConfig.manualReloadInsert);

	const int pauseTicks = ReloadSettings(cachedWeaponType).PauseTicks;
	const int resumeTicks = ReloadSettings(cachedWeaponType).ResumeTicks;
	const int skipTicks = resumeTicks > pauseTicks ? resumeTicks - pauseTicks : 0;
	replaySkipSeconds = std::max(0.0f, static_cast<float>(skipTicks) / 30.0f);

	const uint16_t realRemaining = frozenReloadRemaining;
	if (realRemaining > 0)
	{
		const uint16_t shortenedRemaining = realRemaining > static_cast<uint16_t>(skipTicks)
			? static_cast<uint16_t>(realRemaining - skipTicks)
			: 1;
		weaponObject->weaponData[0].reloadRemaining = shortenedRemaining;
		frozenReloadRemaining = shortenedRemaining;
	}

	ClearSounds(replaySkipSeconds);

	phase = EManualReloadPhase::PlayingFinish;
	finishFrameCounter = 0;
	bMagazineGrabbed = false;
	bMagazineEjected = false;
	ClearBoneSnapshot();

	Logger::log << "[ManualReload] magazine inserted"
		<< " weapon=" << WeaponTypeName(cachedWeaponType)
		<< " (" << static_cast<int>(cachedWeaponType) << ")"
		<< " skipTicks=" << skipTicks
		<< " skipSeconds=" << replaySkipSeconds
		<< " pauseTicks=" << pauseTicks
		<< " resumeTicks=" << resumeTicks
		<< " ammo=" << weaponObject->weaponData[0].ammo
		<< std::endl;
}

void ManualReloadController::HandleManualMagazineGrabInsert()
{
	const Vector3 offHandPos = GetOffHandWorldPosition();
	const Vector3 beltPos = GetBeltMagazineWorldPosition();
	const Vector3 socketPos = magazineSocketPosition;
	const float grabDistance = G().c_BeltMagazineGrabDistance->Value();
	const float insertDistance = G().c_BeltMagazineInsertDistance->Value();
	const float grabDistanceSqr = grabDistance * grabDistance;
	const float insertDistanceSqr = insertDistance * insertDistance;

	const bool offHandNearBelt = (offHandPos - beltPos).lengthSqr() < grabDistanceSqr;
	const bool offHandNearSocket = (offHandPos - socketPos).lengthSqr() < insertDistanceSqr;

	bool gripChanged = false;
	const bool gripHeld = G().GetVR()->GetBoolInput(input.GetTwoHandGripInput(), gripChanged);

	if (!bMagazineGrabbed)
	{
		if (gripChanged && gripHeld && offHandNearBelt)
		{
			bMagazineGrabbed = true;
			bSuppressSwapUntilGripRelease = true;
		}
	}
	else if (offHandNearSocket)
	{
		ResumeManualReloadAnimation();
	}
	else if (!gripHeld)
	{
		bMagazineGrabbed = false;
	}
}

void ManualReloadController::Update()
{
	if (!G().c_DisableEmptyMagazineAutoReload->Value()
		|| G().bUse3DOFAiming
		|| !HasMagazineBones())
	{
		if (phase != EManualReloadPhase::Idle)
		{
			ResetState();
		}
		else
		{
			bMagazineEjected = false;
			bMagazineGrabbed = false;
		}
		return;
	}

	UpdateReloadAnimationPause();

	switch (phase)
	{
	case EManualReloadPhase::Idle:
	{
		IVR* vr = G().GetVR();
		bool bReloadChanged = false;
		const bool reloadPressed = vr->GetBoolInput(input.GetReloadInput(), bReloadChanged);

		if (reloadPressed && bReloadChanged)
		{
			if (bShotgunShellSessionActive)
			{
				bShotgunShellSessionUserCancelled = true;
				EndShotgunShellSession();
			}
			else
			{
				bShotgunShellSessionUserCancelled = false;
				BeginManualReload();
			}
			break;
		}

		if (ShouldAutoStartContinuousReloadSession())
		{
			bShotgunShellSessionActive = true;
		}

		TryBeginShotgunLoadFromBelt();
		break;
	}
	case EManualReloadPhase::PausedAtEject:
	{
		IVR* vr = G().GetVR();
		bool bReloadChanged = false;
		const bool reloadPressed = vr->GetBoolInput(input.GetReloadInput(), bReloadChanged);

		if (reloadPressed && bReloadChanged && bShotgunShellSessionActive)
		{
			bShotgunShellSessionUserCancelled = true;
			EndShotgunShellSession();
			break;
		}

		HandleManualMagazineGrabInsert();
		break;
	}
	default:
		break;
	}
}

//===============================// View-model visuals //===============================//

void ManualReloadController::CaptureReloadStartInsertSocket(const Transform* outBoneTransforms)
{
	if (bHasReloadStartMagSocket)
	{
		return;
	}

	const int gunIndex = WH().GetGunIndex();
	const int magIndex = GetMagazineRootBoneIndex();
	if (gunIndex < 0 || magIndex < 0)
	{
		return;
	}

	reloadStartMagLocalOffset = SkeletonAnim::CapturePointInBoneSpace(
		outBoneTransforms[gunIndex],
		outBoneTransforms[magIndex].translation);
	bHasReloadStartMagSocket = true;
}

void ManualReloadController::UpdateInsertSocketFromGun(const Transform* outBoneTransforms)
{
	if (!bHasReloadStartMagSocket)
	{
		return;
	}

	const int gunIndex = WH().GetGunIndex();
	if (gunIndex < 0)
	{
		return;
	}

	magazineSocketPosition = SkeletonAnim::ApplyPointInBoneSpace(
		outBoneTransforms[gunIndex],
		reloadStartMagLocalOffset);
}

void ManualReloadController::CaptureGripFromResumePose(HaloID& id, Vector3* pos, Vector3* facing, Vector3* up)
{
	if (bHasCapturedGrip || boneReplay.GetSampleCount() <= 0)
	{
		return;
	}

	const int wristIndex = WH().GetLeftWristIndex();
	const int magIndex = GetMagazineRootBoneIndex();
	if (wristIndex < 0 || magIndex < 0)
	{
		return;
	}

	const int pauseTicks = ReloadSettings(cachedWeaponType).PauseTicks;
	const int resumeTicks = ReloadSettings(cachedWeaponType).ResumeTicks;
	const int skipTicks = resumeTicks > pauseTicks ? resumeTicks - pauseTicks : 0;
	const float skipSeconds = static_cast<float>(skipTicks) / 30.0f;

	const int replayIndex = boneReplay.FindSampleIndexAtOrAfter(skipSeconds);
	if (replayIndex < 0)
	{
		return;
	}

	const TransformQuat* sampleQuats = boneReplay.GetSampleQuats(replayIndex);
	if (!sampleQuats)
	{
		return;
	}

	TransformQuat resumeQuats[SkeletonAnim::kMaxBones]{};
	memcpy(resumeQuats, sampleQuats, sizeof(resumeQuats));

	Transform animPoseTransforms[SkeletonAnim::kMaxBones]{};
	SkeletonAnim::EvaluatePose(id, pos, facing, up, resumeQuats, animPoseTransforms);

	gripFromWristLocal = SkeletonAnim::CaptureChildInParentSpace(
		animPoseTransforms[wristIndex],
		animPoseTransforms[magIndex]);
	bHasCapturedGrip = true;
}

bool ManualReloadController::GetGrabbedMagazineTargetMatrix(const Transform* outBoneTransforms, Matrix4& outTargetMatrix) const
{
	const int wristIndex = WH().GetLeftWristIndex();
	if (!bHasCapturedGrip || wristIndex < 0 || !outBoneTransforms)
	{
		return false;
	}

	outTargetMatrix = SkeletonAnim::ApplyChildInParentSpace(
		outBoneTransforms[wristIndex],
		gripFromWristLocal);
	return true;
}

Matrix4 ManualReloadController::GetDetachedMagazineOrientation(const Transform* outBoneTransforms) const
{
	if (bMagazineGrabbed)
	{
		Matrix4 targetMatrix;
		if (GetGrabbedMagazineTargetMatrix(outBoneTransforms, targetMatrix))
		{
			return targetMatrix;
		}

		const ControllerRole offHand = G().bLeftHanded ? ControllerRole::Right : ControllerRole::Left;
		return SkeletonAnim::GetRotationMatrix(G().GetVR()->GetControllerTransform(offHand, true));
	}

	return SkeletonAnim::MakeBeltObjectOrientation(
		SkeletonAnim::FlattenForwardOnXY(G().GetVR()->GetHMDTransform(true).getForwardAxis()));
}

void ManualReloadController::UpdateMagazinePlacement(const HaloID& id, Transform* outBoneTransforms)
{
	if (!IsLocalViewModel(id))
	{
		return;
	}

	if (!ShouldShowBeltMagazine())
	{
		return;
	}

	Vector3 targetPos = GetBeltMagazineWorldPosition();
	if (bMagazineGrabbed)
	{
		Matrix4 targetMatrix;
		if (GetGrabbedMagazineTargetMatrix(outBoneTransforms, targetMatrix))
		{
			targetPos = targetMatrix * Vector3(0.0f, 0.0f, 0.0f);
		}
		else
		{
			targetPos = GetOffHandWorldPosition() + GetMagazineGripWorldOffset();
		}
	}

	const int rootIndex = GetMagazineRootBoneIndex();
	if (rootIndex < 0)
	{
		return;
	}

	SkeletonAnim::RelocateBoneSubtree(
		outBoneTransforms,
		magazineHideBones,
		rootIndex,
		targetPos,
		GetDetachedMagazineOrientation(outBoneTransforms));
}

void ManualReloadController::ApplyBonePin(const HaloID& id, TransformQuat* boneTransforms)
{
	if (!IsLocalViewModel(id))
	{
		return;
	}

	const int phaseInt = static_cast<int>(phase);
	const int kPausedAtEject = static_cast<int>(EManualReloadPhase::PausedAtEject);
	const int kPlayingFinish = static_cast<int>(EManualReloadPhase::PlayingFinish);
	const double now = GetClockSeconds();

	if (phaseInt == kPausedAtEject)
	{
		if (!boneReplay.HasSnapshot())
		{
			const float remainingTicks = static_cast<float>(frozenReloadRemaining);
			const float recordTargetSeconds = std::min(4.0f, std::max(0.5f, remainingTicks / 30.0f + 0.15f));
			boneReplay.BeginCapture(boneTransforms, now, recordTargetSeconds);
		}
		else
		{
			boneReplay.TickCapture(boneTransforms, now);
		}
	}
	else if (phaseInt == kPlayingFinish)
	{
		if (lastBonePinPhase != kPlayingFinish)
		{
			boneReplay.BeginReplay(now, replaySkipSeconds);
		}

		boneReplay.ApplyReplay(boneTransforms, now);
	}

	lastBonePinPhase = phaseInt;
}

void ManualReloadController::PreSkeleton(const HaloID& id, TransformQuat* boneTransforms)
{
	ApplyBonePin(id, boneTransforms);
}

void ManualReloadController::PostSkeleton(HaloID& id, Vector3* pos, Vector3* facing, Vector3* up, Transform* outBoneTransforms)
{
	if (phase == EManualReloadPhase::PlayingEject)
	{
		CaptureReloadStartInsertSocket(outBoneTransforms);
	}
	else if (phase == EManualReloadPhase::PausedAtEject)
	{
		UpdateInsertSocketFromGun(outBoneTransforms);
		CaptureGripFromResumePose(id, pos, facing, up);
	}

	UpdateMagazinePlacement(id, outBoneTransforms);
}

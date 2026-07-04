#include "PhysicalReload.h"
#include "../InputHandler.h"
#include "../Game.h"
#include "Camera.h"
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

const WeaponManualReloadSettings& ReloadSettings()
{
	return Game::instance.weaponManualReloadConfig.GetSettings(WH().GetCachedWeaponType());
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

bool ShouldPausePhysicalReload(uint16_t initialRemaining, uint16_t currentRemaining, int pauseTicks)
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

void TransformToMatrix4(const Transform& inTransform, Matrix4& outMatrix)
{
	for (int x = 0; x < 3; x++)
	{
		for (int y = 0; y < 3; y++)
		{
			const_cast<float*>(outMatrix.get())[x + y * 4] = inTransform.rotation[x + y * 3];
		}
	}
	outMatrix.setColumn(3, inTransform.translation);
}

void ApplyMatrixToTransform(const Matrix4& matrix, Transform& outTransform)
{
	outTransform.translation = matrix * Vector3(0.0f, 0.0f, 0.0f);
	for (int x = 0; x < 3; x++)
	{
		for (int y = 0; y < 3; y++)
		{
			outTransform.rotation[x + y * 3] = matrix.get()[x + y * 4];
		}
	}
}

// Pure animation-space skeleton evaluation (no VR overrides).
void EvaluateAnimPose(
	HaloID& id,
	Vector3* pos,
	Vector3* facing,
	Vector3* up,
	TransformQuat* boneTransforms,
	Transform* outBoneTransforms)
{
	Asset_ModelAnimations* viewModel = Helpers::GetTypedAsset<Asset_ModelAnimations>(id);
	if (!viewModel || !viewModel->Data)
	{
		return;
	}

	AssetData_ModelAnimations* animationData = viewModel->Data;
	Bone* boneArray = animationData->BoneArray;

	Transform root;
	Helpers::MakeTransformFromXZ(up, facing, &root);
	root.translation = *pos;

	if (animationData->NumBones <= 0)
	{
		return;
	}

	int i = 0;
	int lastIndex = 1;
	int16_t bonesToProcess[64]{};
	bonesToProcess[0] = 0;

	do
	{
		const int16_t boneIdx = bonesToProcess[i];
		i++;
		const Bone& currentBone = boneArray[boneIdx];
		const Transform* parentTransform = boneIdx == 0 ? &root : &outBoneTransforms[currentBone.Parent];
		const TransformQuat* currentQuat = &boneTransforms[boneIdx];
		Transform tempTransform;
		Helpers::MakeTransformFromQuat(&currentQuat->rotation, &tempTransform);
		tempTransform.scale = currentQuat->scale;
		tempTransform.translation = currentQuat->translation;
		Helpers::CombineTransforms(parentTransform, &tempTransform, &outBoneTransforms[boneIdx]);

		if (currentBone.LeftLeaf != -1)
		{
			bonesToProcess[lastIndex++] = currentBone.LeftLeaf;
		}
		if (currentBone.RightLeaf != -1)
		{
			bonesToProcess[lastIndex++] = currentBone.RightLeaf;
		}
	} while (i != lastIndex);
}

bool IsDebugLogging()
{
	return G().c_LogPhysicalReloadDebug && G().c_LogPhysicalReloadDebug->Value();
}
}

PhysicalReloadController::PhysicalReloadController(InputHandler& inputHandler)
	: input(inputHandler)
{
	gripFromWristLocal.identity();
}

void PhysicalReloadController::BeginSoundCapture()
{
	Helpers::BeginActiveSoundCapture();
}

void PhysicalReloadController::PauseSounds()
{
	float stopDelaySeconds = G().c_PhysicalReloadSoundStopDelay
		? G().c_PhysicalReloadSoundStopDelay->Value()
		: 0.0f;
	if (stopDelaySeconds < 0.0f)
	{
		stopDelaySeconds = 0.0f;
	}
	Helpers::PauseActiveSounds(static_cast<unsigned int>(stopDelaySeconds * 1000.0f), IsDebugLogging());
}

void PhysicalReloadController::ClearSounds()
{
	Helpers::ClearActiveSounds(IsDebugLogging());
}

void PhysicalReloadController::ResumeSounds(bool bStopActiveSources)
{
	Helpers::ResumeActiveSounds(bStopActiveSources, IsDebugLogging());
}

void PhysicalReloadController::SuppressVanillaReloadControl(unsigned char& reloadControl) const
{
	if (!G().c_DisableEmptyMagazineAutoReload->Value())
	{
		return;
	}

	if (G().bUse3DOFAiming || !WH().HasMagazineBones())
	{
		return;
	}

	reloadControl = 0;
}

bool PhysicalReloadController::ShouldBlockAutoReloadStart() const
{
	if (!G().c_DisableEmptyMagazineAutoReload->Value())
	{
		return false;
	}

	IVR* vr = G().GetVR();
	if (G().bUse3DOFAiming || !WH().HasMagazineBones())
	{
		return !vr->GetBoolInput(input.GetReloadInput());
	}

	return !bManualPhysicalReloadPending;
}

void PhysicalReloadController::ClearBoneSnapshot()
{
	bHasPausedBoneSnapshot = false;
}

void PhysicalReloadController::ResetBonePinState()
{
	ClearBoneSnapshot();
	reloadReplayCount = 0;
	reloadRecordComplete = false;
	reloadRecordTargetSeconds = 0.0f;
	reloadPauseStartSeconds = -1.0;
	reloadReplayStartSeconds = -1.0;
	reloadReplaySkipSeconds = 0.0f;
	bReloadReplaying = false;
	bReloadReplayComplete = false;
	lastBonePinPhase = 0;
	bHasCapturedGrip = false;
	gripFromWristLocal.identity();
}

void PhysicalReloadController::ClearReloadStartInsertSocket()
{
	bHasReloadStartMagSocket = false;
	reloadStartMagLocalOffset = Vector3(0.0f, 0.0f, 0.0f);
	magazineSocketPosition = Vector3(0.0f, 0.0f, 0.0f);
	bHasCapturedGrip = false;
	gripFromWristLocal.identity();
}

void PhysicalReloadController::ResetCycleCore()
{
	ResumeSounds(false);
	phase = EPhysicalReloadPhase::Idle;
	bManualPhysicalReloadPending = false;
	bPhysicalReloadFromEmpty = false;
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

void PhysicalReloadController::ResetState()
{
	bBeltGripStartedReload = false;
	bShotgunShellSessionActive = false;
	ResetCycleCore();
}

void PhysicalReloadController::ResetCycle()
{
	bBeltGripStartedReload = false;
	ResetCycleCore();
}

void PhysicalReloadController::EndShotgunShellSession()
{
	bShotgunShellSessionActive = false;
	ResetCycleCore();
}

void PhysicalReloadController::OnWeaponChanged()
{
	bShotgunShellSessionUserCancelled = false;
	ResetState();
}

bool PhysicalReloadController::ShouldContinueContinuousReloadSession() const
{
	const WeaponManualReloadSettings& settings = ReloadSettings();
	return bShotgunShellSessionActive
		&& settings.ContinuousReload
		&& WH().IsShellByShellReloadWeapon()
		&& WH().CanLoadAnotherShell();
}

bool PhysicalReloadController::ShouldAutoStartContinuousReloadSession() const
{
	const WeaponManualReloadSettings& settings = ReloadSettings();
	return settings.ContinuousReload
		&& !bShotgunShellSessionUserCancelled
		&& WH().IsShellByShellReloadWeapon()
		&& WH().CanLoadAnotherShell()
		&& phase == EPhysicalReloadPhase::Idle
		&& !G().bIsReloading;
}

bool PhysicalReloadController::ShouldShowBeltMagazine() const
{
	if (!WH().HasMagazineBones() || G().bUse3DOFAiming)
	{
		return false;
	}

	if (WH().IsShellByShellReloadWeapon()
		&& bShotgunShellSessionActive
		&& phase == EPhysicalReloadPhase::Idle
		&& WH().CanLoadAnotherShell())
	{
		return true;
	}

	return bMagazineEjected;
}

int PhysicalReloadController::GetActiveReloadAnimIndex() const
{
	const WeaponManualReloadSettings& settings = ReloadSettings();
	if (bPhysicalReloadFromEmpty || settings.ContinuousReload)
	{
		const int primary = WH().GetReloadEmptyAnimIndex();
		return primary >= 0 ? primary : WH().GetReloadExitEmptyAnimIndex();
	}

	const int primary = WH().GetReloadFullAnimIndex();
	return primary >= 0 ? primary : WH().GetReloadExitFullAnimIndex();
}

void PhysicalReloadController::TriggerWeaponReload()
{
	BaseDynamicObject* player = Helpers::GetLocalPlayer();
	if (!player || player->weapon.id == 0xffff)
	{
		return;
	}

	Hooks::CallReloadStart(player->weapon, 0, true);
	G().ReloadStart(player->weapon, 0, true);
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

void PhysicalReloadController::OnReloadEnd()
{
	const bool bPreserveChainedReload = ShouldContinueContinuousReloadSession()
		&& phase != EPhysicalReloadPhase::Idle
		&& phase != EPhysicalReloadPhase::PlayingFinish;

	if (phase == EPhysicalReloadPhase::PlayingFinish && ShouldContinueContinuousReloadSession())
	{
		G().bIsReloading = false;
		return;
	}

	if (bPreserveChainedReload)
	{
		return;
	}

	G().bIsReloading = false;

	if (ShouldContinueContinuousReloadSession() && phase == EPhysicalReloadPhase::Idle)
	{
		ResetCycle();
		return;
	}

	ResetState();
}

Vector3 PhysicalReloadController::GetBeltMagazineWorldPosition() const
{
	Vector3 beltPos = Helpers::GetCamera().position;
	beltPos.z -= G().c_BeltMagazineHipDrop->Value();

	Matrix4 headTransform = G().GetVR()->GetHMDTransform(true);
	Vector3 forward = headTransform.getForwardAxis();
	forward.z = 0.0f;
	if (forward.lengthSqr() < 0.0001f)
	{
		forward = Vector3(1.0f, 0.0f, 0.0f);
	}
	else
	{
		forward.normalize();
	}

	const Vector3 worldUp(0.0f, 0.0f, 1.0f);
	Vector3 left = worldUp.cross(forward);
	left.normalize();

	const Vector3 offset = G().c_BeltMagazineOffset->Value();
	const float scale = G().MetresToWorld(1.0f);
	beltPos += forward * (offset.x * scale);
	beltPos += left * (offset.y * scale);
	return beltPos;
}

Vector3 PhysicalReloadController::GetOffHandWorldPosition() const
{
	const ControllerRole offHand = G().bLeftHanded ? ControllerRole::Right : ControllerRole::Left;
	Matrix4 offHandTransform = G().GetVR()->GetControllerTransform(offHand, true);
	Vector3 handPos = offHandTransform * Vector3(0.0f, 0.0f, 0.0f);
	handPos *= G().MetresToWorld(1.0f);
	handPos += Helpers::GetCamera().position;
	return handPos;
}

Vector3 PhysicalReloadController::GetMagazineGripWorldOffset() const
{
	const ControllerRole offHand = G().bLeftHanded ? ControllerRole::Right : ControllerRole::Left;
	Matrix4 offHandTransform = G().GetVR()->GetControllerTransform(offHand, true);
	const Vector3 controllerOrigin = offHandTransform * Vector3(0.0f, 0.0f, 0.0f);

	Matrix4 handRotation = offHandTransform;
	handRotation.translate(-controllerOrigin);

	Vector3 worldOffset = handRotation * G().c_MagazineGripControllerOffset->Value();
	worldOffset *= G().MetresToWorld(1.0f);
	return worldOffset;
}

bool PhysicalReloadController::GetOffHandNearBelt(bool& gripHeld, bool& gripChanged) const
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

bool PhysicalReloadController::ShouldSuppressTwoHandAim() const
{
	if (!G().c_DisableEmptyMagazineAutoReload->Value())
	{
		return false;
	}

	if (bMagazineEjected || bMagazineGrabbed || phase != EPhysicalReloadPhase::Idle)
	{
		return true;
	}

	bool gripHeld = false;
	bool gripChanged = false;
	return ShouldShowBeltMagazine() && GetOffHandNearBelt(gripHeld, gripChanged);
}

bool PhysicalReloadController::ShouldSkipWeaponHandSwap() const
{
	if (bSuppressSwapUntilGripRelease)
	{
		return true;
	}

	if (!G().c_DisableEmptyMagazineAutoReload->Value())
	{
		return false;
	}

	return phase != EPhysicalReloadPhase::Idle
		|| bMagazineEjected
		|| bMagazineGrabbed
		|| G().bIsReloading;
}

void PhysicalReloadController::TickSwapSuppression()
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

bool PhysicalReloadController::ShouldSuspendShotgunActiveReloadForFire() const
{
	const WeaponManualReloadSettings& settings = ReloadSettings();
	if (!settings.ContinuousReload
		|| !bShotgunShellSessionActive
		|| phase != EPhysicalReloadPhase::PausedAtEject)
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

void PhysicalReloadController::SuspendShotgunActiveReloadForFire()
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

void PhysicalReloadController::OnPreHandleInputs()
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
		|| !bShotgunShellSessionActive
		|| phase != EPhysicalReloadPhase::Idle
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

void PhysicalReloadController::ApplyAnimPin()
{
	if (phase != EPhysicalReloadPhase::PausedAtEject)
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
	if (phase != EPhysicalReloadPhase::PlayingEject
		&& phase != EPhysicalReloadPhase::PausedAtEject
		&& phase != EPhysicalReloadPhase::PlayingFinish)
	{
		return;
	}

	WeaponDynamicObject* weaponObject = GetLocalWeaponObject();
	if (!weaponObject)
	{
		ResetState();
		return;
	}

	if (G().c_LogPhysicalReloadFrames && G().c_LogPhysicalReloadFrames->Value()
		&& (G().bIsReloading || phase != EPhysicalReloadPhase::Idle))
	{
		static EPhysicalReloadPhase lastLoggedPhase = EPhysicalReloadPhase::Idle;
		static int logFrameCounter = 0;
		const bool phaseChanged = phase != lastLoggedPhase;
		if (phaseChanged)
		{
			logFrameCounter = 0;
			lastLoggedPhase = phase;
		}
		else
		{
			logFrameCounter++;
		}

		if (phaseChanged
			|| phase == EPhysicalReloadPhase::PlayingEject
			|| (phase == EPhysicalReloadPhase::PausedAtEject && logFrameCounter % 30 == 0)
			|| phase == EPhysicalReloadPhase::PlayingFinish)
		{
			const uint16_t reloadElapsed = initialReloadRemaining > weaponObject->weaponData[0].reloadRemaining
				? initialReloadRemaining - weaponObject->weaponData[0].reloadRemaining
				: 0;
			Logger::log << "[PhysicalReload] weapon=" << static_cast<int>(WH().GetCachedWeaponType())
				<< " phase=" << static_cast<int>(phase)
				<< " pauseTicks=" << ReloadSettings().PauseTicks
				<< " resumeTicks=" << ReloadSettings().ResumeTicks
				<< " reloadElapsed=" << reloadElapsed
				<< " fpAnim=" << Helpers::GetFirstPersonBaseAnimId()
				<< " fpFrame=" << Helpers::GetFirstPersonBaseAnimFrame()
				<< " reloadEmptyIdx=" << GetActiveReloadAnimIndex()
				<< " reloadState=" << weaponObject->weaponData[0].reloadState
				<< " reloadRemaining=" << weaponObject->weaponData[0].reloadRemaining
				<< " initialRemaining=" << initialReloadRemaining
				<< " ammo=" << weaponObject->weaponData[0].ammo
				<< std::endl;
		}
	}

	if (phase == EPhysicalReloadPhase::PlayingEject)
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

		const int pauseTicks = ReloadSettings().PauseTicks;
		const int reloadAnimIndex = GetActiveReloadAnimIndex();

		if (ShouldPausePhysicalReload(initialReloadRemaining, weaponObject->weaponData[0].reloadRemaining, pauseTicks))
		{
			pausedReloadAnimIndex = reloadAnimIndex >= 0
				? static_cast<uint16_t>(reloadAnimIndex)
				: Helpers::GetFirstPersonBaseAnimId();
			pausedReloadAnimFrame = static_cast<uint16_t>(pauseTicks);
			frozenReloadRemaining = weaponObject->weaponData[0].reloadRemaining;
			phase = EPhysicalReloadPhase::PausedAtEject;
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

			if (IsDebugLogging())
			{
				const Vector3 beltPos = GetBeltMagazineWorldPosition();
				Logger::log << "[PhysicalReload] paused at eject pauseTicks=" << pauseTicks
					<< " beltPos=(" << beltPos.x << "," << beltPos.y << "," << beltPos.z << ")"
					<< std::endl;
			}
			PauseSounds();
		}
	}
	else if (phase == EPhysicalReloadPhase::PausedAtEject)
	{
		ApplyAnimPin();
		PauseSounds();
	}
	else if (phase == EPhysicalReloadPhase::PlayingFinish)
	{
		Weapon& weapon = weaponObject->weaponData[0];
		finishFrameCounter++;

		const int safetyFrames = static_cast<int>(frozenReloadRemaining) * 6 + 60;
		const bool bTimerDone = weapon.reloadRemaining == 0 || weapon.reloadState == 0;
		const bool bFinishTimedOut = finishFrameCounter >= safetyFrames;
		const bool bHasReplay = reloadReplayCount > 1;
		const bool bVisualDone = !bHasReplay || bReloadReplayComplete;

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

void PhysicalReloadController::BeginPhysicalReload()
{
	const WeaponManualReloadSettings& settings = ReloadSettings();
	if (settings.ContinuousReload)
	{
		bShotgunShellSessionActive = true;
	}

	BeginChainedShellReload();
}

void PhysicalReloadController::BeginChainedShellReload()
{
	if (phase != EPhysicalReloadPhase::Idle)
	{
		return;
	}

	bPhysicalReloadFromEmpty = WH().IsLocalMagazineEmpty();
	bManualPhysicalReloadPending = true;
	phase = EPhysicalReloadPhase::PlayingEject;
	initialReloadRemaining = 0;
	BeginSoundCapture();
	TriggerWeaponReload();
	bManualPhysicalReloadPending = false;

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

void PhysicalReloadController::ResumePhysicalReloadAnimation()
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
	hapticsConfig.HandleWeaponHaptics(G().GetVR(), offHand, hapticsConfig.physicalReloadInsert);

	const int pauseTicks = ReloadSettings().PauseTicks;
	const int resumeTicks = ReloadSettings().ResumeTicks;
	const int skipTicks = resumeTicks > pauseTicks ? resumeTicks - pauseTicks : 0;
	reloadReplaySkipSeconds = std::max(0.0f, static_cast<float>(skipTicks) / 30.0f);

	const uint16_t realRemaining = frozenReloadRemaining;
	if (realRemaining > 0)
	{
		const uint16_t shortenedRemaining = realRemaining > static_cast<uint16_t>(skipTicks)
			? static_cast<uint16_t>(realRemaining - skipTicks)
			: 1;
		weaponObject->weaponData[0].reloadRemaining = shortenedRemaining;
		frozenReloadRemaining = shortenedRemaining;
	}

	ClearSounds();

	phase = EPhysicalReloadPhase::PlayingFinish;
	finishFrameCounter = 0;
	bMagazineGrabbed = false;
	bMagazineEjected = false;
	ClearBoneSnapshot();

	if (IsDebugLogging())
	{
		Logger::log << "[PhysicalReload] resume skipTicks=" << skipTicks
			<< " replay=" << (reloadReplayCount > 1 ? "yes" : "no")
			<< std::endl;
	}
}

void PhysicalReloadController::HandlePhysicalMagazineGrabInsert()
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
		ResumePhysicalReloadAnimation();
	}
	else if (!gripHeld)
	{
		bMagazineGrabbed = false;
	}
}

void PhysicalReloadController::Update()
{
	if (!G().c_DisableEmptyMagazineAutoReload->Value()
		|| G().bUse3DOFAiming
		|| !WH().HasMagazineBones())
	{
		if (phase != EPhysicalReloadPhase::Idle)
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
	case EPhysicalReloadPhase::Idle:
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
				BeginPhysicalReload();
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
	case EPhysicalReloadPhase::PausedAtEject:
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

		HandlePhysicalMagazineGrabInsert();
		break;
	}
	default:
		break;
	}
}

//===============================// View-model visuals //===============================//

void PhysicalReloadController::CaptureReloadStartInsertSocket(const Transform* outBoneTransforms)
{
	if (bHasReloadStartMagSocket)
	{
		return;
	}

	const int gunIndex = WH().GetGunIndex();
	const int magIndex = WH().GetMagazineRootBoneIndex();
	if (gunIndex < 0 || magIndex < 0)
	{
		return;
	}

	Matrix4 gunMatrix;
	Transform gunTransform = outBoneTransforms[gunIndex];
	TransformToMatrix4(gunTransform, gunMatrix);

	Matrix4 gunInverse = gunMatrix;
	gunInverse.invertAffine();

	reloadStartMagLocalOffset = gunInverse * outBoneTransforms[magIndex].translation;
	bHasReloadStartMagSocket = true;

	if (IsDebugLogging())
	{
		Logger::log << "[PhysicalReload:Insert] captured mag-well offset from reload start"
			<< " gunBone=" << gunIndex
			<< " magBone=" << magIndex
			<< " localOffset=("
			<< reloadStartMagLocalOffset.x << ","
			<< reloadStartMagLocalOffset.y << ","
			<< reloadStartMagLocalOffset.z << ")"
			<< std::endl;
	}
}

void PhysicalReloadController::UpdateInsertSocketFromGun(const Transform* outBoneTransforms)
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

	Matrix4 gunMatrix;
	Transform gunTransform = outBoneTransforms[gunIndex];
	TransformToMatrix4(gunTransform, gunMatrix);
	magazineSocketPosition = gunMatrix * reloadStartMagLocalOffset;

	if (IsDebugLogging())
	{
		static int debugLogCounter = 0;
		if (debugLogCounter++ % 30 == 0)
		{
			Logger::log << "[PhysicalReload:Insert] socket from reload-start mag-well"
				<< " pos=("
				<< magazineSocketPosition.x << ","
				<< magazineSocketPosition.y << ","
				<< magazineSocketPosition.z << ")"
				<< std::endl;
		}

		const Vector3 up(0.0f, 0.0f, 1.0f);
		G().inGameRenderer.DrawPolygon(
			magazineSocketPosition,
			Vector3(1.0f, 0.0f, 0.0f),
			up,
			6,
			G().MetresToWorld(0.05f),
			D3DCOLOR_ARGB(200, 0, 180, 255),
			false);
	}
}

void PhysicalReloadController::CaptureGripFromResumePose(HaloID& id, Vector3* pos, Vector3* facing, Vector3* up)
{
	if (bHasCapturedGrip || reloadReplayCount <= 0)
	{
		return;
	}

	const int wristIndex = WH().GetLeftWristIndex();
	const int magIndex = WH().GetMagazineRootBoneIndex();
	if (wristIndex < 0 || magIndex < 0)
	{
		return;
	}

	const int pauseTicks = ReloadSettings().PauseTicks;
	const int resumeTicks = ReloadSettings().ResumeTicks;
	const int skipTicks = resumeTicks > pauseTicks ? resumeTicks - pauseTicks : 0;
	const float skipSeconds = static_cast<float>(skipTicks) / 30.0f;

	int replayIndex = -1;
	for (int i = 0; i < reloadReplayCount; i++)
	{
		if (reloadReplayTimes[i] >= skipSeconds)
		{
			replayIndex = i;
			break;
		}
	}

	if (replayIndex < 0)
	{
		return;
	}

	TransformQuat resumeQuats[64]{};
	memcpy(resumeQuats, reloadReplayFrames[replayIndex], sizeof(resumeQuats));

	Transform animPoseTransforms[64]{};
	EvaluateAnimPose(id, pos, facing, up, resumeQuats, animPoseTransforms);

	Matrix4 wristMatrix;
	Transform wristTransform = animPoseTransforms[wristIndex];
	TransformToMatrix4(wristTransform, wristMatrix);

	Matrix4 magMatrix;
	Transform magTransform = animPoseTransforms[magIndex];
	TransformToMatrix4(magTransform, magMatrix);

	Matrix4 wristInverse = wristMatrix;
	wristInverse.invertAffine();

	gripFromWristLocal = wristInverse * magMatrix;
	bHasCapturedGrip = true;

	if (IsDebugLogging())
	{
		const Vector3 t = gripFromWristLocal * Vector3(0.0f, 0.0f, 0.0f);
		Logger::log << "[PhysicalReload:Grip] captured grip from resume pose"
			<< " wristBone=" << wristIndex
			<< " magBone=" << magIndex
			<< " skipSeconds=" << skipSeconds
			<< " replayIndex=" << replayIndex
			<< " localTranslation=(" << t.x << "," << t.y << "," << t.z << ")"
			<< std::endl;
	}
}

bool PhysicalReloadController::GetGrabbedMagazineTargetMatrix(const Transform* outBoneTransforms, Matrix4& outTargetMatrix) const
{
	const int wristIndex = WH().GetLeftWristIndex();
	if (!bHasCapturedGrip || wristIndex < 0 || !outBoneTransforms)
	{
		return false;
	}

	Matrix4 wristMatrix;
	Transform wristTransform = outBoneTransforms[wristIndex];
	TransformToMatrix4(wristTransform, wristMatrix);

	outTargetMatrix = wristMatrix * gripFromWristLocal;
	return true;
}

void PhysicalReloadController::RelocateMagazineBones(
	Transform* outBoneTransforms,
	const Vector3& targetRootPos,
	const Matrix4& targetRootOrientation) const
{
	const int rootIndex = WH().GetMagazineRootBoneIndex();
	if (rootIndex < 0)
	{
		return;
	}

	Matrix4 originalRootMatrix;
	TransformToMatrix4(outBoneTransforms[rootIndex], originalRootMatrix);

	Matrix4 originalRootInverse = originalRootMatrix;
	originalRootInverse.invertAffine();

	Matrix4 newRootMatrix = targetRootOrientation;
	newRootMatrix.setColumn(3, targetRootPos);

	for (int i = 0; i < 64; i++)
	{
		if (!WH().IsMagazineBone(i))
		{
			continue;
		}

		Matrix4 originalBoneMatrix;
		TransformToMatrix4(outBoneTransforms[i], originalBoneMatrix);

		Matrix4 relativeMatrix = originalRootInverse * originalBoneMatrix;
		Matrix4 newBoneMatrix = newRootMatrix * relativeMatrix;

		ApplyMatrixToTransform(newBoneMatrix, outBoneTransforms[i]);
		outBoneTransforms[i].scale = 1.0f;
	}
}

Matrix4 PhysicalReloadController::GetDetachedMagazineOrientation(const Transform* outBoneTransforms) const
{
	if (bMagazineGrabbed)
	{
		Matrix4 targetMatrix;
		if (GetGrabbedMagazineTargetMatrix(outBoneTransforms, targetMatrix))
		{
			return targetMatrix;
		}

		const ControllerRole offHand = G().bLeftHanded ? ControllerRole::Right : ControllerRole::Left;
		Matrix4 controllerTransform = G().GetVR()->GetControllerTransform(offHand, true);

		Matrix4 orientation;
		orientation.identity();
		for (int x = 0; x < 3; x++)
		{
			for (int y = 0; y < 3; y++)
			{
				const_cast<float*>(orientation.get())[x + y * 4] = controllerTransform.get()[x + y * 4];
			}
		}

		return orientation;
	}

	Matrix4 headTransform = G().GetVR()->GetHMDTransform(true);
	Vector3 forward = headTransform.getForwardAxis();
	forward.z = 0.0f;
	if (forward.lengthSqr() < 0.0001f)
	{
		forward = Vector3(1.0f, 0.0f, 0.0f);
	}
	else
	{
		forward.normalize();
	}

	const Vector3 up(0.0f, 0.0f, 1.0f);
	Transform orientationTransform;
	Helpers::MakeTransformFromXZ(&forward, &up, &orientationTransform);

	Matrix4 orientation;
	TransformToMatrix4(orientationTransform, orientation);

	Vector3 left = up.cross(forward);
	if (left.lengthSqr() > 0.0001f)
	{
		left.normalize();
		orientation.rotate(90.0f, left);
	}

	return orientation;
}

void PhysicalReloadController::UpdateMagazinePlacement(const HaloID& id, Transform* outBoneTransforms)
{
	const bool bDebug = IsDebugLogging();
	static int debugLogCounter = 0;

	if (WH().GetCachedViewModelAsset() != id || WH().GetRightWristIndex() < 0)
	{
		if (bDebug && bMagazineEjected && debugLogCounter++ % 60 == 0)
		{
			Logger::log << "[PhysicalReload:Belt] skip placement assetMismatch" << std::endl;
		}
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

	const int rootIndex = WH().GetMagazineRootBoneIndex();
	if (rootIndex < 0)
	{
		return;
	}

	RelocateMagazineBones(outBoneTransforms, targetPos, GetDetachedMagazineOrientation(outBoneTransforms));

	if (bDebug)
	{
		if (debugLogCounter++ % 30 == 0)
		{
			Logger::log << "[PhysicalReload:Belt] placed"
				<< " rootBone=" << rootIndex
				<< " grabbed=" << bMagazineGrabbed
				<< " target=(" << targetPos.x << "," << targetPos.y << "," << targetPos.z << ")"
				<< std::endl;
		}

		Vector3 forward = GetDetachedMagazineOrientation(outBoneTransforms).getForwardAxis();
		forward.z = 0.0f;
		if (forward.lengthSqr() < 0.0001f)
		{
			forward = Vector3(1.0f, 0.0f, 0.0f);
		}
		else
		{
			forward.normalize();
		}

		const Vector3 up(0.0f, 0.0f, 1.0f);
		G().inGameRenderer.DrawPolygon(
			targetPos,
			forward,
			up,
			6,
			G().MetresToWorld(0.08f),
			D3DCOLOR_ARGB(200, 255, 140, 0),
			false);
	}
}

void PhysicalReloadController::ApplyBonePin(const HaloID& id, TransformQuat* boneTransforms)
{
	if (WH().GetCachedViewModelAsset() != id || WH().GetRightWristIndex() < 0)
	{
		return;
	}

	const int phaseInt = static_cast<int>(phase);
	const int kPausedAtEject = static_cast<int>(EPhysicalReloadPhase::PausedAtEject);
	const int kPlayingFinish = static_cast<int>(EPhysicalReloadPhase::PlayingFinish);
	const double now = GetClockSeconds();

	if (phaseInt == kPausedAtEject)
	{
		if (!bHasPausedBoneSnapshot)
		{
			memcpy(pausedBoneTransforms, boneTransforms, sizeof(pausedBoneTransforms));
			bHasPausedBoneSnapshot = true;

			memcpy(reloadReplayFrames[0], boneTransforms, sizeof(reloadReplayFrames[0]));
			reloadReplayTimes[0] = 0.0f;
			reloadReplayCount = 1;
			reloadRecordComplete = false;
			reloadPauseStartSeconds = now;

			const float remainingTicks = static_cast<float>(frozenReloadRemaining);
			reloadRecordTargetSeconds = std::min(4.0f, std::max(0.5f, remainingTicks / 30.0f + 0.15f));
		}
		else
		{
			if (!reloadRecordComplete)
			{
				const float elapsed = static_cast<float>(now - reloadPauseStartSeconds);
				const float lastSampleTime = reloadReplayTimes[reloadReplayCount - 1];
				const bool spacedEnough = (elapsed - lastSampleTime) >= (1.0f / 130.0f);

				if (reloadReplayCount >= kMaxReloadReplayFrames || elapsed >= reloadRecordTargetSeconds)
				{
					reloadRecordComplete = true;
				}
				else if (spacedEnough)
				{
					memcpy(reloadReplayFrames[reloadReplayCount], boneTransforms, sizeof(reloadReplayFrames[0]));
					reloadReplayTimes[reloadReplayCount] = elapsed;
					reloadReplayCount++;
				}
			}

			memcpy(boneTransforms, pausedBoneTransforms, sizeof(pausedBoneTransforms));
		}
	}
	else if (phaseInt == kPlayingFinish)
	{
		if (lastBonePinPhase != kPlayingFinish)
		{
			reloadReplayStartSeconds = now - reloadReplaySkipSeconds;
			bReloadReplayComplete = false;

			const float lastTime = reloadReplayCount > 0
				? reloadReplayTimes[reloadReplayCount - 1]
				: 0.0f;
			bReloadReplaying = reloadReplayCount > 1 && reloadReplaySkipSeconds < lastTime;
			if (reloadReplayCount > 1 && reloadReplaySkipSeconds >= lastTime)
			{
				bReloadReplayComplete = true;
			}
		}

		if (bReloadReplaying)
		{
			const float t = static_cast<float>(now - reloadReplayStartSeconds);
			const float lastTime = reloadReplayTimes[reloadReplayCount - 1];

			if (t >= lastTime)
			{
				memcpy(boneTransforms, reloadReplayFrames[reloadReplayCount - 1], sizeof(reloadReplayFrames[0]));
				bReloadReplaying = false;
				bReloadReplayComplete = true;
			}
			else
			{
				int idx = reloadReplayCount - 1;
				for (int i = 1; i < reloadReplayCount; i++)
				{
					if (reloadReplayTimes[i] >= t)
					{
						idx = i;
						break;
					}
				}
				memcpy(boneTransforms, reloadReplayFrames[idx], sizeof(reloadReplayFrames[0]));
			}
		}
	}

	lastBonePinPhase = phaseInt;
}

void PhysicalReloadController::PreSkeleton(const HaloID& id, TransformQuat* boneTransforms)
{
	ApplyBonePin(id, boneTransforms);
}

void PhysicalReloadController::PostSkeleton(HaloID& id, Vector3* pos, Vector3* facing, Vector3* up, Transform* outBoneTransforms)
{
	if (phase == EPhysicalReloadPhase::PlayingEject)
	{
		CaptureReloadStartInsertSocket(outBoneTransforms);
	}
	else if (phase == EPhysicalReloadPhase::PausedAtEject)
	{
		UpdateInsertSocketFromGun(outBoneTransforms);
		CaptureGripFromResumePose(id, pos, facing, up);
	}

	UpdateMagazinePlacement(id, outBoneTransforms);
}

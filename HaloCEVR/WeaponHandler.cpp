#include "WeaponHandler.h"
#include "Helpers/Objects.h"
#include "Helpers/Camera.h"
#include "Helpers/Assets.h"
#include "Helpers/Maths.h"
#include "Helpers/FirstPersonAnim.h"
#include "Logger.h"
#include "Game.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>

// This is a working decomp of the game's original logic for updating the view model's skeleton
// Only kept here for reference when working on the replacement function below
static void ReferenceUpdateViewModelImpl(HaloID& id, Vector3* pos, Vector3* facing, Vector3* up, TransformQuat* boneTransforms, Transform* outBoneTransforms)
{
	Asset_ModelAnimations* viewModel = Helpers::GetTypedAsset<Asset_ModelAnimations>(id);
	AssetData_ModelAnimations* animationData = viewModel->Data;
	Bone* boneArray = animationData->BoneArray;

	Transform root;
	Helpers::MakeTransformFromXZ(up, facing, &root);
	root.translation = *pos;

	int i = 0;

	if (animationData->NumBones > 0)
	{
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
				bonesToProcess[lastIndex] = currentBone.LeftLeaf;
				lastIndex++;
			}
			if (currentBone.RightLeaf != -1)
			{
				bonesToProcess[lastIndex] = currentBone.RightLeaf;
				lastIndex++;
			}

		} while (i != lastIndex);
	}
}

bool WeaponHandler::IsMagazineBoneName(const char* name)
{
	if (!name || !name[0])
	{
		return false;
	}

	// Halo CE first-person weapons use a dedicated "frame magazine" node.
	// Avoid substring matching — it incorrectly includes siblings linked in the
	// bone tree (e.g. trigger, charging handle, display) via MarkMagazineSubtree.
	return _stricmp(name, "frame magazine") == 0
		|| _stricmp(name, "magazine") == 0
		|| _stricmp(name, "clip") == 0;
}

void WeaponHandler::MarkMagazineBone(int boneIndex)
{
	if (boneIndex >= 0 && boneIndex < 64)
	{
		cachedViewModel.magazineHideBones[boneIndex] = true;
	}
}

void WeaponHandler::MarkMagazineDescendants(Bone* boneArray, int numBones, int rootIndex)
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

bool WeaponHandler::IsFirstPersonWeaponAnimationsAsset(AssetData_ModelAnimations* animationData) const
{
	if (!animationData || !animationData->BoneArray || animationData->NumBones <= 0)
	{
		return false;
	}

	bool hasFrameGun = false;
	bool hasFrameRWrist = false;
	const int numBones = std::min(animationData->NumBones, 128);

	for (int i = 0; i < numBones; i++)
	{
		const char* name = animationData->BoneArray[i].BoneName;
		if (!name[0])
		{
			continue;
		}

		if (strstr(name, "frame gun") != nullptr)
		{
			hasFrameGun = true;
		}

		if (strstr(name, "r wrist") != nullptr)
		{
			hasFrameRWrist = true;
		}
	}

	return hasFrameGun && hasFrameRWrist;
}

bool WeaponHandler::IsLocalMagazineEmpty() const
{
	BaseDynamicObject* player = Helpers::GetLocalPlayer();
	if (!player || player->weapon.id == 0xffff)
	{
		return false;
	}

	WeaponDynamicObject* weaponObject = static_cast<WeaponDynamicObject*>(Helpers::GetDynamicObject(player->weapon));
	if (!weaponObject)
	{
		return false;
	}

	return weaponObject->weaponData[0].ammo == 0;
}

bool WeaponHandler::HasMagazineBones() const
{
	return cachedViewModel.bHasMagazineBones;
}

bool WeaponHandler::ShouldShowBeltMagazine() const
{
	return HasMagazineBones()
		&& !Game::instance.bUse3DOFAiming
		&& Game::instance.bMagazineEjected;
}

int WeaponHandler::GetReloadEmptyAnimIndex() const
{
	return cachedViewModel.reloadEmptyAnimIndex;
}

int WeaponHandler::GetReloadExitEmptyAnimIndex() const
{
	return cachedViewModel.reloadExitEmptyAnimIndex;
}

int WeaponHandler::GetReloadFullAnimIndex() const
{
	return cachedViewModel.reloadFullAnimIndex;
}

int WeaponHandler::GetReloadExitFullAnimIndex() const
{
	return cachedViewModel.reloadExitFullAnimIndex;
}

int WeaponHandler::GetActiveReloadAnimIndex() const
{
	return Game::instance.bPhysicalReloadFromEmpty
		? cachedViewModel.reloadEmptyAnimIndex
		: cachedViewModel.reloadFullAnimIndex;
}

int WeaponHandler::GetActiveReloadExitAnimIndex() const
{
	if (Game::instance.bPhysicalReloadFromEmpty)
	{
		return cachedViewModel.reloadExitEmptyAnimIndex;
	}

	return cachedViewModel.reloadExitFullAnimIndex;
}

bool WeaponHandler::ShouldUsePhysicalMagazineReload() const
{
	return ShouldShowBeltMagazine()
		&& Game::instance.c_DisableEmptyMagazineAutoReload->Value();
}

bool WeaponHandler::SupportsPhysicalMagazineReload() const
{
	return HasMagazineBones();
}

int WeaponHandler::ResolveMagazineRootBoneIndex() const
{
	if (cachedViewModel.magazineRootBoneIndex >= 0)
	{
		return cachedViewModel.magazineRootBoneIndex;
	}

	for (int i = 0; i < 64; i++)
	{
		if (cachedViewModel.magazineHideBones[i])
		{
			return i;
		}
	}

	return -1;
}

Vector3 WeaponHandler::GetBeltMagazineWorldPosition() const
{
	// Anchor to hip height in world units (ShowRoomCentre uses z -= 0.62 for feet).
	Vector3 beltPos = Helpers::GetCamera().position;
	beltPos.z -= Game::instance.c_BeltMagazineHipDrop->Value();

	Matrix4 headTransform = Game::instance.GetVR()->GetHMDTransform(true);
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

	const Vector3 offset = Game::instance.c_BeltMagazineOffset->Value();
	const float scale = Game::instance.MetresToWorld(1.0f);
	beltPos += forward * (offset.x * scale);
	beltPos += left * (offset.y * scale);

	return beltPos;
}

Vector3 WeaponHandler::GetMagazineSocketWorldPosition() const
{
	return cachedViewModel.magazineSocketPosition;
}

Vector3 WeaponHandler::GetOffHandWorldPosition() const
{
	const ControllerRole offHand = Game::instance.bLeftHanded ? ControllerRole::Right : ControllerRole::Left;
	Matrix4 offHandTransform = Game::instance.GetVR()->GetControllerTransform(offHand, true);
	Vector3 handPos = offHandTransform * Vector3(0.0f, 0.0f, 0.0f);
	handPos *= Game::instance.MetresToWorld(1.0f);
	handPos += Helpers::GetCamera().position;
	return handPos;
}

Vector3 WeaponHandler::GetMagazineGripWorldOffset() const
{
	const ControllerRole offHand = Game::instance.bLeftHanded ? ControllerRole::Right : ControllerRole::Left;
	Matrix4 offHandTransform = Game::instance.GetVR()->GetControllerTransform(offHand, true);
	const Vector3 controllerOrigin = offHandTransform * Vector3(0.0f, 0.0f, 0.0f);

	Matrix4 handRotation = offHandTransform;
	handRotation.translate(-controllerOrigin);

	Vector3 worldOffset = handRotation * Game::instance.c_MagazineGripControllerOffset->Value();
	worldOffset *= Game::instance.MetresToWorld(1.0f);
	return worldOffset;
}

void WeaponHandler::CaptureGripFromResumePose(HaloID& id, Vector3* pos, Vector3* facing, Vector3* up)
{
	if (bHasCapturedGrip || reloadReplayCount <= 0)
	{
		return;
	}

	const int wristIndex = cachedViewModel.leftWristIndex;
	const int magIndex = ResolveMagazineRootBoneIndex();
	if (wristIndex < 0 || magIndex < 0)
	{
		return;
	}

	// Find the replay frame that corresponds to the resume tick.
	// The replay buffer records the live animation during the eject pause; at the resume tick
	// the hand is holding the fresh magazine before insertion — the correct grip frame.
	const int pauseTicks = Game::instance.GetPhysicalReloadPauseTicks();
	const int resumeTicks = Game::instance.GetPhysicalReloadResumeTicks();
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
		return; // Resume frame not yet in the replay buffer; will retry next tick
	}

	TransformQuat resumeQuats[64]{};
	memcpy(resumeQuats, reloadReplayFrames[replayIndex], sizeof(resumeQuats));

	// Evaluate in pure animation space (no VR overrides) so the wrist and magazine
	// positions reflect exactly what the animation intends at this frame.
	Transform animPoseTransforms[64]{};
	ReferenceUpdateViewModelImpl(id, pos, facing, up, resumeQuats, animPoseTransforms);

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

	if (Game::instance.c_LogPhysicalReloadDebug->Value())
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

bool WeaponHandler::GetGrabbedMagazineTargetMatrix(const Transform* outBoneTransforms, Matrix4& outTargetMatrix) const
{
	const int wristIndex = cachedViewModel.leftWristIndex;
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

void WeaponHandler::RelocateMagazineBones(Transform* outBoneTransforms, const Vector3& targetRootPos, const Matrix4& targetRootOrientation)
{
	const int rootIndex = ResolveMagazineRootBoneIndex();
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
		if (!cachedViewModel.magazineHideBones[i])
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

Matrix4 WeaponHandler::GetDetachedMagazineOrientation(const Transform* outBoneTransforms) const
{
	if (Game::instance.bMagazineGrabbed)
	{
		Matrix4 targetMatrix;
		if (GetGrabbedMagazineTargetMatrix(outBoneTransforms, targetMatrix))
		{
			return targetMatrix;
		}

		const ControllerRole offHand = Game::instance.bLeftHanded ? ControllerRole::Right : ControllerRole::Left;
		Matrix4 controllerTransform = Game::instance.GetVR()->GetControllerTransform(offHand, true);

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

	// Belt orientation: follow body yaw only so the mag doesn't tilt with gun aim.
	Matrix4 headTransform = Game::instance.GetVR()->GetHMDTransform(true);
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
	return orientation;
}

void WeaponHandler::UpdatePhysicalMagazinePlacement(const HaloID& id, Transform* outBoneTransforms)
{
	const bool bDebug = Game::instance.c_LogPhysicalReloadDebug->Value();
	static int debugLogCounter = 0;

	if (cachedViewModel.currentAsset != id || cachedViewModel.rightWristIndex < 0)
	{
		if (bDebug && Game::instance.bMagazineEjected && debugLogCounter++ % 60 == 0)
		{
			Logger::log << "[PhysicalReload:Belt] skip placement assetMismatch"
				<< " passAsset=" << id
				<< " cachedAsset=" << cachedViewModel.currentAsset
				<< " rightWrist=" << cachedViewModel.rightWristIndex
				<< std::endl;
		}
		return;
	}

	if (!ShouldShowBeltMagazine())
	{
		if (bDebug && debugLogCounter++ % 60 == 0
			&& Game::instance.physicalReloadPhase != EPhysicalReloadPhase::Idle)
		{
			Logger::log << "[PhysicalReload:Belt] skip show"
				<< " hasMagBones=" << HasMagazineBones()
				<< " b3DOF=" << Game::instance.bUse3DOFAiming
				<< " bMagEjected=" << Game::instance.bMagazineEjected
				<< " phase=" << static_cast<int>(Game::instance.physicalReloadPhase)
				<< std::endl;
		}
		return;
	}

	Vector3 targetPos = GetBeltMagazineWorldPosition();
	if (Game::instance.bMagazineGrabbed)
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

	const int rootIndex = ResolveMagazineRootBoneIndex();
	if (rootIndex < 0)
	{
		if (bDebug && debugLogCounter++ % 30 == 0)
		{
			Logger::log << "[PhysicalReload:Belt] no magazine root bone resolved" << std::endl;
		}
		return;
	}

	int markedBoneCount = 0;
	for (int i = 0; i < 64; i++)
	{
		if (cachedViewModel.magazineHideBones[i])
		{
			markedBoneCount++;
		}
	}

	RelocateMagazineBones(outBoneTransforms, targetPos, GetDetachedMagazineOrientation(outBoneTransforms));

	if (bDebug)
	{
		if (debugLogCounter++ % 30 == 0)
		{
			const Vector3 camPos = Helpers::GetCamera().position;
			Logger::log << "[PhysicalReload:Belt] placed"
				<< " rootBone=" << rootIndex
				<< " markedBones=" << markedBoneCount
				<< " grabbed=" << Game::instance.bMagazineGrabbed
				<< " target=(" << targetPos.x << "," << targetPos.y << "," << targetPos.z << ")"
				<< " cam=(" << camPos.x << "," << camPos.y << "," << camPos.z << ")"
				<< " magScale=" << outBoneTransforms[rootIndex].scale
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
			Game::instance.inGameRenderer.DrawPolygon(
			targetPos,
			forward,
			up,
			6,
			Game::instance.MetresToWorld(0.08f),
			D3DCOLOR_ARGB(200, 255, 140, 0),
			false);
	}
}

void WeaponHandler::ClearPhysicalReloadBoneSnapshot()
{
	bHasPausedBoneSnapshot = false;
}

void WeaponHandler::ResetPhysicalReloadBonePinState()
{
	ClearPhysicalReloadBoneSnapshot();
	reloadReplayCount = 0;
	reloadRecordComplete = false;
	reloadRecordTargetSeconds = 0.0f;
	reloadPauseStartSeconds = -1.0;
	reloadReplayStartSeconds = -1.0;
	reloadReplaySkipSeconds = 0.0f;
	bReloadReplaying = false;
	bReloadReplayComplete = false;
	lastBonePinPhase = 0; // EPhysicalReloadPhase::Idle
	bHasCapturedGrip = false;
	gripFromWristLocal.identity();
}

static double GetPhysicalReloadClockSeconds()
{
	using namespace std::chrono;
	return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// Physical reload bone record/replay.
// SetViewModelPosition runs once per skeleton pass; we only touch the local weapon asset (gated
// by currentAsset/rightWristIndex) so non-weapon skeletons are untouched (see skeleton-pipeline.md).
//
// PausedAtEject: snapshot the eject pose and keep displaying it, while recording the live clip's
//   remaining bone stream (it keeps running underneath on the engine's own clock) up to roughly the
//   real remaining reload duration.
// PlayingFinish: replay from the resume tick (skipping the virtual insert segment) so only
//   chamber/finish animates after the player inserts the magazine.
void WeaponHandler::ClearReloadStartInsertSocket()
{
	bHasReloadStartMagSocket = false;
	reloadStartMagLocalOffset = Vector3(0.0f, 0.0f, 0.0f);
	bHasCapturedGrip = false;
	gripFromWristLocal.identity();
}



void WeaponHandler::SetReloadReplaySkipSeconds(float skipSeconds)
{
	reloadReplaySkipSeconds = std::max(0.0f, skipSeconds);
}

void WeaponHandler::CaptureReloadStartInsertSocket(const Transform* outBoneTransforms)
{
	if (bHasReloadStartMagSocket)
	{
		return;
	}

	const int gunIndex = cachedViewModel.gunIndex;
	const int magIndex = ResolveMagazineRootBoneIndex();
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

	if (Game::instance.c_LogPhysicalReloadDebug->Value())
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

void WeaponHandler::UpdateInsertSocketFromGun(const Transform* outBoneTransforms)
{
	if (!bHasReloadStartMagSocket)
	{
		return;
	}

	const int gunIndex = cachedViewModel.gunIndex;
	if (gunIndex < 0)
	{
		return;
	}

	Matrix4 gunMatrix;
	Transform gunTransform = outBoneTransforms[gunIndex];
	TransformToMatrix4(gunTransform, gunMatrix);
	cachedViewModel.magazineSocketPosition = gunMatrix * reloadStartMagLocalOffset;

	if (Game::instance.c_LogPhysicalReloadDebug->Value())
	{
		static int debugLogCounter = 0;
		if (debugLogCounter++ % 30 == 0)
		{
			Logger::log << "[PhysicalReload:Insert] socket from reload-start mag-well"
				<< " pos=("
				<< cachedViewModel.magazineSocketPosition.x << ","
				<< cachedViewModel.magazineSocketPosition.y << ","
				<< cachedViewModel.magazineSocketPosition.z << ")"
				<< std::endl;
		}

		const Vector3 up(0.0f, 0.0f, 1.0f);
		Game::instance.inGameRenderer.DrawPolygon(
			cachedViewModel.magazineSocketPosition,
			Vector3(1.0f, 0.0f, 0.0f),
			up,
			6,
			Game::instance.MetresToWorld(0.05f),
			D3DCOLOR_ARGB(200, 0, 180, 255),
			false);
	}
}

void WeaponHandler::ApplyPhysicalReloadBonePin(const HaloID& id, TransformQuat* boneTransforms)
{
	if (cachedViewModel.currentAsset != id || cachedViewModel.rightWristIndex < 0)
	{
		return;
	}

	const int phase = static_cast<int>(Game::instance.physicalReloadPhase);
	const int kPausedAtEject = static_cast<int>(EPhysicalReloadPhase::PausedAtEject);
	const int kPlayingFinish = static_cast<int>(EPhysicalReloadPhase::PlayingFinish);
	const double now = GetPhysicalReloadClockSeconds();

	if (phase == kPausedAtEject)
	{
		if (!bHasPausedBoneSnapshot)
		{
			// First eject frame: this pose is the eject point. Snapshot it for the frozen display
			// and seed the recording with it as frame 0 (t = 0).
			memcpy(pausedBoneTransforms, boneTransforms, sizeof(pausedBoneTransforms));
			bHasPausedBoneSnapshot = true;

			memcpy(reloadReplayFrames[0], boneTransforms, sizeof(reloadReplayFrames[0]));
			reloadReplayTimes[0] = 0.0f;
			reloadReplayCount = 1;
			reloadRecordComplete = false;
			reloadPauseStartSeconds = now;

			// Capture roughly the real remaining reload time so playback matches the engine's
			// reload timer during PlayingFinish (30 ticks/sec), with a small tail margin.
			const float remainingTicks = static_cast<float>(Game::instance.frozenReloadRemaining);
			reloadRecordTargetSeconds = std::min(4.0f, std::max(0.5f, remainingTicks / 30.0f + 0.15f));
		}
		else
		{
			// Keep recording the live clip until we've captured the rest of the reload.
			if (!reloadRecordComplete)
			{
				const float elapsed = static_cast<float>(now - reloadPauseStartSeconds);
				const float lastSampleTime = reloadReplayTimes[reloadReplayCount - 1];
				// Throttle to ~130 Hz so the fixed buffer always spans the target duration.
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

			// Always display the frozen eject pose during the hold.
			memcpy(boneTransforms, pausedBoneTransforms, sizeof(pausedBoneTransforms));
		}
	}
	else if (phase == kPlayingFinish)
	{
		// Initialise playback on entry to PlayingFinish.
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
				// Find the recorded frame nearest the elapsed playback time.
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

	lastBonePinPhase = phase;
}

void WeaponHandler::UpdateViewModel(HaloID& id, Vector3* pos, Vector3* facing, Vector3* up, TransformQuat* boneTransforms, Transform* outBoneTransforms)
{
	Vector3& camPos = Helpers::GetCamera().position;

	pos->x = camPos.x;
	pos->y = camPos.y;
	pos->z = camPos.z;

	Matrix4 handTransform;
	CalculateHandTransform(pos, handTransform);

	if (Game::instance.bUse3DOFAiming)
	{
		// Initialize yaw offset tracking when entering 3DOF mode
		if (!bWasIn3DOFMode)
		{
			lastYawOffset = Game::instance.GetVR()->GetYawOffset();
			bWasIn3DOFMode = true;
		}

		// Apply HMD translation so weapon follows player's body position when leaning
		Matrix4 hmdTransform = Game::instance.GetVR()->GetHMDTransform();
		Vector3 hmdPosition = hmdTransform * Vector3(0.0f, 0.0f, 0.0f);
		Vector3 hmdOffset = hmdPosition * Game::instance.MetresToWorld(1.0f);
		pos->x += hmdOffset.x;
		pos->y += hmdOffset.y;
		pos->z += hmdOffset.z;

		// Apply configurable weapon offset in world space (hot-reloadable)
		Vector3 configOffset = Game::instance.c_3DOFWeaponOffset->Value();
        Vector3 weaponOffset = configOffset * Game::instance.MetresToWorld(1.0f);
		pos->x += weaponOffset.x;
		pos->y += weaponOffset.y;
		pos->z += weaponOffset.z;

		// Apply controller rotation to weapon facing/up vectors
		// Only use pitch and yaw from the controller, zero out roll

		// Extract rotation from controller
		Vector3 controllerPos = handTransform * Vector3(0.0f, 0.0f, 0.0f);
		Matrix4 rotationOnly = handTransform;
		rotationOnly.translate(-controllerPos);

		// Get the facing direction from controller (this has pitch and yaw)
		Vector3 facingDir = rotationOnly * Vector3(1.0f, 0.0f, 0.0f);

		// Detect snap turns and rotate smoothed direction to prevent lerping artifact
		float currentYawOffset = Game::instance.GetVR()->GetYawOffset();
		float yawDelta = currentYawOffset - lastYawOffset;

		// Detect snap turns and rotate smoothed direction
		Helpers::RotateForSnapTurn(smoothed3DOFFacingDir, yawDelta, Game::instance.c_SnapTurnAmount->Value());

		// Update tracked yaw offset for next frame
		lastYawOffset = currentYawOffset;

		// Apply cosmetic motion smoothing to facing direction
		float smoothingAmount = Game::instance.c_3DOFWeaponSmoothingAmount->Value();
		float clampedSmoothing = std::clamp(smoothingAmount, 0.0f, 2.0f);

		if (clampedSmoothing > 0.0f)
		{
			const float scaleFactor = (-20.0f / 9.0f);
			float h = 90.0f * log2(1.0f - exp(clampedSmoothing * scaleFactor));
			float t = 1.0f - pow(2.0f, Game::instance.lastDeltaTime * h);
			smoothed3DOFFacingDir = Helpers::Lerp(smoothed3DOFFacingDir, facingDir, t);
			facingDir = smoothed3DOFFacingDir;
			facingDir.normalize();
		}
		else
		{
			// No smoothing - update cached value for when smoothing is re-enabled
			smoothed3DOFFacingDir = facingDir;
		}

		// Use world up (0, 0, 1) to derive a roll-free orientation
		// Cross product of world up and facing gives us the right vector
		Vector3 worldUp(0.0f, 0.0f, 1.0f);
		Vector3 rightDir = worldUp.cross(facingDir);
		rightDir.normalize();

		// Cross product of facing and right gives us the corrected up vector (roll-free)
		Vector3 upDir = facingDir.cross(rightDir);
		upDir.normalize();

		facing->x = facingDir.x;
		facing->y = facingDir.y;
		facing->z = facingDir.z;

		up->x = upDir.x;
		up->y = upDir.y;
		up->z = upDir.z;
	}
	else
	{
		// 6DOF MODE: Standard camera-relative orientation
		// Reset the 3DOF mode flag so we reinitialize when switching back
		bWasIn3DOFMode = false;

		facing->x = 1.0f;
		facing->y = 0.0f;
		facing->z = 0.0f;

		up->x = 0.0f;
		up->y = 0.0f;
		up->z = 1.0f;
	}

	Asset_ModelAnimations* viewModel = Helpers::GetTypedAsset<Asset_ModelAnimations>(id);
	if (!viewModel || !viewModel->Data)
	{
		Logger::log << "[UpdateViewModel] Can't get view model asset" << std::endl;
		return;
	}

	AssetData_ModelAnimations* animationData = viewModel->Data;
	if (!IsFirstPersonWeaponAnimationsAsset(animationData))
	{
		ReferenceUpdateViewModelImpl(id, pos, facing, up, boneTransforms, outBoneTransforms);
		return;
	}

	Bone* boneArray = animationData->BoneArray;

	Transform root;
	Helpers::MakeTransformFromXZ(up, facing, &root);
	root.translation = *pos;

	const bool bShouldUpdateCache = cachedViewModel.currentAsset != id;
	if (bShouldUpdateCache)
	{
		UpdateCache(id, animationData);
		Game::instance.ResetPhysicalReloadState();
	}

	ApplyPhysicalReloadBonePin(id, boneTransforms);

	Transform unmodifiedHandTransform;
	CalculateBoneTransform(cachedViewModel.rightWristIndex, boneArray, root, boneTransforms, unmodifiedHandTransform);

	Transform realTransforms[64]{};

	if (animationData->NumBones > 0)
	{
		int lastIndex = 1;
		int16_t bonesToProcess[64]{};

		bonesToProcess[0] = 0;

		int i = 0;
		do
		{
			const int16_t boneIndex = bonesToProcess[i];
			i++;
			if (boneIndex < 0 || boneIndex >= animationData->NumBones || boneIndex >= 64)
			{
				continue;
			}

			const Bone& currentBone = boneArray[boneIndex];
			Transform* parentTransform = boneIndex == 0 ? &root : &outBoneTransforms[currentBone.Parent];
			const TransformQuat* currentQuat = &boneTransforms[boneIndex];
			Transform tempTransform;
			Transform modifiedTransform;
			// For all bones but the root sub in the ACTUAL transform for the calculations (free from scaling/transform issues)
			if (boneIndex > 0)
			{
				modifiedTransform = *parentTransform;
				*parentTransform = realTransforms[currentBone.Parent];
			}

			Helpers::MakeTransformFromQuat(&currentQuat->rotation, &tempTransform);
			tempTransform.scale = currentQuat->scale;

			if (boneIndex == cachedViewModel.displayIndex && Game::instance.bLeftHanded)
			{
				// Bit of a nasty place to do this, but we need to unflip the ammo counter on the BR
				Matrix3 rot = tempTransform.rotation;
				rot *= Matrix3(
					1.0f, 0.0f, 0.0f,
					0.0f, -1.0f, 0.0f,
					0.0f, 0.0f, 1.0f
				);
				
				for (int i = 0; i < 9; i++)
				{
					tempTransform.rotation[i] = rot[i];
				}
			}

			tempTransform.translation = currentQuat->translation;
			Helpers::CombineTransforms(parentTransform, &tempTransform, &outBoneTransforms[boneIndex]);

			if (boneIndex > 0)
			{
				// Restore the modified transform
				*parentTransform = modifiedTransform;
			}
			// Cache the calculated transform for this bone
			realTransforms[boneIndex] = outBoneTransforms[boneIndex];
			if (currentBone.Parent == 0 || boneArray[currentBone.Parent].Parent == 0)
			{
				// Hide arms/root in 6DOF mode only
				if (!Game::instance.bUse3DOFAiming)
				{
					outBoneTransforms[boneIndex].scale = 0.0f;
				}
			}
			else if (boneIndex == cachedViewModel.rightWristIndex)
			{
				// Skip VR hand tracking in 3DOF mode - use original weapon bone transforms
				if (!Game::instance.bUse3DOFAiming)
				{
					// 6DOF MODE: VR hand tracking
					// This is dreadful code. Rework to be less insane
					if (!Game::instance.bUseTwoHandAim)
					{
						Matrix4 newTransform = handTransform;
						if (currentBone.RightLeaf != -1)
						{
							Bone& GunBone = boneArray[currentBone.RightLeaf];
							if (currentBone.RightLeaf == cachedViewModel.gunIndex)
							{
								const TransformQuat* GunQuat = &boneTransforms[currentBone.RightLeaf];
								Helpers::MakeTransformFromQuat(&GunQuat->rotation, &tempTransform);

								Matrix4 rotation;
								for (int x = 0; x < 3; x++)
								{
									for (int y = 0; y < 3; y++)
									{
										rotation[x + y * 4] = tempTransform.rotation[x + y * 3];
									}
								}

								if (Game::instance.bLeftHanded)
								{
									Matrix4 scale;
									scale.scale(1.0f, -1.0f, 1.0f);

									rotation = scale * rotation * scale;
								}

								newTransform = newTransform * rotation.invert();
							}
							else
							{
								Logger::log << "ERROR: Right leaf of " << currentBone.BoneName << " is " << GunBone.BoneName << std::endl;
							}

						}
						MoveBoneToTransform(boneIndex, newTransform, realTransforms, outBoneTransforms);
					}
					else
					{
						MoveBoneToTransform(boneIndex, handTransform, realTransforms, outBoneTransforms);
					}
					CreateEndCap(boneIndex, currentBone, outBoneTransforms);
				}
			}
			else if (boneIndex == cachedViewModel.leftWristIndex)
			{
				// Skip VR hand tracking in 3DOF mode - use original weapon bone transforms
				if (!Game::instance.bUse3DOFAiming)
				{
					// 6DOF MODE: VR hand tracking
					if (!Game::instance.bUseTwoHandAim)
					{
						Matrix4 newTransform = Game::instance.GetVR()->GetControllerTransform(Game::instance.bLeftHanded ? ControllerRole::Right : ControllerRole::Left, true);
						// Apply scale only to translation portion
						Vector3 translation = newTransform * Vector3(0.0f, 0.0f, 0.0f);
						newTransform.translate(-translation);
						translation *= Game::instance.MetresToWorld(1.0f);
						translation += *pos;
						newTransform.translate(translation);

						MoveBoneToTransform(boneIndex, newTransform, realTransforms, outBoneTransforms);
					}
					else
					{
						// Convert both hand transforms to matrix4
						Matrix4 leftMatrix;
						TransformToMatrix4(outBoneTransforms[boneIndex], leftMatrix);
						Matrix4 rightMatrix;
						TransformToMatrix4(unmodifiedHandTransform, rightMatrix);

						// Get inverse of dominant hand, apply it to non-dominant to get delta
						rightMatrix.invertAffine();
						Matrix4 deltaMatrix = rightMatrix * leftMatrix;

						if (Game::instance.bLeftHanded)
						{
							Matrix4 flip;
							flip.scale(1.0f, -1.0f, 1.0f);

							deltaMatrix = flip * deltaMatrix * flip;
							leftMatrix = handTransform * deltaMatrix;
						}
						else
						{
							// Apply delta to controller transform
							leftMatrix = handTransform * deltaMatrix;
						}

						// Move non-dominant hand to new transform
						MoveBoneToTransform(boneIndex, leftMatrix, realTransforms, outBoneTransforms);
					}

					CreateEndCap(boneIndex, currentBone, outBoneTransforms);
				}				
			}
			else if (boneIndex == cachedViewModel.gunIndex)
			{
				// Skip hand-relative gun calculations in 3DOF mode
				if (!Game::instance.bUse3DOFAiming)
				{
					// 6DOF MODE: Calculate gun position/rotation relative to hand
					Vector3& gunPos = outBoneTransforms[boneIndex].translation;
					Matrix3 gunRot = outBoneTransforms[boneIndex].rotation;

					if (Game::instance.bLeftHanded)
					{
						gunRot = gunRot * Matrix3(
							1.0f, 0.0f, 0.0f,
							0.0f, -1.0f, 0.0f,
							0.0f, 0.0f, 1.0f
						);
					}

					Vector3 handPos = handTransform * Vector3(0.0f, 0.0f, 0.0f);
					Matrix4 handRotation = handTransform.translate(-handPos);
					Matrix3 handRotation3;

					for (int i = 0; i < 3; i++)
					{
						handRotation3.setColumn(i, &handRotation.get()[i * 4]);
					}

					Matrix3 inverseHand = handRotation3;
					inverseHand.invert();

					cachedViewModel.fireOffset = (gunPos - handPos) + (gunRot * cachedViewModel.cookedFireOffset);
					cachedViewModel.fireOffset = inverseHand * cachedViewModel.fireOffset;

					cachedViewModel.gunOffset = (gunPos - handPos);
					cachedViewModel.gunOffset = inverseHand * cachedViewModel.gunOffset;

					cachedViewModel.fireRotation = cachedViewModel.cookedFireRotation * gunRot * inverseHand;
				}				
			}

#if DRAW_DEBUG_AIM
			if (currentBone.Parent != -1)
			{
				Game::instance.inGameRenderer.DrawLine3D(outBoneTransforms[boneIndex].translation, outBoneTransforms[currentBone.Parent].translation, D3DCOLOR_ARGB(127, 127, 127, 127), false);
			}
#endif

			if (currentBone.LeftLeaf != -1 && lastIndex < 64)
			{
				bonesToProcess[lastIndex] = currentBone.LeftLeaf;
				lastIndex++;
			}
			if (currentBone.RightLeaf != -1 && lastIndex < 64)
			{
				bonesToProcess[lastIndex] = currentBone.RightLeaf;
				lastIndex++;
			}

		} while (i != lastIndex);
	}

	const EPhysicalReloadPhase reloadPhase = Game::instance.physicalReloadPhase;
	if (reloadPhase == EPhysicalReloadPhase::PlayingEject)
	{
		CaptureReloadStartInsertSocket(outBoneTransforms);
	}
	else if (reloadPhase == EPhysicalReloadPhase::PausedAtEject)
	{
		UpdateInsertSocketFromGun(outBoneTransforms);
		CaptureGripFromResumePose(id, pos, facing, up);
	}

	UpdatePhysicalMagazinePlacement(id, outBoneTransforms);
}

inline void WeaponHandler::CalculateBoneTransform(int boneIndex, Bone* boneArray, Transform& root, TransformQuat* boneTransforms, Transform& outTransform) const
{
	// Clear to identity
	Vector3 xVec = Vector3(1.0f, 0.0f, 0.0f);
	Vector3 zVec = Vector3(0.0f, 0.0f, 1.0f);
	Helpers::MakeTransformFromXZ(&zVec, &xVec, &outTransform);

	if (boneIndex < 0)
	{
		return;
	}

	int currentIndex = boneIndex;
	
	while (true)
	{
		Bone& currentBone = boneArray[currentIndex];

		// Convert bone from TransformQuat to Transform
		const TransformQuat* currentQuat = &boneTransforms[currentIndex];
		Transform tempTransform;
		Helpers::MakeTransformFromQuat(&currentQuat->rotation, &tempTransform);
		tempTransform.scale = currentQuat->scale;
		tempTransform.translation = currentQuat->translation;

		// Apply current transform to child transform
		Helpers::CombineTransforms(&tempTransform, &outTransform, &outTransform);

		if (currentIndex == 0)
		{
			break;
		}

		// Get next bone
		currentIndex = currentBone.Parent;
	}

	// Do root
	Helpers::CombineTransforms(&root, &outTransform, &outTransform);
}

inline void WeaponHandler::CalculateHandTransform(Vector3* pos, Matrix4& handTransform) const
{
	Matrix4 newTransform = GetDominantHandTransform();

	// Apply scale only to translation portion
	{
		Vector3 translation = newTransform * Vector3(0.0f, 0.0f, 0.0f);
		newTransform.translate(-translation);
		translation *= Game::instance.MetresToWorld(1.0f);
		translation += *pos;
		newTransform.translate(translation);
	}

	handTransform = newTransform;
}

void WeaponHandler::CreateEndCap(int boneIndex, const Bone& currentBone, Transform* outBoneTransforms) const
{
	// Parent bone to the position of the current bone with 0 scale to act as an end cap
	int idx = currentBone.Parent;
	outBoneTransforms[idx].translation = outBoneTransforms[boneIndex].translation;
	for (int j = 0; j < 9; j++)
	{
		outBoneTransforms[idx].rotation[j] = outBoneTransforms[boneIndex].rotation[j];
	}
	outBoneTransforms[idx].scale = 0.0f;
}

void WeaponHandler::MoveBoneToTransform(int boneIndex, const Matrix4& newTransform, Transform* realTransforms, Transform* outBoneTransforms) const
{
	// Move hands to match controllers
	Vector3 newTranslation = newTransform * Vector3(0.0f, 0.0f, 0.0f);
	Matrix4 newRotation4 = Game::instance.bLeftHanded ? newTransform * Matrix4().scale(1.0f, -1.0f, 1.0f) : newTransform;
	newRotation4.translate(-newTranslation);
	newRotation4.rotateZ(localRotation.z);
	newRotation4.rotateY(localRotation.y);
	newRotation4.rotateX(localRotation.x);

	//Add Local offset
	newTranslation += newRotation4 * localOffset;

	outBoneTransforms[boneIndex].translation = newTranslation;
	for (int x = 0; x < 3; x++)
	{
		for (int y = 0; y < 3; y++)
		{
			outBoneTransforms[boneIndex].rotation[x + y * 3] = newRotation4.get()[x + y * 4];
		}
	}
	realTransforms[boneIndex] = outBoneTransforms[boneIndex]; // Re-cache value to use updated position
}

void WeaponHandler::LogViewModelBoneHierarchyNode(Bone* boneArray, int numBones, int boneIndex, int depth) const
{
	if (boneIndex < 0 || boneIndex >= numBones || depth > 64)
	{
		return;
	}

	std::string indent(static_cast<size_t>(depth) * 2, ' ');
	const Bone& bone = boneArray[boneIndex];

	Logger::log << "[WeaponHandler] " << indent << "[" << boneIndex << "] " << bone.BoneName;
	if (boneIndex < 64 && cachedViewModel.magazineHideBones[boneIndex])
	{
		Logger::log << " [magazine]";
	}
	Logger::log << std::endl;

	LogViewModelBoneHierarchyNode(boneArray, numBones, bone.LeftLeaf, depth + 1);
	LogViewModelBoneHierarchyNode(boneArray, numBones, bone.RightLeaf, depth + 1);
}

void WeaponHandler::LogViewModelBoneHierarchy(AssetData_ModelAnimations* animationData, const char* weaponAssetPath) const
{
	if (!animationData || !animationData->BoneArray)
	{
		return;
	}

	Bone* boneArray = animationData->BoneArray;
	const int numBones = animationData->NumBones;

	Logger::log << "[WeaponHandler] === View model bone hierarchy";
	if (weaponAssetPath && weaponAssetPath[0])
	{
		Logger::log << " weapon=" << weaponAssetPath;
	}
	Logger::log << " ===" << std::endl;
	Logger::log << "[WeaponHandler] Bone count: " << numBones << std::endl;

	for (int i = 0; i < numBones; i++)
	{
		const Bone& bone = boneArray[i];

		auto boneNameOrNone = [&](int index) -> const char*
		{
			if (index >= 0 && index < numBones)
			{
				return boneArray[index].BoneName;
			}

			return "none";
		};

		Logger::log << "[WeaponHandler] Bone[" << i << "] \"" << bone.BoneName << "\""
			<< " parent=" << bone.Parent << " (\"" << boneNameOrNone(bone.Parent) << "\")"
			<< " left=" << bone.LeftLeaf << " (\"" << boneNameOrNone(bone.LeftLeaf) << "\")"
			<< " right=" << bone.RightLeaf << " (\"" << boneNameOrNone(bone.RightLeaf) << "\")";

		if (i < 64 && cachedViewModel.magazineHideBones[i])
		{
			Logger::log << " [magazine]";
		}

		Logger::log << std::endl;
	}

	if (numBones > 0)
	{
		Logger::log << "[WeaponHandler] -- Tree from bone 0 --" << std::endl;
		LogViewModelBoneHierarchyNode(boneArray, numBones, 0, 0);
	}

	Logger::log << "[WeaponHandler] === End bone hierarchy ===" << std::endl;
}

void WeaponHandler::UpdateCache(HaloID& id, AssetData_ModelAnimations* animationData)
{
	if (!animationData || !animationData->BoneArray || animationData->NumBones <= 0 || animationData->NumBones > 256)
	{
		Logger::log << "[UpdateCache] Invalid animation data for asset " << id << std::endl;
		return;
	}

#if DRAW_DEBUG_AIM
	Logger::log << "[UpdateCache] Swapped weapons, recaching " << id << std::endl;
#endif
	cachedViewModel.currentAsset = id;
	cachedViewModel.leftWristIndex = -1;
	cachedViewModel.rightWristIndex = -1;
	cachedViewModel.gunIndex = -1;
	cachedViewModel.displayIndex = -1;
	cachedViewModel.bHasMagazineBones = false;
	cachedViewModel.magazineRootBoneIndex = -1;
	cachedViewModel.reloadEmptyAnimIndex = -1;
	cachedViewModel.reloadExitEmptyAnimIndex = -1;
	cachedViewModel.reloadFullAnimIndex = -1;
	cachedViewModel.reloadExitFullAnimIndex = -1;
	memset(cachedViewModel.magazineHideBones, 0, sizeof(cachedViewModel.magazineHideBones));

	Bone* boneArray = animationData->BoneArray;

	for (int i = 0; i < animationData->NumAnimations; i++)
	{
		const char* animName = animationData->AnimationArray[i].N00000429;
		if (!animName || !animName[0])
		{
			continue;
		}

		if (cachedViewModel.reloadEmptyAnimIndex < 0 && strstr(animName, "reload-empty"))
		{
			cachedViewModel.reloadEmptyAnimIndex = i;
		}

		if (cachedViewModel.reloadExitEmptyAnimIndex < 0
			&& (strstr(animName, "exit-empty") || strstr(animName, "exit empty")
				|| strstr(animName, "exit_empty") || strstr(animName, "reload-exit-empty")
				|| strstr(animName, "reload-exit")))
		{
			cachedViewModel.reloadExitEmptyAnimIndex = i;
		}

		if (cachedViewModel.reloadFullAnimIndex < 0
			&& (strstr(animName, "reload-full") || strstr(animName, "reload full")))
		{
			cachedViewModel.reloadFullAnimIndex = i;
		}

		if (cachedViewModel.reloadExitFullAnimIndex < 0
			&& (strstr(animName, "exit-full") || strstr(animName, "exit full")
				|| strstr(animName, "exit_full") || strstr(animName, "reload-exit-full")))
		{
			cachedViewModel.reloadExitFullAnimIndex = i;
		}
	}

	if (cachedViewModel.reloadEmptyAnimIndex >= 0 || cachedViewModel.reloadExitEmptyAnimIndex >= 0
		|| cachedViewModel.reloadFullAnimIndex >= 0 || cachedViewModel.reloadExitFullAnimIndex >= 0)
	{
		Logger::log << "[WeaponHandler] Reload anim indices: empty=" << cachedViewModel.reloadEmptyAnimIndex
			<< " exitEmpty=" << cachedViewModel.reloadExitEmptyAnimIndex
			<< " full=" << cachedViewModel.reloadFullAnimIndex
			<< " exitFull=" << cachedViewModel.reloadExitFullAnimIndex << std::endl;
	}

	for (int i = 0; i < animationData->NumBones; i++)
	{
		Bone& CurrentBone = boneArray[i];

		if (cachedViewModel.leftWristIndex == -1 && strstr(CurrentBone.BoneName, "l wrist"))
		{
#if DRAW_DEBUG_AIM
			Logger::log << "[UpdateCache] Found Left Wrist @ " << i << std::endl;
#endif
			cachedViewModel.leftWristIndex = i;
		}
		else if (cachedViewModel.rightWristIndex == -1 && strstr(CurrentBone.BoneName, "r wrist"))
		{
#if DRAW_DEBUG_AIM
			Logger::log << "[UpdateCache] Found Right Wrist @ " << i << std::endl;
#endif
			cachedViewModel.rightWristIndex = i;
		}
		else if (cachedViewModel.gunIndex == -1 && strstr(CurrentBone.BoneName, "gun"))
		{
#if DRAW_DEBUG_AIM
			Logger::log << "[UpdateCache] Found Gun @ " << i << std::endl;
#endif
			cachedViewModel.gunIndex = i;
		}
		else if (cachedViewModel.gunIndex == -1 && strstr(CurrentBone.BoneName, "body"))
		{
#if DRAW_DEBUG_AIM
			Logger::log << "[UpdateCache] Found Gun @ " << i << std::endl;
#endif
			cachedViewModel.gunIndex = i;
		}
		else if (cachedViewModel.displayIndex == -1 && strstr(CurrentBone.BoneName, "display"))
		{
#if DRAW_DEBUG_AIM
			Logger::log << "[UpdateCache] Found Display @ " << i << std::endl;
#endif
			cachedViewModel.displayIndex = i;
		}
#if DRAW_DEBUG_AIM
		else
		{
			Logger::log << "[UpdateCache] Skipped Bone " << CurrentBone.BoneName << std::endl;
		}
#endif
	}

	for (int i = 0; i < animationData->NumBones && i < 64; i++)
	{
		if (IsMagazineBoneName(boneArray[i].BoneName))
		{
			MarkMagazineBone(i);
			cachedViewModel.bHasMagazineBones = true;

			if (_stricmp(boneArray[i].BoneName, "frame magazine") == 0)
			{
				cachedViewModel.magazineRootBoneIndex = i;
			}
			else if (cachedViewModel.magazineRootBoneIndex < 0)
			{
				cachedViewModel.magazineRootBoneIndex = i;
			}

#if DRAW_DEBUG_AIM
			Logger::log << "[UpdateCache] Found magazine bone " << boneArray[i].BoneName << " @ " << i << std::endl;
#endif
		}
	}

	if (cachedViewModel.bHasMagazineBones)
	{
		const char* rootName = cachedViewModel.magazineRootBoneIndex >= 0
			? boneArray[cachedViewModel.magazineRootBoneIndex].BoneName
			: "unknown";

		if (cachedViewModel.magazineRootBoneIndex >= 0)
		{
			MarkMagazineDescendants(boneArray, animationData->NumBones, cachedViewModel.magazineRootBoneIndex);
		}

		// Also pick up any bones parented under an already-marked magazine bone.
		bool bMarkChanged = true;
		while (bMarkChanged)
		{
			bMarkChanged = false;
			for (int i = 0; i < animationData->NumBones && i < 64; i++)
			{
				if (cachedViewModel.magazineHideBones[i])
				{
					continue;
				}

				const int parentIndex = boneArray[i].Parent;
				if (parentIndex >= 0 && parentIndex < 64 && cachedViewModel.magazineHideBones[parentIndex])
				{
					MarkMagazineBone(i);
					bMarkChanged = true;
				}
			}
		}

		int markedBoneCount = 0;
		for (int i = 0; i < 64; i++)
		{
			if (cachedViewModel.magazineHideBones[i])
			{
				markedBoneCount++;
			}
		}

		Logger::log << "[WeaponHandler] Magazine bone cached: index "
			<< cachedViewModel.magazineRootBoneIndex << " (\"" << rootName << "\")"
			<< " markedBones=" << markedBoneCount << std::endl;
	}

	{
		std::string weaponAssetPath;
		BaseDynamicObject* player = Helpers::GetLocalPlayer();
		if (player)
		{
			BaseDynamicObject* weaponObj = Helpers::GetDynamicObject(player->weapon);
			if (weaponObj)
			{
				Asset_Weapon* weapon = Helpers::GetTypedAsset<Asset_Weapon>(weaponObj->tagID);
				if (weapon)
				{
					weaponAssetPath = weapon->WeaponAsset;
				}
			}
		}

		LogViewModelBoneHierarchy(animationData, weaponAssetPath.empty() ? nullptr : weaponAssetPath.c_str());
	}

	cachedViewModel.fireOffset = Vector3();
	cachedViewModel.cookedFireOffset = Vector3();
	cachedViewModel.cookedFireRotation = Matrix3();
	cachedViewModel.gunOffset = Vector3();

	// weapon model can be found from this chain:
	// player->WeaponID (DynamicObject)->WeaponID (weapon Asset)->WeaponData->ViewModelID (GBX Asset)
	// The local offset of the fire VFX (i.e. end of the barrel) is found in the "primary trigger" socket

	BaseDynamicObject* player = Helpers::GetLocalPlayer();
	if (!player)
	{
		Logger::log << "[UpdateCache] Can't find local player" << std::endl;
		return;
	}

	BaseDynamicObject* weaponObj = Helpers::GetDynamicObject(player->weapon);
	if (!weaponObj)
	{
		Logger::log << "[UpdateCache] Can't find weapon from WeaponID " << player->weapon << std::endl;
		Logger::log << "[UpdateCache] Player Tag = " << player->tagID << std::endl;
		return;
	}

	Asset_Weapon* weapon = Helpers::GetTypedAsset<Asset_Weapon>(weaponObj->tagID);
	if (!weapon)
	{
		Logger::log << "[UpdateCache] Can't find weapon asset from TagID " << weaponObj->tagID << std::endl;
		return;
	}

	cachedViewModel.weaponType = GetWeaponType(weapon);

	if (!weapon->WeaponData)
	{
		Logger::log << "[UpdateCache] Can't find weapon data in weapon asset " << weaponObj->tagID << std::endl;
		Logger::log << "[UpdateCache] Weapon Type = " << weapon->GroupID << std::endl;
		Logger::log << "[UpdateCache] Weapon Path = " << weapon->WeaponAsset << std::endl;
		return;
	}


	Asset_GBXModel* model = Helpers::GetTypedAsset<Asset_GBXModel>(weapon->WeaponData->ViewModelID);
	if (!model)
	{
		Logger::log << "[UpdateCache] Can't find GBX model from ViewModelID = " << weapon->WeaponData->ViewModelID << std::endl;
		return;
	}

#if DRAW_DEBUG_AIM
	std::string reversedGroup;
	reversedGroup.assign(model->GroupID, 4);
	std::reverse(reversedGroup.begin(), reversedGroup.end());
	Logger::log << "[UpdateCache] GBXModelTag = " << reversedGroup << std::endl;
	Logger::log << "[UpdateCache] GBXModelPath = " << model->ModelPath << std::endl;
#endif

	if (!model->ModelData)
	{
		return;
	}

#if DRAW_DEBUG_AIM
	Logger::log << "[UpdateCache] NumSockets = " << model->ModelData->NumSockets << std::endl;
#endif

	for (int i = 0; i < model->ModelData->NumSockets; i++)
	{
		GBXSocket& socket = model->ModelData->Sockets[i];
#if DRAW_DEBUG_AIM
		Logger::log << "[UpdateCache] Socket = " << socket.SocketName << std::endl;
#endif

		if (strstr(socket.SocketName, "primary trigger"))
		{
#if DRAW_DEBUG_AIM
			Logger::log << "[UpdateCache] Found Effects location, Num Transforms = " << socket.NumTransforms << std::endl;
#endif

			if (socket.NumTransforms == 0)
			{
				Logger::log << "[UpdateCache] " << socket.SocketName << " has no transforms" << std::endl;
				break;
			}

			cachedViewModel.cookedFireOffset = socket.Transforms[0].Position;
			Transform rotation;
			Helpers::MakeTransformFromQuat(&socket.Transforms[0].QRotation, &rotation);
			cachedViewModel.cookedFireRotation = rotation.rotation;

#if DRAW_DEBUG_AIM
			Logger::log << "[UpdateCache] Position = " << socket.Transforms[0].Position << std::endl;
			Logger::log << "[UpdateCache] Quaternion = " << socket.Transforms[0].QRotation << std::endl;
#endif
			break;
		}
	}

	bHasCapturedGrip = false;
	gripFromWristLocal.identity();
}

inline WeaponType WeaponHandler::GetWeaponType(Asset_Weapon* weapon) const
{
	WeaponType foundType = WeaponType::Unknown;
	if (strstr(weapon->WeaponAsset, "\\plasma pistol\\"))
	{
		foundType = WeaponType::PlasmaPistol;
	}
	else if (strstr(weapon->WeaponAsset, "\\sniper rifle\\"))
	{
		foundType = WeaponType::Sniper;
	}
	else if (strstr(weapon->WeaponAsset, "\\pistol\\"))
	{
		foundType = WeaponType::Pistol;
	}
	else if (strstr(weapon->WeaponAsset, "\\plasma rifle\\"))
	{
		foundType = WeaponType::PlasmaRifle;
	}
	else if (strstr(weapon->WeaponAsset, "\\shotgun\\"))
	{
		foundType = WeaponType::Shotgun;
	}
	else if (strstr(weapon->WeaponAsset, "\\assault rifle\\"))
	{
		foundType = WeaponType::AssaultRifle;
	}
	else if (strstr(weapon->WeaponAsset, "\\rocket launcher\\"))
	{
		foundType = WeaponType::RocketLauncher;
	}
	else if (strstr(weapon->WeaponAsset, "\\flamethrower\\"))
	{
		foundType = WeaponType::Flamethrower;
	}
	else if (strstr(weapon->WeaponAsset, "\\plasma_cannon\\"))
	{
		foundType = WeaponType::PlasmaCannon;
	}
	else if (strstr(weapon->WeaponAsset, "\\needler\\"))
	{
		foundType = WeaponType::Needler;
	}
	else if (strstr(weapon->WeaponAsset, "\\fuel rod\\"))
	{
		foundType = WeaponType::FuelRod;
	}
	else
	{
		Logger::log << "[UpdateCache] Unknown weapon with asset " << weapon->WeaponAsset << std::endl;
	}

	return foundType;
}

inline void WeaponHandler::TransformToMatrix4(Transform& inTransform, Matrix4& outMatrix) const
{
	// Assumes scale of 1!
	for (int x = 0; x < 3; x++)
	{
		for (int y = 0; y < 3; y++)
		{
			// Not sure why get is const, you can directly set the values with setrow/setcolumn anyway
			const_cast<float*>(outMatrix.get())[x + y * 4] = inTransform.rotation[x + y * 3];
		}
	}
	outMatrix.setColumn(3, inTransform.translation);
}

inline void WeaponHandler::ApplyMatrixToTransform(const Matrix4& matrix, Transform& outTransform) const
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

Vector3 WeaponHandler::GetScopeLocation(WeaponType type) const
{
	Vector3 Scale = Game::instance.bLeftHanded ? Vector3(1.0f, -1.0f, 1.0f) : Vector3(1.0f, 1.0f, 1.0f);

	switch (type)
	{
	case WeaponType::RocketLauncher:
		return Game::instance.c_ScopeOffsetRocket->Value() * Scale;
	case WeaponType::Sniper:
		return Game::instance.c_ScopeOffsetSniper->Value() * Scale;
	case WeaponType::Unknown:
	case WeaponType::Pistol:
		return Game::instance.c_ScopeOffsetPistol->Value() * Scale;
	}
	return Vector3(0.0f, 0.0f, 0.0f);
}

Matrix4 WeaponHandler::GetDominantHandTransform() const
{
	Matrix4 controllerTransform;
	Vector3 actualControllerPos;
	Vector3 toOffHand;
	Vector3 smoothedPosition; 

	if (!Game::instance.GetCalculatedHandPositions(controllerTransform, actualControllerPos, toOffHand))
	{
		return controllerTransform;
	}

	Vector3 upVector = controllerTransform.getForwardAxis();

	// In the unlikely event the player decides to put their hand directly above their other hand, avoid a DIV/0 error when doing the lookat
	if (upVector.dot(toOffHand) == 1.0f)
	{
		upVector += controllerTransform.getUpAxis() * 0.001f;
	}

	/*
	Matrix3 rot;
	for (int i = 0; i < 3; i++)
	{
		offHandTransform.setColumn(i, &rot.get()[i * 4]);
	}
	*/
	smoothedPosition = Game::instance.GetSmoothedInput();
	controllerTransform.lookAt(smoothedPosition, upVector);

	controllerTransform.translate(-actualControllerPos);
	controllerTransform.rotate(-90.0f, controllerTransform.getUpAxis());
	controllerTransform.rotate(-90.0f, controllerTransform.getLeftAxis());
	controllerTransform.translate(actualControllerPos);

	// Apply offset from weapon aiming here
	Matrix4 cachedRot4;

	for (int i = 0; i < 3; i++)
	{
		cachedRot4.setColumn(i, cachedViewModel.fireRotation.getColumn(i));
	}

	controllerTransform *= cachedRot4.invertAffine();
	return controllerTransform;
}

bool WeaponHandler::GetLocalWeaponAim(Vector3& outPosition, Vector3& outAim, Vector3& upDir) const
{
	HaloID playerID;
	if (!Helpers::GetLocalPlayerID(playerID))
	{
		return false;
	}

	UnitDynamicObject* player = static_cast<UnitDynamicObject*>(Helpers::GetDynamicObject(playerID));
	if (!player)
	{
		return false;
	}

	// TODO: Handedness
	Matrix4 controllerPos = GetDominantHandTransform();

	Vector3 handPos = controllerPos * Vector3(0.0f, 0.0f, 0.0f);
	Matrix4 handRotation = controllerPos.translate(-handPos);

	Matrix3 handRotation3;

	for (int i = 0; i < 3; i++)
	{
		handRotation3.setColumn(i, &handRotation.get()[i * 4]);
	}

	Matrix3 finalRot;

	if (Game::instance.bUse3DOFAiming)
	{
		// 3DOF MODE: Use pure controller rotation (matching bullet direction in RelocatePlayer)
		// fireRotation is NOT updated in 3DOF mode, so skip it
		finalRot = handRotation3;

		// Position from HMD instead of controller
		Matrix4 hmdTransform = Game::instance.GetVR()->GetHMDTransform(true);
		Vector3 hmdPos = hmdTransform * Vector3(0.0f, 0.0f, 0.0f);
		outPosition = hmdPos;
	}
	else
	{
		// 6DOF MODE: Use fireRotation offset (existing behavior)
		finalRot = cachedViewModel.fireRotation * handRotation3;
		outPosition = handPos + handRotation * cachedViewModel.fireOffset * Game::instance.WorldToMetres(1.0f);
	}

	outAim = finalRot * Vector3(1.0f, 0.0f, 0.0f);
	upDir = finalRot * Vector3(0.0f, 0.0f, 1.0f);

#if DRAW_DEBUG_AIM
	// N.b. - This function is in local (i.e. vr) coordinate space, convert to world for debug to be correct
	Vector3 worldOutPos = Helpers::GetCamera().position + outPosition * Game::instance.MetresToWorld(1.0f);
	Game::instance.inGameRenderer.DrawCoordinate(worldOutPos, finalRot, 0.02f);
	Vector3 worldHandPos = Helpers::GetCamera().position + handPos * Game::instance.MetresToWorld(1.0f);
	Game::instance.inGameRenderer.DrawCoordinate(worldHandPos, handRotation3, 0.015f, false);

	Vector3 aimTarget = worldOutPos + outAim * Game::instance.MetresToWorld(Game::instance.c_CrosshairDistance->Value());
	Game::instance.inGameRenderer.DrawLine3D(worldOutPos, aimTarget, D3DCOLOR_ARGB(255, 255, 20, 20));

	Game::instance.inGameRenderer.DrawLine3D(lastFireLocation, lastFireAim, D3DCOLOR_ARGB(255, 20, 255, 255));
#endif

	return true;
}

bool WeaponHandler::GetWorldWeaponAim(Vector3& outPosition, Vector3& outAim, Vector3& upDir) const
{
	bool bSuccess = GetLocalWeaponAim(outPosition, outAim, upDir);

	outPosition = Helpers::GetCamera().position + outPosition * Game::instance.MetresToWorld(1.0f);

	return bSuccess;
}


bool WeaponHandler::GetLocalWeaponScope(Vector3& outPosition, Vector3& outAim, Vector3& upDir) const
{
	HaloID playerID;
	if (!Helpers::GetLocalPlayerID(playerID))
	{
		return false;
	}

	UnitDynamicObject* player = static_cast<UnitDynamicObject*>(Helpers::GetDynamicObject(playerID));
	if (!player)
	{
		return false;
	}

	// TODO: Handedness
	Matrix4 controllerPos = GetDominantHandTransform();

	Vector3 handPos = controllerPos * Vector3(0.0f, 0.0f, 0.0f);
	Matrix4 handRotation = controllerPos.translate(-handPos);

	Matrix3 handRotation3;

	for (int i = 0; i < 3; i++)
	{
		handRotation3.setColumn(i, &handRotation.get()[i * 4]);
	}

	Matrix3 finalRot;

	if (Game::instance.bUse3DOFAiming)
	{
		// 3DOF MODE: Use pure controller rotation (matching bullet direction in RelocatePlayer)
		// fireRotation is NOT updated in 3DOF mode, so skip it
		finalRot = handRotation3;

		// Position from HMD instead of controller
		Matrix4 hmdTransform = Game::instance.GetVR()->GetHMDTransform(true);
		Vector3 hmdPos = hmdTransform * Vector3(0.0f, 0.0f, 0.0f);
		outPosition = hmdPos;
	}
	else
	{
		// 6DOF MODE: Use fireRotation offset and scope position (existing behavior)
		finalRot = cachedViewModel.fireRotation * handRotation3;

		Vector3 scopeOffset = GetScopeLocation(cachedViewModel.weaponType);
		Vector3 gunOffset = handPos + handRotation * cachedViewModel.gunOffset * Game::instance.WorldToMetres(1.0f);
		outPosition = gunOffset + finalRot * scopeOffset;
	}

	outAim = finalRot * Vector3(1.0f, 0.0f, 0.0f);
	upDir = finalRot * Vector3(1.0f, 0.0f, 1.0f);

#if DRAW_DEBUG_AIM
	// N.b. - This function is in local (i.e. vr) coordinate space, convert to world for debug to be correct
	Vector3 worldOutPos = Helpers::GetCamera().position + outPosition * Game::instance.MetresToWorld(1.0f);
	Game::instance.inGameRenderer.DrawCoordinate(worldOutPos, finalRot, 0.02f);
	Vector3 worldHandPos = Helpers::GetCamera().position + handPos * Game::instance.MetresToWorld(1.0f);
	Game::instance.inGameRenderer.DrawCoordinate(worldHandPos, handRotation3, 0.015f, false);

	Vector3 aimTarget = worldOutPos + outAim * Game::instance.MetresToWorld(Game::instance.c_CrosshairDistance->Value());
	Game::instance.inGameRenderer.DrawLine3D(worldOutPos, aimTarget, D3DCOLOR_ARGB(255, 255, 20, 20));
#endif

	return true;
}

bool WeaponHandler::GetWorldWeaponScope(Vector3& outPosition, Vector3& outAim, Vector3& upDir) const
{
	bool bSuccess = GetLocalWeaponScope(outPosition, outAim, upDir);

	outPosition = Helpers::GetCamera().position + outPosition * Game::instance.MetresToWorld(1.0f);

	return bSuccess;
}

bool WeaponHandler::IsSniperScope() const
{
	return cachedViewModel.weaponType == WeaponType::Sniper;
}

void WeaponHandler::RelocatePlayer(HaloID& PlayerID)
{
	// Teleport the player to the controller position so the bullet comes from there instead
	weaponFiredPlayer = static_cast<UnitDynamicObject*>(Helpers::GetDynamicObject(PlayerID));
	if (weaponFiredPlayer)
	{
		// TODO: Handedness
		Matrix4 controllerPos = GetDominantHandTransform();

		// Apply scale only to translation portion
		Vector3 translation = controllerPos * Vector3(0.0f, 0.0f, 0.0f);
		controllerPos.translate(-translation);
		translation *= Game::instance.MetresToWorld(1.0f);
		translation += weaponFiredPlayer->position;

		controllerPos.translate(translation);

		Vector3 handPos = controllerPos * Vector3(0.0f, 0.0f, 0.0f);
		Matrix4 handRotation = controllerPos.translate(-handPos);

		Matrix3 handRotation3;

		for (int i = 0; i < 3; i++)
		{
			handRotation3.setColumn(i, &handRotation.get()[i * 4]);
		}

		// Cache the real values so we can restore them after running the original fire function
		realPlayerPosition = weaponFiredPlayer->position;
		realPlayerAim = weaponFiredPlayer->aim;

		if (Game::instance.bUse3DOFAiming)
		{
			// 3DOF MODE: Bullets originate from HMD (user's eyes)
			// Get HMD position in VR space
			Matrix4 hmdTransform = Game::instance.GetVR()->GetHMDTransform(true);
			Vector3 hmdPos = hmdTransform * Vector3(0.0f, 0.0f, 0.0f);

			// Convert to game world units and add to player base position
			Vector3 hmdPosWorld = hmdPos * Game::instance.MetresToWorld(1.0f);
			weaponFiredPlayer->position = realPlayerPosition + hmdPosWorld;

			// Aim direction uses controller rotation directly
			// Note: fireRotation is NOT updated in 3DOF mode (skipped in UpdateViewModel),
			// so we use handRotation3 directly for aim direction
			weaponFiredPlayer->aim = handRotation3 * Vector3(1.0f, 0.0f, 0.0f);
		}
		else
		{
			// 6DOF MODE: Bullets originate from hand/gun barrel (existing behavior)
			weaponFiredPlayer->position = handPos + handRotation * cachedViewModel.fireOffset;
			weaponFiredPlayer->aim = (cachedViewModel.fireRotation * handRotation3) * Vector3(1.0f, 0.0f, 0.0f);
		}

#if DRAW_DEBUG_AIM
		Vector3 internalFireOffset = Helpers::GetCamera().position - realPlayerPosition;
		lastFireLocation = weaponFiredPlayer->position + internalFireOffset;
		lastFireAim = lastFireLocation + weaponFiredPlayer->aim * 1.0f;
		Logger::log << "FireOffset: " << cachedViewModel.fireOffset << std::endl;
		Logger::log << "FireAim: " << cachedViewModel.fireRotation << std::endl;
		Logger::log << "Player Position: " << realPlayerPosition << std::endl;
		Logger::log << "Last fire location: " << lastFireLocation << std::endl;
#endif
	}
}

void WeaponHandler::PreFireWeapon(HaloID& WeaponID, short param2)
{
	BaseDynamicObject* Object = Helpers::GetDynamicObject(WeaponID);

	weaponFiredPlayer = nullptr;

	// Check if the weapon is being used by the player
	HaloID PlayerID;
	bool foundPlayer = Helpers::GetLocalPlayerID(PlayerID);
	if (Object && foundPlayer && PlayerID == Object->parent)
	{
		HandleWeaponHaptics();
		Game::instance.weaponHapticsConfig.WeaponFired(cachedViewModel.weaponType);
		RelocatePlayer(PlayerID);
	}
}

void WeaponHandler::SetPlasmaPistolCharge()
{
	HaloID PlayerID;
	bool foundPlayer = Helpers::GetLocalPlayerID(PlayerID);

	if (foundPlayer && cachedViewModel.weaponType == WeaponType::PlasmaPistol)
	{
		Game::instance.weaponHapticsConfig.SetPlasmaPistolCharging();
	}
}

void WeaponHandler::HandlePlasmaPistolCharge()
{
	IVR* vr = Game::instance.GetVR();

	if (vr && cachedViewModel.weaponType == WeaponType::PlasmaPistol)
	{
		bool isCharging = Game::instance.weaponHapticsConfig.IsPlasmaPistolCharging();

		if (isCharging)
		{
			HandleWeaponHaptics();
		}
	}
}

inline void WeaponHandler::HandleWeaponHaptics() const
{
	IVR* vr = Game::instance.GetVR();
	
	if (cachedViewModel.weaponType != WeaponType::Unknown)
	{
		WeaponHapticsConfigManager hapticsManager = Game::instance.weaponHapticsConfig;
		WeaponHaptic haptic = hapticsManager.GetWeaponHaptics(cachedViewModel.weaponType);

#if HAPTICS_DEBUG
		Logger::log << "[Weapon Haptics] triggering haptics for weapon " << haptic.Description << std::endl;
#endif

		ControllerRole dominantHand = ControllerRole::Right;
		ControllerRole nondominantHand = ControllerRole::Left;
		
		if (Game::instance.bLeftHanded)
		{
#if HAPTICS_DEBUG
			Logger::log << "[Weapon Haptics] Left is hand dominant hand. " << haptic.Description << std::endl;
#endif

			dominantHand = ControllerRole::Left;
			nondominantHand = ControllerRole::Right;
		}
		else
		{
#if HAPTICS_DEBUG
			Logger::log << "[Weapon Haptics] Right is hand dominant hand. " << haptic.Description << std::endl;
#endif
		}

		if (Game::instance.bUseTwoHandAim)
		{
#if HAPTICS_DEBUG
			Logger::log << "[Weapon Haptics] Gun is in two handed mode. " << haptic.Description << std::endl;
#endif
			WeaponHapticArg dominantHaptics = haptic.TwoHand.Dominant;
			WeaponHapticArg nondominantHaptics = haptic.TwoHand.Nondominant;
			hapticsManager.HandleWeaponHaptics(vr, dominantHand, dominantHaptics);
			hapticsManager.HandleWeaponHaptics(vr, nondominantHand, nondominantHaptics);
		}
		else 
		{
#if HAPTICS_DEBUG
			Logger::log << "[Weapon Haptics] Gun is in one handed mode. " << haptic.Description << std::endl;
#endif
			WeaponHapticArg hapticArgs = haptic.OneHand;
			hapticsManager.HandleWeaponHaptics(vr, dominantHand, hapticArgs);
		}
	}
	else
	{
		Logger::log << "[Weapon Haptics] Attempted to find gun haptics for weapon type " << static_cast<int>(cachedViewModel.weaponType) << " but vrinput was null" << std::endl;
	}
}

void WeaponHandler::PostFireWeapon(HaloID& weaponID, short param2)
{
	// Restore state after firing the weapon
	if (weaponFiredPlayer)
	{
		weaponFiredPlayer->position = realPlayerPosition;
		weaponFiredPlayer->aim = realPlayerAim;
		weaponFiredPlayer = nullptr;
	}
}

void WeaponHandler::PreThrowGrenade(HaloID& playerID)
{
	weaponFiredPlayer = nullptr;

	// Check if the weapon is being used by the player
	HaloID PlayerID;
	if (Helpers::GetLocalPlayerID(PlayerID) && PlayerID == playerID)
	{
		RelocatePlayer(PlayerID);
	}
}

void WeaponHandler::PostThrowGrenade(HaloID& playerID)
{
	if (weaponFiredPlayer)
	{
		weaponFiredPlayer->position = realPlayerPosition;
		weaponFiredPlayer->aim = realPlayerAim;
		weaponFiredPlayer = nullptr;
	}
}

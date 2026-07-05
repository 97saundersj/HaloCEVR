#pragma once

#include <cstdint>
#include "../Maths/Matrices.h"
#include "../Maths/Vectors.h"
#include "../WeaponHandler.h"
#include "Maths.h"
#include "Objects.h"
#include "SkeletonAnim.h"

enum class EManualReloadPhase
{
	Idle,
	PlayingEject,
	PausedAtEject,
	PlayingFinish
};

class InputHandler;
struct AssetData_ModelAnimations;
struct Bone;

class ManualReloadController
{
public:
	explicit ManualReloadController(InputHandler& inputHandler);

	// InputHandler
	void Update();
	void SuppressVanillaReloadControl(unsigned char& reloadControl) const;
	bool ShouldSuppressTwoHandAim() const;
	bool ShouldSkipWeaponHandSwap() const;
	void TickSwapSuppression();

	// Game
	void OnReloadEnd();

	// Hooks
	bool ShouldBlockAutoReloadStart() const;
	void OnPreHandleInputs();

	// WeaponHandler — cache reload metadata when the view-model asset changes.
	void OnViewModelCached(const HaloID& id, AssetData_ModelAnimations* animationData, WeaponType weaponType);
	void PreSkeleton(const HaloID& id, TransformQuat* boneTransforms);
	void PostSkeleton(HaloID& id, Vector3* pos, Vector3* facing, Vector3* up, Transform* outBoneTransforms);

private:
	InputHandler& input;

	// View-model reload metadata (owned here, not on WeaponHandler).
	HaloID cachedViewModelAsset{ 0, 0 };
	WeaponType cachedWeaponType = WeaponType::Unknown;
	bool magazineHideBones[64]{};
	bool bHasMagazineBones = false;
	int magazineRootBoneIndex = -1;
	int reloadEmptyAnimIndex = -1;
	int reloadExitEmptyAnimIndex = -1;
	int reloadFullAnimIndex = -1;
	int reloadExitFullAnimIndex = -1;
	uint16_t magazineCapacity = 0;

	EManualReloadPhase phase = EManualReloadPhase::Idle;
	bool bMagazineEjected = false;
	bool bMagazineGrabbed = false;
	bool bManualReloadPending = false;
	bool bManualReloadFromEmpty = false;
	bool bShotgunShellSessionActive = false;
	bool bShotgunShellSessionUserCancelled = false;
	uint16_t pausedReloadAnimIndex = 0;
	uint16_t pausedReloadAnimFrame = 0;
	uint16_t frozenReloadRemaining = 0;
	uint16_t initialReloadRemaining = 0;

	bool bBeltGripStartedReload = false;
	bool bSuppressSwapUntilGripRelease = false;
	int finishFrameCounter = 0;

	// Bone pin / record-replay (engine FP reload is not seekable).
	SkeletonAnim::SampleBuffer boneReplay;
	float replaySkipSeconds = 0.0f;
	int lastBonePinPhase = 0;

	// Mag-well insert target (gun-local at reload start).
	bool bHasReloadStartMagSocket = false;
	Vector3 reloadStartMagLocalOffset{};
	Vector3 magazineSocketPosition{};

	// Magazine-in-wrist local pose from the resume-tick animation frame.
	bool bHasCapturedGrip = false;
	Matrix4 gripFromWristLocal;

	void ResetState();
	void ResetCycle();
	void ResetCycleCore();
	void EndShotgunShellSession();
	void ClearReloadMetadata();
	void CacheReloadMetadata(const HaloID& id, AssetData_ModelAnimations* animationData, WeaponType weaponType);
	void MarkMagazineBone(int boneIndex);
	void MarkMagazineDescendants(Bone* boneArray, int numBones, int rootIndex);

	bool HasMagazineBones() const { return bHasMagazineBones; }
	bool IsMagazineBone(int boneIndex) const;
	int GetMagazineRootBoneIndex() const;
	bool IsLocalMagazineEmpty() const;
	bool IsShellByShellReloadWeapon() const;
	bool CanLoadAnotherShell() const;
	bool ShouldContinueContinuousReloadSession() const;
	bool ShouldAutoStartContinuousReloadSession() const;
	bool ShouldShowBeltMagazine() const;
	int GetActiveReloadAnimIndex() const;
	bool IsLocalViewModel(const HaloID& id) const;

	void TriggerWeaponReload();
	void TriggerWeaponReloadEnd();
	void BeginManualReload();
	void BeginChainedShellReload();
	void ResumeManualReloadAnimation();
	void HandleManualMagazineGrabInsert();
	void TryBeginShotgunLoadFromBelt();
	void SuspendShotgunActiveReloadForFire();
	bool ShouldSuspendShotgunActiveReloadForFire() const;

	void ApplyAnimPin();
	void UpdateReloadAnimationPause();
	void ApplyBonePin(const HaloID& id, TransformQuat* boneTransforms);
	void UpdateMagazinePlacement(const HaloID& id, Transform* outBoneTransforms);
	void CaptureReloadStartInsertSocket(const Transform* outBoneTransforms);
	void UpdateInsertSocketFromGun(const Transform* outBoneTransforms);
	void CaptureGripFromResumePose(HaloID& id, Vector3* pos, Vector3* facing, Vector3* up);
	void ClearBoneSnapshot();
	void ResetBonePinState();
	void ClearReloadStartInsertSocket();

	Vector3 GetBeltMagazineWorldPosition() const;
	Vector3 GetOffHandWorldPosition() const;
	Vector3 GetMagazineGripWorldOffset() const;
	bool GetOffHandNearBelt(bool& gripHeld, bool& gripChanged) const;
	bool GetGrabbedMagazineTargetMatrix(const Transform* outBoneTransforms, Matrix4& outTargetMatrix) const;
	Matrix4 GetDetachedMagazineOrientation(const Transform* outBoneTransforms) const;

	void BeginSoundCapture();
	void PauseSounds();
	void ClearSounds();
	void ResumeSounds(bool bStopActiveSources);
};

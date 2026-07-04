#pragma once

#include <cstdint>
#include "../Maths/Matrices.h"
#include "../Maths/Vectors.h"
#include "Maths.h"
#include "Objects.h"

enum class EPhysicalReloadPhase
{
	Idle,
	PlayingEject,
	PausedAtEject,
	PlayingFinish
};

class InputHandler;

class PhysicalReloadController
{
public:
	explicit PhysicalReloadController(InputHandler& inputHandler);

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

	// WeaponHandler
	void OnWeaponChanged();
	void PreSkeleton(const HaloID& id, TransformQuat* boneTransforms);
	void PostSkeleton(HaloID& id, Vector3* pos, Vector3* facing, Vector3* up, Transform* outBoneTransforms);

private:
	InputHandler& input;

	EPhysicalReloadPhase phase = EPhysicalReloadPhase::Idle;
	bool bMagazineEjected = false;
	bool bMagazineGrabbed = false;
	bool bManualPhysicalReloadPending = false;
	bool bPhysicalReloadFromEmpty = false;
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
	static constexpr int kMaxReloadReplayFrames = 320;
	TransformQuat pausedBoneTransforms[64]{};
	bool bHasPausedBoneSnapshot = false;
	TransformQuat reloadReplayFrames[kMaxReloadReplayFrames][64]{};
	float reloadReplayTimes[kMaxReloadReplayFrames]{};
	int reloadReplayCount = 0;
	bool reloadRecordComplete = false;
	float reloadRecordTargetSeconds = 0.0f;
	double reloadPauseStartSeconds = -1.0;
	double reloadReplayStartSeconds = -1.0;
	float reloadReplaySkipSeconds = 0.0f;
	bool bReloadReplaying = false;
	bool bReloadReplayComplete = false;
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

	bool ShouldContinueContinuousReloadSession() const;
	bool ShouldAutoStartContinuousReloadSession() const;
	bool ShouldShowBeltMagazine() const;
	int GetActiveReloadAnimIndex() const;

	void TriggerWeaponReload();
	void TriggerWeaponReloadEnd();
	void BeginPhysicalReload();
	void BeginChainedShellReload();
	void ResumePhysicalReloadAnimation();
	void HandlePhysicalMagazineGrabInsert();
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
	void RelocateMagazineBones(Transform* outBoneTransforms, const Vector3& targetRootPos, const Matrix4& targetRootOrientation) const;

	void BeginSoundCapture();
	void PauseSounds();
	void ClearSounds();
	void ResumeSounds(bool bStopActiveSources);
};

#pragma once
#include <cstdint>
#include "Maths/Vectors.h"
#include "Maths/Matrices.h"
#include "Helpers/Maths.h"
#include "Helpers/Objects.h"

#define DRAW_DEBUG_AIM 0

enum class WeaponType
{
	Unknown,
	Pistol,
	AssaultRifle,
	Shotgun,
	RocketLauncher,
	Sniper,
	Flamethrower,
	PlasmaPistol,
	PlasmaRifle,
	PlasmaCannon,
	Needler,
	FuelRod
};

class WeaponHandler
{
public:
	void UpdateViewModel(struct HaloID& id, struct Vector3* pos, struct Vector3* facing, struct Vector3* up, struct TransformQuat* boneTransforms, struct Transform* outBoneTransforms);
	void HandlePlasmaPistolCharge();
	void SetPlasmaPistolCharge();
	void PreFireWeapon(HaloID& weaponID, short param2);
	void PostFireWeapon(HaloID& weaponID, short param2);
	void PreThrowGrenade(HaloID& playerID);
	void PostThrowGrenade(HaloID& playerID);

	bool GetLocalWeaponAim(Vector3& outPosition, Vector3& outAim, Vector3& upDir) const;
	bool GetWorldWeaponAim(Vector3& outPosition, Vector3& outAim, Vector3& upDir) const;
	bool GetLocalWeaponScope(Vector3& outPosition, Vector3& outAim, Vector3& upDir) const;
	bool GetWorldWeaponScope(Vector3& outPosition, Vector3& outAim, Vector3& upDir) const;

	bool IsSniperScope() const;

	bool IsLocalMagazineEmpty() const;
	bool HasMagazineBones() const;
	bool ShouldShowBeltMagazine() const;
	bool ShouldUsePhysicalMagazineReload() const;
	bool SupportsPhysicalMagazineReload() const;
	bool IsShellByShellReloadWeapon() const;
	bool CanLoadAnotherShell() const;
	Vector3 GetBeltMagazineWorldPosition() const;
	Vector3 GetMagazineSocketWorldPosition() const;
	int GetReloadEmptyAnimIndex() const;
	int GetReloadExitEmptyAnimIndex() const;
	int GetReloadFullAnimIndex() const;
	int GetReloadExitFullAnimIndex() const;
	int GetActiveReloadAnimIndex() const;
	int GetActiveReloadExitAnimIndex() const;
	WeaponType GetCachedWeaponType() const { return cachedViewModel.weaponType; }
	// Clears only the frozen eject pose; leaves the recorded replay buffer intact.
	void ClearPhysicalReloadBoneSnapshot();
	// Clears snapshot + replay buffer (new reload or full state reset).
	void ResetPhysicalReloadBonePinState();
	// Clears the mag-well insert target captured at reload start.
	void ClearReloadStartInsertSocket();
	// True once the recorded "rest of reload" bone playback (PlayingFinish) has reached its end.
	bool IsReloadReplayComplete() const { return bReloadReplayComplete; }
	// True when a usable rest-of-reload recording was captured during the eject pause.
	bool HasReloadReplay() const { return reloadReplayCount > 1; }
	// Seconds into the recorded reload to begin playback (skips virtual mag insert on resume).
	void SetReloadReplaySkipSeconds(float skipSeconds);

	Vector3 localOffset;
	Vector3 localRotation;

protected:
	void RelocatePlayer(HaloID& PlayerID);

	inline void CalculateBoneTransform(int boneIndex, struct Bone* boneArray, struct Transform& root, struct TransformQuat* boneTransforms, struct Transform& outTransform) const;
	inline void CalculateHandTransform(Vector3* pos, Matrix4& handTransform) const;
	inline void CreateEndCap(int boneIndex, const struct Bone& currentBone, struct Transform* outBoneTransforms) const;
	inline void MoveBoneToTransform(int boneIndex, const class Matrix4& newTransform, struct Transform* realTransforms, struct Transform* outBoneTransforms) const;
	inline void UpdateCache(struct HaloID& id, struct AssetData_ModelAnimations* animationData);
	inline WeaponType GetWeaponType(struct Asset_Weapon* weapon) const;
	inline void HandleWeaponHaptics() const;

	inline void TransformToMatrix4(struct Transform& inTransform, class Matrix4& outMatrix) const;

	bool IsFirstPersonWeaponAnimationsAsset(struct AssetData_ModelAnimations* animationData) const;
	void MarkMagazineBone(int boneIndex);
	void MarkMagazineDescendants(struct Bone* boneArray, int numBones, int rootIndex);
	void UpdatePhysicalMagazinePlacement(const HaloID& id, struct Transform* outBoneTransforms);
	void CaptureReloadStartInsertSocket(const struct Transform* outBoneTransforms);
	// Samples the resume-tick animation (from the replay buffer) to derive the magazine-in-wrist-space transform.
	void CaptureGripFromResumePose(struct HaloID& id, struct Vector3* pos, struct Vector3* facing, struct Vector3* up);
	bool GetGrabbedMagazineTargetMatrix(const struct Transform* outBoneTransforms, Matrix4& outTargetMatrix) const;
	void UpdateInsertSocketFromGun(const struct Transform* outBoneTransforms);
	void RelocateMagazineBones(struct Transform* outBoneTransforms, const Vector3& targetRootPos, const class Matrix4& targetRootOrientation);
	Matrix4 GetDetachedMagazineOrientation(const struct Transform* outBoneTransforms) const;
	void ApplyMatrixToTransform(const class Matrix4& matrix, struct Transform& outTransform) const;
	int ResolveMagazineRootBoneIndex() const;
	Vector3 GetOffHandWorldPosition() const;
	Vector3 GetMagazineGripWorldOffset() const;
	void LogViewModelBoneHierarchy(struct AssetData_ModelAnimations* animationData, const char* weaponAssetPath) const;
	void LogViewModelBoneHierarchyNode(struct Bone* boneArray, int numBones, int boneIndex, int depth) const;
	void ApplyPhysicalReloadBonePin(const HaloID& id, struct TransformQuat* boneTransforms);

	inline Vector3 GetScopeLocation(WeaponType Type) const;

	Matrix4 GetDominantHandTransform() const;

	struct ViewModelCache
	{
		HaloID currentAsset{ 0, 0 };
		int leftWristIndex = -1;
		int rightWristIndex = -1;
		int gunIndex = -1;
		int displayIndex = -1;
		Vector3 cookedFireOffset;
		Matrix3 cookedFireRotation;
		Vector3 fireOffset;
		Vector3 gunOffset;
		Matrix3 fireRotation;
		WeaponType weaponType = WeaponType::Unknown;
		bool IsShooting = false;
		bool magazineHideBones[64]{};
		bool bHasMagazineBones = false;
		int magazineRootBoneIndex = -1;
		int reloadEmptyAnimIndex = -1;
		int reloadExitEmptyAnimIndex = -1;
		int reloadFullAnimIndex = -1;
		int reloadExitFullAnimIndex = -1;
		uint16_t magazineCapacity = 0;
		Vector3 magazineSocketPosition{};

	} cachedViewModel;

	UnitDynamicObject* weaponFiredPlayer = nullptr;
	Vector3 realPlayerPosition;
	Vector3 realPlayerAim;

	// Smoothed facing direction for 3DOF mode weapon model
	Vector3 smoothed3DOFFacingDir = Vector3(1.0f, 0.0f, 0.0f);
	bool bWasIn3DOFMode = false;

	TransformQuat pausedBoneTransforms[64]{};
	bool bHasPausedBoneSnapshot = false;

	// Physical reload animation record/replay (see ApplyPhysicalReloadBonePin).
	// The game's first-person reload is a one-shot clip that runs to completion on its own clock
	// regardless of our timer freeze, and exposes no seekable frame field. So during the eject
	// pause we capture the live "rest of reload" bone stream (eject -> end) while showing the
	// frozen eject pose, then replay it over real time on insert so the remainder animates
	// instead of snapping to the finished pose.
	static constexpr int kMaxReloadReplayFrames = 320;
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
	int lastBonePinPhase = 0; // EPhysicalReloadPhase::Idle (defined in Helpers/PhysicalReload.h)

	// Magazine position in frame-gun local space at reload start (mag seated in well).
	bool bHasReloadStartMagSocket = false;
	Vector3 reloadStartMagLocalOffset{};

	// Magazine-bone-in-wrist-local-space, derived from the eject-frame animation.
	// Captures both translation and rotation so the mag sits exactly as the animation intends.
	bool bHasCapturedGrip = false;
	Matrix4 gripFromWristLocal;

	// Track previous yaw offset to detect snap turns
	float lastYawOffset = 0.0f;

	// Debug stuff for checking where bullets are coming from/going
#if DRAW_DEBUG_AIM
	mutable Vector3 lastFireLocation;
	mutable Vector3 lastFireAim;
#endif
};

#pragma once

#include "Maths.h"
#include "Objects.h"
#include "../Maths/Matrices.h"
#include "../Maths/Vectors.h"
#include <cstdint>

struct WeaponDynamicObject;

namespace SkeletonAnim
{
constexpr int kMaxBones = 64;

void TransformToMatrix4(const Transform& inTransform, Matrix4& outMatrix);
void ApplyMatrixToTransform(const Matrix4& matrix, Transform& outTransform);
Matrix4 GetRotationMatrix(const Matrix4& transform);

// World-space orientation helpers (pass in head/controller transforms from VR).
Vector3 FlattenForwardOnXY(Vector3 forward);
void GetLeveledBasisFromForward(const Vector3& flatForward, Vector3& outForward, Vector3& outLeft);
Matrix4 MakeBeltObjectOrientation(const Vector3& flatForward);

// Evaluate a skeleton pose from animation quats (no VR overrides).
void EvaluatePose(
	HaloID& id,
	Vector3* pos,
	Vector3* facing,
	Vector3* up,
	const TransformQuat* boneQuats,
	Transform* outBones);

Vector3 CapturePointInBoneSpace(const Transform& parentBone, const Vector3& worldPoint);
Vector3 ApplyPointInBoneSpace(const Transform& parentBone, const Vector3& localPoint);

Matrix4 CaptureChildInParentSpace(const Transform& parentBone, const Transform& childBone);
Matrix4 ApplyChildInParentSpace(const Transform& parentBone, const Matrix4& childLocal);

void RelocateBoneSubtree(
	Transform* bones,
	const bool* boneMask,
	int rootIndex,
	const Vector3& targetRootPos,
	const Matrix4& targetRootOrientation);

void PinWeaponFirstPersonAnimation(WeaponDynamicObject* weapon, uint16_t animId, uint16_t frame);

// Record animation quats during live playback and replay by time (engine anims are not seekable).
class SampleBuffer
{
public:
	static constexpr int kMaxSamples = 320;

	void Reset();

	bool HasSnapshot() const { return bHasSnapshot; }
	bool HasMultipleSamples() const { return sampleCount > 1; }
	bool IsCaptureComplete() const { return captureComplete; }
	bool IsReplaying() const { return bReplaying; }
	bool IsReplayComplete() const { return bReplayComplete; }

	int GetSampleCount() const { return sampleCount; }
	const TransformQuat* GetSampleQuats(int index) const;
	float GetSampleTime(int index) const;
	int FindSampleIndexAtOrAfter(float seconds) const;

	void BeginCapture(const TransformQuat* boneQuats, double now, float recordTargetSeconds);
	void TickCapture(TransformQuat* boneQuats, double now);

	void BeginReplay(double now, float skipSeconds);
	void ApplyReplay(TransformQuat* boneQuats, double now);

	void ClearSnapshot() { bHasSnapshot = false; }

private:
	TransformQuat pausedSnapshot[kMaxBones]{};
	bool bHasSnapshot = false;

	TransformQuat samples[kMaxSamples][kMaxBones]{};
	float sampleTimes[kMaxSamples]{};
	int sampleCount = 0;

	bool captureComplete = false;
	float captureTargetSeconds = 0.0f;
	double captureStartSeconds = -1.0;

	double replayStartSeconds = -1.0;
	float replaySkipSeconds = 0.0f;
	bool bReplaying = false;
	bool bReplayComplete = false;
};

}

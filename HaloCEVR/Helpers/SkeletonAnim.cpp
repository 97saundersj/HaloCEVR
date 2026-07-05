#include "SkeletonAnim.h"
#include "Assets.h"
#include "FirstPersonAnim.h"
#include <algorithm>
#include <cstring>

namespace SkeletonAnim
{

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

Matrix4 GetRotationMatrix(const Matrix4& transform)
{
	Matrix4 rotation;
	rotation.identity();
	for (int x = 0; x < 3; x++)
	{
		for (int y = 0; y < 3; y++)
		{
			const_cast<float*>(rotation.get())[x + y * 4] = transform.get()[x + y * 4];
		}
	}
	return rotation;
}

Vector3 FlattenForwardOnXY(Vector3 forward)
{
	forward.z = 0.0f;
	if (forward.lengthSqr() < 0.0001f)
	{
		return Vector3(1.0f, 0.0f, 0.0f);
	}

	forward.normalize();
	return forward;
}

void GetLeveledBasisFromForward(const Vector3& flatForward, Vector3& outForward, Vector3& outLeft)
{
	outForward = flatForward;
	const Vector3 worldUp(0.0f, 0.0f, 1.0f);
	outLeft = worldUp.cross(outForward);
	outLeft.normalize();
}

Matrix4 MakeBeltObjectOrientation(const Vector3& flatForward)
{
	const Vector3 up(0.0f, 0.0f, 1.0f);
	Vector3 forward = flatForward;
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

void EvaluatePose(
	HaloID& id,
	Vector3* pos,
	Vector3* facing,
	Vector3* up,
	const TransformQuat* boneQuats,
	Transform* outBones)
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
	int16_t bonesToProcess[kMaxBones]{};
	bonesToProcess[0] = 0;

	do
	{
		const int16_t boneIdx = bonesToProcess[i];
		i++;
		const Bone& currentBone = boneArray[boneIdx];
		const Transform* parentTransform = boneIdx == 0 ? &root : &outBones[currentBone.Parent];
		const TransformQuat* currentQuat = &boneQuats[boneIdx];
		Transform tempTransform;
		Helpers::MakeTransformFromQuat(&currentQuat->rotation, &tempTransform);
		tempTransform.scale = currentQuat->scale;
		tempTransform.translation = currentQuat->translation;
		Helpers::CombineTransforms(parentTransform, &tempTransform, &outBones[boneIdx]);

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

Vector3 CapturePointInBoneSpace(const Transform& parentBone, const Vector3& worldPoint)
{
	Matrix4 parentMatrix;
	TransformToMatrix4(parentBone, parentMatrix);

	Matrix4 parentInverse = parentMatrix;
	parentInverse.invertAffine();

	return parentInverse * worldPoint;
}

Vector3 ApplyPointInBoneSpace(const Transform& parentBone, const Vector3& localPoint)
{
	Matrix4 parentMatrix;
	TransformToMatrix4(parentBone, parentMatrix);
	return parentMatrix * localPoint;
}

Matrix4 CaptureChildInParentSpace(const Transform& parentBone, const Transform& childBone)
{
	Matrix4 parentMatrix;
	Matrix4 childMatrix;
	TransformToMatrix4(parentBone, parentMatrix);
	TransformToMatrix4(childBone, childMatrix);

	Matrix4 parentInverse = parentMatrix;
	parentInverse.invertAffine();

	return parentInverse * childMatrix;
}

Matrix4 ApplyChildInParentSpace(const Transform& parentBone, const Matrix4& childLocal)
{
	Matrix4 parentMatrix;
	TransformToMatrix4(parentBone, parentMatrix);
	return parentMatrix * childLocal;
}

void RelocateBoneSubtree(
	Transform* bones,
	const bool* boneMask,
	int rootIndex,
	const Vector3& targetRootPos,
	const Matrix4& targetRootOrientation)
{
	if (rootIndex < 0 || !bones || !boneMask)
	{
		return;
	}

	Matrix4 originalRootMatrix;
	TransformToMatrix4(bones[rootIndex], originalRootMatrix);

	Matrix4 originalRootInverse = originalRootMatrix;
	originalRootInverse.invertAffine();

	Matrix4 newRootMatrix = targetRootOrientation;
	newRootMatrix.setColumn(3, targetRootPos);

	for (int i = 0; i < kMaxBones; i++)
	{
		if (!boneMask[i])
		{
			continue;
		}

		Matrix4 originalBoneMatrix;
		TransformToMatrix4(bones[i], originalBoneMatrix);

		Matrix4 relativeMatrix = originalRootInverse * originalBoneMatrix;
		Matrix4 newBoneMatrix = newRootMatrix * relativeMatrix;

		ApplyMatrixToTransform(newBoneMatrix, bones[i]);
		bones[i].scale = 1.0f;
	}
}

void PinWeaponFirstPersonAnimation(WeaponDynamicObject* weapon, uint16_t animId, uint16_t frame)
{
	if (!weapon)
	{
		return;
	}

	if (animId != 0xFFFF)
	{
		weapon->animation = animId;
	}

	weapon->animFrame = frame;

	if (Helpers::HasFirstPersonAnimBase())
	{
		if (animId != 0xFFFF)
		{
			Helpers::SetFirstPersonBaseAnimId(animId);
		}

		Helpers::SetFirstPersonBaseAnimFrame(frame);
	}
}

void SampleBuffer::Reset()
{
	bHasSnapshot = false;
	sampleCount = 0;
	captureComplete = false;
	captureTargetSeconds = 0.0f;
	captureStartSeconds = -1.0;
	replayStartSeconds = -1.0;
	replaySkipSeconds = 0.0f;
	bReplaying = false;
	bReplayComplete = false;
}

const TransformQuat* SampleBuffer::GetSampleQuats(int index) const
{
	if (index < 0 || index >= sampleCount)
	{
		return nullptr;
	}

	return samples[index];
}

float SampleBuffer::GetSampleTime(int index) const
{
	if (index < 0 || index >= sampleCount)
	{
		return 0.0f;
	}

	return sampleTimes[index];
}

int SampleBuffer::FindSampleIndexAtOrAfter(float seconds) const
{
	for (int i = 0; i < sampleCount; i++)
	{
		if (sampleTimes[i] >= seconds)
		{
			return i;
		}
	}

	return -1;
}

void SampleBuffer::BeginCapture(const TransformQuat* boneQuats, double now, float recordTargetSeconds)
{
	memcpy(pausedSnapshot, boneQuats, sizeof(pausedSnapshot));
	bHasSnapshot = true;

	memcpy(samples[0], boneQuats, sizeof(samples[0]));
	sampleTimes[0] = 0.0f;
	sampleCount = 1;
	captureComplete = false;
	captureStartSeconds = now;
	captureTargetSeconds = recordTargetSeconds;
}

void SampleBuffer::TickCapture(TransformQuat* boneQuats, double now)
{
	if (!bHasSnapshot)
	{
		return;
	}

	if (!captureComplete)
	{
		const float elapsed = static_cast<float>(now - captureStartSeconds);
		const float lastSampleTime = sampleTimes[sampleCount - 1];
		const bool spacedEnough = (elapsed - lastSampleTime) >= (1.0f / 130.0f);

		if (sampleCount >= kMaxSamples || elapsed >= captureTargetSeconds)
		{
			captureComplete = true;
		}
		else if (spacedEnough)
		{
			memcpy(samples[sampleCount], boneQuats, sizeof(samples[0]));
			sampleTimes[sampleCount] = elapsed;
			sampleCount++;
		}
	}

	memcpy(boneQuats, pausedSnapshot, sizeof(pausedSnapshot));
}

void SampleBuffer::BeginReplay(double now, float skipSeconds)
{
	replayStartSeconds = now - skipSeconds;
	replaySkipSeconds = skipSeconds;
	bReplayComplete = false;

	const float lastTime = sampleCount > 0 ? sampleTimes[sampleCount - 1] : 0.0f;
	bReplaying = sampleCount > 1 && skipSeconds < lastTime;
	if (sampleCount > 1 && skipSeconds >= lastTime)
	{
		bReplayComplete = true;
	}
}

void SampleBuffer::ApplyReplay(TransformQuat* boneQuats, double now)
{
	if (!bReplaying)
	{
		return;
	}

	const float t = static_cast<float>(now - replayStartSeconds);
	const float lastTime = sampleTimes[sampleCount - 1];

	if (t >= lastTime)
	{
		memcpy(boneQuats, samples[sampleCount - 1], sizeof(samples[0]));
		bReplaying = false;
		bReplayComplete = true;
		return;
	}

	int idx = sampleCount - 1;
	for (int i = 1; i < sampleCount; i++)
	{
		if (sampleTimes[i] >= t)
		{
			idx = i;
			break;
		}
	}

	memcpy(boneQuats, samples[idx], sizeof(samples[0]));
}

}

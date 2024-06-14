#define EMULATE_VR 0
#include "Game.h"
#include "Logger.h"
#include "Hooking/Hooks.h"
#include "Helpers/DX9.h"
#include "Helpers/RenderTarget.h"
#include "Helpers/Renderer.h"
#include "Helpers/Camera.h"
#include "Helpers/Controls.h"
#include "Helpers/Menus.h"
#include "Helpers/Objects.h"
#include "Helpers/Maths.h"
#include "Helpers/Assets.h"

#if EMULATE_VR
#include "VR/VREmulator.h"
#else
#include "VR/OpenVR.h"
#endif
#include "DirectXWrappers/IDirect3DDevice9ExWrapper.h"


void Game::Init()
{
	Logger::log << "HaloCEVR initialising..." << std::endl;

	SetupConfigs();

	CreateConsole();

	PatchGame();

#if EMULATE_VR
	vr = new VREmulator();
#else
	vr = new OpenVR();
#endif

	vr->Init();

#define RegisterBoolInput(x) x = vr->RegisterBoolInput("default", #x);
#define RegisterVector2Input(x) x = vr->RegisterVector2Input("default", #x);

	RegisterBoolInput(Jump);
	RegisterBoolInput(SwitchGrenades);
	RegisterBoolInput(Interact);
	RegisterBoolInput(SwitchWeapons);
	RegisterBoolInput(Melee);
	RegisterBoolInput(Flashlight);
	RegisterBoolInput(Grenade);
	RegisterBoolInput(Fire);
	RegisterBoolInput(MenuForward);
	RegisterBoolInput(MenuBack);
	RegisterBoolInput(Crouch);
	RegisterBoolInput(Zoom);
	RegisterBoolInput(Reload);

	RegisterVector2Input(Move);
	RegisterVector2Input(Look);

	// Temp?
	RegisterBoolInput(Recentre);


#undef RegisterBoolInput
#undef RegisterVector2Input

	BackBufferWidth = vr->GetViewWidth();
	BackBufferHeight = vr->GetViewHeight();

	Logger::log << "HaloCEVR initialised" << std::endl;
}

void Game::Shutdown()
{
	Logger::log << "HaloCEVR shutting down..." << std::endl;

	MH_STATUS HookStatus = MH_DisableHook(MH_ALL_HOOKS);

	if (HookStatus != MH_OK)
	{
		Logger::log << "Could not remove hooks: " << MH_StatusToString(HookStatus) << std::endl;
	}

	HookStatus = MH_Uninitialize();

	if (HookStatus != MH_OK)
	{
		Logger::log << "Could not uninitialise MinHook: " << MH_StatusToString(HookStatus) << std::endl;
	}

	if (c_ShowConsole && c_ShowConsole->Value())
	{
		if (ConsoleOut)
		{
			fclose(ConsoleOut);
		}
		FreeConsole();
	}
}

void Game::OnInitDirectX()
{
	Logger::log << "Game has finished DirectX initialisation" << std::endl;

	if (!Helpers::GetDirect3DDevice9())
	{
		Logger::log << "Couldn't get game's direct3d device" << std::endl;
		return;
	}

	SetForegroundWindow(GetActiveWindow());

	vr->OnGameFinishInit();

	UISurface = vr->GetUISurface();
}

void Game::PreDrawFrame(struct Renderer* renderer, float deltaTime)
{
	LastDeltaTime = deltaTime;

	RenderState = ERenderState::UNKNOWN;

	//CalcFPS(deltaTime);

	vr->SetMouseVisibility(Helpers::IsMouseVisible());
	vr->UpdatePoses();

	StoreRenderTargets();

	sRect* Window = Helpers::GetWindowRect();
	Window->top = 0;
	Window->left = 0;
	Window->right = vr->GetViewWidth();
	Window->bottom = vr->GetViewHeight();

	//(*reinterpret_cast<short*>(0x69c642)) = vr->GetViewWidth() - 8;
	//(*reinterpret_cast<short*>(0x69c640)) = vr->GetViewHeight() - 8;

	// Clear UI surface
	IDirect3DSurface9* CurrentSurface = nullptr;
	Helpers::GetDirect3DDevice9()->GetRenderTarget(0, &CurrentSurface);
	Helpers::GetDirect3DDevice9()->SetRenderTarget(0, UISurface);
	Helpers::GetDirect3DDevice9()->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0);
	Helpers::GetDirect3DDevice9()->SetRenderTarget(0, CurrentSurface);
	CurrentSurface->Release();

	frustum1 = renderer->frustum;
	frustum2 = renderer->frustum2;

	if (bNeedsRecentre)
	{
		bNeedsRecentre = false;
		vr->Recentre();
	}


	UnitDynamicObject* WeaponFiredPlayer = static_cast<UnitDynamicObject*>(Helpers::GetLocalPlayer());

	if (WeaponFiredPlayer)
	{
		//Logger::log << "Pos: " << WeaponFiredPlayer->Position << " Centre: " << WeaponFiredPlayer->Centre << std::endl;
	}

	vr->PreDrawFrame(renderer, deltaTime);
}

void Game::PreDrawEye(Renderer* renderer, float deltaTime, int eye)
{
	RenderState = eye == 0 ? ERenderState::LEFT_EYE : ERenderState::RIGHT_EYE;

	renderer->frustum = frustum1;
	renderer->frustum2 = frustum2;

	// For performance reasons, we should prevent the game from calling SceneStart/SceneEnd for each eye
	if (eye == 0)
	{
		reinterpret_cast<IDirect3DDevice9ExWrapper*>(Helpers::GetDirect3DDevice9())->bIgnoreNextEnd = true;
	}
	else
	{
		reinterpret_cast<IDirect3DDevice9ExWrapper*>(Helpers::GetDirect3DDevice9())->bIgnoreNextStart = true;
	}

	vr->UpdateCameraFrustum(&renderer->frustum, eye);
	vr->UpdateCameraFrustum(&renderer->frustum2, eye);

	RenderTarget* primaryRenderTarget = Helpers::GetRenderTargets();

	primaryRenderTarget[0].renderSurface = vr->GetRenderSurface(eye);
	primaryRenderTarget[0].renderTexture = vr->GetRenderTexture(eye);
	primaryRenderTarget[0].width = vr->GetViewWidth();
	primaryRenderTarget[0].height = vr->GetViewHeight();
}


void Game::PostDrawEye(struct Renderer* renderer, float deltaTime, int eye)
{
	// UI should be drawn via an overlay

#if EMULATE_VR
	RECT TargetRect;
	TargetRect.left = 0;
	TargetRect.right = 200;
	TargetRect.top = 0;
	TargetRect.bottom = 200;
	Helpers::GetDirect3DDevice9()->StretchRect(UISurface, NULL, Helpers::GetRenderTargets()[0].renderSurface, &TargetRect, D3DTEXF_NONE);
#endif
}

void Game::PreDrawMirror(struct Renderer* renderer, float deltaTime)
{
	RenderState = ERenderState::GAME;

	renderer->frustum = frustum1;
	renderer->frustum2 = frustum2;

	RestoreRenderTargets();
}

void Game::PostDrawMirror(struct Renderer* renderer, float deltaTime)
{
	// Do something here to copy the image into the backbuffer correctly
}

void Game::PostDrawFrame(struct Renderer* renderer, float deltaTime)
{
	vr->PostDrawFrame(renderer, deltaTime);
}

bool Game::PreDrawHUD()
{
	// Only render UI once per frame
	if (GetRenderState() != ERenderState::LEFT_EYE)
	{
		// ...but try to avoid breaking the game view (for now at least)
		return GetRenderState() == ERenderState::GAME;
	}

	Helpers::GetDirect3DDevice9()->GetRenderTarget(0, &UIRealSurface);
	Helpers::GetDirect3DDevice9()->SetRenderTarget(0, UISurface);
	UIRealSurface = Helpers::GetRenderTargets()[1].renderSurface;
	Helpers::GetRenderTargets()[1].renderSurface = UISurface;

	return true;
}

void Game::PostDrawHUD()
{
	// Only render UI once per frame
	if (GetRenderState() != ERenderState::LEFT_EYE)
	{
		return;
	}

	Helpers::GetRenderTargets()[1].renderSurface = UIRealSurface;
	Helpers::GetDirect3DDevice9()->SetRenderTarget(0, UIRealSurface);
	UIRealSurface->Release();
}

bool Game::PreDrawMenu()
{
	// Only render UI once per frame
	if (GetRenderState() != ERenderState::LEFT_EYE)
	{
		// ...but try to avoid breaking the game view (for now at least)
		return GetRenderState() == ERenderState::GAME;
	}

	Helpers::GetDirect3DDevice9()->GetRenderTarget(0, &UIRealSurface);
	Helpers::GetDirect3DDevice9()->SetRenderTarget(0, UISurface);
	UIRealSurface = Helpers::GetRenderTargets()[1].renderSurface;
	Helpers::GetRenderTargets()[1].renderSurface = UISurface;

	return true;
}

void Game::PostDrawMenu()
{
	// Only render UI once per frame
	if (GetRenderState() != ERenderState::LEFT_EYE)
	{
		return;
	}

	Helpers::GetRenderTargets()[1].renderSurface = UIRealSurface;
	Helpers::GetDirect3DDevice9()->SetRenderTarget(0, UIRealSurface);
	UIRealSurface->Release();
}


bool Game::PreDrawLoading(int param1, struct Renderer* renderer)
{
	// Only render UI once per frame
	if (GetRenderState() != ERenderState::LEFT_EYE)
	{
		// ...but try to avoid breaking the game view (for now at least)
		return GetRenderState() == ERenderState::GAME;
	}

	UIRealSurface = Helpers::GetRenderTargets()[1].renderSurface;
	Helpers::GetRenderTargets()[1].renderSurface = UISurface;

	return true;
}

void Game::PostDrawLoading(int param1, struct Renderer* renderer)
{
	// Only render UI once per frame
	if (GetRenderState() != ERenderState::LEFT_EYE)
	{
		return;
	}

	Helpers::GetRenderTargets()[1].renderSurface = UIRealSurface;
}

/*
* Reference Implementation of UpdateViewModel:
	Asset& ViewModel = Helpers::GetAssetArray()[id.Index];
	AssetData* Data = ViewModel.Data;
	Bone* BoneArray = Data->BoneArray;

	Transform Root;
	Helpers::MakeTransformFromXZ(up, facing, &Root);
	Root.Translation = *pos;

	int i = 0;

	if (Data->NumBones > 0)
	{
		int lastIndex = 1;
		int16_t Processed[64]{};

		Processed[0] = 0;

		do
		{
			const int16_t boneIdx = Processed[i];
			i++;
			const Bone& CurrentBone = BoneArray[boneIdx];
			const Transform* ParentTransform = boneIdx == 0 ? &Root : &OutBoneTransforms[CurrentBone.Parent];
			const TransformQuat* CurrentQuat = &BoneTransforms[boneIdx];
			Transform TempTransform;
			Helpers::MakeTransformFromQuat(&CurrentQuat->Rotation, &TempTransform);
			TempTransform.Scale = CurrentQuat->Scale;
			TempTransform.Translation = CurrentQuat->Translation;
			Helpers::CombineTransforms(ParentTransform, &TempTransform, &OutBoneTransforms[boneIdx]);

			if (CurrentBone.LeftLeaf != -1)
			{
				Processed[lastIndex] = CurrentBone.LeftLeaf;
				lastIndex++;
			}
			if (CurrentBone.RightLeaf != -1)
			{
				Processed[lastIndex] = CurrentBone.RightLeaf;
				lastIndex++;
			}

		} while (i != lastIndex);
	}
*/


#pragma optimize("", off)


Vector3 LocalOffset(-0.0455f, 0.0096f, 0.0056f);
Vector3 LocalRotation(-11.0f, 0.0f, 0.0f);
bool bDoRotation = true;

Vector3 LastFireLocation(0.0f, 0.0f, 0.0f);
Vector3 LastFireAim(0.0f, 0.0f, 0.0f);

void Game::UpdateViewModel(HaloID& id, Vector3* pos, Vector3* facing, Vector3* up, TransformQuat* BoneTransforms, Transform* OutBoneTransforms)
{
	// For now just move it to be in a consistent position, we'll set the actual positions later in the function
	Vector3& camPos = Helpers::GetCamera().position;

	pos->x = camPos.x;
	pos->y = camPos.y;
	pos->z = camPos.z;

#if EMULATE_VR
	pos->x = -28.1572f;
	pos->y = 21.4454f;
	pos->z = 0.617999f;
#endif

	facing->x = 1.0f;
	facing->y = 0.0f;
	facing->z = 0.0f;

	up->x = 0.0f;
	up->y = 0.0f;
	up->z = 1.0f;

	// Reimplementation of original function:
	Asset_ModelAnimations* ViewModel = Helpers::GetTypedAsset<Asset_ModelAnimations>(id);
	if (!ViewModel)
	{
		Logger::log << "[UpdateViewModel] Can't get view model asset" << std::endl;
		return;
	}

	AssetData_ModelAnimations* Data = ViewModel->Data;
	Bone* BoneArray = Data->BoneArray;

	Transform Root;
	Helpers::MakeTransformFromXZ(up, facing, &Root);
	Root.Translation = *pos;

	//VR_START
	const bool bShouldUpdateCache = CachedViewModel.CurrentAsset != id;
	if (bShouldUpdateCache)
	{
		VM_UpdateCache(id, Data);
	}

	Matrix4 HandTransform;

	Transform RealTransforms[64]{};
	//VR_END

	if (Data->NumBones > 0)
	{
		int lastIndex = 1;
		int16_t Processed[64]{};

		Processed[0] = 0;

		int i = 0;
		do
		{
			const int16_t BoneIndex = Processed[i];
			i++;
			const Bone& CurrentBone = BoneArray[BoneIndex];
			Transform* ParentTransform = BoneIndex == 0 ? &Root : &OutBoneTransforms[CurrentBone.Parent];
			const TransformQuat* CurrentQuat = &BoneTransforms[BoneIndex];
			Transform TempTransform;
			// VR_START
			Transform ModifiedTransform;
			// For all bones but the root sub in the ACTUAL transform for the calculations (free from scaling/transform issues)
			if (BoneIndex > 0)
			{
				ModifiedTransform = *ParentTransform;
				*ParentTransform = RealTransforms[CurrentBone.Parent];
			}
			// VR_END
			Helpers::MakeTransformFromQuat(&CurrentQuat->Rotation, &TempTransform);
			TempTransform.Scale = CurrentQuat->Scale;
			TempTransform.Translation = CurrentQuat->Translation;
			Helpers::CombineTransforms(ParentTransform, &TempTransform, &OutBoneTransforms[BoneIndex]);

			//VR_START
			if (BoneIndex > 0)
			{
				// Restore the modified transform
				*ParentTransform = ModifiedTransform;
			}
			// Cache the calculated transform for this bone
			RealTransforms[BoneIndex] = OutBoneTransforms[BoneIndex];
			if (CurrentBone.Parent == 0 || BoneArray[CurrentBone.Parent].Parent == 0)
			{
				// Hide arms/root
				OutBoneTransforms[BoneIndex].Scale = 0.0f;
			}
			else if (BoneIndex == CachedViewModel.RightWristIndex)
			{
				Matrix4 NewTransform = vr->GetControllerTransform(ControllerRole::Right, true);
				// Apply scale only to translation portion
				Vector3 Translation = NewTransform * Vector3(0.0f, 0.0f, 0.0f);
				NewTransform.translate(-Translation);
				Translation *= MetresToWorld(1.0f);
				Translation += *pos;

				NewTransform.translate(Translation);

				HandTransform = NewTransform;

				// TODO: Check this works for other guns
				// Not sure if I should keep this? Rotates the hand to make the gun face forwards when controller does
				/*
				if (CurrentBone.RightLeaf != -1)
				{
					Bone& GunBone = BoneArray[CurrentBone.RightLeaf];
					if (CurrentBone.RightLeaf == CachedViewModel.GunIndex)
					{
						const TransformQuat* GunQuat = &BoneTransforms[CurrentBone.RightLeaf];
						Helpers::MakeTransformFromQuat(&GunQuat->Rotation, &TempTransform);

						Matrix4 Rotation;
						for (int x = 0; x < 3; x++)
						{
							for (int y = 0; y < 3; y++)
							{
								Rotation[x + y * 4] = TempTransform.Rotation[x + y * 3];
							}
						}

						NewTransform = NewTransform * Rotation.invert();
					}
					else
					{
						Logger::log << "ERROR: Right leaf of " << CurrentBone.BoneName << " is " << GunBone.BoneName << std::endl;
					}

				}
				*/

				VM_MoveBoneToTransform(BoneIndex, CurrentBone, NewTransform, BoneArray, RealTransforms, OutBoneTransforms);
				VM_CreateEndCap(BoneIndex, CurrentBone, OutBoneTransforms);
			}
			else if (BoneIndex == CachedViewModel.LeftWristIndex)
			{
				Matrix4 NewTransform = vr->GetControllerTransform(ControllerRole::Left, true);
				// Apply scale only to translation portion
				Vector3 Translation = NewTransform * Vector3(0.0f, 0.0f, 0.0f);
				NewTransform.translate(-Translation);
				Translation *= MetresToWorld(1.0f);
				Translation += *pos;
				NewTransform.translate(Translation);

				VM_MoveBoneToTransform(BoneIndex, CurrentBone, NewTransform, BoneArray, RealTransforms, OutBoneTransforms);
				VM_CreateEndCap(BoneIndex, CurrentBone, OutBoneTransforms);
			}
			else if (BoneIndex == CachedViewModel.GunIndex)
			{
				Vector3& GunPos = OutBoneTransforms[BoneIndex].Translation;
				Matrix3 GunRot = OutBoneTransforms[BoneIndex].Rotation;

				Vector3 HandPos = HandTransform * Vector3(0.0f, 0.0f, 0.0f);
				Matrix4 HandRotation = HandTransform.translate(-HandPos);
				Matrix3 HandRotation3;

				for (int i = 0; i < 3; i++)
				{
					HandRotation3.setColumn(i, &HandRotation.get()[i * 4]);
				}

				Matrix3 InverseHand = HandRotation3;
				InverseHand.invert();

				CachedViewModel.FireOffset = (GunPos - HandPos) + (GunRot * CachedViewModel.CookedFireOffset);
				CachedViewModel.FireOffset = InverseHand * CachedViewModel.FireOffset;

				CachedViewModel.FireRotation = CachedViewModel.CookedFireRotation * GunRot * InverseHand;


				LastFireLocation = HandPos + HandRotation3 * CachedViewModel.FireOffset;
				LastFireAim = LastFireLocation + (HandRotation3 * CachedViewModel.FireRotation) * Vector3(1.0f, 0.0f, 0.0f);
			}
			/*
			else if (strstr(CurrentBone.BoneName, "l thumb tip"))
			{
				OutBoneTransforms[BoneIndex].Translation = LastFireAim;
			}
			else if (strstr(CurrentBone.BoneName, "l thumb mid"))
			{
				OutBoneTransforms[BoneIndex].Translation = LastFireLocation;
			}
			//*/
			//VR_END

			if (CurrentBone.LeftLeaf != -1)
			{
				Processed[lastIndex] = CurrentBone.LeftLeaf;
				lastIndex++;
			}
			if (CurrentBone.RightLeaf != -1)
			{
				Processed[lastIndex] = CurrentBone.RightLeaf;
				lastIndex++;
			}

		} while (i != lastIndex);
	}
}


void Game::VM_CreateEndCap(int BoneIndex, const Bone& CurrentBone, Transform* OutBoneTransforms)
{
	// Parent bone to the position of the current bone with 0 scale to act as an end cap
	int idx = CurrentBone.Parent;
	OutBoneTransforms[idx].Translation = OutBoneTransforms[BoneIndex].Translation;
	for (int j = 0; j < 9; j++)
	{
		OutBoneTransforms[idx].Rotation[j] = OutBoneTransforms[BoneIndex].Rotation[j];
	}
	OutBoneTransforms[idx].Scale = 0.0f;
}

void Game::VM_MoveBoneToTransform(int BoneIndex, const Bone& CurrentBone, const Matrix4& NewTransform, Bone* BoneArray, Transform* RealTransforms, Transform* OutBoneTransforms)
{
	// Move hands to match controllers
	Vector3 NewTranslation = NewTransform * Vector3(0.0f, 0.0f, 0.0f);
	Matrix4 NewRotation4 = NewTransform;
	NewRotation4.translate(-NewTranslation);
	NewRotation4.rotateZ(LocalRotation.z);
	NewRotation4.rotateY(LocalRotation.y);
	NewRotation4.rotateX(LocalRotation.x);

	//Add Local offset
	NewTranslation += NewRotation4 * LocalOffset;

	OutBoneTransforms[BoneIndex].Translation = NewTranslation;
	for (int x = 0; x < 3; x++)
	{
		for (int y = 0; y < 3; y++)
		{
			OutBoneTransforms[BoneIndex].Rotation[x + y * 3] = NewRotation4.get()[x + y * 4];
		}
	}
	RealTransforms[BoneIndex] = OutBoneTransforms[BoneIndex]; // Re-cache value to use updated position
}

inline void Game::VM_UpdateCache(HaloID& id, AssetData_ModelAnimations* Data)
{
	Logger::log << "[VM_UpdateCache] Swapped weapons, recaching " << id << std::endl;
	CachedViewModel.CurrentAsset = id;
	CachedViewModel.LeftWristIndex = -1;
	CachedViewModel.RightWristIndex = -1;
	CachedViewModel.GunIndex = -1;

	Bone* BoneArray = Data->BoneArray;

	for (int i = 0; i < Data->NumBones; i++)
	{
		Bone& CurrentBone = BoneArray[i];

		if (strstr(CurrentBone.BoneName, "l wrist"))
		{
			Logger::log << "[VM_UpdateCache] Found Left Wrist @ " << i << std::endl;
			CachedViewModel.LeftWristIndex = i;
		}
		else if (strstr(CurrentBone.BoneName, "r wrist"))
		{
			Logger::log << "[VM_UpdateCache] Found Right Wrist @ " << i << std::endl;
			CachedViewModel.RightWristIndex = i;
		}
		else if (strstr(CurrentBone.BoneName, "gun"))
		{
			Logger::log << "[VM_UpdateCache] Found Gun @ " << i << std::endl;
			CachedViewModel.GunIndex = i;
		}
	}

	CachedViewModel.FireOffset = Vector3();

	// Weapon model can be found from this chain:
	// Player->WeaponID (DynamicObject)->WeaponID (Weapon Asset)->WeaponData->ViewModelID (GBX Asset)
	// The local offset of the fire VFX (i.e. end of the barrel) is found in the "primary trigger" socket

	BaseDynamicObject* Player = Helpers::GetLocalPlayer();
	if (!Player)
	{
		Logger::log << "[VM_UpdateCache] Can't find local player" << std::endl;
		return;
	}

	BaseDynamicObject* WeaponObj = Helpers::GetDynamicObject(Player->Weapon);
	if (!WeaponObj)
	{
		Logger::log << "[VM_UpdateCache] Can't find weapon from WeaponID " << Player->Weapon << std::endl;
		Logger::log << "[VM_UpdateCache] Player Tag = " << Player->TagID << std::endl;
		return;
	}

	Asset_Weapon* Weapon = Helpers::GetTypedAsset<Asset_Weapon>(WeaponObj->TagID);
	if (!Weapon)
	{
		Logger::log << "[VM_UpdateCache] Can't find weapon asset from TagID " << WeaponObj->TagID << std::endl;
		return;
	}

	if (!Weapon->WeaponData)
	{
		Logger::log << "[VM_UpdateCache] Can't find weapon data in weapon asset " << WeaponObj->TagID << std::endl;
		Logger::log << "[VM_UpdateCache] Weapon Type = " << Weapon->GroupID << std::endl;
		Logger::log << "[VM_UpdateCache] Weapon Path = " << Weapon->WeaponAsset << std::endl;
		return;
	}


	Asset_GBXModel* Model = Helpers::GetTypedAsset<Asset_GBXModel>(Weapon->WeaponData->ViewModelID);
	if (!Model)
	{
		Logger::log << "[VM_UpdateCache] Can't find GBX model from ViewModelID = " << Weapon->WeaponData->ViewModelID << std::endl;
		return;
	}

	Logger::log << "[VM_UpdateCache] GBXModelTag = " << std::setw(4) << Model->GroupID << std::endl;
	Logger::log << "[VM_UpdateCache] GBXModelPath = " << Model->ModelPath << std::endl;

	if (!Model->ModelData)
	{
		return;
	}

	Logger::log << "[VM_UpdateCache] NumSockets = " << Model->ModelData->NumSockets << std::endl;

	for (int i = 0; i < Model->ModelData->NumSockets; i++)
	{
		GBXSocket& Socket = Model->ModelData->Sockets[i];
		Logger::log << "[VM_UpdateCache] Socket = " << Socket.SocketName << std::endl;

		if (strstr(Socket.SocketName, "primary trigger"))
		{
			Logger::log << "[VM_UpdateCache] Found Effects location, Num Transforms = " << Socket.NumTransforms << std::endl;

			if (Socket.NumTransforms == 0)
			{
				Logger::log << "[VM_UpdateCache] " << Socket.SocketName << " has no transforms" << std::endl;
				break;
			}

			CachedViewModel.CookedFireOffset = Socket.Transforms[0].Position;
			Transform Rotation;
			Helpers::MakeTransformFromQuat(&Socket.Transforms[0].QRotation, &Rotation);
			CachedViewModel.CookedFireRotation = Rotation.Rotation;

			Logger::log << "[VM_UpdateCache] Position = " << Socket.Transforms[0].Position << std::endl;
			Logger::log << "[VM_UpdateCache] Quaternion = " << Socket.Transforms[0].QRotation << std::endl;
			break;
		}
	}
}
#pragma optimize("", on)

void Game::PreFireWeapon(HaloID& WeaponID, short param2, bool param3)
{
	BaseDynamicObject* Object = Helpers::GetDynamicObject(WeaponID);

	WeaponFiredPlayer = nullptr;

	// Check if the weapon is being used by the player
	HaloID PlayerID;
	if (Object && Helpers::GetLocalPlayerID(PlayerID) && PlayerID == Object->Parent)
	{
		// Teleport the player to the controller position so the bullet comes from there instead
		WeaponFiredPlayer = static_cast<UnitDynamicObject*>(Helpers::GetDynamicObject(PlayerID));
		if (WeaponFiredPlayer)
		{
			// TODO: Handedness
			Matrix4 ControllerPos = vr->GetControllerTransform(ControllerRole::Right, false);

			// Apply scale only to translation portion
			Vector3 Translation = ControllerPos * Vector3(0.0f, 0.0f, 0.0f);
			ControllerPos.translate(-Translation);
			Translation *= MetresToWorld(1.0f);
			Translation += WeaponFiredPlayer->Position;

#if EMULATE_VR
			// Emulate standing at (-28.1572, 21.4454, 0.0f) holding the gun 0.618 units up
			Translation += Vector3(-28.1572f, 21.4454f, 0.618f) - WeaponFiredPlayer->Position;
#endif
			ControllerPos.translate(Translation);

			Vector3 HandPos = ControllerPos * Vector3(0.0f, 0.0f, 0.0f);
			Matrix4 HandRotation = ControllerPos.translate(-HandPos);

			Matrix3 HandRotation3;

			for (int i = 0; i < 3; i++)
			{
				HandRotation3.setColumn(i, &HandRotation.get()[i * 4]);
			}

			// Cache the real values so we can restore them after running the original fire function
			PlayerPosition = WeaponFiredPlayer->Position;
			// What are the other aims for??
			PlayerAim = WeaponFiredPlayer->Aim;

			// angle's off (or more likely offset) + changes when crouching
			// maybe get camera position and use delta between camera and player position somehow

			Vector3 InternalFireOffset = Helpers::GetCamera().position - WeaponFiredPlayer->Position;

			WeaponFiredPlayer->Position = HandPos + HandRotation * CachedViewModel.FireOffset;// -InternalFireOffset;
			WeaponFiredPlayer->Aim = (HandRotation3 * CachedViewModel.FireRotation) * Vector3(1.0f, 0.0f, 0.0f);

			LastFireLocation = WeaponFiredPlayer->Position;// +InternalFireOffset;
			LastFireAim = LastFireLocation + WeaponFiredPlayer->Aim * 1.0f;
			Logger::log << "FireOffset: " << CachedViewModel.FireOffset << std::endl;
			Logger::log << "Player Position: " << PlayerPosition << std::endl;
			Logger::log << "Last fire location: " << LastFireLocation << std::endl;
		}
	}
}

void Game::PostFireWeapon(HaloID& WeaponID, short param2, bool param3)
{
	// Restore state after firing the weapon
	if (WeaponFiredPlayer)
	{
		WeaponFiredPlayer->Position = PlayerPosition;
		WeaponFiredPlayer->Aim = PlayerAim;
	}
}

#define ApplyBoolInput(x) controls.##x = vr->GetBoolInput(x) ? 127 : 0;
#define ApplyImpulseBoolInput(x) controls.##x = vr->GetBoolInput(x, bHasChanged) && bHasChanged ? 127 : 0;

void Game::UpdateInputs()
{
	if (GetAsyncKeyState(VK_TAB) & 0x1)
	{
		bDoRotation ^= true;
		Logger::log << "Do Rotation: " << bDoRotation << std::endl;
	}

	if (bDoRotation)
	{
		if (GetAsyncKeyState(VK_LEFT))
		{
			LocalRotation.x -= 0.1f;
			Logger::log << "R: " << LocalRotation << std::endl;
		}
		if (GetAsyncKeyState(VK_RIGHT))
		{
			LocalRotation.x += 0.1f;
			Logger::log << "R: " << LocalRotation << std::endl;
		}
		if (GetAsyncKeyState(VK_DOWN))
		{
			LocalRotation.y -= 0.1f;
			Logger::log << "R: " << LocalRotation << std::endl;
		}
		if (GetAsyncKeyState(VK_UP))
		{
			LocalRotation.y += 0.1f;
			Logger::log << "R: " << LocalRotation << std::endl;
		}
		if (GetAsyncKeyState(VK_RCONTROL))
		{
			LocalRotation.z -= 0.1f;
			Logger::log << "R: " << LocalRotation << std::endl;
		}
		if (GetAsyncKeyState(VK_RSHIFT))
		{
			LocalRotation.z += 0.1f;
			Logger::log << "R: " << LocalRotation << std::endl;
		}
	}
	else
	{
		if (GetAsyncKeyState(VK_LEFT))
		{
			LocalOffset.x -= 0.0001f;
			Logger::log << LocalOffset << std::endl;
		}
		if (GetAsyncKeyState(VK_RIGHT))
		{
			LocalOffset.x += 0.0001f;
			Logger::log << LocalOffset << std::endl;
		}
		if (GetAsyncKeyState(VK_DOWN))
		{
			LocalOffset.y -= 0.0001f;
			Logger::log << LocalOffset << std::endl;
		}
		if (GetAsyncKeyState(VK_UP))
		{
			LocalOffset.y += 0.0001f;
			Logger::log << LocalOffset << std::endl;
		}
		if (GetAsyncKeyState(VK_RCONTROL))
		{
			LocalOffset.z -= 0.0001f;
			Logger::log << LocalOffset << std::endl;
		}
		if (GetAsyncKeyState(VK_RSHIFT))
		{
			LocalOffset.z += 0.0001f;
			Logger::log << LocalOffset << std::endl;
		}
	}

	// Don't bother simulating inputs if we aren't actually in vr
#if EMULATE_VR
	return;
#endif

	vr->UpdateInputs();

	static bool bHasChanged = false;

	Controls& controls = Helpers::GetControls();

	ApplyBoolInput(Jump);
	ApplyImpulseBoolInput(SwitchGrenades);
	ApplyBoolInput(Interact);
	ApplyImpulseBoolInput(SwitchWeapons);
	ApplyBoolInput(Melee);
	ApplyBoolInput(Flashlight);
	ApplyBoolInput(Grenade);
	ApplyBoolInput(Fire);
	ApplyBoolInput(MenuForward);
	ApplyBoolInput(MenuBack);
	ApplyBoolInput(Crouch);
	ApplyImpulseBoolInput(Zoom);
	ApplyBoolInput(Reload);

	unsigned char MotionControlFlashlight = UpdateFlashlight();
	if (MotionControlFlashlight > 0)
	{
		controls.Flashlight = MotionControlFlashlight;
	}

	if (vr->GetBoolInput(Recentre))
	{
		bNeedsRecentre = true;
	}

	Vector2 MoveInput = vr->GetVector2Input(Move);

	controls.Left = -MoveInput.x;
	controls.Forwards = MoveInput.y;
}


void Game::UpdateCamera(float& yaw, float& pitch)
{
	// Don't bother simulating inputs if we aren't actually in vr
#if EMULATE_VR
	return;
#endif

	Vector2 LookInput = vr->GetVector2Input(Look);

	float YawOffset = vr->GetYawOffset();

	if (c_SnapTurn->Value())
	{
		if (LastSnapState == 1)
		{
			if (LookInput.x < 0.4f)
			{
				LastSnapState = 0;
			}
		}
		else if (LastSnapState == -1)
		{
			if (LookInput.x > -0.4f)
			{
				LastSnapState = 0;
			}
		}
		else
		{
			if (LookInput.x > 0.6f)
			{
				LastSnapState = 1;
				YawOffset += c_SnapTurnAmount->Value();
			}
			else if (LookInput.x < -0.6f)
			{
				LastSnapState = -1;
				YawOffset -= c_SnapTurnAmount->Value();
			}
		}
	}
	else
	{
		YawOffset += LookInput.x * c_SmoothTurnAmount->Value() * LastDeltaTime;
	}

	vr->SetYawOffset(YawOffset);

	Vector3 LookHMD = vr->GetHMDTransform().getLeftAxis();
	// Get current camera angle
	Vector3 LookGame = Helpers::GetCamera().lookDir;
	// Apply deltas
	float YawHMD = atan2(LookHMD.y, LookHMD.x);
	float YawGame = atan2(LookGame.y, LookGame.x);
	yaw = (YawHMD - YawGame);

	float PitchHMD = atan2(LookHMD.z, sqrt(LookHMD.x * LookHMD.x + LookHMD.y * LookHMD.y));
	float PitchGame = atan2(LookGame.z, sqrt(LookGame.x * LookGame.x + LookGame.y * LookGame.y));
	pitch = (PitchHMD - PitchGame);
}

void Game::SetMousePosition(int& x, int& y)
{
	// Don't bother simulating inputs if we aren't actually in vr
#if EMULATE_VR
	return;
#endif

	Vector2 MousePos = vr->GetMousePos();
	x = static_cast<int>(MousePos.x * 640);
	y = static_cast<int>(MousePos.y * 480);
}

void Game::UpdateMouseInfo(MouseInfo* MouseInfo)
{
	// Don't bother simulating inputs if we aren't actually in vr
#if EMULATE_VR
	return;
#endif

	if (vr->GetMouseDown())
	{
		if (MouseDownState < 255)
		{
			MouseDownState++;
		}
	}
	else
	{
		MouseDownState = 0;
	}

	MouseInfo->buttonState[0] = MouseDownState;
}

void Game::SetViewportScale(Viewport* viewport)
{
	float Width = vr->GetViewWidthStretch();
	float Height = vr->GetViewHeightStretch();

	viewport->left = -Width;
	viewport->right = Width;
	viewport->bottom = Height;
	viewport->top = -Height;
}

#undef ApplyBoolInput

float Game::MetresToWorld(float m)
{
	return m / 3.048f;
}

float Game::WorldToMetres(float w)
{
	return w * 3.048f;
}

void Game::CreateConsole()
{
	if (!c_ShowConsole || !c_ShowConsole->Value())
	{
		return;
	}

	AllocConsole();
	freopen_s(&ConsoleOut, "CONOUT$", "w", stdout);
	std::cout.clear();
}

void Game::PatchGame()
{
	MH_STATUS HookStatus;

	if ((HookStatus = MH_Initialize()) != MH_OK)
	{
		Logger::log << "Could not initialise MinHook: " << MH_StatusToString(HookStatus) << std::endl;
	}
	else
	{
		Logger::log << "Creating hooks" << std::endl;
		Hooks::InitHooks();
		Logger::log << "Enabling hooks" << std::endl;
		Hooks::EnableAllHooks();
	}
}

void Game::SetupConfigs()
{
	Config config;

	// Put all mod configs here
	c_ShowConsole = config.RegisterBool("ShowConsole", "Create a console window at launch for debugging purposes", false);
	c_DrawMirror = config.RegisterBool("DrawMirror", "Update the desktop window display to show the current game view, rather than leaving it on the splash screen", true);
	c_UIOverlayDistance = config.RegisterFloat("UIOverlayDistance", "Distance in metres in front of the HMD to display the UI", 15.0f);
	c_UIOverlayScale = config.RegisterFloat("UIOverlayScale", "Width of the UI overlay in metres", 10.0f);
	c_UIOverlayCurvature = config.RegisterFloat("UIOverlayCurvature", "Curvature of the UI Overlay, on a scale of 0 to 1", 0.1f);
	c_SnapTurn = config.RegisterBool("SnapTurn", "The look input will instantly rotate the view by a fixed amount, rather than smoothly rotating", true);
	c_SnapTurnAmount = config.RegisterFloat("SnapTurnAmount", "Rotation in degrees a single snap turn will rotate the view by", 45.0f);
	c_SmoothTurnAmount = config.RegisterFloat("SmoothTurnAmount", "Rotation in degrees per second the view will turn at when not using snap turning", 90.0f);
	c_LeftHandFlashlightDistance = config.RegisterFloat("LeftHandFlashlight", "Bringing the left hand within this distance of the head will toggle the flashlight (<0 to disable)", 0.2f);
	c_RightHandFlashlightDistance = config.RegisterFloat("RightHandFlashlight", "Bringing the right hand within this distance of the head will toggle the flashlight (<0 to disable)", -1.0f);

	config.LoadFromFile("VR/config.txt");
	config.SaveToFile("VR/config.txt");

	Logger::log << "Loaded configs" << std::endl;
}

unsigned char Game::UpdateFlashlight()
{
	Vector3 HeadPos = vr->GetHMDTransform() * Vector3(0.0f, 0.0f, 0.0f);

	float LeftDistance = c_LeftHandFlashlightDistance->Value();
	float RightDistance = c_RightHandFlashlightDistance->Value();

	if (LeftDistance > 0.0f)
	{
		Vector3 HandPos = vr->GetControllerTransform(ControllerRole::Left) * Vector3(0.0f, 0.0f, 0.0f);

		if ((HeadPos - HandPos).lengthSqr() < LeftDistance * LeftDistance)
		{
			return 127;
		}
	}

	if (RightDistance > 0.0f)
	{
		Vector3 HandPos = vr->GetControllerTransform(ControllerRole::Right) * Vector3(0.0f, 0.0f, 0.0f);

		if ((HeadPos - HandPos).lengthSqr() < RightDistance * RightDistance)
		{
			return 127;
		}
	}

	return 0;
}

void Game::CalcFPS(float deltaTime)
{
	FramesSinceFPSUpdate++;
	TimeSinceFPSUpdate += deltaTime;

	if (TimeSinceFPSUpdate > 1.0f)
	{
		TimeSinceFPSUpdate = 0.0f;
		FPS = FramesSinceFPSUpdate;
		FramesSinceFPSUpdate = 0;
		Logger::log << FPS << std::endl;
	}
}

void Game::StoreRenderTargets()
{
	for (int i = 0; i < 8; i++)
	{
		gameRenderTargets[i].width = Helpers::GetRenderTargets()[i].width;
		gameRenderTargets[i].height = Helpers::GetRenderTargets()[i].height;
		gameRenderTargets[i].format = Helpers::GetRenderTargets()[i].format;
		gameRenderTargets[i].renderSurface = Helpers::GetRenderTargets()[i].renderSurface;
		gameRenderTargets[i].renderTexture = Helpers::GetRenderTargets()[i].renderTexture;
	}
}

void Game::RestoreRenderTargets()
{
	for (int i = 0; i < 8; i++)
	{
		Helpers::GetRenderTargets()[i].width = gameRenderTargets[i].width;
		Helpers::GetRenderTargets()[i].height = gameRenderTargets[i].height;
		Helpers::GetRenderTargets()[i].format = gameRenderTargets[i].format;
		Helpers::GetRenderTargets()[i].renderSurface = gameRenderTargets[i].renderSurface;
		Helpers::GetRenderTargets()[i].renderTexture = gameRenderTargets[i].renderTexture;
	}
}

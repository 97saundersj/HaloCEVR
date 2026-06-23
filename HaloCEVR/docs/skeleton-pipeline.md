# Skeleton pipeline (SetViewModelPosition)

Notes for reverse-engineering Halo CE 1.0.10 first-/third-person rendering and IK.

## Hook: `SetViewModelPosition` (`Hooks::H_SetViewModelPosition`)

The game calls `SetViewModelPosition` once **per animated skeleton pass**, not only for the FP weapon.

| Parameter (stdcall) | Register / stack | Role |
|---------------------|------------------|------|
| Animations asset ID | `EAX` | `HaloID` of an `Asset_ModelAnimations` tag |
| Root position       | `ECX` | World/root translation (input, may be overwritten) |
| Out transforms      | `[esp+4]` | `Transform[64]` — computed bone matrices |
| Quat transforms     | `[esp+8]` | `TransformQuat[64]` — animated bone locals from the anim system |
| Facing              | `[esp+0xC]` | Root facing vector |
| Up                  | `[esp+0x10]` | Root up vector |

HaloCEVR replaces the entire function with `WeaponHandler::UpdateViewModel`.

## FP weapon vs other skeletons

**First-person weapon** animation tags use a consistent bone naming scheme:

- `frame gun`, `frame r wriste`, `frame l wriste`, `frame magazine`, …

**Third-person player** (and other) rigs use different names (`pelvis`, `spine`, etc.) and must **not** go through VR weapon logic (camera-locked root, controller hand IK, magazine relocation, physical-reload bone pin).

Detection: `WeaponHandler::IsFirstPersonWeaponAnimationsAsset` — requires `frame gun` + `frame r wrist` bones and a local equipped weapon.

Non-weapon passes call `ReferenceUpdateViewModelImpl` (vanilla decomp) with the game's original root position/orientation.

## Physical reload bone pin

During empty-mag physical reload we snapshot the incoming `TransformQuat[64]` at magazine eject and restore it each frame while paused so the FP pose freezes.

**Requirement:** pin/snapshot only when `HaloID` matches `cachedViewModel.currentAsset` (the equipped weapon's animations tag). Applying weapon bone data to a third-person pass caused the TP model to render at the camera with corrupted bones ("inside" the avatar).

Implementation: `WeaponHandler::ApplyPhysicalReloadBonePin(id, boneTransforms)`.

Snapshot is cleared on reload resume (`ClearPhysicalReloadBoneSnapshot`) and full state reset.

## Related globals

- **First-person anim ID/frame** — not on the weapon object; resolved from `DrawViewModel` (`Helpers::FirstPersonAnimBase`). Used for reload timer pinning; may not match all builds.
- **Weapon view model cache** — `WeaponHandler::cachedViewModel` populated in `UpdateCache` when the FP weapon asset changes. Chain: `player.weapon → Asset_Weapon → WeaponData.ViewModelID → GBX model sockets`.

## Useful debug

- `LogPhysicalReloadFrames=1` in `VR/config.txt` — reload phase, ammo, timer.
- `[WeaponHandler] === View model bone hierarchy ===` — logged on weapon swap; confirms magazine bone index and reload anim indices.

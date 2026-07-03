#pragma once

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

	void Update();
	void ApplyAnimPin();
	void ResetState();
	void ResetCycle();
	void EndShotgunShellSession();
	void PrepareShotgunFireDuringReload();
	void HandleReloadEnd();

	bool ShouldBlockAutoReloadStart() const;
	void SuppressVanillaReloadControl(unsigned char& reloadControl) const;
	bool ShouldSuppressTwoHandAim() const;
	bool IsBlockingWeaponHandSwap() const;
	void TickSwapSuppression();
	bool ShouldSkipWeaponHandSwapDuringReload() const;
	bool ShouldContinueContinuousReloadSession() const;
	bool ShouldAutoStartContinuousReloadSession() const;

	void BeginChainedShellReload();
	void SuspendShotgunActiveReloadForFire();
	void TryBeginShotgunLoadFromBelt();
	void TriggerWeaponReload();
	void TriggerWeaponReloadEnd();

private:
	InputHandler& input;

	bool bBeltGripStartedReload = false;
	bool bSuppressSwapUntilGripRelease = false;

	void ResetCycleCore();
	bool IsOffHandNearBeltMagazine() const;
	bool ShouldSuspendShotgunActiveReloadForFire() const;
	void UpdateReloadAnimationPause();
	void BeginPhysicalReload();
	void ResumePhysicalReloadAnimation();
	void HandlePhysicalMagazineGrabInsert();
};

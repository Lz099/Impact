#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "ImpactTypes.h"
#include "ImpactAmmoComponent.generated.h"

/** Independent ammo state. The owner handles animation, weapon switching and menus. */
UCLASS(ClassGroup = Impact, meta = (BlueprintSpawnableComponent))
class IMPACT_API UImpactAmmoComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UImpactAmmoComponent();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	/** Any active reload blocks both weapons. None/unknown weapons cannot shoot or reload. */
	bool CanShoot(EImpactWeapon Weapon) const;
	bool ConsumeRound(EImpactWeapon Weapon);
	bool BeginReload(EImpactWeapon Weapon);
	/** Cancels only the pending transfer; no ammunition has been moved by the reload yet. */
	void CancelReload();
	bool IsReloading() const { return ReloadWeapon != EImpactWeapon::None; }
	EImpactWeapon GetReloadWeapon() const { return ReloadWeapon; }

	/** Active reload duration in seconds (rifle 2, pistol 1.5), or zero while idle. */
	float GetReloadDuration() const;
	/** Fraction in [0, 1] during reload; resets to zero on cancellation/completion. */
	float GetReloadProgress() const;
	int32 GetMagazine(EImpactWeapon Weapon) const;
	int32 GetReserve(EImpactWeapon Weapon) const;

	/** Clamps capacity to >= 0, returns overflow to reserve, never auto-fills a larger magazine.
	 * An active reload keeps its timer and uses the current capacity when it completes. */
	void SetRifleCapacity(int32 Capacity);
	/** Configuration changes are accepted only outside a reload. */
	void SetRifleReloadDuration(float Seconds);

	/** Same path as TickComponent, usable without a world. Nonpositive/nonfinite time is ignored.
	 * Transfers ammo exactly once at completion; excess time is discarded. */
	void AdvanceReload(float DeltaTime);

private:
	struct FWeaponAmmo
	{
		int32 Capacity;
		int32 Magazine;
		int32 Reserve;
		float ReloadDuration;
	};

	FWeaponAmmo* FindAmmo(EImpactWeapon Weapon);
	const FWeaponAmmo* FindAmmo(EImpactWeapon Weapon) const;

	FWeaponAmmo Rifle{30, 30, 180, 2.0f};
	FWeaponAmmo Pistol{12, 12, 72, 1.5f};
	EImpactWeapon ReloadWeapon = EImpactWeapon::None;
	float ReloadElapsed = 0.0f;
};

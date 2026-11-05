#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "ImpactTypes.h"
#include "ImpactWeaponComponent.generated.h"

class UEnhancedInputComponent;
class UInputAction;
class UInputMappingContext;
class USkeletalMeshComponent;
class UImpactAmmoComponent;
class UImpactWeaponPresentationComponent;

/**
 * Lets the ALS showcase character fire the weapon it holds (rifle or pistol overlay).
 * Left click fires; the rifle keeps firing while held, the pistol fires once per click.
 * Bullets leave from the "muzzle" bone of the weapon mesh and go where the crosshair points.
 */
UCLASS(ClassGroup = Impact)
class IMPACT_API UImpactWeaponComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UImpactWeaponComponent();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	EImpactWeapon GetCurrentWeapon() const;
	bool IsAiming() const;

	/** Current cone half angle, for the crosshair. */
	float GetSpreadDegrees() const;
	FImpactWeaponSpec GetCurrentSpec() const;

	/** Time of the last bullet that hit a person, for the hit marker. */
	float GetLastBodyHitTime() const { return LastBodyHitTime; }

	/** Same as holding or releasing the left button (also used by automatic tests). */
	void SetTriggerHeld(bool bHeld);

	int32 GetShotCount() const { return ShotCount; }

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(EEndPlayReason::Type EndPlayReason) override;

private:
	void BindInput();
	void OnFirePressed();
	void OnFireReleased();
	void OnXRay();
	void OnReload();
	void OnInspect();
	void OnLaser();
	void OnVision();
	void OnLamp();
	void OnCloseInspection();
	bool CanFire() const;
	void FireShot();
	void ApplyRecoil(float DeltaTime);
	USkeletalMeshComponent* FindWeaponMesh() const;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> FireAction;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> XRayAction;
	UPROPERTY(Transient) TObjectPtr<UInputAction> ReloadAction;
	UPROPERTY(Transient) TObjectPtr<UInputAction> InspectAction;
	UPROPERTY(Transient) TObjectPtr<UInputAction> LaserAction;
	UPROPERTY(Transient) TObjectPtr<UInputAction> VisionAction;
	UPROPERTY(Transient) TObjectPtr<UInputAction> LampAction;
	UPROPERTY(Transient) TObjectPtr<UInputAction> CloseInspectionAction;
	UPROPERTY(Transient) TObjectPtr<UImpactAmmoComponent> Ammo;
	UPROPERTY(Transient) TObjectPtr<UImpactWeaponPresentationComponent> Presentation;

	UPROPERTY(Transient)
	TObjectPtr<UInputMappingContext> MappingContext;

	UPROPERTY(Transient)
	TObjectPtr<UEnhancedInputComponent> WeaponInput;

	FRandomStream Random{1234};
	EImpactWeapon LastWeapon = EImpactWeapon::None;
	bool bTriggerHeld = false;
	float Cooldown = 0.0f;
	float Bloom = 0.0f;
	float PendingRecoilPitch = 0.0f;
	float PendingRecoilYaw = 0.0f;
	float RecoilDebtPitch = 0.0f;
	float LastShotTime = -100.0f;
	float LastBodyHitTime = -100.0f;
	int32 ShotCount = 0;
};

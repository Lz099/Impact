#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ImpactTypes.h"
#include "ImpactFX.generated.h"

class UInstancedStaticMeshComponent;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class UPointLightComponent;
class UStaticMeshComponent;
class USkeletalMeshComponent;

/**
 * Visual effects of shots, all driven from code: muzzle flash, tracers, bullet holes,
 * blood droplets that fly, fall and leave splats where they land.
 * One instance per world, owned by UImpactGameplaySubsystem.
 */
UCLASS(NotPlaceable, Transient)
class IMPACT_API AImpactFX : public AActor
{
	GENERATED_BODY()

public:
	AImpactFX();

	virtual void Tick(float DeltaTime) override;

	void MuzzleFlash(const FVector& Location, const FVector& Direction, EImpactWeapon Weapon);
	void Tracer(const FVector& From, const FVector& To);
	void BulletHole(const FHitResult& Hit);

	/** Droplets thrown from a wound. Amount ~1 for a pistol wound, more for exits and big vessels. */
	void BloodSpray(const FVector& Location, const FVector& Direction, float Amount, float Speed);

	/** One drop falling from a wound (the trail left by someone bleeding). */
	void BloodDrop(const FVector& Location, const FVector& Velocity, float Size);

	/** Flat blood stain on whatever surface is at Location (ground, wall). Persistent stains never fade (pools). */
	class UDecalComponent* BloodSplat(const FVector& Location, const FVector& Normal, float Size, bool bPersistent = false);
	class UDecalComponent* BloodWound(USkeletalMeshComponent* Mesh, FName Bone, const FVector& Location, const FVector& Normal, float Size);

private:
	struct FFlash
	{
		int32 Slot = 0;
		float Age = 0.0f;
		float Peak = 0.0f;
	};

	struct FTracer
	{
		int32 Slot = 0;
		FVector From = FVector::ZeroVector;
		FVector Direction = FVector::ForwardVector;
		float Length = 0.0f;
		float Travelled = 0.0f;
	};

	struct FDrop
	{
		FVector Location = FVector::ZeroVector;
		FVector Velocity = FVector::ZeroVector;
		float Size = 1.0f;
		float Age = 0.0f;
	};

	UMaterialInterface* LoadMaterial(const TCHAR* Path) const;
	static int32 TakeSlot(TArray<float>& SlotTimes, float Now);

	UPROPERTY()
	TArray<TObjectPtr<UPointLightComponent>> FlashLights;

	UPROPERTY()
	TArray<TObjectPtr<UStaticMeshComponent>> FlashMeshes;

	UPROPERTY()
	TArray<TObjectPtr<UStaticMeshComponent>> TracerMeshes;

	UPROPERTY()
	TObjectPtr<UInstancedStaticMeshComponent> Drops;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> DecalMaterial;

	TArray<FFlash> Flashes;
	TArray<float> FlashSlotTimes;
	TArray<FTracer> Tracers;
	TArray<float> TracerSlotTimes;
	TArray<FDrop> DropStates;
	bool bDropMaterialSet = false;
	int32 SplatCount = 0;
};

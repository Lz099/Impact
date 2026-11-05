#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "ImpactAnatomy.h"
#include "ImpactPhysiology.h"
#include "ImpactTypes.h"
#include "ImpactBodyComponent.generated.h"

class UDecalComponent;
class UImpactReactionComponent;
class UMaterialInstanceDynamic;

/** What is left of a bullet after crossing a body. */
struct FImpactBulletResult
{
	bool bExited = false;
	FVector ExitPoint = FVector::ZeroVector;
	float EnergyOut = 0.0f;
};

/**
 * The body of a person that can be shot: anatomy (where each bullet goes inside), physiology (blood, oxygen,
 * consciousness, what each limb can still do), blood coming out of the wounds, and the decisions that follow
 * (collapse, die, keep a limb hanging, lose the legs). The physical motion itself is UImpactReactionComponent.
 */
UCLASS(ClassGroup = Impact, Meta = (BlueprintSpawnableComponent))
class IMPACT_API UImpactBodyComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UImpactBodyComponent();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	FString DisplayName;

	FImpactBulletResult ReceiveBullet(const FImpactBulletHit& Bullet);

	/** Text for the label above the head. */
	FString GetLabel() const;

	/** 0 to 1: how much a body still has to spend on moving. Flight asks for a sprint it may not get. */
	float GetEffort() const { return Effort; }

	/** The bone carrying the worst bleeding external wound, or None. Says what a body on the ground is
	 *  trying to deal with. */
	FName GetWorstWoundBone() const
	{
		FName Worst;
		float Most = 0.0f;
		for (const FImpactWound& Wound : Physiology.GetWounds())
		{
			if (Wound.bExternal && Wound.Bleed > Most)
			{
				Most = Wound.Bleed;
				Worst = Wound.Bone;
			}
		}
		return Worst;
	}

	const FImpactPhysiology& GetPhysiology() const { return Physiology; }
	FImpactPhysiology& GetPhysiology() { return Physiology; }
	const FImpactAnatomy& GetAnatomy() const { return Anatomy; }

	/** X-ray: organs and the paths of the last bullets. */
	void DrawXRay(float Duration) const;

	/** World position of a wound now (it follows its bone). */
	FVector GetWoundLocation(const FImpactWound& Wound) const;

	/** French name of the body part a physics body belongs to, e.g. "cuisse gauche". */
	static FString DescribeBodyPart(FName BoneName, bool bFromBehind);

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(EEndPlayReason::Type EndPlayReason) override;

private:
	void UpdateBody(bool bNewInjury);
	void EmitBlood(float DeltaTime);

	/** -ImpactSangPeau: blood spreading on the skin round each wound, drawn as a translucent layer over the body. */
	void TrySetupSkinBlood(float DeltaTime);
	void UpdateSkinBlood();
	void UpdateHands();

	/** -ImpactPNJArmes: what a wounded man does with the weapon in his hands - keeps it in the right hand while the left holds the
	 *  wound, or drops it to hold the wound with both. True while the right hand is still busy with a weapon. */
	bool UpdateWeaponHold();
	void DropWeapon(const TCHAR* Why);
	bool IsStanceLeg(EImpactLimb Leg) const;

	/** A bullet path stored in the space of the bone it entered (drawn by the X-ray view). */
	struct FPathRecord
	{
		FName Bone;
		FVector Entry = FVector::ZeroVector;
		FVector Exit = FVector::ZeroVector;
		FVector Before = FVector::ZeroVector;
		TArray<FVector> Hits;
		bool bExited = false;
	};

	FImpactAnatomy Anatomy;
	FImpactPhysiology Physiology;
	TArray<FPathRecord> RecentPaths;
	FString LastEvent;
	FString Activity;
	EImpactConsciousness LastConsciousness = EImpactConsciousness::Conscious;
	TArray<float> WoundDripBudgets;
	TArray<int32> MarkedWounds;
	int32 ProcessedVisualWounds = 0;
	float PoolLostMl = 0.0f;
	float LogTimer = 0.0f;
	int32 LastHandSide = -2;
	int32 LastSecondHandSide = INDEX_NONE;

	// The gesture keeps its choices: picking the nearest arm again on every frame made the two hands swap
	// several times a second, and each swap started the approach over, so neither hand ever arrived.
	int32 HandWound = INDEX_NONE;  // wound the hand is holding
	int32 HandSide = INDEX_NONE;   // 0 left, 1 right
	int32 SecondHandSide = INDEX_NONE;
	float SecondHandSince = 0.0f;
	float Effort = 1.0f;
	int32 HandFailedSides = 0;     // one bit per arm that tried this wound and could not get there
	float HandWoundSince = 0.0f;   // a wound is held for a while before another may take the hand
	TArray<float> WoundBlockedUntil; // a wound no arm could reach is left alone for a while, or the choice ping-pongs
	bool bHandAxesLogged = false;

	TWeakObjectPtr<UDecalComponent> Pool;

	// -ImpactSangPeau
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> SkinBlood;
	FDelegateHandle SkinBloodHandle;
	bool bSkinBloodFailed = false;
	float SkinBloodWait = 0.0f;
	int32 SkinBloodSlotsUsed = 0;
	FString SkinBloodSummary;
};

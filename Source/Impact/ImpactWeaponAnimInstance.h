#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "ImpactWeaponAnimInstance.generated.h"

struct FImpactWeaponAnimInstanceProxy;

/**
 * Native left-arm controls applied to the incoming post-process pose, after ALS.
 *
 * UE 5.7 requires an AnimBlueprint child to register the post-process input:
 * create an otherwise empty AnimGraph with Input Pose (name InPose) -> Output Pose.
 * The proxy replaces that output's runtime link with its native graph. Do not add
 * other animation logic to this bridge asset. Install the GENERATED class using
 * SetOverridePostProcessAnimBP only when the mesh has no existing post-process.
 * The abstract native class alone cannot supply GetLinkedInputPoseNode().
 */
UCLASS(Abstract, Blueprintable, Transient)
class IMPACT_API UImpactWeaponAnimInstance : public UAnimInstance
{
	GENERATED_BODY()

public:
	/**
	 * Game thread only, preferably before the mesh animation tick. Positions and
	 * rotation are in CHARACTER MESH component space (not world/weapon space).
	 * Elbow is a pole target, not a forced elbow position. Weight is clamped to
	 * [0,1]; zero immediately restores the incoming pose without conversions.
	 * Targets persist until changed. Fade/reset Weight when the interaction ends.
	 */
	UFUNCTION(BlueprintCallable, Category = "Impact|Weapon Animation")
	void SetLeftHandTarget(FVector ComponentSpaceLocation, FRotator ComponentSpaceRotation,
		FVector ComponentSpaceElbow, float Weight);

protected:
	virtual FAnimInstanceProxy* CreateAnimInstanceProxy() override;
	virtual void DestroyAnimInstanceProxy(FAnimInstanceProxy* InProxy) override;

private:
	friend struct FImpactWeaponAnimInstanceProxy;

	// Game-thread mailbox. Only PreUpdate copies these values into the proxy.
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Impact|Weapon Animation", meta = (AllowPrivateAccess = "true"))
	FVector LeftHandLocation = FVector::ZeroVector;

	UPROPERTY(Transient, BlueprintReadOnly, Category = "Impact|Weapon Animation", meta = (AllowPrivateAccess = "true"))
	FRotator LeftHandRotation = FRotator::ZeroRotator;

	UPROPERTY(Transient, BlueprintReadOnly, Category = "Impact|Weapon Animation", meta = (AllowPrivateAccess = "true"))
	FVector LeftElbowLocation = FVector::ZeroVector;

	UPROPERTY(Transient, BlueprintReadOnly, Category = "Impact|Weapon Animation", meta = (AllowPrivateAccess = "true"))
	float LeftHandWeight = 0.0f;
};

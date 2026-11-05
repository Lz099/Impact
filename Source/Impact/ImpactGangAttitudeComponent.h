#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "ImpactGangAttitudeComponent.generated.h"

class AAlsCharacter;
class UAnimSequence;
struct FImpactGangPayload;

/**
 * -ImpactAttitudeGang. How an armed man of a gang carries himself, laid over what ALS animates: his legs, his balance, his falls
 * and everything on the floor stay as they are, and the layer fades out the moment ALS takes over (ragdoll, lying, getting up)
 * or a hand goes to a wound. On top of the normal pose, after the whole ALS graph (FAlsAnimationInstanceProxy::PoseEditor):
 *  - the weapon held like a gangster: a pistol turned on its side, a rifle canted and held lower, an exaggerated kick at each
 *    round, the chest out and the head tilted towards the gun; the barrel is then brought back onto the player, so where the
 *    rounds go and where the gun points stay the same thing;
 *  - attitude takes (Mixamo, /Game/Impact/Anims/Gang) on the upper body: a loose idle with the weight on one leg, gestures
 *    while hanging about, taunts between two bursts, celebrating when the player is hit or down, yelling at him when he is
 *    spotted, a homie going down;
 *  - a swagger while walking (shoulders rolling with the steps, nods, arms wide), in step with ALS's own feet.
 * The soldier's brain still decides where he goes and when he fires; this only changes how he looks doing it.
 */
UCLASS()
class IMPACT_API UImpactGangAttitudeComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UImpactGangAttitudeComponent();

	static bool IsEnabled();

	/** Game thread, just before his animation updates: what the animation thread will lay over the pose this frame. */
	void FillPayload(FImpactGangPayload& Out);

	/** For the label over his head. */
	FString GetAttitudeText() const;

	/** Benches: plays one move now (1 arms wide, 2 chest thump, 3 pointing, 4 celebrating, 5 come on, 6 talking). */
	void BenchMove(int32 Which);
	/** Benches: no move of their own, only BenchMove (the pose bench photographs each one alone). */
	static bool bBenchMovesOnly;

protected:
	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
	enum class EMove : uint8
	{
		None,
		Gesture,   // hanging about: talking with the hands, looking round
		Taunt,     // between bursts, or when the player hides
		Yell,      // the player just spotted, or a homie down
		Celebrate, // the player hit by him, or down
		Reload
	};

	struct FTake
	{
		TObjectPtr<UAnimSequence> Sequence = nullptr;
		uint8 Mask = 0;
		float Length = 0.0f;
	};

	void LoadTakes();
	const FTake* PickTake(EMove Kind);
	void StartMove(EMove Kind, const TCHAR* Why, bool bFreeHandOnly = false);
	bool MoveUsesRightArm() const;
	void UpdateWeaponFrame(const AAlsCharacter& Me);

	UPROPERTY(Transient)
	TArray<TObjectPtr<UAnimSequence>> Loaded;

	TArray<FTake> Idles;
	TArray<FTake> Gestures;
	TArray<FTake> Taunts;
	TArray<FTake> Yells;
	TArray<FTake> Celebrations;
	TArray<FTake> Reloads;
	TArray<FTake> GunRuns;

	// What the layer is doing, smoothed so nothing pops.
	float Master = 0.0f;
	float IdleWeight = 0.0f;
	float RunWeight = 0.0f;
	float AimBlend = 0.0f;
	float SwaggerBlend = 0.0f;
	float GunUpBlend = 0.0f;
	bool bFrameLogged = false;
	float IdleTime = 0.0f;
	float RunTime = 0.0f;
	int32 IdleIndex = 0;
	int32 RunIndex = 0;

	EMove Move = EMove::None;
	const FTake* MoveTake = nullptr;
	uint8 ProcMove = 0;      // the move made in code when there is no take for it
	float MoveLength = 0.0f;
	float MoveStart = 0.0f;  // where in its take the move starts (a long take is played as a few seconds out of it)
	float MoveTime = 0.0f;
	float MoveWeight = 0.0f;
	bool bMoveCut = false;      // fading out early: he has to fire and the move holds his gun arm
	bool bMoveRightArm = false; // the move takes the gun arm
	TArray<TWeakObjectPtr<const AActor>> HomiesDown;
	EMove Pending = EMove::None;
	float PendingDelay = 0.0f;
	const TCHAR* PendingWhy = TEXT("");
	float NextGesture = 4.0f;
	float SinceBurst = 100.0f;
	float SinceHomieDown = 100.0f;
	float SinceTaunt = 100.0f;
	float HomieTimer = 0.0f;
	float Recoil = 0.0f;
	int32 ShotsSeen = 0;
	int32 BurstsSeen = 0;
	int32 HitsSeen = 0;
	bool bHitHimThisBurst = false;
	bool bWasAlerted = false;
	bool bWasReloading = false;
	bool bPlayerDownSeen = false;
	uint8 FreeArms = 0;
	float LogTimer = 0.0f;
	float AimErrorSeen = -1.0f;

	// Measures. [corps-mou]: degrees between the animated pose and the pose the physics gave, while he runs (left arm, right arm,
	// head, chest). [chute-temps]: from the start of a ragdoll, how long until the hips are on the floor and how fast they fell.
	float LastSpeed = 0.0f;
	float DragSum[4] = {0.0f, 0.0f, 0.0f, 0.0f};
	float DragMax[4] = {0.0f, 0.0f, 0.0f, 0.0f};
	int32 DragCount = 0;
	float FallStart = -1.0f;
	float FallFloorZ = 0.0f;
	float FallHeight = 0.0f;
	float FallPrevZ = 0.0f;
	float FallFastest = 0.0f;
	bool bWasRagdoll = false;
	bool bFallBodyLogged = false;
	// [chute-saut]: how far the hips rise in the first 0.4 s of a ragdoll (a body that is falling does not go up).
	float FallStartZ = 0.0f;
	float FallRise = 0.0f;
	bool bRiseLogged = false;
	// [arme-lachee]: the weapon he let go, followed for a few seconds (does it reach the floor, is it still drawn).
	bool bWasArmed = false;
	bool bWeaponGoneLogged = false;
	TWeakObjectPtr<AActor> DroppedWeapon;
	float DroppedTime = -1.0f;
	int32 DroppedChecks = 0;

	// The weapon in the gun hand's frame, measured from the last pose (it hangs on ALS's gun bone, under the hand).
	bool bGun = false;
	bool bRifle = false;
	FVector GripInHand = FVector::ZeroVector;
	FVector BoreInHand = FVector::ZeroVector;
	FVector MuzzleInHand = FVector::ZeroVector;
	bool bTwoHanded = false;
	FVector AimTarget = FVector::ZeroVector;

	FRandomStream Random;
};

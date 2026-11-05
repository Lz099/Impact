#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "GameplayTagContainer.h"
#include "ImpactTypes.h"
#include "ImpactPhysiology.h"
#include "ImpactReactionComponent.generated.h"

class AAlsCharacter;
class UPhysicalAnimationComponent;
class USkeletalMeshComponent;

/**
 * Physical side of a body on top of the ALS animation.
 *
 * Every bone follows its animated pose through a "muscle" (Physical Animation Component) whose strength can drop.
 * A bullet pushes the exact bone it hit, the muscles around it go slack for a moment and come back, so the body
 * gives way where it was hit and returns to the animation. A limb whose resting tone stays low hangs.
 *
 * Balance: each hit also pushes the whole body. The push becomes recovery steps walked by ALS
 * (the character keeps facing the shooter and steps back or aside). When the steps needed are more than
 * the legs can give, the character falls (ALS ragdoll); a dead body falls limp.
 */
UCLASS(ClassGroup = Impact)
class IMPACT_API UImpactReactionComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UImpactReactionComponent();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	/** Local physical reaction to a bullet. Strength scales the push (1 = normal hit of that weapon). */
	void ApplyBulletHit(const FImpactBulletHit& Bullet, float Strength);

	/** Resting muscle tone of a limb (1 healthy, near 0 the limb hangs). Root bone of the limb, e.g. "clavicle_l". */
	void SetLimbTone(FName LimbRoot, float Tone);

	/** One authority for voluntary motion. Cancels commands immediately when awareness is lost. */
	void SetControlState(EImpactConsciousness State);
	bool HasVoluntaryControl() const { return bVoluntaryControl; }
	int32 GetActiveForceCount() const { return ActiveForceCount; }
	bool IsBracing() const { return bVoluntaryControl && BraceTime > 0.0f; }
	bool CanHoldWound() const;

	/** How far (cm) the legs can still step to catch the body. 0 = cannot stand. */
	void SetBalanceCapacity(float Centimeters) { BalanceCapacity = Centimeters; }

	/**
	 * A shove with no bullet behind it: the same balance path a hit goes through, for the test bench and later
	 * for anything that pushes a body (a door, a car, another person). DeltaV is the speed change in cm/s.
	 */
	void Shove(const FVector& Direction, float DeltaV, const TCHAR* Reason);

	/** Centre of mass of the whole body and how fast it is travelling. False when it cannot be measured. */
	bool GetBalanceState(FVector& OutCentre, FVector& OutVelocity, FVector& OutSupport, bool& bOutLeftDown, bool& bOutRightDown) const;

	/** Collapse now (ragdoll), pushed horizontally with this velocity (cm/s). bLimp: no muscle at all (dead, unconscious). */
	void Collapse(const FVector& PushVelocity, bool bLimp, const TCHAR* Reason);

	/** Puts a hand on a point of the body (a wound): the arm goes soft and the hand is pulled there. Side 0 left, 1 right. */
	void SetHandTarget(int32 Side, const FVector& WorldTarget, const FVector& Outward, bool bActive);

	/** Where a hand is going, for the X-ray view. Returns false when the hand is free. */
	bool GetHandGoal(const int32 Side, FVector& OutTarget, FVector& OutApproach, bool& bOutApproached) const
	{
		const FHand& Hand = Hands[Side];
		OutTarget = Hand.Target;
		OutApproach = Hand.Target + Hand.Outward * 14.0f;
		bOutApproached = Hand.bApproached;
		return Hand.Weight > 0.0f;
	}

	/** Distance (cm) between the centre of a hand and its target. */
	float GetHandDistance(int32 Side) const { return Hands[Side].Distance; }

	/** Shortest distance (cm) from the surface of the hand to the wound. 0 = the hand is against the body. */
	float GetHandGap(int32 Side) const { return Hands[Side].Gap; }

	/** True once the hand has been held against the wound long enough for the pressure to count. */
	bool IsHandPressing(int32 Side) const { return CanHoldWound() && Hands[Side].bActive && Hands[Side].Weight > 0.5f && Hands[Side].ContactTime >= ContactHoldTime; }

	/** False once that arm has pulled towards the wound and never got near it (middle of the back, arm too short). */
	bool IsHandReachable(int32 Side) const { return Hands[Side].bReachable; }

	/** Limp: the body dips each time the hurt leg takes the weight. Side 0 left, 1 right, -1 none; Amount 0..1. */
	void SetLimp(int32 Side, float Amount)
	{
		LimpSide = Side;
		LimpAmount = Amount;
	}

	/** Degrees the trunk is currently leaning over the hurt leg (0 when walking normally). */
	float GetLimpLean() const { return LimpLean; }

	/** Centimetres the body is currently dropped on the hurt leg. */
	float GetLimpDip() const { return LimpDip; }

	/**
	 * Drag the body along the ground on its arms, in this world direction. The legs are left passive: this is for
	 * someone whose legs no longer answer but who is awake and still has an arm. Zero direction stops it.
	 * bLeftArm / bRightArm say which arms can still pull; with one arm it still crawls, slower and crookedly.
	 */
	void SetCrawl(const FVector& WorldDirection, bool bLeftArm, bool bRightArm);

	// -ImpactSolArme, for the soldier's brain: on the floor with his weapon. Where to aim (he is turned to it and the prone take
	// aims), where to crawl to (a crawl he owns, which the body's own wish does not override, and which stops there).
	void SetGroundAimTarget(const FVector& World, bool bOn) { GroundAimTarget = World; bGroundAimOn = bOn; }
	void SetGroundGoal(const FVector& World, bool bOn);
	bool HasGroundGoalReached() const { return bGroundGoalReached; }
	/** Lying in an aiming take, settled, barrel near the target: he may fire. */
	bool IsGroundAimReady() const;
	/** Angle between the barrel and the line to the aim target, degrees. */
	float GetGroundAimError() const { return GroundAimError; }
	/** A round just left: the firing take for a moment. */
	void NotifyGroundShot();

	bool IsCrawling() const { return bCrawling; }

	/** Metres covered since the crawl started, for checking it actually travels. */
	float GetCrawlDistance() const { return CrawlDistance; }

	/** How spent the body is, 0 fresh to 1 barely standing: it sinks and stoops as it goes. */
	void SetWeakness(const float Amount) { Weakness = FMath::Clamp(Amount, 0.0f, 1.0f); }
	/** -ImpactVentreCourbe: how much a hole in the belly folds him over it, 0 none to 1. */
	void SetGutWound(const float Amount) { GutWound = FMath::Clamp(Amount, 0.0f, 1.0f); }

	/** Someone awake and hurt on the ground writhes: the animation plays and the muscles follow it. */
	void SetWrithing(bool bOn);

	/** Lets a fallen, conscious body get back up when it has settled. */
	void SetMayGetUp(bool bMay) { bMayGetUp = bMay; }
	/** Benches only: keeps him down whatever the body decides. SetMayGetUp alone was overwritten by the body component every frame,
	 *  depending on which ticked last, and a bench meant to measure a man lying there measured a man standing. */
	void SetHeldDown(bool bHeld) { bHeldDown = bHeld; }
	/** -ImpactArmeEnMain: the weapon was just changed (ALS puts it back on its gun bone): into the hand again at once if he is down. */
	void HoldWeaponAgain();

	bool IsDown() const;
	/** -ImpactSolAnime: on the floor and animated, out of the ragdoll (ALS's LyingDown action). */
	bool IsLyingDown() const;
	bool IsLimp() const { return bLimp; }
	bool IsWrithing() const { return bWrithing; }

	/** Seconds the ragdoll has been at rest (what the ground posture waits for before it starts). */
	float GetSettledTime() const { return SettledTime; }

	/** True while the ground take is playing into the ragdoll slot. */
	bool IsGroundPostureActive() const { return GroundMontage != nullptr || IsLyingDown(); }
	float GetBalanceCapacity() const { return BalanceCapacity; }
	bool IsStaggering() const { return StaggerRemaining > 1.0f; }
	/** How much of the standing hit pose is showing, 0..1 (-ImpactAttitudeGang steps aside for it). */
	float GetHitPoseWeight() const { return HitPoseWeight; }

	/** Short description for the label: "recule", "à terre"... empty when nothing special. */
	FString GetStateText() const;

protected:
	virtual void BeginPlay() override;

private:
	struct FBone
	{
		FName Name;
		int32 Parent = INDEX_NONE;
		float BaseStrength = 1000.0f;
		float RestTone = 1.0f;     // persistent (injury)
		float Tone = 1.0f;         // current, dips on hits
		float Hold = 0.0f;         // seconds the bone stays physical after a hit
		float Blend = 0.0f;        // physics blend weight shown
		float AppliedStrength = -1.0f;
		float Cap = 1.0f;
		bool bSteered = false;          // tone limit this frame (an arm reaching for a wound is soft)
		bool bSimulating = false;
	};

	struct FHand
	{
		bool bActive = false;
		FVector Target = FVector::ZeroVector;
		FVector Outward = FVector::ForwardVector;
		float Weight = 0.0f;
		bool bApproached = false;
		bool bClearedTorso = false;
		float Time = 0.0f;
		float Distance = 1.0e6f;        // centre of the hand to the wound
		float Gap = 1.0e6f;             // surface of the hand to the wound: the real contact
		float ContactTime = 0.0f;       // s the contact has lasted without a break
		bool bReachable = true;
		FVector LastTarget = FVector::ZeroVector;
		FVector TargetVelocity = FVector::ZeroVector; // the wound travels with the body
		bool bHadTarget = false;
	};

	/** Hand against the wound below this gap (cm); contact considered lost above the second one. */
	static constexpr float ContactGap = 3.0f;
	static constexpr float ReleaseGap = 8.0f;
	static constexpr float ContactHoldTime = 0.25f;

	AAlsCharacter* GetCharacter() const;
	int32 FindBone(FName Name) const;
	bool IsInSubtree(int32 Bone, int32 Root) const;
	int32 Distance(int32 A, int32 B) const;
	void UpdateBones(float DeltaTime);
	void UpdateStagger(float DeltaTime);
	void UpdateBalance(float DeltaTime);
	void UpdateDown(float DeltaTime);
	void UpdateHands(float DeltaTime);
	void UpdateLimp(float DeltaTime);
	void UpdateCrawl(float DeltaTime);
	/** -ImpactMortTraversable: a dead body no longer meets the living (pawns) or the camera. */
	void UpdateDeadCollision();
	/** The first moments of the ragdoll after an animated fall: measured ([fin-chute]), and with -ImpactFinChuteDouce calmed. */
	void UpdateFallEnd(float DeltaTime);
	void UpdateBrace(float DeltaTime);
	void UpdateHitPose(float DeltaTime);
	void UpdateGroundPose();
	void UpdateGroundRagdoll(float DeltaTime);
	void UpdatePacedJointTargets(float DeltaTime, float DegreesPerSecond);
	void ReleasePacedJointTargets();
	void UpdateGroundAnchor(float DeltaTime);
	void PushedByPawns(float DeltaTime);
	void StopGroundSlot(float BlendOut);
	bool TryEnterLyingDown();
	/** -ImpactChuteAnimee: a conscious man goes down through a fall animation instead of a ragdoll. */
	bool TryStartAnimatedFall(const FVector& PushVelocity, const TCHAR* Reason);
	void RefreshPoseNow() const;
	/** -ImpactArmeEnMain: while he is down, what he holds rides on his hand bones instead of ALS's gun bones; back on them when he is up. */
	void KeepWeaponInHand(bool bHold);
	void UpdateLyingDown(float DeltaTime);
	// -ImpactSolMixamo / -ImpactReleveMixamo: the Mixamo takes downloaded on 23 October (Tools/import_mixamo_23_10.py).
	UAnimMontage* MakeMixamoGetUp();
	void UpdateMixamoCrawl(float DeltaTime);
	bool IsFacingDown() const;
	bool WantsGroundAim() const;
	bool SwitchLyingTake(UAnimSequenceBase* Take, float Blend);
	// [sol-enchaine] / -ImpactSolEnchaine: when in a take the pose comes closest to the one the body is in now, and by how much the
	// body would have to turn about Z for it to fit. Scores are the RMS distance in cm of head, chest, hands, forearms, feet and calves
	// to the pelvis. Returns false when the take cannot be sampled.
	bool MatchTakePose(const UAnimSequenceBase* Take, float& OutTime, float& OutYaw, float& OutScore, float& OutYawAtZero,
	                   float& OutScoreAtZero, float& OutRawScoreAtZero, FVector& OutPelvisInMesh) const;
	// [elan]: a man hit while he runs, followed for a second.
	void UpdateElanWatch(float DeltaTime);
	void UpdateGroundTakeChoice(float DeltaTime);
	void UpdateGroundAim(float DeltaTime);
	void WatchStandingGhost(FGameplayTag Action);
	// -ImpactChuteMelee: the fall as a ragdoll guided by the fall animation instead of the animation alone.
	UAnimSequenceBase* ChooseFallTake(const FVector& PushVelocity, const TCHAR* Reason, const TCHAR*& Chosen, bool bByPush = false);
	void StartBlendedFall(const FVector& PushVelocity, const TCHAR* Reason, const FTransform& Frame);
	void UpdateBlendedFall(float DeltaTime);
	UAnimSequenceBase* PickMixamoGroundTake() const;
	/** The one way out of LyingDown: back into the ragdoll, so whatever pushes or shoots him acts on a body again. */
	void ReturnToRagdoll(const TCHAR* Reason, bool bLimpNow);
	static FName GroundSlotName();
	void CancelHandCommands();
	void ApplyActiveForce(const FVector& Force, FName Bone, bool bAcceleration);
	void ApplyActiveTorque(const FVector& Torque, FName Bone, bool bAcceleration);
	bool IsArmUsable(int32 Side) const;

	/** Opens the shoulder and elbow of one arm to a living range while it reaches, and puts them back after. */
	void SetArmReaching(int32 Side, bool bReaching);
	void ResetBones();
	void PrepareMesh() const;

	/**
	 * Makes sure the mesh pose the physical animation component is about to read actually exists.
	 * See the comment on the definition: without this the engine indexes an empty array and the game dies.
	 */
	void GuardMusclePose();

	UPROPERTY(Transient)
	TObjectPtr<UPhysicalAnimationComponent> PhysicalAnimation;

	TArray<FBone> Bones;
	int32 PoseGuardSkips = 0;          // frames the muscle drive was held back for want of a pose
	FVector StaggerDirection = FVector::ZeroVector;
	float StaggerRemaining = 0.0f;     // cm still to step
	float BalanceCapacity = 140.0f;

	// The balance loop. The centre of mass we can measure follows the animation, so it never falls out of the
	// support on its own: what diverges has to be kept here. Pushes feed this pendulum, steps cancel it.
	FVector BalanceOffset = FVector::ZeroVector;    // cm, centre of mass away from the support, world XY
	FVector BalanceVelocity = FVector::ZeroVector;  // cm/s
	FVector StepTarget = FVector::ZeroVector;
	float StepTimer = 0.0f;                         // >0 while a catching step is in the air
	float BalanceLogTimer = 0.0f;
	int32 StepsTaken = 0;    // cm the legs can absorb
	float DownTime = 0.0f;
	float SettledTime = 0.0f;
	float GroundContactTime = 0.0f; // how long the pelvis has been on the floor without a break, while down
	int32 ActiveForceCount = 0;
	bool bLimp = false;
	bool bVoluntaryControl = true;
	bool bMayGetUp = true;
	int32 RecentFalls = 0;
	float LastFallTime = -100.0f;
	bool bGaveUpStanding = false;
	bool bWasRagdolling = false;
	bool bWasLying = false;
	bool bHeldDown = false;
	bool bTurnedToThreat = false;
	bool bArmLimitsLogged = false;
	mutable FVector LastCentre = FVector::ZeroVector;
	mutable FVector CentreVelocity = FVector::ZeroVector;
	mutable bool bCentreKnown = false;
	mutable bool bMassesChecked = false;

	// The physics asset limits a shoulder to 50/40/25 degrees, which is right for a corpse and far too little
	// for someone reaching across himself. The original values are kept so they can be put back.
	struct FArmLimits
	{
		bool bSaved = false;
		bool bWide = false;
		uint8 Swing1 = 0;
		uint8 Swing2 = 0;
		uint8 Twist = 0;
		float Swing1Degrees = 0.0f;
		float Swing2Degrees = 0.0f;
		float TwistDegrees = 0.0f;
		uint8 ElbowSwing1 = 0;
		float ElbowSwing1Degrees = 0.0f;
	};
	FArmLimits ArmLimits[2];
	FString LastReason;

	FHand Hands[2];
	// A hit reaction taken from a mocap take, but only the displacement it puts on the trunk, driven by the
	// muscles. The legs and the balance stay with the systems that own them: playing these whole-body froze
	// the legs of a running man while his chest played a standing reaction.
	UPROPERTY(Transient)
	TObjectPtr<class UAnimSequence> HitPoseAnim;
	float HitPoseTime = 0.0f;
	float HitPoseWeight = 0.0f;
	float HitPoseRate = 1.0f;
	float HitPoseLength = 0.0f;

	bool bWrithing = false;
	bool bGroundPosed = false;

	UPROPERTY(Transient)
	TObjectPtr<class UAnimSequenceBase> GroundTake;

	// The looping ground take, played into the ragdoll slot. Kept as a pointer so it can be stopped
	// on its own: Montage_Stop with no argument stops the get-up montage too.
	UPROPERTY(Transient)
	TObjectPtr<class UAnimMontage> GroundMontage;

	// -ImpactSolAnime: the take playing for real while he lies on the floor, and since when.
	UPROPERTY(Transient)
	TObjectPtr<class UAnimMontage> LyingMontage;
	// -ImpactReleveMixamo: the Mixamo get-up playing. It has none of the ALS notify that ends GettingUp: ended from here.
	UPROPERTY(Transient)
	TObjectPtr<class UAnimMontage> MixamoGetUp;
	// -ImpactSolMixamo: he wants to drag himself (SetCrawl asked for it), and how far he has gone in the crawl take.
	bool bMixamoCrawl = false;
	// -ImpactSolArme: see SetGroundAimTarget / SetGroundGoal.
	FVector GroundAimTarget = FVector::ZeroVector;
	FVector CrawlGoal = FVector::ZeroVector;
	bool bGroundAimOn = false;
	bool bCrawlGoalOwned = false;
	bool bGroundGoalReached = false;
	bool bGunOffsetKnown = false;
	float GroundAimError = 180.0f;
	float GunYawOffset = 0.0f;
	float LastTakeSwitch = -100.0f;
	float GroundShotUntil = -1.0f;
	float TakeChoiceTimer = 0.0f;
	float GroundAimLogTimer = 0.0f;
	// -ImpactSolEnchaine: the gun's offset learnt for each prone take, so a swap does not start from zero again.
	TMap<FName, float> GunOffsetByTake;
	// -ImpactSolEnchaine: the turn and shift still to give the body over a take switch's crossfade, so the incoming take lands
	// where the body already lies instead of the crossfade pivoting it.
	float SwitchYawLeft = 0.0f;
	FVector SwitchShiftLeft = FVector::ZeroVector;
	float SwitchFixTimeLeft = 0.0f;
	FVector SwitchPivotInMesh = FVector::ZeroVector;
	// [sol-enchaine]: the half second after a switch - the biggest one-frame jump of head, hands, feet, pelvis, and how far the
	// body's heading (pelvis to head, seen from above) moved.
	TArray<FVector> SwitchPoints;
	float SwitchWatchTime = -1.0f;
	float SwitchWorst = 0.0f;
	int32 SwitchWorstAt = 0;
	float SwitchHeadingFrom = 0.0f;
	float SwitchTurnTravel = 0.0f;
	FString SwitchLabel;
	// [sol-tourne]: the body's heading while lying, three seconds at a time.
	float LyingHeadingLast = 0.0f;
	bool bLyingHeadingKnown = false;
	float LyingTurnWindow = 0.0f;
	float LyingTurnTravel = 0.0f;
	float LyingTurnPeak = 0.0f;
	int32 LyingTurnSwitches = 0;
	// [elan]: set at the hit, -1 when nobody is being watched.
	float ElanTime = -1.0f;
	float ElanSpeed = 0.0f;
	FVector ElanFrom = FVector::ZeroVector;
	FVector ElanLast = FVector::ZeroVector;
	float ElanSmooth = 0.0f;
	float ElanLowest = 0.0f;
	// -ImpactChuteElan: his velocity as he went down (Collapse), carried into the fall's pelvis path.
	FVector RunVelocityAtFall = FVector::ZeroVector;
	FVector BlendedFallCarry = FVector::ZeroVector;
	// [fantome]: frames a man lying on the floor was drawn standing, and how many were logged.
	int32 GhostFrames = 0;
	int32 GhostLogged = 0;
	bool bGhostNow = false;
	// [sol-entree]: where the hands, feet and head were just before the take took over, to measure the jump into it.
	TArray<FVector> EntryPoints;
	// -ImpactSolMixamo: the ragdoll still showing as the take takes over, fading out over LyingPhysicsFade s.
	float LyingPhysicsFade = 0.0f;
	// The mesh slides from the ragdoll pelvis's own turn (From) to the take lying flat (To) during the fade.
	FTransform LyingFadeFrom = FTransform::Identity;
	FTransform LyingFadeTo = FTransform::Identity;
	float EntryWorst = 0.0f;
	int32 EntryWorstAt = 0;
	// [sol-entree-corps] (-ImpactSolEntreeImages): the pelvis, head and right hand bodies, and the component, just before the entry.
	TArray<FVector> EntryBodyStart;
	FVector EntryComponentStart = FVector::ZeroVector;
	int32 EntryTraceFrames = 0;
	// -ImpactSolEntreeJuste: when the entry was last put off a frame to bring the mesh onto the body.
	float LastEntryDeferral = -100.0f;
	int32 EntryDeferrals = 0;
	// -ImpactChuteMelee: the fall animation the ragdoll's joints follow, the frame he stood in when it began (where its pelvis path
	// is drawn), and how far into it he is.
	UPROPERTY(Transient)
	TObjectPtr<class UAnimMontage> BlendedFallMontage;
	// -ImpactChuteGuideJuste: the fall take itself, read directly for the pelvis path.
	UPROPERTY(Transient)
	TObjectPtr<class UAnimSequenceBase> BlendedFallTake;
	bool bBlendedFall = false;
	float BlendedFallTime = 0.0f;
	float BlendedFallLength = 0.0f;
	FTransform BlendedFallFrame = FTransform::Identity;
	FVector BlendedFallLastTarget = FVector::ZeroVector;
	bool bBlendedFallHasTarget = false;
	float BlendedFallDrift = 0.0f;
	float MixamoCrawlDistance = 0.0f;
	float MixamoCrawlLogged = 0.0f;
	float LyingTime = 0.0f;
	bool bGetUpFromLying = false;
	bool bFallingAnimated = false;
	bool bLimpAfterFall = false; // lost consciousness or died during an animated fall: limp once it is over

	// KeepWeaponInHand: each thing that hung on a gun bone, the bone it hung on, where it sat there, and where it sits in the hand.
	struct FCarried
	{
		TWeakObjectPtr<USceneComponent> Item;
		FName Socket;
		FName Hand;
		FTransform OnSocket = FTransform::Identity;
		FTransform InHand = FTransform::Identity;
		bool bGripKnown = false; // InHand learnt from a standing pose
		bool bHeld = false;
		uint8 Collision = 0;     // ECollisionEnabled it had, given back with the gun bone
	};
	TArray<FCarried> Carried;
	bool bWeaponInHand = false;
	float GroundRagdollWeight = 0.0f;
	bool bGroundHeld = false;
	float LastPushLog = 0.0f;
	float LastPushedLog = 0.0f;
	TArray<float> GroundBaseDamping;

	// -ImpactSolCible: the ragdoll joints are given their target by us instead of by the animation, one step at
	// a time from the pose the body actually fell in, so the way into the ground pose has a human pace.
	TArray<FQuat> PacedTargets;
	TArray<FQuat> PacedStart;
	TArray<bool> PacedTargetsSet;
	bool bPacedTargets = false;
	float PacedTime = 0.0f;
	float PacedDuration = -1.0f;

	// What the posture changed and must give back (see the -ImpactSol... switches in the .cpp).
	float GroundMuscleApplied = -1.0f;
	bool bGroundVelocityDrive = false;

	// -ImpactSolAncre: where the curled body is held, and when something last moved him on purpose.
	FVector GroundAnchor = FVector::ZeroVector;
	bool bGroundAnchored = false;
	float LastGroundPushTime = -100.0f;
	float WritheTime = 0.0f;
	float BraceTime = 0.0f;
	bool bBraced = false;   // latched: the hands reached the ground at some point during the fall
	bool bCrawling = false;
	bool bCrawlArmOk[2] = {true, true};
	FVector CrawlDirection = FVector::ZeroVector;
	FVector CrawlLastLocation = FVector::ZeroVector;
	float CrawlDistance = 0.0f;
	float CrawlPhase = 0.0f;
	FVector CrawlPlant = FVector::ZeroVector;
	bool bHasCrawlPlant = false;
	int32 CrawlArm = 0;
	float CrawlLogTimer = 0.0f;
	// The turn onto the front before crawling, measured whichever way it is done: [retournement].
	bool bRolling = false;
	bool bRollStuckLogged = false;
	float RollTime = 0.0f;
	float RollPeakHips = 0.0f;  // rad/s
	float RollPeakLegs = 0.0f;  // rad/s, calves against the pelvis
	float RollPeakRise = 0.0f;  // cm/s, pelvis upwards
	float RollStartZ = 0.0f;
	float RollMaxZ = 0.0f;
	float RollPushTime = 0.0f;   // how long the soft turn has been pushing (it waits for him to land first)
	float RollPeakHipsAt = 0.0f; // s after the start of the turn
	float RollPeakLegsAt = 0.0f;
	float RollPeakRiseAt = 0.0f;
	bool bDead = false;
	bool bDeadCollisionSet = false;
	float FallEndWatch = -1.0f;   // s since the ragdoll took over from a fall, or -1
	float FallEndSettle = 0.0f;   // s of calming left
	float FallEndPeakHips = 0.0f; // rad/s
	float FallEndPeakLimb = 0.0f; // rad/s, the fastest part
	FName FallEndPeakLimbName;
	float FallEndPeakSpeed = 0.0f; // cm/s, pelvis
	float FallEndStartZ = 0.0f;
	float FallEndMaxZ = 0.0f;

	bool bWasOnHurtLeg = false;
	int32 LimpSide = -1;
	float LimpAmount = 0.0f;
	float LimpDip = 0.0f;
	float LimpLean = 0.0f;
	float Weakness = 0.0f;
	float WeakStoop = 0.0f;
	float GutWound = 0.0f;
	float GutStoop = 0.0f;
	float HandLogTimer = 0.0f;
	FVector BaseMeshLocation = FVector::ZeroVector;
	FQuat BaseMeshRotation = FQuat::Identity;
	bool bBaseMeshStored = false;
};

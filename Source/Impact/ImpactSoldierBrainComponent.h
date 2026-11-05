#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "ImpactSoldierBrainComponent.generated.h"

class AAlsCharacter;
class AImpactCivilianController;

enum class EImpactSoldierState : uint8
{
	Idle,
	Engage,    // no cover found: crouched, firing, side-stepping
	SeekCover, // running to a cover
	InCover,   // crouched and hidden
	Peek,      // up or out to fire a burst
	Down       // on the floor
};

enum class EImpactCoverReason : uint8
{
	First,
	Engage,
	Exposed,
	PlayerMoved,
	Flanked,
	TooClose,
	Hit,
	PeeksDone,
	Flank,
	Crawl,
	// Too far to hit from. The mirror of TooClose, which existed from the start while this one did
	// not: a soldier whose player walked out of range stayed put and fired across the map for
	// nothing. See SoldierReach in the .cpp for the measurement.
	TooFar
};

struct FImpactCoverPoint
{
	FVector Location = FVector::ZeroVector;   // on the floor
	FVector Peek = FVector::ZeroVector;       // where he fires from (the spot itself over low cover)
	FVector FromPlayer = FVector::ForwardVector;
	FVector PlayerAnchor = FVector::ZeroVector;
	float Score = 0.0f;
	float PathLength = 0.0f;
	bool bLow = false;      // fired over, standing up
	bool bHideOnly = false; // hides him but there is nowhere to fire from
	bool bValid = false;
};

/**
 * -ImpactPNJCouvert. The brain of an armed man, driven by his controller (AImpactCivilianController) instead of standing still and
 * aiming in the open. He looks for cover from the player - a spot behind a box or a wall that hides him crouched, with somewhere to
 * fire from (standing over a low box, a step out round a corner) - runs there, crouches, comes up or out to fire a burst and gets
 * back down; he changes cover when the player moves round him, sees him, comes too close, or has not been seen for a while (then he
 * goes round). Before he is alerted he still takes cover, without firing. Down with a weapon (-ImpactSolArme), he aims and fires
 * from the floor, and drags himself out of sight when the player can see him. Nothing here moves anyone while
 * AImpactCivilianController::bFleeingDisabled is set (the benches).
 */
UCLASS()
class IMPACT_API UImpactSoldierBrainComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UImpactSoldierBrainComponent();

	static bool IsEnabled();

	void Think(float DeltaTime, AImpactCivilianController& Brain, AAlsCharacter& Me, APawn& Player);
	void ThinkDown(float DeltaTime, AImpactCivilianController& Brain, AAlsCharacter& Me, APawn& Player);
	void OnHurt() { bHitPending = true; }

	/** The shooter's gate: whether he may fire this frame. */
	bool MayFire() const { return bMayFire; }
	/** How long the shooter waits before the first round once he can fire: short at a peek, where he has come up to fire. */
	float GetPeekReact() const;
	FString GetStateText() const;
	EImpactSoldierState GetState() const { return State; }
	/** Seconds since he last saw the player (0 while he sees him). */
	float GetUnseenTime() const { return UnseenTime; }

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(EEndPlayReason::Type EndPlayReason) override;

private:
	void RequestCover(EImpactCoverReason Why, AImpactCivilianController& Brain, AAlsCharacter& Me, APawn& Player);
	bool FindCover(const AAlsCharacter& Me, const APawn& Player, EImpactCoverReason Why, FImpactCoverPoint& Out);
	float ScoreCover(const FImpactCoverPoint& Cover, const AAlsCharacter& Me, const APawn& Player, EImpactCoverReason Why, bool bRifle) const;
	bool CoverStillHidden(const APawn& Player) const;
	bool MuzzleClear(const AAlsCharacter& Me, const APawn& Player) const;
	void StartMoveTo(AImpactCivilianController& Brain, AAlsCharacter& Me, const FVector& Where, bool bStrafe);
	void ApplyPosture(AImpactCivilianController& Brain, AAlsCharacter& Me, APawn& Player);
	void Enter(EImpactSoldierState NewState, float Duration, const TCHAR* Why);
	void Claim(const FVector& Where);
	void Release();

	EImpactSoldierState State = EImpactSoldierState::Idle;
	EImpactCoverReason LastReason = EImpactCoverReason::First;
	FImpactCoverPoint Cover;
	FImpactCoverPoint GroundCover;
	float StateTime = 0.0f;
	float StateDuration = 0.0f;
	float SearchCooldown = 0.0f;
	float ValidateTimer = 0.0f;
	float ExposedTime = 0.0f;
	float UnseenTime = 0.0f;
	float SightTime = 0.0f;
	// How long he has been out of range, and how long he must leave it alone after moving for that
	// reason. Without the second one he relocated every second and never got a burst off.
	float TooFarTime = 0.0f;
	float TooFarCooldown = 0.0f;
	// Where the player was the last time this man actually saw him, and how long he has been sweeping
	// for him since. Before these existed his head was locked on the player's live position at every
	// instant, through walls: measured at 0 to 3 degrees of the player's chest, always, in every state.
	FVector LastSeenAt = FVector::ZeroVector;
	bool bEverSeen = false;
	float ScanTime = 0.0f;
	float StrafeTimer = 0.0f;
	float PeekReact = 0.4f;
	float CrawlTimer = 0.0f;
	float DownExposed = 0.0f;
	float CrawlLogTimer = 0.0f;
	int32 PeeksLeft = 0;
	int32 BurstsAtPeek = 0;
	bool bMayFire = false;
	bool bHitPending = false;
	bool bNoNav = false;
	bool bDirectMove = false;
	bool bCrawling = false;
	bool bGroundAiming = false;
	bool bCoverHidden = true;   // the player cannot see him at his cover (checked twice a second)
	bool bPeekLogged = false;
	bool bPeekFired = false;
	FVector MoveTarget = FVector::ZeroVector;
	FVector LastSeenChest = FVector::ZeroVector;
	TArray<TPair<FVector, float>, TInlineAllocator<4>> Recent;
	mutable FRandomStream Random;
};

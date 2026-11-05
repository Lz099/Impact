#pragma once

#include "CoreMinimal.h"
#include "AIController.h"
#include "ImpactCivilianController.generated.h"

class AAlsCharacter;

/** What a passer-by is doing, shown on the label. */
enum class EImpactCivilianState : uint8
{
	Calm,     // standing around
	Startled, // just heard a shot, about to react
	Fleeing,  // running away from the shooter
	Hiding,   // reached safety, watching the danger
	Down      // on the ground: the body decides
};

/**
 * Brain of a passer-by. Calm until a shot is heard; then, after a short human delay, runs away from the shooter
 * along the navigation mesh. Someone who is hit panics and flees at once if the legs allow it (walking when a leg
 * is hurt), and stays down otherwise. Physical reactions (steps after a hit) pause the run for a moment.
 */
UCLASS()
class IMPACT_API AImpactCivilianController : public AAIController
{
	GENERATED_BODY()

public:
	AImpactCivilianController();

	virtual void Tick(float DeltaTime) override;

	/** A gunshot at Location, fired by Shooter. */
	void HearGunshot(const FVector& Location, AActor* Shooter);

	/** This person was just hit. */
	void Hurt(AActor* Shooter);

	EImpactCivilianState GetState() const { return State; }

	/** Short French text for the label ("fuit", "panique"...), empty when calm. */
	FString GetStateText() const;

	/** Disables fleeing (automatic tests that need people to stay in place). */
	static bool bFleeingDisabled;

protected:
	virtual void OnPossess(APawn* InPawn) override;
	virtual void OnUnPossess() override;

private:
	AAlsCharacter* GetAlsCharacter() const;
	bool CanWalk(float& OutSpeedFactor) const;
	bool RefreshMovementSpeed() const;
	void StartFleeing();
	FVector PickFleePoint() const;
	void UpdateDirectFlee(AAlsCharacter* Als);

	EImpactCivilianState State = EImpactCivilianState::Calm;
	TWeakObjectPtr<AActor> Threat;
	FVector ThreatLocation = FVector::ZeroVector;
	float ReactionDelay = 0.0f;
	float StateTime = 0.0f;
	float RepathTimer = 0.0f;
	bool bHurt = false;
	bool bMovePaused = false;

	// Without a navigation path: run straight away from the threat, turning around obstacles.
	bool bDirectFlee = false;
	FVector FleeDirection = FVector::ForwardVector;
	FVector FleeStart = FVector::ZeroVector;
	FRandomStream Random;
};

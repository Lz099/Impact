#include "ImpactCivilianController.h"

#include "AlsCharacter.h"
#include "AlsCharacterMovementComponent.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Impact.h"
#include "ImpactBodyComponent.h"
#include "ImpactReactionComponent.h"
#include "ImpactActiveBodyComponent.h"
#include "ImpactSoldierBrainComponent.h"
#include "NavigationSystem.h"
#include "Navigation/PathFollowingComponent.h"
#include "Utility/AlsGameplayTags.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(ImpactCivilianController)

namespace
{
	/**
	 * The fastest a body can still go. Flight always asks for a sprint; what it gets depends on what is
	 * left of the man. A chest full of holes takes the sprint away long before the legs give out, and
	 * until this existed nothing did: the comment beside the sprint claimed ALS would hold it down to
	 * "the injury limit", and no such limit had ever been written.
	 */
	FGameplayTag GaitFor(const AActor* Pawn)
	{
		const UImpactBodyComponent* Body = Pawn != nullptr ? Pawn->FindComponentByClass<UImpactBodyComponent>() : nullptr;
		if (Body == nullptr)
		{
			return AlsGaitTags::Sprinting;
		}
		const float Effort = Body->GetEffort();
		return Effort > 0.65f ? AlsGaitTags::Sprinting : Effort > 0.30f ? AlsGaitTags::Running : AlsGaitTags::Walking;
	}
}

bool AImpactCivilianController::bFleeingDisabled = false;

namespace
{
	constexpr float HearingRange = 6000.0f;  // cm: a gunshot is heard far away
	constexpr float FleeDistance = 2500.0f;  // cm run away from the threat
}

AImpactCivilianController::AImpactCivilianController()
{
	PrimaryActorTick.bCanEverTick = true;
}

void AImpactCivilianController::OnPossess(APawn* InPawn)
{
	Super::OnPossess(InPawn);
	Random.Initialize(GetTypeHash(InPawn->GetName()));
	RefreshMovementSpeed();
}

void AImpactCivilianController::OnUnPossess()
{
	if (const AAlsCharacter* Als = GetAlsCharacter())
	{
		if (auto* Movement = Cast<UAlsCharacterMovementComponent>(Als->GetCharacterMovement()))
		{
			Movement->SetMaxGroundSpeedMultiplier(1.0f);
		}
	}
	Super::OnUnPossess();
}

AAlsCharacter* AImpactCivilianController::GetAlsCharacter() const
{
	return Cast<AAlsCharacter>(GetPawn());
}

bool AImpactCivilianController::CanWalk(float& OutSpeedFactor) const
{
	OutSpeedFactor = 1.0f;
	const APawn* MyPawn = GetPawn();
	const UImpactBodyComponent* Body = MyPawn != nullptr ? MyPawn->FindComponentByClass<UImpactBodyComponent>() : nullptr;
	const UImpactReactionComponent* Reaction = MyPawn != nullptr ? MyPawn->FindComponentByClass<UImpactReactionComponent>() : nullptr;
	if (Body == nullptr)
	{
		return true;
	}

	const FImpactPhysiology& Physiology = Body->GetPhysiology();
	const float Legs = FMath::Min(Physiology.GetLimbCapability(EImpactLimb::LeftLeg), Physiology.GetLimbCapability(EImpactLimb::RightLeg));
	const EImpactConsciousness Consciousness = Physiology.GetConsciousness();
	const UImpactActiveBodyComponent* Active = MyPawn != nullptr ? MyPawn->FindComponentByClass<UImpactActiveBodyComponent>() : nullptr;
	if (Legs < 0.25f || Consciousness > EImpactConsciousness::Confused || (Reaction != nullptr && Reaction->IsDown()) || (Active != nullptr && Active->IsDown()))
	{
		return false;
	}

	// How fast he can still go. This used to be 1.0 or 0.5 and nothing in between, so someone emptying of
	// blood ran exactly as fast as a healthy man until he suddenly halved: reported from play as "sa vitesse,
	// sa respiration, tout est constant". It now falls off continuously with the blood, the breath and the legs.
	const float FromBlood = 1.0f - FMath::Clamp(Physiology.GetBloodLossFraction() / 0.4f, 0.0f, 1.0f) * 0.75f;
	const float FromBreath = FMath::Lerp(0.35f, 1.0f, FMath::Clamp(Physiology.GetBrainOxygen(), 0.0f, 1.0f));
	const float FromLegs = FMath::Clamp(Legs, 0.3f, 1.0f);
	OutSpeedFactor = FMath::Min3(FromBlood, FromBreath, FromLegs);
	if (Consciousness == EImpactConsciousness::Confused)
	{
		OutSpeedFactor *= 0.6f;
	}
	return true;
}

bool AImpactCivilianController::RefreshMovementSpeed() const
{
	float SpeedFactor = 1.0f;
	const bool bCanWalk = CanWalk(SpeedFactor);
	if (const AAlsCharacter* Als = GetAlsCharacter())
	{
		if (auto* Movement = Cast<UAlsCharacterMovementComponent>(Als->GetCharacterMovement()))
		{
			Movement->SetMaxGroundSpeedMultiplier(bCanWalk ? SpeedFactor : 0.0f);
		}
	}
	return bCanWalk;
}

void AImpactCivilianController::HearGunshot(const FVector& Location, AActor* Shooter)
{
	const APawn* MyPawn = GetPawn();
	if (MyPawn == nullptr || Shooter == MyPawn || State == EImpactCivilianState::Down)
	{
		return;
	}

	const float Distance = FVector::Distance(Location, MyPawn->GetActorLocation());
	if (Distance > HearingRange)
	{
		return;
	}

	Threat = Shooter;
	ThreatLocation = Location;

	if (State == EImpactCivilianState::Calm)
	{
		// Turn towards the noise, then react after a short human delay (longer when far away).
		State = EImpactCivilianState::Startled;
		StateTime = 0.0f;
		ReactionDelay = Random.FRandRange(0.25f, 0.7f) * (0.6f + Distance / HearingRange);
		SetFocus(Shooter);
	}
	else if (State == EImpactCivilianState::Hiding && Distance < FleeDistance)
	{
		StartFleeing();
	}
}

void AImpactCivilianController::Hurt(AActor* Shooter)
{
	bHurt = true;
	if (UImpactSoldierBrainComponent* Soldier = GetPawn() != nullptr ? GetPawn()->FindComponentByClass<UImpactSoldierBrainComponent>() : nullptr)
	{
		Soldier->OnHurt();
	}
	Threat = Shooter;
	if (Shooter != nullptr)
	{
		ThreatLocation = Shooter->GetActorLocation();
	}

	if (State != EImpactCivilianState::Down && State != EImpactCivilianState::Fleeing)
	{
		// The steps that catch the hit come first, then panic.
		State = EImpactCivilianState::Startled;
		StateTime = 0.0f;
		ReactionDelay = 0.35f;
	}
}

FVector AImpactCivilianController::PickFleePoint() const
{
	const FVector Here = GetPawn()->GetActorLocation();
	FVector Away = (Here - ThreatLocation).GetSafeNormal2D();
	if (Away.IsNearlyZero())
	{
		Away = FRotator(0.0f, Random.FRandRange(0.0f, 360.0f), 0.0f).Vector();
	}
	const FVector Side = FVector::CrossProduct(FVector::UpVector, Away);

	UNavigationSystemV1* Navigation = FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
	for (const float Distance : {FleeDistance, FleeDistance * 0.6f, FleeDistance * 0.3f})
	{
		const FVector Wanted = Here + Away * Distance + Side * Random.FRandRange(-0.35f, 0.35f) * Distance;
		FNavLocation Found;
		if (Navigation != nullptr && Navigation->GetRandomReachablePointInRadius(Wanted, Distance * 0.25f, Found))
		{
			return Found.Location;
		}
	}
	return Here + Away * FleeDistance;
}

void AImpactCivilianController::StartFleeing()
{
	AAlsCharacter* Als = GetAlsCharacter();
	if (Als == nullptr || bFleeingDisabled || !RefreshMovementSpeed())
	{
		return;
	}

	const FVector Point = PickFleePoint();
	ClearFocus(EAIFocusPriority::Gameplay);
	Als->SetDesiredRotationMode(AlsRotationModeTags::VelocityDirection);
	Als->SetDesiredStance(AlsStanceTags::Standing);
	// As fast as what is left of him allows.
	Als->SetDesiredGait(GaitFor(Als));

	const EPathFollowingRequestResult::Type Result = MoveToLocation(Point, 80.0f, true, true, false, false);
	State = EImpactCivilianState::Fleeing;
	StateTime = 0.0f;
	bDirectFlee = Result == EPathFollowingRequestResult::Failed;
	FleeStart = GetPawn()->GetActorLocation();
	FleeDirection = (Point - FleeStart).GetSafeNormal2D();

	UE_LOG(LogImpact, Display, TEXT("[civil] t=%.2f %s %s vers %s (%s)"), GetWorld()->GetTimeSeconds(), *GetPawn()->GetName(),
	       bHurt ? TEXT("panique et fuit") : TEXT("fuit"), *Point.ToCompactString(),
	       Result == EPathFollowingRequestResult::Failed ? TEXT("sans navigation") : TEXT("chemin trouvé"));
}

void AImpactCivilianController::Tick(const float DeltaTime)
{
	Super::Tick(DeltaTime);

	AAlsCharacter* Als = GetAlsCharacter();
	if (Als == nullptr)
	{
		return;
	}
	// Refresh before down/stagger early returns, so recovery and missing body state cannot leave an old limit.
	const bool bCanWalk = RefreshMovementSpeed();
	StateTime += DeltaTime;

	const UImpactReactionComponent* Reaction = Als->FindComponentByClass<UImpactReactionComponent>();
	const UImpactActiveBodyComponent* Active = Als->FindComponentByClass<UImpactActiveBodyComponent>();
	const bool bDown = (Reaction != nullptr && Reaction->IsDown()) || (Active != nullptr && Active->IsDown());
	const FGameplayTag Overlay = Als->GetOverlayMode();
	const bool bArmed = Overlay == AlsOverlayModeTags::Rifle || Overlay == AlsOverlayModeTags::PistolOneHanded
	                    || Overlay == AlsOverlayModeTags::PistolTwoHanded;
	// -ImpactPNJCouvert: an armed man thinks like a soldier (UImpactSoldierBrainComponent), except on the benches that need people to
	// stay where they are.
	UImpactSoldierBrainComponent* Soldier = bArmed && !bFleeingDisabled ? Als->FindComponentByClass<UImpactSoldierBrainComponent>() : nullptr;
	APawn* Player = UGameplayStatics::GetPlayerPawn(this, 0);
	if (bDown)
	{
		if (State != EImpactCivilianState::Down)
		{
			StopMovement();
			State = EImpactCivilianState::Down;
		}
		// An armed man on the floor stops aiming: the aiming arm, driven relative to a trunk that lies on its back, pointed the gun
		// straight up at the sky. He keeps the weapon in his hand.
		Als->SetDesiredAiming(false);
		if (Soldier != nullptr && Player != nullptr)
		{
			Soldier->ThinkDown(DeltaTime, *this, *Als, *Player);
		}
		return;
	}
	if (State == EImpactCivilianState::Down)
	{
		// Back on the feet.
		State = bHurt || Threat.IsValid() ? EImpactCivilianState::Startled : EImpactCivilianState::Calm;
		ReactionDelay = 0.4f;
	}

	// The steps that absorb a hit take over the legs for a moment.
	UPathFollowingComponent* PathFollowing = GetPathFollowingComponent();
	if (Reaction != nullptr && Reaction->IsStaggering())
	{
		if (!bMovePaused && PathFollowing != nullptr && PathFollowing->GetStatus() == EPathFollowingStatus::Moving)
		{
			// -ImpactElanGarde: paused, not stopped. PauseMove's default resets the velocity, which is StopMovementImmediately - a man
			// running at 5,8 m/s stood still in one frame when the round made him take a step. Reported from play: "il s'arrete
			// brusquement". Measured by [elan] and by the "fuite" bench: 20 % of his speed left in the 1,6 s after the round.
			static const bool bKeepMomentum = FParse::Param(FCommandLine::Get(), TEXT("ImpactElanGarde"));
			PathFollowing->PauseMove(FAIRequestID::CurrentRequest,
			                         bKeepMomentum ? EPathFollowingVelocityMode::Keep : EPathFollowingVelocityMode::Reset);
			bMovePaused = true;
		}
		return;
	}
	if (bMovePaused && PathFollowing != nullptr)
	{
		bMovePaused = false;
		PathFollowing->ResumeMove();
		if (State == EImpactCivilianState::Fleeing)
		{
			ClearFocus(EAIFocusPriority::Gameplay);
			Als->SetDesiredRotationMode(AlsRotationModeTags::VelocityDirection);
		}
	}

	// -ImpactPNJArmes: a man with a weapon in his hands does not run from the shooter, he aims at him - for as long as he holds it.
	// Once he has dropped it (a bad wound, see UImpactBodyComponent::UpdateWeaponHold) he is a frightened civilian again.
	if (Soldier != nullptr && Player != nullptr)
	{
		Soldier->Think(DeltaTime, *this, *Als, *Player);
		return;
	}
	if (bArmed)
	{
		if (Player != nullptr)
		{
			if (GetFocusActor() != Player)
			{
				SetFocus(Player);
				StopMovement();
				UE_LOG(LogImpact, Display, TEXT("[arme] t=%.2f %s vise le joueur"), GetWorld()->GetTimeSeconds(), *GetPawn()->GetName());
			}
			Als->SetDesiredRotationMode(AlsRotationModeTags::ViewDirection);
			Als->SetDesiredAiming(true);
			Als->SetDesiredStance(AlsStanceTags::Standing);
		}
		return;
	}

	switch (State)
	{
	case EImpactCivilianState::Startled:
		ReactionDelay -= DeltaTime;
		if (ReactionDelay <= 0.0f)
		{
			StartFleeing();
		}
		break;

	case EImpactCivilianState::Fleeing:
		{
			if (!bCanWalk)
			{
				break;
			}
			Als->SetDesiredGait(GaitFor(Als));

			if (bDirectFlee)
			{
				UpdateDirectFlee(Als);
			}

			const bool bArrived = bDirectFlee
				                      ? FVector::Dist2D(Als->GetActorLocation(), FleeStart) > FleeDistance * 0.8f || StateTime > 9.0f
				                      : StateTime > 0.5f && (PathFollowing == nullptr || PathFollowing->GetStatus() == EPathFollowingStatus::Idle);
			if (bArrived)
			{
				// Far enough: stop, turn to watch the danger, crouch.
				State = EImpactCivilianState::Hiding;
				StateTime = 0.0f;
				if (Threat.IsValid())
				{
					SetFocus(Threat.Get());
				}
				Als->SetDesiredRotationMode(AlsRotationModeTags::ViewDirection);
				Als->SetDesiredGait(AlsGaitTags::Walking);
				Als->SetDesiredStance(AlsStanceTags::Crouching);
				UE_LOG(LogImpact, Display, TEXT("[civil] t=%.2f %s se cache"), GetWorld()->GetTimeSeconds(), *GetPawn()->GetName());
			}
		}
		break;

	default:
		break;
	}
}

void AImpactCivilianController::UpdateDirectFlee(AAlsCharacter* Als)
{
	// Look a little ahead; if a wall is there, try turning left or right until the way is clear.
	const FVector Here = Als->GetActorLocation();
	FCollisionQueryParams Params(SCENE_QUERY_STAT(ImpactFlee), false, Als);
	const auto Clear = [&](const FVector& Direction)
	{
		FHitResult Hit;
		return !GetWorld()->SweepSingleByChannel(Hit, Here, Here + Direction * 180.0f, FQuat::Identity, ECC_Pawn, FCollisionShape::MakeSphere(30.0f), Params);
	};

	if (!Clear(FleeDirection))
	{
		for (const float Angle : {35.0f, -35.0f, 70.0f, -70.0f, 110.0f, -110.0f})
		{
			const FVector Turned = FleeDirection.RotateAngleAxis(Angle, FVector::UpVector);
			if (Clear(Turned))
			{
				FleeDirection = Turned;
				break;
			}
		}
	}
	Als->AddMovementInput(FleeDirection, 1.0f);
}

FString AImpactCivilianController::GetStateText() const
{
	const AAlsCharacter* Als = GetAlsCharacter();
	const UImpactSoldierBrainComponent* Soldier = Als != nullptr ? Als->FindComponentByClass<UImpactSoldierBrainComponent>() : nullptr;
	if (Soldier != nullptr && !bFleeingDisabled)
	{
		const FGameplayTag Overlay = Als->GetOverlayMode();
		if (Overlay == AlsOverlayModeTags::Rifle || Overlay == AlsOverlayModeTags::PistolOneHanded || Overlay == AlsOverlayModeTags::PistolTwoHanded)
		{
			return Soldier->GetStateText();
		}
	}
	switch (State)
	{
	case EImpactCivilianState::Startled:
		return TEXT("sursaute");
	case EImpactCivilianState::Fleeing:
		return bHurt ? TEXT("panique, fuit") : TEXT("fuit");
	case EImpactCivilianState::Hiding:
		return TEXT("se cache");
	default:
		return FString();
	}
}

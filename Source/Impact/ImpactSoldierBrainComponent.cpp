#include "ImpactSoldierBrainComponent.h"

#include "AlsCharacter.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Engine/StaticMeshActor.h"
#include "GameFramework/PlayerController.h"
#include "Impact.h"
#include "ImpactBodyComponent.h"
#include "ImpactCivilianController.h"
#include "ImpactGangAttitudeComponent.h"
#include "ImpactNpcShooterComponent.h"
#include "ImpactReactionComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Navigation/PathFollowingComponent.h"
#include "NavigationPath.h"
#include "NavigationSystem.h"
#include "Utility/AlsGameplayTags.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(ImpactSoldierBrainComponent)

namespace
{
	// How far a soldier can actually hit from. Measured on the soldiers bench, 23 October, by counting
	// where every round went (the log writes "decor", "rate" or "joueur <bone>" for each one):
	//
	//   run              rounds   mean distance   mean cone   scenery   miss   HIT   player killed
	//   soldats1            43        10,2 m        5,1 deg      22      4     17      twice
	//   soldat_terre3       11         7,0 m        4,6 deg       3      2      6      once
	//   soldats6            23        22,9 m        6,0 deg      20      3      0      never
	//
	// The spread is an ANGLE (VRandCone in UImpactNpcShooterComponent), so the miss radius grows with
	// the distance: six degrees at 23 m is about 2,4 m of scatter, far wider than a man. The hit rate
	// follows that square law closely - 40 % observed at 10 m, 0 of 23 at 23 m - so past about 15 m a
	// soldier is firing for nothing. soldats6 is the only run that matches the current binary, and in
	// it the player was never touched once.
	//
	// It is a distance, not an aim bug: the cover brain posted them that far and then let them stay
	// there while the bench walked the player away. -ImpactSoldatPortee= sweeps it (centimetres).
	float SoldierReach(const bool bRifle)
	{
		static const float Reach = []
		{
			float Value = 1500.0f;
			FParse::Value(FCommandLine::Get(), TEXT("ImpactSoldatPortee="), Value);
			return FMath::Max(400.0f, Value);
		}();
		return bRifle ? Reach : Reach * 0.7f;
	}

	// The boxes and walls of the level, read once per world: what a man can crouch behind.
	TWeakObjectPtr<const UWorld> ObstacleWorld;
	TArray<FBox> Obstacles;

	const TArray<FBox>& GetObstacles(const UWorld* World)
	{
		if (ObstacleWorld.Get() != World)
		{
			ObstacleWorld = World;
			Obstacles.Reset();
			// Every actor that carries a static mesh, not only AStaticMeshActor. That one class is what a
			// bare mesh dragged into a level becomes; everything built as a Blueprint is an ordinary
			// AActor with a mesh component inside, and TActorIterator<AStaticMeshActor> never visits one.
			// On L_Als_Playground that was the whole difference between knowing the level and not: of its
			// 211 actors only 22 are AStaticMeshActor - eight engine cubes, two arches, two crates, two
			// barrels, three stairs, a cart, two rocks, a bench and a fountain. The 71 walls, 43 pillars
			// and 17 blocks that actually make the place are BP_SimpleObject Blueprints, and the soldiers
			// could not see a single one of them as cover.
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				bool bHasMesh = false;
				for (const UActorComponent* Part : It->GetComponents())
				{
					const UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(Part);
					if (Mesh != nullptr && Mesh->GetStaticMesh() != nullptr && Mesh->IsCollisionEnabled())
					{
						bHasMesh = true;
						break;
					}
				}
				if (!bHasMesh)
				{
					continue;
				}
				const FBox Box = It->GetComponentsBoundingBox();
				const FVector Extent = Box.GetExtent();
				// Not the floor (flat or huge), not a speck.
				if (!Box.IsValid || Extent.Z < 25.0f || Extent.X > 3000.0f || Extent.Y > 3000.0f || (Extent.X < 20.0f && Extent.Y < 20.0f))
				{
					continue;
				}
				Obstacles.Add(Box);
			}
			UE_LOG(LogImpact, Display, TEXT("[couvert] %d obstacles dans la carte"), Obstacles.Num());
		}
		return Obstacles;
	}

	// Covers taken by soldiers, so two do not crouch behind the same box. Weak: the level reloads after the player's death.
	TArray<TPair<TWeakObjectPtr<const UImpactSoldierBrainComponent>, FVector>> Claims;
	// One cover search per frame, whoever asks: each costs 150 to 250 traces.
	uint64 LastSearchFrame = 0;

	bool IsArmed(const AAlsCharacter& Me)
	{
		const FGameplayTag Overlay = Me.GetOverlayMode();
		return Overlay == AlsOverlayModeTags::Rifle || Overlay == AlsOverlayModeTags::PistolOneHanded || Overlay == AlsOverlayModeTags::PistolTwoHanded;
	}

	// As fast as what is left of him allows (the same rule as a fleeing civilian).
	FGameplayTag RunGait(const AAlsCharacter& Me)
	{
		const UImpactBodyComponent* Body = Me.FindComponentByClass<UImpactBodyComponent>();
		const float Effort = Body != nullptr ? Body->GetEffort() : 1.0f;
		return Effort > 0.65f ? AlsGaitTags::Sprinting : Effort > 0.30f ? AlsGaitTags::Running : AlsGaitTags::Walking;
	}

	// The player's eyes for "can he see me": the head, as SeesPlayer looks from, and the camera, which is what the player looks with.
	void PlayerEyes(const APawn& Player, FVector& Head, FVector& Camera)
	{
		const ACharacter* Target = Cast<ACharacter>(&Player);
		Head = Target != nullptr ? Target->GetMesh()->GetSocketLocation(TEXT("head")) : Player.GetPawnViewLocation();
		Camera = Head;
		if (const APlayerController* Controller = Cast<APlayerController>(Player.GetController()))
		{
			FRotator Unused;
			Controller->GetPlayerViewPoint(Camera, Unused);
		}
	}

	FVector PlayerChest(const APawn& Player)
	{
		const ACharacter* Target = Cast<ACharacter>(&Player);
		return Target != nullptr ? Target->GetMesh()->GetSocketLocation(TEXT("spine_02")) : Player.GetActorLocation();
	}

	// Line of sight on the channel SeesPlayer uses, nobody's body counting as cover.
	FCollisionQueryParams CoverQuery(const UWorld* World)
	{
		FCollisionQueryParams Query(SCENE_QUERY_STAT(ImpactCover), false);
		for (TActorIterator<APawn> It(World); It; ++It)
		{
			Query.AddIgnoredActor(*It);
		}
		return Query;
	}
}

UImpactSoldierBrainComponent::UImpactSoldierBrainComponent()
{
	// The controller drives it.
	PrimaryComponentTick.bCanEverTick = false;
}

bool UImpactSoldierBrainComponent::IsEnabled()
{
	static const bool bEnabled = FParse::Param(FCommandLine::Get(), TEXT("ImpactPNJCouvert"));
	return bEnabled;
}

void UImpactSoldierBrainComponent::BeginPlay()
{
	Super::BeginPlay();
	Random.Initialize(GetTypeHash(GetOwner()->GetName()) ^ 0x5011d1e5);
}

void UImpactSoldierBrainComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	Release();
	Super::EndPlay(EndPlayReason);
}

float UImpactSoldierBrainComponent::GetPeekReact() const
{
	return State == EImpactSoldierState::Peek || State == EImpactSoldierState::Down ? PeekReact : Random.FRandRange(1.0f, 2.0f);
}

FString UImpactSoldierBrainComponent::GetStateText() const
{
	switch (State)
	{
	case EImpactSoldierState::Engage:
		return TEXT("pas de couvert, tire");
	case EImpactSoldierState::SeekCover:
		return LastReason == EImpactCoverReason::Flank ? TEXT("contourne") : LastReason == EImpactCoverReason::First ? TEXT("court se couvrir")
		                                                                                                         : TEXT("change de couvert");
	case EImpactSoldierState::InCover:
		return TEXT("à couvert");
	case EImpactSoldierState::Peek:
		return TEXT("tire");
	case EImpactSoldierState::Down:
		return bCrawling ? TEXT("rampe à l'abri") : bMayFire ? TEXT("tire à terre") : bGroundAiming ? TEXT("vise à terre") : FString();
	default:
		return FString();
	}
}

void UImpactSoldierBrainComponent::Enter(const EImpactSoldierState NewState, const float Duration, const TCHAR* Why)
{
	State = NewState;
	StateTime = 0.0f;
	StateDuration = Duration;
	UE_LOG(LogImpact, Display, TEXT("[soldat] t=%.2f %s : %s (%s)"), GetWorld()->GetTimeSeconds(), *GetOwner()->GetName(), *GetStateText(), Why);
}

void UImpactSoldierBrainComponent::Claim(const FVector& Where)
{
	Claims.RemoveAll([this](const TPair<TWeakObjectPtr<const UImpactSoldierBrainComponent>, FVector>& Each) { return !Each.Key.IsValid() || Each.Key.Get() == this; });
	Claims.Add({this, Where});
}

void UImpactSoldierBrainComponent::Release()
{
	Claims.RemoveAll([this](const TPair<TWeakObjectPtr<const UImpactSoldierBrainComponent>, FVector>& Each) { return !Each.Key.IsValid() || Each.Key.Get() == this; });
}

void UImpactSoldierBrainComponent::StartMoveTo(AImpactCivilianController& Brain, AAlsCharacter& Me, const FVector& Where, const bool bStrafe)
{
	MoveTarget = Where;
	bDirectMove = bNoNav;
	if (!bNoNav)
	{
		const EPathFollowingRequestResult::Type Result = Brain.MoveToLocation(Where, bStrafe ? 15.0f : 40.0f, false, true, true, bStrafe, nullptr, false);
		bDirectMove = Result == EPathFollowingRequestResult::Failed;
	}
}

bool UImpactSoldierBrainComponent::CoverStillHidden(const APawn& Player) const
{
	FVector Head, Camera;
	PlayerEyes(Player, Head, Camera);
	const FCollisionQueryParams Query = CoverQuery(GetWorld());
	for (const FVector& Eye : {Head, Camera})
	{
		for (const float Up : {70.0f, 95.0f})
		{
			if (!GetWorld()->LineTraceTestByChannel(Eye, Cover.Location + FVector(0.0f, 0.0f, Up), ECC_Visibility, Query))
			{
				return false;
			}
		}
	}
	return true;
}

bool UImpactSoldierBrainComponent::MuzzleClear(const AAlsCharacter& Me, const APawn& Player) const
{
	// The head may see over the box while the barrel, lower, would fire into it.
	const FVector Chest = PlayerChest(Player);
	const FVector Muzzle = UImpactNpcShooterComponent::FindMuzzlePoint(&Me, Chest);
	FHitResult Hit;
	return !GetWorld()->LineTraceSingleByChannel(Hit, Muzzle, Chest, ECC_Visibility, CoverQuery(GetWorld())) || Hit.Distance > 200.0f;
}

float UImpactSoldierBrainComponent::ScoreCover(const FImpactCoverPoint& Candidate, const AAlsCharacter& Me, const APawn& Player, const EImpactCoverReason Why,
                                               const bool bRifle) const
{
	const FVector Here = Me.GetActorLocation();
	const FVector P = Player.GetActorLocation();
	const bool bProne = Why == EImpactCoverReason::Crawl;
	float Score = 10.0f;
	// What a metre of walking costs him. At 0,8 a point per metre, on a score that starts at 10, any
	// cover more than about twelve metres away was already worth less than standing still - and the
	// measurement says the nearest wall tall enough to hide behind is 11,2 m away on average, with 64 %
	// of searches made more than 10 m from any of them. So the one number that decides whether a man
	// crosses open ground to reach a wall was set just below the distance at which the walls are.
	// -ImpactSoldatMarche= sweeps it.
	static const float PerMetre = []
	{
		// Swept on the bench, three runs each: 0,8 gave 74 % empty searches, 39 % of the time in the
		// open and 18 % in cover; 0,35 gave 69 / 35 / 20; 0,15 gave 71 / 38 / 18. Hits and kills were
		// flat across all three, so this buys cover without costing lethality.
		float Value = 0.35f;
		FParse::Value(FCommandLine::Get(), TEXT("ImpactSoldatMarche="), Value);
		return FMath::Max(0.0f, Value);
	}();
	Score -= Candidate.PathLength / 100.0f * (bProne ? 2.0f : PerMetre);
	if (Candidate.PathLength > (bProne ? 600.0f : 3000.0f))
	{
		return -100.0f;
	}
	if (!bProne)
	{
		// At a rifle's or a pistol's distance from him, never next to him.
		const float D = FVector::Dist2D(Candidate.Location, P);
		const float Min = bRifle ? 900.0f : 600.0f;
		// Was a flat 25 m for a rifle, with a penalty of 0,3 point per metre past it - which is almost
		// nothing on a score that starts at 10, so a cover 34 m away cost under 3 points and won on its
		// shorter path. That is how the soldiers ended up firing 23 rounds from 22,9 m without touching
		// the player once. The ceiling is now what a man can actually hit from, and leaving it costs
		// four times more.
		const float Max = SoldierReach(bRifle);
		if (D < 400.0f)
		{
			return -100.0f;
		}
		Score -= D < Min ? (Min - D) / 100.0f * 0.6f : D > Max ? (D - Max) / 100.0f * 1.2f : 0.0f;
		// Not towards him, unless going round - or unless the whole point is to close the distance,
		// in which case this penalty was pulling the wrong way.
		if (Why != EImpactCoverReason::Flank && Why != EImpactCoverReason::TooFar && D < FVector::Dist2D(Here, P) - 300.0f)
		{
			Score -= 3.0f * FMath::Max(0.0f, FVector::DotProduct((Candidate.Location - Here).GetSafeNormal2D(), (P - Here).GetSafeNormal2D()));
		}
		// Not across his line of fire.
		const FVector Aim = FRotator(0.0f, Player.GetControlRotation().Yaw, 0.0f).Vector();
		const float SideNow = FVector::CrossProduct(Aim, Here - P).Z;
		const float SideThen = FVector::CrossProduct(Aim, Candidate.Location - P).Z;
		if (FMath::Sign(SideNow) != FMath::Sign(SideThen) && FVector::DotProduct(Aim, Here - P) > 0.0f)
		{
			Score -= 3.0f;
		}
		Score += Candidate.bLow ? 1.5f : 0.0f;
		Score -= Candidate.bHideOnly ? 3.0f : 0.0f;
		// Going round: well to the side of where he was.
		if (Why == EImpactCoverReason::PeeksDone || Why == EImpactCoverReason::Flank)
		{
			const float Angle = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(
				FVector::DotProduct((Candidate.Location - P).GetSafeNormal2D(), (Here - P).GetSafeNormal2D()), -1.0f, 1.0f)));
			if (Angle <= 90.0f)
			{
				Score += 3.0f * FMath::Clamp((Angle - 20.0f) / 40.0f, 0.0f, 1.0f);
			}
		}
	}
	// Not where another soldier is, not back where he just was.
	for (const TPair<TWeakObjectPtr<const UImpactSoldierBrainComponent>, FVector>& Other : Claims)
	{
		if (!Other.Key.IsValid() || Other.Key.Get() == this)
		{
			continue;
		}
		const float D = FVector::Dist2D(Other.Value, Candidate.Location);
		if (D < 250.0f)
		{
			return -100.0f;
		}
		Score -= D < 500.0f ? 2.0f : 0.0f;
	}
	const float Now = GetWorld()->GetTimeSeconds();
	for (const TPair<FVector, float>& Was : Recent)
	{
		if (Now - Was.Value < 20.0f && FVector::Dist2D(Was.Key, Candidate.Location) < 200.0f)
		{
			Score -= 3.0f;
		}
	}
	return Score;
}

bool UImpactSoldierBrainComponent::FindCover(const AAlsCharacter& Me, const APawn& Player, const EImpactCoverReason Why, FImpactCoverPoint& Out)
{
	const double Start = FPlatformTime::Seconds();
	int32 Queries = 0;
	UWorld* World = GetWorld();
	FVector Head, Camera;
	PlayerEyes(Player, Head, Camera);
	const FVector Chest = PlayerChest(Player);
	const FVector Here = Me.GetActorLocation();
	const float FloorZ = Here.Z - Me.GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	const bool bProne = Why == EImpactCoverReason::Crawl;
	const float HideZ = bProne ? 45.0f : 95.0f;
	const float ChestZ = bProne ? 25.0f : 70.0f;
	// TooFar searches as wide as the first search: he has ground to make up, and a 18 m ring around a
	// man standing 34 m away does not contain anything close enough to be worth the move.
	const float Radius = bProne ? 500.0f
	                     : Why == EImpactCoverReason::Engage || Why == EImpactCoverReason::First || Why == EImpactCoverReason::TooFar
	                     ? 2500.0f
	                     : 1800.0f;
	const bool bRifle = Me.GetOverlayMode() == AlsOverlayModeTags::Rifle;
	const FCollisionQueryParams Query = CoverQuery(World);
	const auto Blocked = [&](const FVector& A, const FVector& B)
	{
		++Queries;
		return World->LineTraceTestByChannel(A, B, ECC_Visibility, Query);
	};
	const auto FloorAt = [&](const FVector& P, FVector& OutFloor)
	{
		FHitResult Floor;
		++Queries;
		if (!World->LineTraceSingleByChannel(Floor, FVector(P.X, P.Y, FloorZ + 150.0f), FVector(P.X, P.Y, FloorZ - 150.0f), ECC_Visibility, Query)
		    || Floor.ImpactNormal.Z < 0.75f)
		{
			return false;
		}
		OutFloor = Floor.ImpactPoint;
		++Queries;
		// Room for him crouched.
		return !World->OverlapAnyTestByChannel(OutFloor + FVector(0.0f, 0.0f, 62.0f), FQuat::Identity, ECC_Pawn, FCollisionShape::MakeCapsule(32.0f, 56.0f), Query);
	};
	UNavigationSystemV1* Nav = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
	const ANavigationData* NavData = Nav != nullptr ? Nav->GetDefaultNavDataInstance(FNavigationSystem::DontCreate) : nullptr;
	bNoNav = NavData == nullptr;

	// Spots in the player's shadow: rays from his eyes towards rings round the soldier; where one is blocked, the first free floor
	// just behind the obstacle along it. A ray grazing the end of a wall gives its corner.
	// Four rings of sixteen instead of three of eight: 64 rays where there were 24. This is the phase
	// that works - 12,6 % of its candidates become covers against 1,6 % for the box phase - and it was
	// the one that was capped. Each ray costs one trace plus at most two floor probes, so 64 rays are
	// far cheaper than the 16 000 box candidates they replace.
	int32 FloorHits = 0;
	TArray<FImpactCoverPoint, TInlineAllocator<32>> Found;
	for (int32 Ring = 1; Ring <= 4; ++Ring)
	{
		for (int32 Step = 0; Step < 16; ++Step)
		{
			FVector Toward = Here + FRotator(0.0f, Step * 22.5f + Ring * 7.0f, 0.0f).Vector() * (Radius * Ring / 4.0f);
			// Probed at the height the filter will judge it on, not lower. Measured on the bench: of 2480
			// candidates in one run, 2169 - ninety percent - died on the very first test, "not hidden at
			// 95 cm from the player's head", while the test at 70 cm killed exactly none. The generator
			// was asking "is something in the way at chest height" and the filter was asking "is he
			// hidden standing crouched", so it manufactured candidates it was then certain to reject.
			Toward.Z = FloorZ + HideZ;
			FHitResult Hit;
			++Queries;
			if (!World->LineTraceSingleByChannel(Hit, Head, Toward, ECC_Visibility, Query) || FVector::Dist2D(Hit.ImpactPoint, Head) < 400.0f)
			{
				continue;
			}
			// Not the ground. The ray leaves the player's eye at about 170 cm and aims at 95 cm twelve
			// to twenty-five metres away, so it skims: on anything but a billiard table it meets a rise
			// in the floor long before it meets a wall, and the spot "just behind" a patch of grass is
			// open ground that the hiding test then rightly throws away. A floor faces up, a wall does
			// not, which is the whole test.
			if (Hit.ImpactNormal.Z > 0.7f)
			{
				++FloorHits;
				continue;
			}
			const FVector Along = (Toward - Head).GetSafeNormal2D();
			// 150 and 210 removed: the filter then asks whether something is within 120 cm behind the
			// candidate, so a spot set back further than that can only die on that test. They accounted
			// for the second rejection motive, "colle", 5,8 % of all deaths.
			for (const float Behind : {45.0f, 95.0f})
			{
				FVector Floor;
				if (FloorAt(Hit.ImpactPoint + Along * Behind, Floor))
				{
					if (!Found.ContainsByPredicate([&Floor](const FImpactCoverPoint& Each) { return FVector::Dist2D(Each.Location, Floor) < 80.0f; }))
					{
						FImpactCoverPoint Candidate;
						Candidate.Location = Floor;
						Candidate.FromPlayer = Along;
						Found.Add(Candidate);
					}
					break;
				}
			}
		}
	}

	// How many came from the rings, so the two sources can be told apart in the log: the ring phase is
	// capped at 24 by construction, the box phase is not, and until now nothing said which of the two
	// was producing the candidates that survive.
	const int32 FromRings = Found.Num();

	// What there actually is to hide behind. Before tuning the search any further it has to be known
	// whether the search is failing or whether the ground is simply bare: 64 rays produced 1,5
	// candidates per search, which is the signature of rays that hit nothing at all.
	int32 NearBoxes = 0;
	float Nearest = 1e9f;
	for (const FBox& Box : GetObstacles(World))
	{
		// The true distance to the box in plan, by clamping the man onto it. Subtracting the box's own
		// half-length from the distance to its centre - which is what the radius test of the obstacle
		// phase still does - makes a twenty-metre wall read as one metre away from anywhere along its
		// length, and that is what first made this bench look as though the soldiers were standing on
		// top of cover they could not use.
		const FVector2D Flat(Here.X, Here.Y);
		const FVector2D Onto(FMath::Clamp(Here.X, Box.Min.X, Box.Max.X), FMath::Clamp(Here.Y, Box.Min.Y, Box.Max.Y));
		const float D = FVector2D::Distance(Flat, Onto);
		const bool bTall = Box.Max.Z >= FloorZ + HideZ;
		if (bTall)
		{
			Nearest = FMath::Min(Nearest, D);
			NearBoxes += D <= Radius ? 1 : 0;
		}
	}

	// And round every box and wall near enough: the spot right behind it away from the player, and its four corners.
	for (const FBox& Box : GetObstacles(World))
	{
		const FVector Centre = Box.GetCenter();
		const FVector Extent = Box.GetExtent();
		// A box whose top is below HideZ cannot hide a crouching man, so it must not be a source of
		// candidates either: it was admitting anything 40 cm tall and the filter killed all of it.
		if (FVector::Dist2D(Centre, Here) - FMath::Max(Extent.X, Extent.Y) > Radius || Box.Min.Z > FloorZ + 150.0f
		    || Box.Max.Z < FloorZ + HideZ)
		{
			continue;
		}
		const FVector Away = FVector(Centre.X - Head.X, Centre.Y - Head.Y, 0.0f).GetSafeNormal();
		if (Away.IsNearlyZero())
		{
			continue;
		}
		// Out of the box along the line from the player, and a step further.
		const float ToEdge = FMath::Min(FMath::Abs(Away.X) > 0.01f ? Extent.X / FMath::Abs(Away.X) : 1e6f, FMath::Abs(Away.Y) > 0.01f ? Extent.Y / FMath::Abs(Away.Y) : 1e6f);
		// Points ALONG the far side of the box, not just its middle and its four corners. Five fixed
		// points is right for a crate and useless for a wall: the walls of this level are 20 m long, so
		// the four "corners" sit ten metres out in the open and the filter rightly threw them away.
		// Measured: declaring the walls took the obstacle list from 22 to 172 and the candidates from
		// 2480 to 10724 a run, while the share rejected as "not hidden" stayed at 88 % - more offers,
		// the same bad places. The far side is sampled every two metres instead, which is roughly a
		// man's width apart, plus the two ends.
		TArray<FVector, TInlineAllocator<16>> Spots;
		Spots.Add(Centre + Away * (ToEdge + 55.0f));
		// Points along the length of the box, not only its middle and its four corners: five fixed
		// points is right for a crate and useless for a twenty-metre wall, whose "corners" sit ten
		// metres out in the open.
		//
		// This was removed once on a bad argument and the measurement put it back. Per CANDIDATE the
		// rays are eight times better (12,6 % of them become covers against 1,6 % for the boxes), and
		// that number was read as "the box phase is noise". It is the wrong number: what a soldier
		// needs is a cover per SEARCH, not a good hit rate on a small pool. Measured on three runs
		// each, the same bench:
		//
		//    with these points     72 % of searches empty, 38 % of the time in the open
		//    without them          83 % of searches empty, 55 % of the time in the open
		//
		// Pushing each point out along the box's own thickness instead of the bearing from its centre
		// was tried too, and was worse again (82 % empty): for a wall seen end-on the thickness axis
		// is arbitrary and the point lands beside it. The bearing from the centre is kept.
		const bool bLongX = Extent.X >= Extent.Y;
		const float Half = bLongX ? Extent.X : Extent.Y;
		const FVector AlongBox = bLongX ? FVector(1.0f, 0.0f, 0.0f) : FVector(0.0f, 1.0f, 0.0f);
		const float Depth = bLongX ? Extent.Y : Extent.X;
		const int32 Steps = FMath::Clamp(FMath::CeilToInt(Half / 100.0f), 1, 12);
		for (int32 Step = -Steps; Step <= Steps; ++Step)
		{
			Spots.Add(Centre + AlongBox * (Half * Step / Steps) + Away * (Depth + 55.0f));
		}
		for (const FVector2D Corner : {FVector2D(1.0f, 1.0f), FVector2D(1.0f, -1.0f), FVector2D(-1.0f, 1.0f), FVector2D(-1.0f, -1.0f)})
		{
			Spots.Add(Centre + FVector(Corner.X * (Extent.X + 50.0f), Corner.Y * (Extent.Y + 50.0f), 0.0f));
		}
		for (const FVector& Spot : Spots)
		{
			FVector Floor;
			if (FVector::Dist2D(Spot, Here) > Radius || !FloorAt(Spot, Floor)
			    || Found.ContainsByPredicate([&Floor](const FImpactCoverPoint& Each) { return FVector::Dist2D(Each.Location, Floor) < 80.0f; }))
			{
				continue;
			}
			FImpactCoverPoint Candidate;
			Candidate.Location = Floor;
			Candidate.FromPlayer = FVector(Floor.X - Head.X, Floor.Y - Head.Y, 0.0f).GetSafeNormal();
			Found.Add(Candidate);
		}
	}

	// Why each candidate dies. The search line already printed how many were made and how many were
	// kept, never what killed the rest, and on this bench 630 searches out of 917 keep nothing at all
	// while 21159 candidates produce 817 covers - 3,9 %. Counting by motive costs nothing: the three
	// hiding tests below are the same three terms as before, split out of one ||, in the same order,
	// so the short-circuit and the "requetes" total stay comparable with yesterday's logs.
	int32 RejHaut = 0, RejBas = 0, RejCam = 0, RejColle = 0, RejNav = 0, RejChemin = 0, RejNote = 0;
	int32 KeptRings = 0, KeptBoxes = 0;
	TArray<FImpactCoverPoint, TInlineAllocator<32>> Kept;
	for (int32 Index = 0; Index < Found.Num(); ++Index)
	{
		FImpactCoverPoint& Candidate = Found[Index];
		const bool bFromBox = Index >= FromRings;
		const FVector S = Candidate.Location;
		const FVector Up = FVector::UpVector;
		// Hidden, crouched, from both of the player's eyes.
		if (!Blocked(Head, S + Up * HideZ))
		{
			++RejHaut;
			continue;
		}
		if (!Blocked(Head, S + Up * ChestZ))
		{
			++RejBas;
			continue;
		}
		if (!Blocked(Camera, S + Up * HideZ))
		{
			++RejCam;
			continue;
		}
		// By something right there, not by a wall far off.
		if (!Blocked(S + Up * ChestZ, S + Up * ChestZ - Candidate.FromPlayer * 120.0f))
		{
			++RejColle;
			continue;
		}
		if (!bProne)
		{
			// Somewhere to fire from: standing over it, or a step out to the side.
			if (!Blocked(S + Up * 150.0f, Chest))
			{
				Candidate.bLow = true;
				Candidate.Peek = S;
			}
			else
			{
				const FVector Across = FVector::CrossProduct(Up, Candidate.FromPlayer);
				for (const float Side : {75.0f, -75.0f, 120.0f, -120.0f})
				{
					FVector Floor;
					if (FloorAt(S + Across * Side, Floor) && !Blocked(Floor + Up * 100.0f, Chest))
					{
						Candidate.Peek = Floor;
						break;
					}
				}
			}
			if (Candidate.Peek.IsZero())
			{
				// Hidden but nowhere to fire from: better than the open (scored lower).
				Candidate.bHideOnly = true;
			}
		}
		// Reachable: on the navigation mesh (and not snapped to the player's side of a thin wall), or in a straight line.
		if (NavData != nullptr)
		{
			FNavLocation OnNav;
			if (!Nav->ProjectPointToNavigation(S, OnNav, FVector(50.0f, 50.0f, 150.0f)) || FVector::Dist2D(OnNav.Location, S) > 40.0f)
			{
				++RejNav;
				continue;
			}
			Candidate.PathLength = FVector::Dist2D(Here, S) * 1.3f;
		}
		else
		{
			++Queries;
			if (World->SweepTestByChannel(FVector(Here.X, Here.Y, FloorZ + 60.0f), S + Up * 60.0f, FQuat::Identity, ECC_Pawn, FCollisionShape::MakeSphere(30.0f), Query))
			{
				++RejChemin;
				continue;
			}
			Candidate.PathLength = FVector::Dist2D(Here, S);
		}
		Candidate.Score = ScoreCover(Candidate, Me, Player, Why, bRifle);
		if (Candidate.Score > -50.0f)
		{
			Kept.Add(Candidate);
			++(bFromBox ? KeptBoxes : KeptRings);
		}
		else
		{
			++RejNote;
		}
	}
	Kept.Sort([](const FImpactCoverPoint& A, const FImpactCoverPoint& B) { return A.Score > B.Score; });

	// The best three walked for real: a cover with a long way round is not near.
	bool bFound = false;
	for (int32 Index = 0; Index < Kept.Num() && Index < 3; ++Index)
	{
		FImpactCoverPoint Candidate = Kept[Index];
		if (NavData != nullptr && !bProne)
		{
			UNavigationPath* Path = UNavigationSystemV1::FindPathToLocationSynchronously(World, Here, Candidate.Location, const_cast<AAlsCharacter*>(&Me));
			if (Path == nullptr || !Path->IsValid() || Path->IsPartial()
			    || Path->GetPathLength() > 1.6f * FVector::Dist2D(Here, Candidate.Location) + 300.0f)
			{
				continue;
			}
			Candidate.PathLength = Path->GetPathLength();
			Candidate.Score = ScoreCover(Candidate, Me, Player, Why, bRifle);
		}
		if (!bFound || Candidate.Score > Out.Score)
		{
			Out = Candidate;
			bFound = true;
		}
	}
	if (bFound)
	{
		Out.bValid = true;
		Out.PlayerAnchor = Player.GetActorLocation();
	}
	UE_LOG(LogImpact, Display, TEXT("[couvert] t=%.2f %s : %s, %d candidats, %d gardes, %d requetes, %.2f ms%s%s%s"), World->GetTimeSeconds(), *GetOwner()->GetName(),
	       bFound ? *FString::Printf(TEXT("couvert a %.1f m (joueur a %.1f m), score %.1f, %s"), Out.PathLength / 100.0f,
	                                 FVector::Dist2D(Out.Location, Player.GetActorLocation()) / 100.0f, Out.Score,
	                                 Out.bLow ? TEXT("bas") : Out.bHideOnly ? TEXT("cache seulement") : TEXT("coin"))
	              : TEXT("aucun couvert"),
	       Found.Num(), Kept.Num(), Queries, (FPlatformTime::Seconds() - Start) * 1000.0, bNoNav ? TEXT(", sans navigation") : TEXT(""),
	       *FString::Printf(TEXT("%s | rejets haut95=%d bas70=%d cam95=%d colle=%d nav=%d chemin=%d note=%d | anneaux %d->%d boites %d->%d | obstacles=%d"),
	                        bProne ? TEXT(", a terre") : TEXT(""), RejHaut, RejBas, RejCam, RejColle, RejNav, RejChemin, RejNote,
	                        FromRings, KeptRings, Found.Num() - FromRings, KeptBoxes, GetObstacles(World).Num())
	       , *FString::Printf(TEXT(" | %d murs assez hauts a portee, le plus proche a %.1f m | %d rayons perdus dans le sol"),
	                          NearBoxes, Nearest / 100.0f, FloorHits));
	return bFound;
}

void UImpactSoldierBrainComponent::RequestCover(const EImpactCoverReason Why, AImpactCivilianController& Brain, AAlsCharacter& Me, APawn& Player)
{
	if (SearchCooldown > 0.0f || LastSearchFrame == GFrameCounter)
	{
		return; // again on the next think
	}
	LastSearchFrame = GFrameCounter;
	SearchCooldown = 1.0f;
	LastReason = Why;
	FImpactCoverPoint Best;
	if (FindCover(Me, Player, Why, Best))
	{
		// The cover he is behind still hides him: a new one has to be clearly better, or he would hop between equal spots.
		if (State == EImpactSoldierState::InCover && CoverStillHidden(Player) && Best.Score < Cover.Score + 2.0f
		    && Why != EImpactCoverReason::PeeksDone && Why != EImpactCoverReason::Flank && Why != EImpactCoverReason::Flanked)
		{
			Cover.PlayerAnchor = Player.GetActorLocation();
			return;
		}
		Cover = Best;
		Claim(Cover.Location);
		StartMoveTo(Brain, Me, Cover.Location, false);
		static const TCHAR* Reasons[] = {TEXT("premier"), TEXT("en combat"), TEXT("vu a decouvert"), TEXT("le joueur a bouge"), TEXT("contourne"),
		                                 TEXT("trop pres"), TEXT("touche"), TEXT("assez tire d'ici"), TEXT("le contourne"), TEXT("a terre"),
		                                 TEXT("trop loin")};
		static_assert(static_cast<int32>(EImpactCoverReason::TooFar) == 10, "Reasons[] is indexed by EImpactCoverReason");
		Enter(EImpactSoldierState::SeekCover, 0.0f, Reasons[static_cast<int32>(Why)]);
	}
	// Nothing better was found. For most reasons he stays where he is - but not for the three that say
	// the place itself is no good: too close, seen behind it, and now too far. TooFar was missing here,
	// and that was the rest of the problem: the ceiling on distance and the new reason brought the
	// hits back, yet the average range would not come down below 21 to 25 m whatever the wait was set
	// to, because a man with no cover near enough simply stayed in the far one and kept firing. He now
	// leaves it and closes in the open, which is exactly what the soldiers of the 03:04 run did - the
	// run that still killed the player twice, at 10,2 m and a 40 % hit rate.
	else if (State == EImpactSoldierState::InCover && Why != EImpactCoverReason::TooClose
	         && Why != EImpactCoverReason::Exposed && Why != EImpactCoverReason::TooFar)
	{
		Cover.PlayerAnchor = Player.GetActorLocation();
	}
	else if (State != EImpactSoldierState::Engage)
	{
		Enter(EImpactSoldierState::Engage, 0.0f, TEXT("pas de couvert"));
	}
}

void UImpactSoldierBrainComponent::Think(const float DeltaTime, AImpactCivilianController& Brain, AAlsCharacter& Me, APawn& Player)
{
	UImpactNpcShooterComponent* Gun = Me.FindComponentByClass<UImpactNpcShooterComponent>();
	if (Gun == nullptr || !IsArmed(Me))
	{
		bMayFire = false;
		Release();
		return;
	}
	if (State == EImpactSoldierState::Down)
	{
		// Back on his feet: out of the open first.
		if (UImpactReactionComponent* Reaction = Me.FindComponentByClass<UImpactReactionComponent>())
		{
			Reaction->SetGroundGoal(FVector::ZeroVector, false);
			Reaction->SetGroundAimTarget(FVector::ZeroVector, false);
		}
		bCrawling = false;
		State = EImpactSoldierState::Idle;
		bHitPending = true;
	}
	StateTime += DeltaTime;
	SearchCooldown -= DeltaTime;
	ValidateTimer -= DeltaTime;
	StrafeTimer -= DeltaTime;

	const bool bAlerted = Gun->IsAlerted();
	const FVector Here = Me.GetActorLocation();
	const FVector P = Player.GetActorLocation();
	const float ToPlayer = FVector::Dist2D(Here, P);
	// What the shooter saw, four times a second; stale during an action (getting up), so ignored then.
	const bool bSeen = Gun->SeesPlayerNow() && !Me.GetLocomotionAction().IsValid();
	// Seen for a while close enough, he does not wait for a shot.
	SightTime = bSeen && ToPlayer < 2000.0f ? SightTime + DeltaTime : 0.0f;
	if (!bAlerted && SightTime > 2.5f)
	{
		Gun->Alert();
	}
	// Exposed: the player can see him at his cover (his own head may see over a low box while the box still hides him).
	if (ValidateTimer <= 0.0f && (State == EImpactSoldierState::InCover || State == EImpactSoldierState::SeekCover))
	{
		bCoverHidden = CoverStillHidden(Player);
	}
	ExposedTime = !bCoverHidden && bSeen && (State == EImpactSoldierState::InCover || State == EImpactSoldierState::SeekCover) ? ExposedTime + DeltaTime : 0.0f;
	UnseenTime = bSeen ? 0.0f : UnseenTime + DeltaTime;
	// What he knows, as opposed to what is true. Everything below used the player's live position; this
	// is the only place his own knowledge is written down.
	if (bSeen)
	{
		LastSeenAt = P;
		bEverSeen = true;
	}
	ScanTime += DeltaTime;

	// Where his head actually points, measured here rather than in the subsystem, because only here is
	// the state known. The subsystem's [visee] line averages every armed man in every state and came
	// out at 3 deg whatever was changed - it was sampling Peek and Engage, where he is aiming and
	// SHOULD be locked on. What has to be seen turning is the head in cover. Four times a second.
	{
		static TMap<const UImpactSoldierBrainComponent*, float> LastLook;
		float& When = LastLook.FindOrAdd(this, -100.0f);
		const float Now = GetWorld()->GetTimeSeconds();
		if (Now - When > 0.25f)
		{
			When = Now;
			const FVector Eyes = Me.GetPawnViewLocation();
			const float Look = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct(
				Brain.GetControlRotation().Vector(), (P - Eyes).GetSafeNormal()), -1.0f, 1.0f)));
			UE_LOG(LogImpact, Display, TEXT("[regard] t=%.2f %s : %s, regard a %.0f deg du joueur, le voit %d, perdu depuis %.1f s"),
			       Now, *Me.GetName(), *GetStateText(), Look, bSeen ? 1 : 0, UnseenTime);
		}
	}

	if (bHitPending)
	{
		bHitPending = false;
		if (State != EImpactSoldierState::SeekCover)
		{
			SearchCooldown = 0.0f;
			RequestCover(EImpactCoverReason::Hit, Brain, Me, Player);
		}
	}
	if (ToPlayer < 500.0f && State != EImpactSoldierState::SeekCover && (bAlerted || !UImpactGangAttitudeComponent::IsEnabled()))
	{
		RequestCover(EImpactCoverReason::TooClose, Brain, Me, Player);
	}
	// And its mirror, which did not exist. A soldier picked his cover against where the player stood
	// at that moment; when the player then walked away, InCover only asked "do I still see him from
	// here" and "does this still hide me". Both stayed true across the whole level, so he never moved
	// and kept firing at 26, 30, 34 m. Measured on the soldiers bench, 23 October: 23 rounds from
	// 22,9 m, 20 of them into the scenery, the player untouched for the whole run. The margin is four
	// metres so that a man already at a good distance does not set off looking again for nothing.
	//
	// The two timers are not decoration. Asking every think, as the "too close" rule does, brought the
	// distance down and the hits back - but the soldiers then relocated up to 33 times in 42 s and got
	// only 3 to 9 rounds off instead of 23, because the bench teleports the player 13 to 18 m away
	// every ten seconds and a man cannot close that before being told to move again. So he must be out
	// of range for a sustained moment before it counts, and once he has moved for that reason he is
	// left alone long enough to actually shoot.
	// -ImpactSoldatRepli= is the wait, swept on the bench below.
	static const float TooFarWait = []
	{
		float Value = 2.0f;
		FParse::Value(FCommandLine::Get(), TEXT("ImpactSoldatRepli="), Value);
		return FMath::Max(0.0f, Value);
	}();
	TooFarCooldown = FMath::Max(0.0f, TooFarCooldown - DeltaTime);
	const bool bOutOfReach = bAlerted && ToPlayer > SoldierReach(Me.GetOverlayMode() == AlsOverlayModeTags::Rifle) + 400.0f;
	TooFarTime = bOutOfReach ? TooFarTime + DeltaTime : 0.0f;
	// Peek is excluded, and that omission was a real regression of this rule on the day it was written:
	// a man who has just stood up to fire was sent running again about thirty milliseconds later. Of 98
	// peeks measured over the six runs of 26 October, only 6 ended normally - he began to shoulder the
	// weapon and left. It is also why the volume of fire fell from 23 rounds a run to 13. Peek lasts
	// 2,5 s at most, so the rule gets its turn back immediately afterwards.
	if (TooFarTime > 1.5f && TooFarCooldown <= 0.0f
	    && State != EImpactSoldierState::SeekCover && State != EImpactSoldierState::Idle
	    && State != EImpactSoldierState::Peek)
	{
		TooFarTime = 0.0f;
		TooFarCooldown = TooFarWait;
		RequestCover(EImpactCoverReason::TooFar, Brain, Me, Player);
	}

	switch (State)
	{
	case EImpactSoldierState::Idle:
		// -ImpactAttitudeGang: a gang hangs about on its corner until something starts (a shot heard, a round taken, the player
		// seen close for a while); a soldier takes cover at once.
		if (!UImpactGangAttitudeComponent::IsEnabled() || bAlerted)
		{
			RequestCover(EImpactCoverReason::First, Brain, Me, Player);
		}
		bMayFire = false;
		break;

	case EImpactSoldierState::Engage:
		// Nothing usable found: never still in the open. Crouched, firing, a few steps aside now and then, and looking again.
		bMayFire = bAlerted;
		if (StrafeTimer <= 0.0f)
		{
			StrafeTimer = Random.FRandRange(2.0f, 4.0f);
			const FVector Across = FVector::CrossProduct(FVector::UpVector, (P - Here).GetSafeNormal2D());
			StartMoveTo(Brain, Me, Here + Across * (Random.FRand() < 0.5f ? -1.0f : 1.0f) * Random.FRandRange(300.0f, 500.0f), true);
		}
		if (SearchCooldown <= 0.0f)
		{
			RequestCover(EImpactCoverReason::Engage, Brain, Me, Player);
		}
		break;

	case EImpactSoldierState::SeekCover:
	{
		// Caught in the open on the way, he fires back.
		bMayFire = bAlerted && ExposedTime > 0.3f;
		const float Left = FVector::Dist2D(Here, Cover.Location);
		if (ValidateTimer <= 0.0f)
		{
			ValidateTimer = 0.5f;
			if (!CoverStillHidden(Player))
			{
				RequestCover(EImpactCoverReason::PlayerMoved, Brain, Me, Player);
				break;
			}
		}
		const UPathFollowingComponent* Path = Brain.GetPathFollowingComponent();
		const bool bIdle = !bDirectMove && (Path == nullptr || Path->GetStatus() == EPathFollowingStatus::Idle);
		if (Left < 60.0f || (bIdle && StateTime > 0.5f && Left < 120.0f))
		{
			Brain.StopMovement();
			bDirectMove = false;
			PeeksLeft = Random.RandRange(2, 4);
			Cover.PlayerAnchor = P;
			Recent.Insert({Cover.Location, GetWorld()->GetTimeSeconds()}, 0);
			Recent.SetNum(FMath::Min(Recent.Num(), 4));
			Enter(EImpactSoldierState::InCover, Random.FRandRange(0.8f, 2.0f), TEXT("arrive"));
		}
		else if (StateTime > Cover.PathLength / 250.0f + 3.0f || (bIdle && StateTime > 0.5f))
		{
			// Blocked on the way: that cover counts as a bad one.
			Recent.Insert({Cover.Location, GetWorld()->GetTimeSeconds()}, 0);
			Recent.SetNum(FMath::Min(Recent.Num(), 4));
			SearchCooldown = 0.0f;
			RequestCover(LastReason, Brain, Me, Player);
		}
		break;
	}

	case EImpactSoldierState::InCover:
		bMayFire = false;
		if (Gun->GetMagazineFraction() < 0.4f && !Gun->IsReloading())
		{
			Gun->ReloadNow();
		}
		if (ExposedTime > 0.3f)
		{
			// He sees him behind it: it is no cover any more.
			bMayFire = bAlerted;
			RequestCover(EImpactCoverReason::Exposed, Brain, Me, Player);
			break;
		}
		if (ValidateTimer <= 0.0f)
		{
			ValidateTimer = 0.5f;
			const float Moved = FVector::Dist2D(P, Cover.PlayerAnchor);
			const FVector Now = (Cover.Location - P).GetSafeNormal2D();
			const float Bearing = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct(Cover.FromPlayer, Now), -1.0f, 1.0f)));
			if (Bearing > 60.0f)
			{
				RequestCover(EImpactCoverReason::Flanked, Brain, Me, Player);
				break;
			}
			if (Moved > 400.0f || Bearing > 35.0f)
			{
				// He moved: still hidden from where he is now, and still somewhere to fire from at him, or off to another cover.
				const FVector Up(0.0f, 0.0f, Cover.bLow ? 150.0f : 100.0f);
				const bool bCanFire = !Cover.bHideOnly && !GetWorld()->LineTraceTestByChannel(Cover.Peek + Up, PlayerChest(Player), ECC_Visibility, CoverQuery(GetWorld()));
				if (CoverStillHidden(Player) && bCanFire)
				{
					Cover.PlayerAnchor = P;
				}
				else
				{
					RequestCover(EImpactCoverReason::PlayerMoved, Brain, Me, Player);
					break;
				}
			}
		}
		if (bAlerted && StateTime >= StateDuration && !Gun->IsReloading())
		{
			if (PeeksLeft <= 0 || Cover.bHideOnly)
			{
				RequestCover(EImpactCoverReason::PeeksDone, Brain, Me, Player);
			}
			else if (UnseenTime > 6.0f && Random.FRand() < 0.5f)
			{
				RequestCover(EImpactCoverReason::Flank, Brain, Me, Player);
			}
			else if (UImpactNpcShooterComponent::IsTurnFree(Gun))
			{
				// Up or out to fire, when one of the two firing turns is free: the others stay down.
				BurstsAtPeek = Gun->GetBurstsDone();
				PeekReact = Random.FRandRange(0.3f, 0.6f);
				if (!Cover.bLow)
				{
					StartMoveTo(Brain, Me, Cover.Peek, true);
				}
				Enter(EImpactSoldierState::Peek, 2.5f, TEXT("sort tirer"));
			}
			else
			{
				StateDuration += 0.5f;
			}
		}
		break;

	case EImpactSoldierState::Peek:
	{
		const bool bInPlace = Cover.bLow ? Me.GetStance() == AlsStanceTags::Standing : FVector::Dist2D(Here, Cover.Peek) < 35.0f;
		const bool bClear = MuzzleClear(Me, Player);
		bMayFire = StateTime > 0.3f && bInPlace && bClear;
		const bool bDone = Gun->GetBurstsDone() > BurstsAtPeek || StateTime > 2.5f || (StateTime > 1.2f && !bSeen);
		if (bDone && Gun->GetBurstsDone() <= BurstsAtPeek && !bPeekLogged)
		{
			// Why a peek ended without a burst.
			bPeekLogged = true;
			const FVector Toward = (P - Here).GetSafeNormal2D();
			UE_LOG(LogImpact, Display, TEXT("[soldat] t=%.2f %s : sorti sans tirer (en place %d, canon libre %d, le voit %d, tour libre %d, face au joueur %.0f deg)"),
			       GetWorld()->GetTimeSeconds(), *GetOwner()->GetName(), bInPlace ? 1 : 0, bClear ? 1 : 0, bSeen ? 1 : 0,
			       UImpactNpcShooterComponent::IsTurnFree(Gun) ? 1 : 0,
			       FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct(Me.GetActorForwardVector().GetSafeNormal2D(), Toward), -1.0f, 1.0f))));
		}
		if (bDone)
		{
			bPeekLogged = false;
			// A peek that fired nothing counts twice: this is not a place to fire from any more.
			PeeksLeft -= Gun->GetBurstsDone() > BurstsAtPeek ? 1 : 2;
			if (!Cover.bLow)
			{
				StartMoveTo(Brain, Me, Cover.Location, true);
			}
			Enter(EImpactSoldierState::InCover, Random.FRandRange(1.0f, 3.0f), TEXT("se remet a couvert"));
		}
		break;
	}

	default:
		break;
	}

	// Without a path (no navigation), straight there.
	if (bDirectMove && (State == EImpactSoldierState::SeekCover || State == EImpactSoldierState::Peek || State == EImpactSoldierState::Engage))
	{
		const FVector To = (MoveTarget - Here).GetSafeNormal2D();
		if (FVector::Dist2D(MoveTarget, Here) > 30.0f && !To.IsNearlyZero())
		{
			Me.AddMovementInput(To, 1.0f);
		}
		else
		{
			bDirectMove = false;
		}
	}
	ApplyPosture(Brain, Me, Player);
}

void UImpactSoldierBrainComponent::ApplyPosture(AImpactCivilianController& Brain, AAlsCharacter& Me, APawn& Player)
{
	// ALS ignores a stance asked during an action (a get-up): asked again every think. Never StopMovement here.
	//
	// Where he points his head. This used to be SetFocus(&Player) in every state, and that one line is
	// why nobody ever saw a soldier look for anything: his head was welded to the player's live
	// position at every instant, through walls, measured at 0 to 3 degrees of the player's chest in all
	// 388 readings. It also pointed his rifle through the wall he was hiding behind. A man can only aim
	// at what he knows: the player while he sees him, the place he last saw him for a few seconds
	// after, and after that he sweeps - which is the head turning you are meant to see.
	// bWatchful: behind cover he is not firing, and a man behind cover does not hold a stare on someone
	// he cannot shoot at - he watches his sector and glances. Passed true only there, so that the head
	// turning is seen during the long stretches in cover, and so that coming out to fire is a real turn
	// towards the player rather than a weapon rising on an already-aimed head.
	const auto Focus = [this, &Brain, &Player, &Me](const bool bWatchful = false)
	{
		const bool bSees = UnseenTime <= 0.0f && !bWatchful;
		if (bSees || (!bEverSeen && !bWatchful))
		{
			if (Brain.GetFocusActor() != &Player)
			{
				Brain.SetFocus(&Player);
			}
			return;
		}
		Brain.ClearFocus(EAIFocusPriority::Gameplay);
		const FVector Known = bEverSeen ? LastSeenAt : Player.GetActorLocation();
		if (UnseenTime < 2.5f && !bWatchful)
		{
			// Still on the spot where he went out of sight: the head stays, the man waits.
			Brain.SetFocalPoint(Known, EAIFocusPriority::Gameplay);
			return;
		}
		// Then he sweeps around it, a little under a third of a turn each way, slowly enough to be
		// read as looking rather than as twitching. Each man gets his own phase so five of them do
		// not scan in unison.
		const FVector Here = Me.GetActorLocation();
		const FVector Toward = (Known - Here).GetSafeNormal2D();
		const float Phase = ScanTime * 0.9f + static_cast<float>(GetUniqueID() % 360) * 0.0175f;
		// Narrower behind cover, where he knows roughly where the man is, than when he has lost him.
		const float Sweep = (bWatchful ? 32.0f : 55.0f) * FMath::Sin(Phase);
		Brain.SetFocalPoint(Here + Toward.RotateAngleAxis(Sweep, FVector::UpVector) * 1500.0f
		                    + FVector(0.0f, 0.0f, 120.0f), EAIFocusPriority::Gameplay);
	};
	switch (State)
	{
	case EImpactSoldierState::SeekCover:
	{
		const float Left = FVector::Dist2D(Me.GetActorLocation(), Cover.Location);
		if (Left > 250.0f && !bMayFire)
		{
			// Running, not aiming (ALS will not sprint while aiming).
			Me.SetDesiredStance(AlsStanceTags::Standing);
			Me.SetDesiredGait(Left > 600.0f ? RunGait(Me) : AlsGaitTags::Running);
			Me.SetDesiredRotationMode(AlsRotationModeTags::VelocityDirection);
			Me.SetDesiredAiming(false);
			Brain.ClearFocus(EAIFocusPriority::Gameplay);
		}
		else
		{
			// The last steps crouched, facing him, or firing back on the way.
			Me.SetDesiredStance(Left > 250.0f ? AlsStanceTags::Standing : AlsStanceTags::Crouching);
			Me.SetDesiredGait(AlsGaitTags::Running);
			Me.SetDesiredRotationMode(AlsRotationModeTags::ViewDirection);
			Me.SetDesiredAiming(bMayFire);
			Focus();
		}
		break;
	}
	case EImpactSoldierState::InCover:
		Me.SetDesiredStance(AlsStanceTags::Crouching);
		Me.SetDesiredGait(AlsGaitTags::Walking);
		Me.SetDesiredRotationMode(AlsRotationModeTags::ViewDirection);
		Me.SetDesiredAiming(false);
		Focus(true);
		break;
	case EImpactSoldierState::Peek:
		Me.SetDesiredStance(Cover.bLow ? AlsStanceTags::Standing : AlsStanceTags::Crouching);
		Me.SetDesiredGait(AlsGaitTags::Walking);
		Me.SetDesiredRotationMode(AlsRotationModeTags::ViewDirection);
		Me.SetDesiredAiming(true);
		Focus();
		break;
	case EImpactSoldierState::Engage:
		// -ImpactAttitudeGang: in the open a gangster stands and fires, he does not kneel like a soldier.
		Me.SetDesiredStance(UImpactGangAttitudeComponent::IsEnabled() ? AlsStanceTags::Standing : AlsStanceTags::Crouching);
		Me.SetDesiredGait(AlsGaitTags::Walking);
		Me.SetDesiredRotationMode(AlsRotationModeTags::ViewDirection);
		Me.SetDesiredAiming(true);
		Focus();
		break;
	default:
		Me.SetDesiredAiming(false);
		Focus();
		break;
	}
}

void UImpactSoldierBrainComponent::ThinkDown(const float DeltaTime, AImpactCivilianController& Brain, AAlsCharacter& Me, APawn& Player)
{
	// -ImpactSolArme: on the floor with his weapon. He aims at the player and fires (UImpactReactionComponent turns him and picks the
	// prone take; the shooter fires once the barrel points at him); seen for a few seconds, he drags himself out of sight, and stays
	// there until the player moves.
	static const bool bGroundArmed = FParse::Param(FCommandLine::Get(), TEXT("ImpactSolArme"));
	UImpactReactionComponent* Reaction = Me.FindComponentByClass<UImpactReactionComponent>();
	UImpactNpcShooterComponent* Gun = Me.FindComponentByClass<UImpactNpcShooterComponent>();
	// Cleared BEFORE Enter, not after. Enter writes the state line using GetStateText(), which reads
	// these two: clearing them afterwards meant the journal said "tire a terre" at the exact instant a
	// man fell and would not fire a single round. That one line is what made the ground aiming look as
	// though it worked - 62 readings say otherwise, only 3 of them ready to fire.
	bMayFire = false;
	bGroundAiming = false;
	if (State != EImpactSoldierState::Down)
	{
		Release();
		Brain.StopMovement();
		Enter(EImpactSoldierState::Down, 0.0f, TEXT("tombe"));
		DownExposed = 0.0f;
		CrawlTimer = 0.0f;
		bCrawling = false;
		GroundCover = FImpactCoverPoint();
	}
	if (!bGroundArmed || Reaction == nullptr || Gun == nullptr || !IsArmed(Me) || !Reaction->IsLyingDown())
	{
		if (Reaction != nullptr)
		{
			Reaction->SetGroundGoal(FVector::ZeroVector, false);
			Reaction->SetGroundAimTarget(FVector::ZeroVector, false);
		}
		bCrawling = false;
		return;
	}
	StateTime += DeltaTime;
	CrawlTimer -= DeltaTime;
	SearchCooldown -= DeltaTime;
	const FVector Chest = PlayerChest(Player);
	// Can the player see him lying there.
	FVector Head, Camera;
	PlayerEyes(Player, Head, Camera);
	const FVector MyHead = Me.GetMesh()->GetSocketLocation(TEXT("head")) + FVector(0.0f, 0.0f, 8.0f);
	const FCollisionQueryParams Query = CoverQuery(GetWorld());
	const bool bSeen = !GetWorld()->LineTraceTestByChannel(Head, MyHead, ECC_Visibility, Query)
	                   || !GetWorld()->LineTraceTestByChannel(Camera, MyHead, ECC_Visibility, Query);
	if (bSeen)
	{
		LastSeenChest = Chest;
	}
	DownExposed = bSeen ? DownExposed + DeltaTime : 0.0f;

	const UImpactBodyComponent* Body = Me.FindComponentByClass<UImpactBodyComponent>();
	const bool bAwake = Body == nullptr || Body->GetPhysiology().GetConsciousness() == EImpactConsciousness::Conscious;
	if (bCrawling)
	{
		Reaction->SetGroundAimTarget(LastSeenChest, false);
		if (Reaction->HasGroundGoalReached() || StateTime > 20.0f || !bAwake)
		{
			Reaction->SetGroundGoal(FVector::ZeroVector, false);
			bCrawling = false;
			StateTime = 0.0f;
			UE_LOG(LogImpact, Display, TEXT("[soldat] t=%.2f %s : a l'abri a terre"), GetWorld()->GetTimeSeconds(), *GetOwner()->GetName());
		}
		return;
	}
	// Aiming where the player is, or was last seen: ready when he shows again.
	Reaction->SetGroundAimTarget(LastSeenChest.IsZero() ? Chest : LastSeenChest, true);
	bGroundAiming = bAwake;
	bMayFire = bSeen && Reaction->IsGroundAimReady();
	CrawlLogTimer -= DeltaTime;
	if (CrawlLogTimer <= 0.0f)
	{
		CrawlLogTimer = 1.0f;
		UE_LOG(LogImpact, Display, TEXT("[soldat] t=%.2f %s a terre : le voit %d, pret %d, canon a %.0f deg, expose %.1f s, conscient %d"), GetWorld()->GetTimeSeconds(),
		       *GetOwner()->GetName(), bSeen ? 1 : 0, Reaction->IsGroundAimReady() ? 1 : 0, Reaction->GetGroundAimError(), DownExposed, bAwake ? 1 : 0);
	}
	PeekReact = 0.4f;
	// Seen for a few seconds after shooting back: out of sight, if a hiding spot is near enough to crawl to.
	if (bAwake && DownExposed > 4.0f && CrawlTimer <= 0.0f && SearchCooldown <= 0.0f && LastSearchFrame != GFrameCounter)
	{
		CrawlTimer = 3.0f;
		SearchCooldown = 1.0f;
		LastSearchFrame = GFrameCounter;
		if (FindCover(Me, Player, EImpactCoverReason::Crawl, GroundCover))
		{
			Reaction->SetGroundGoal(GroundCover.Location, true);
			bCrawling = true;
			StateTime = 0.0f;
			UE_LOG(LogImpact, Display, TEXT("[soldat] t=%.2f %s : rampe vers un abri a %.1f m"), GetWorld()->GetTimeSeconds(), *GetOwner()->GetName(),
			       FVector::Dist2D(Me.GetActorLocation(), GroundCover.Location) / 100.0f);
		}
	}
}

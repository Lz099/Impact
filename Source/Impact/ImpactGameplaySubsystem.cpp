#include "ImpactGameplaySubsystem.h"

#include "AIController.h"
#include "AlsCharacter.h"
#include "CanvasItem.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Debug/DebugDrawService.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "HAL/IConsoleManager.h"
#include "Impact.h"
#include "ImpactAutoCapture.h"
#include "ImpactAudio.h"
#include "ImpactCivilianController.h"
#include "ImpactAnatomy.h"
#include "ImpactBodyComponent.h"
#include "ImpactEquipmentComponent.h"
#include "ImpactFX.h"
#include "ImpactReactionComponent.h"
#include "ImpactActiveBodyComponent.h"
#include "ImpactNpcShooterComponent.h"
#include "ImpactSoldierBrainComponent.h"
#include "ImpactGangAttitudeComponent.h"
#include "ImpactPlayerHealthComponent.h"
#include "ImpactTypes.h"
#include "ImpactWeaponComponent.h"
#include "ImpactWeaponPresentationComponent.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "UnrealClient.h"
#include "Misc/CommandLine.h"
#include "NavigationSystem.h"
#include "NavigationPath.h"
#include "NavMesh/NavMeshBoundsVolume.h"
#include "Components/BrushComponent.h"
#include "Engine/StaticMeshActor.h"
#include "Misc/Paths.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/ConstraintInstance.h"
#include "Utility/AlsGameplayTags.h"
#include "HAL/PlatformStackWalk.h"
#include "HAL/FileManager.h"
#include "Animation/AnimSequence.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Misc/FileHelper.h"
#include <atomic>

#include UE_INLINE_GENERATED_CPP_BY_NAME(ImpactGameplaySubsystem)

namespace
{
	/**
	 * -ImpactGuetteGel. Twice the game froze in a bench and stayed frozen until it was killed, and nothing said where: no crash, no
	 * report, nothing in the log after the last line of the frame. The engine's own hang detector is off (HangDuration=0 in
	 * BaseEngine.ini) and turning it on from the command line did nothing visible. So a thread of our own watches the game's ticks;
	 * if none comes for eight seconds, it writes where the game thread and the render thread are stuck, to the log and to
	 * Saved/_preuves_reprise/gel_<date>.txt. It changes nothing in the game.
	 */
	class FImpactHangWatch final : public FRunnable
	{
	public:
		std::atomic<uint64> Beat{0};
		std::atomic<bool> bStop{false};

		virtual uint32 Run() override
		{
			uint64 Seen = 0;
			double Since = FPlatformTime::Seconds();
			bool bReported = false;
			while (!bStop.load())
			{
				FPlatformProcess::Sleep(0.5f);
				const uint64 Now = Beat.load();
				if (Now != Seen || Now == 0)
				{
					Seen = Now;
					Since = FPlatformTime::Seconds();
					bReported = false;
					continue;
				}
				if (bReported || FPlatformTime::Seconds() - Since < 8.0)
				{
					continue;
				}
				bReported = true;
				FPlatformStackWalk::InitStackWalking();
				FString Report = FString::Printf(TEXT("[gel] le jeu ne tourne plus depuis %.0f s\n"), FPlatformTime::Seconds() - Since);
				Report += Stack(TEXT("jeu"), GGameThreadId);
				if (GRenderThreadId != 0)
				{
					Report += Stack(TEXT("rendu"), GRenderThreadId);
				}
				const FString File = FPaths::ProjectSavedDir() / TEXT("_preuves_reprise") / FString::Printf(TEXT("gel_%s.txt"), *FDateTime::Now().ToString());
				FFileHelper::SaveStringToFile(Report, *File);
				UE_LOG(LogImpact, Error, TEXT("%s(ecrit dans %s)"), *Report, *File);
			}
			return 0;
		}

		static FString Stack(const TCHAR* Name, const uint32 ThreadId)
		{
			uint64 Frames[96];
			const int32 Count = FPlatformStackWalk::CaptureThreadStackBackTrace(ThreadId, Frames, UE_ARRAY_COUNT(Frames));
			FString Out = FString::Printf(TEXT("--- fil %s (%u), %d appels\n"), Name, ThreadId, Count);
			for (int32 Index = 0; Index < Count; ++Index)
			{
				ANSICHAR Line[1024];
				Line[0] = '\0';
				FPlatformStackWalk::ProgramCounterToHumanReadableString(Index, Frames[Index], Line, sizeof(Line));
				Out += FString(ANSI_TO_TCHAR(Line)) + TEXT("\n");
			}
			return Out;
		}
	};

	FImpactHangWatch* GHangWatch = nullptr;
	FRunnableThread* GHangWatchThread = nullptr;
}

namespace
{
	const TCHAR* PasserByClassPath = TEXT("/ALS/ALSExtras/AI/B_Als_AICharacter.B_Als_AICharacter_C");
	constexpr int32 PasserByCount = 5;
}

bool UImpactGameplaySubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UImpactGameplaySubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	if (GHangWatch == nullptr && FParse::Param(FCommandLine::Get(), TEXT("ImpactGuetteGel")))
	{
		GHangWatch = new FImpactHangWatch();
		GHangWatchThread = FRunnableThread::Create(GHangWatch, TEXT("ImpactGuetteGel"), 0, TPri_BelowNormal);
	}
	DrawHandle = UDebugDrawService::Register(TEXT("Game"), FDebugDrawDelegate::CreateUObject(this, &ThisClass::DrawOverlay));
	FParse::Value(FCommandLine::Get(), TEXT("ImpactTest="), TestName);
	bXRay = FParse::Param(FCommandLine::Get(), TEXT("ImpactXRay"));

	FString Witnesses;
	if (FParse::Value(FCommandLine::Get(), TEXT("ImpactWitness="), Witnesses, false))
	{
		TArray<FString> Parts;
		Witnesses.ParseIntoArray(Parts, TEXT(","));
		for (const FString& Part : Parts)
		{
			WitnessTimes.Add(FCString::Atof(*Part));
		}
		WitnessTimes.Sort();
	}
}

void UImpactGameplaySubsystem::Deinitialize()
{
	// The watch stops with the world: otherwise the ticks stopping for the shutdown would read as a freeze.
	if (GHangWatch != nullptr)
	{
		GHangWatch->bStop.store(true);
		if (GHangWatchThread != nullptr)
		{
			GHangWatchThread->WaitForCompletion();
			delete GHangWatchThread;
			GHangWatchThread = nullptr;
		}
		delete GHangWatch;
		GHangWatch = nullptr;
	}
	UDebugDrawService::Unregister(DrawHandle);
	Super::Deinitialize();
}

void UImpactGameplaySubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);

	FActorSpawnParameters Parameters;
	Parameters.Name = TEXT("ImpactFX");
	Parameters.ObjectFlags = RF_Transient;
	FX = InWorld.SpawnActor<AImpactFX>(FVector::ZeroVector, FRotator::ZeroRotator, Parameters);

	UE_LOG(LogImpact, Display, TEXT("[impact] systems ready on %s%s"), *InWorld.GetMapName(),
	       TestName.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(", test %s"), *TestName));

	// -ImpactAnimSequentielle. The muscles (UPhysicalAnimationComponent::UpdateTargetActors) read the skeleton's pose arrays
	// with no bounds check, and while the mesh evaluates its animation on a worker thread those arrays are swapped out and left
	// empty: "Array index out of bounds: 2 (or 49) into an array of size 0", three times today, every one at the same address
	// under UPhysicalAnimationComponent::TickComponent (named from the minidumps). GuardMusclePose checks the pose in our own
	// tick, which can come before the mesh starts that evaluation. Evaluated on the game thread, the arrays are never empty.
	if (FParse::Param(FCommandLine::Get(), TEXT("ImpactAnimSequentielle")))
	{
		if (IConsoleVariable* Parallel = IConsoleManager::Get().FindConsoleVariable(TEXT("a.ParallelAnimEvaluation")))
		{
			Parallel->Set(0, ECVF_SetByCode);
			UE_LOG(LogImpact, Display, TEXT("[anims] evaluation de l'animation sur le fil principal (a.ParallelAnimEvaluation 0)"));
		}
	}

	if (FParse::Param(FCommandLine::Get(), TEXT("ImpactSonsArmureWav")))
	{
		ImpactAudio::WriteArmorSoundsToDisk();
		UE_LOG(LogImpact, Display, TEXT("[sons] sons d'armure ecrits dans Saved/_preuves_reprise/sons"));
	}

	// -ImpactAnimsPretes: every animation of ours loaded and compressed now, before anyone is shot, instead of at the first round
	// and read while still being compressed (see AnimReady in ImpactReactionComponent.cpp). Kept loaded for the whole game.
	if (FParse::Param(FCommandLine::Get(), TEXT("ImpactAnimsPretes")))
	{
		TArray<FString> Files;
		const FString Content = FPaths::ProjectContentDir();
		IFileManager::Get().FindFilesRecursive(Files, *(Content / TEXT("Impact/Anims")), TEXT("*.uasset"), true, false);
		for (const FString& File : Files)
		{
			FString Relative = File;
			FPaths::MakePathRelativeTo(Relative, *Content);
			const FString Name = FPaths::GetBaseFilename(File);
			const FString Object = FString::Printf(TEXT("/Game/%s/%s.%s"), *FPaths::GetPath(Relative), *Name, *Name);
			UAnimSequence* Take = LoadObject<UAnimSequence>(nullptr, *Object, nullptr, LOAD_NoWarn | LOAD_Quiet);
			if (Take == nullptr)
			{
				continue;
			}
			const bool bWasReady = Take->IsCompressedDataValid();
			const double Start = FPlatformTime::Seconds();
			if (!bWasReady)
			{
				Take->CacheDerivedDataForCurrentPlatform();
			}
			UE_LOG(LogImpact, Display, TEXT("[anims] %s : %s"), *Name, bWasReady ? TEXT("deja prete au chargement")
			       : *FString::Printf(TEXT("pas prete au chargement, compressee en %.2f s, prete %d"), FPlatformTime::Seconds() - Start,
			                          Take->IsCompressedDataValid() ? 1 : 0));
			PreloadedAnims.Add(Take);
		}
	}
}

TStatId UImpactGameplaySubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UImpactGameplaySubsystem, STATGROUP_Tickables);
}

void UImpactGameplaySubsystem::RegisterBody(UImpactBodyComponent* Body)
{
	Bodies.AddUnique(Body);
}

void UImpactGameplaySubsystem::ReportGunshot(AActor* Shooter, const FVector& Location)
{
	++GunshotCount;
	for (const TWeakObjectPtr<UImpactBodyComponent>& Body : Bodies)
	{
		const APawn* Pawn = Body.IsValid() ? Cast<APawn>(Body->GetOwner()) : nullptr;
		// -ImpactPNJTirent: an armed man who hears a shot within forty metres turns on the player.
		if (UImpactNpcShooterComponent* Shooter = Pawn != nullptr ? Pawn->FindComponentByClass<UImpactNpcShooterComponent>() : nullptr)
		{
			if (FVector::Dist(Pawn->GetActorLocation(), Location) < 4000.0f)
			{
				Shooter->Alert();
			}
		}
		if (AImpactCivilianController* Brain = Pawn != nullptr ? Cast<AImpactCivilianController>(Pawn->GetController()) : nullptr)
		{
			Brain->HearGunshot(Location, Shooter);
		}
	}
}

AAlsCharacter* UImpactGameplaySubsystem::GetPlayerCharacter() const
{
	const APlayerController* Controller = GetWorld()->GetFirstPlayerController();
	return Controller != nullptr ? Cast<AAlsCharacter>(Controller->GetPawn()) : nullptr;
}

void UImpactGameplaySubsystem::Tick(const float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (!GetWorld()->HasBegunPlay())
	{
		return;
	}

	SetupPlayer();
	// The navigation mesh is loaded empty ("Recreating dtNavMesh instance due mismatch in ... maxTiles"), and with dynamic generation
	// nothing rebuilds a tile that nothing has changed: every path failed. Built once, at the start of the game.
	if (!bNavBuilt)
	{
		bNavBuilt = true;
		if (UNavigationSystemV1* Nav = FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld()))
		{
			// And the level's navigation volume covers a 20 m square in one corner of the playground: stretched first over the
			// whole of the decor (the static meshes, the sky left out).
			FBox Decor(ForceInit);
			for (TActorIterator<AStaticMeshActor> It(GetWorld()); It; ++It)
			{
				const FBox Box = It->GetComponentsBoundingBox();
				if (Box.IsValid && Box.GetExtent().X < 50000.0f && Box.GetExtent().Y < 50000.0f)
				{
					Decor += Box;
				}
			}
			for (TActorIterator<ANavMeshBoundsVolume> It(GetWorld()); It && Decor.IsValid; ++It)
			{
				ANavMeshBoundsVolume* Volume = *It;
				UBrushComponent* Brush = Volume->GetBrushComponent();
				if (Brush == nullptr || Brush->Bounds.BoxExtent.IsNearlyZero())
				{
					continue;
				}
				const FVector Unit = Brush->Bounds.BoxExtent / Volume->GetActorScale3D().ComponentMax(FVector(0.001f));
				const FVector Wanted = Decor.GetExtent() + FVector(300.0f);
				Brush->SetMobility(EComponentMobility::Movable);
				Volume->SetActorLocation(Decor.GetCenter());
				Volume->SetActorScale3D(Wanted / Unit);
				Nav->OnNavigationBoundsUpdated(Volume);
				UE_LOG(LogImpact, Display, TEXT("[nav] zone de navigation etiree : centre %s, demi-taille %s"), *Decor.GetCenter().ToCompactString(),
				       *Wanted.ToCompactString());
				break;
			}
			Nav->Build();
			UE_LOG(LogImpact, Display, TEXT("[nav] maillage de navigation construit au lancement"));
		}
	}
	// [nav]: whether the navigation mesh answers, twice at the start: the passers-by's paths all failed ("sans navigation").
	NavReportTimer -= DeltaTime;
	if (NavReportTimer <= 0.0f && NavReports < 2)
	{
		NavReportTimer = 4.0f;
		++NavReports;
		UNavigationSystemV1* Nav = FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
		const ANavigationData* Data = Nav != nullptr ? Nav->GetDefaultNavDataInstance(FNavigationSystem::DontCreate) : nullptr;
		const AAlsCharacter* Who = GetPlayerCharacter();
		FNavLocation OnNav;
		const bool bProjected = Nav != nullptr && Who != nullptr && Nav->ProjectPointToNavigation(Who->GetActorLocation(), OnNav, FVector(200.0f, 200.0f, 400.0f));
		int32 Points = -1;
		if (bProjected)
		{
			const UNavigationPath* Path = UNavigationSystemV1::FindPathToLocationSynchronously(GetWorld(), Who->GetActorLocation(), Who->GetActorLocation() + Who->GetActorForwardVector() * 1500.0f);
			Points = Path != nullptr && Path->IsValid() ? Path->PathPoints.Num() : 0;
		}
		UE_LOG(LogImpact, Display, TEXT("[nav] systeme %s, maillage %s, construction %s, joueur %s sur le maillage, chemin de 15 m : %d points"),
		       Nav != nullptr ? TEXT("present") : TEXT("ABSENT"), Data != nullptr ? *Data->GetName() : TEXT("ABSENT"),
		       Nav != nullptr && Nav->IsNavigationBuildInProgress() ? TEXT("en cours") : TEXT("finie"), bProjected ? TEXT("projete") : TEXT("PAS"), Points);
	}
	if (GHangWatch != nullptr)
	{
		GHangWatch->Beat.fetch_add(1);
	}

	CollisionTimer -= DeltaTime;
	if (CollisionTimer <= 0.0f)
	{
		CollisionTimer = 0.5f;
		ConfigureCharacterCollision();
	}

	RunTest(DeltaTime);
	DetectBugs(DeltaTime);

	// Where the armed passers-by really point their weapons, in play as on the benches: the angle between the barrel and the line
	// from the muzzle to the player's chest, and the same for where their controller looks. Reported from play: "il vise mal le
	// joueur, il vise n'importe comment". The second number tells the brain from the body: a small one with a large first one is
	// the arm and the gun, not the aim.
	AimLogTimer -= DeltaTime;
	if (AimLogTimer <= 0.0f)
	{
		AimLogTimer = 1.0f;
		const ACharacter* PlayerCharacter = UGameplayStatics::GetPlayerCharacter(GetWorld(), 0);
		for (const TWeakObjectPtr<UImpactBodyComponent>& Body : Bodies)
		{
			const AAlsCharacter* Als = Body.IsValid() ? Cast<AAlsCharacter>(Body->GetOwner()) : nullptr;
			const UImpactReactionComponent* React = Als != nullptr ? Als->FindComponentByClass<UImpactReactionComponent>() : nullptr;
			const UImpactActiveBodyComponent* Active = Als != nullptr ? Als->FindComponentByClass<UImpactActiveBodyComponent>() : nullptr;
			if (Als == nullptr || PlayerCharacter == nullptr || (React == nullptr && Active == nullptr) || (React != nullptr && React->IsDown())
			    || (Active != nullptr && Active->IsDown()) || !Als->IsDesiredAiming())
			{
				continue;
			}
			TArray<USkeletalMeshComponent*> Meshes;
			Als->GetComponents(Meshes);
			for (const USkeletalMeshComponent* Weapon : Meshes)
			{
				if (Weapon == Als->GetMesh() || !Weapon->IsVisible() || !Weapon->DoesSocketExist(TEXT("muzzle")))
				{
					continue;
				}
				const FVector Muzzle = Weapon->GetSocketLocation(TEXT("muzzle"));
				const FVector Barrel = (Muzzle - Weapon->GetComponentLocation()).GetSafeNormal();
				const FVector Chest = PlayerCharacter->GetMesh()->GetSocketLocation(TEXT("spine_03"));
				const float GunError = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct(Barrel, (Chest - Muzzle).GetSafeNormal()), -1.0f, 1.0f)));
				const AController* Brain = Als->GetController();
				const FVector Eyes = Als->GetPawnViewLocation();
				const float LookError = Brain != nullptr ? FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct(
					Brain->GetControlRotation().Vector(), (Chest - Eyes).GetSafeNormal()), -1.0f, 1.0f))) : -1.0f;
				// One angle was not enough to say WHAT is wrong. Measured over the six runs of 26 October,
				// 43 readings out of 98 had the barrel 27 to 61 deg off the player while the eyes were on
				// him to within a degree: he looks straight at you and the rifle points at a corner of the
				// scenery. Whether that is the body turned away (yaw) or the weapon held too high or low
				// (pitch) decides which end to fix, so the error is split here rather than guessed at.
				const FVector Want = (Chest - Muzzle).GetSafeNormal();
				const float Pitch = FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp(Barrel.Z, -1.0f, 1.0f))
				                                            - FMath::Asin(FMath::Clamp(Want.Z, -1.0f, 1.0f)));
				const float Yaw = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(
					FVector::DotProduct(Barrel.GetSafeNormal2D(), Want.GetSafeNormal2D()), -1.0f, 1.0f)));
				// And the axis itself is in question: Barrel above is grip-to-muzzle, which is near enough
				// the bore on a rifle and about seventeen degrees high on a pistol, where the grip sits
				// under and behind the slide. The socket's own forward vector is the other candidate. Both
				// are printed until one is shown to be the right one; nothing is changed on a guess.
				const FVector Socket = Weapon->GetSocketQuaternion(TEXT("muzzle")).GetForwardVector();
				const float SocketError = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct(Socket, Want), -1.0f, 1.0f)));
				UE_LOG(LogImpact, Display,
				       TEXT("[visee] t=%.1f %s : canon a %.0f deg du joueur (tangage %+.0f, lacet %.0f), axe douille %.0f deg, regard a %.0f deg (%s, %.0f m)"),
				       GetWorld()->GetTimeSeconds(), *Body->DisplayName, GunError, Pitch, Yaw, SocketError, LookError,
				       *Als->GetOverlayMode().GetTagName().ToString(), FVector::Dist(Muzzle, Chest) / 100.0f);
				break;
			}
		}
	}

	if (bXRay)
	{
		for (const TWeakObjectPtr<UImpactBodyComponent>& Body : Bodies)
		{
			if (Body.IsValid())
			{
				Body->DrawXRay(0.0f);
			}
		}
	}
}

void UImpactGameplaySubsystem::SetupPlayer()
{
	AAlsCharacter* Player = GetPlayerCharacter();
	if (Player == nullptr)
	{
		return;
	}

	if (Player->FindComponentByClass<UImpactWeaponComponent>() == nullptr)
	{
		UImpactWeaponComponent* Weapon = NewObject<UImpactWeaponComponent>(Player, TEXT("ImpactWeapon"));
		Weapon->RegisterComponent();
	}
	// -ImpactPNJTirent: the player can be shot, and every armed man around can shoot him.
	if (UImpactPlayerHealthComponent::IsEnabled())
	{
		if (Player->FindComponentByClass<UImpactPlayerHealthComponent>() == nullptr)
		{
			NewObject<UImpactPlayerHealthComponent>(Player, TEXT("ImpactPlayerHealth"))->RegisterComponent();
		}
		for (const TWeakObjectPtr<UImpactBodyComponent>& Body : Bodies)
		{
			AActor* Owner = Body.IsValid() ? Body->GetOwner() : nullptr;
			if (Owner != nullptr && Owner != Player && Owner->FindComponentByClass<UImpactNpcShooterComponent>() == nullptr)
			{
				NewObject<UImpactNpcShooterComponent>(Owner, TEXT("ImpactNpcShooter"))->RegisterComponent();
			}
			// -ImpactPNJCouvert: and the soldier's brain, used while he holds a weapon.
			if (UImpactSoldierBrainComponent::IsEnabled() && Owner != nullptr && Owner != Player && Owner->FindComponentByClass<UImpactSoldierBrainComponent>() == nullptr)
			{
				NewObject<UImpactSoldierBrainComponent>(Owner, TEXT("ImpactSoldierBrain"))->RegisterComponent();
			}
			// -ImpactAttitudeGang: and how he carries himself.
			if (UImpactGangAttitudeComponent::IsEnabled() && Owner != nullptr && Owner != Player && Owner->FindComponentByClass<UImpactGangAttitudeComponent>() == nullptr)
			{
				NewObject<UImpactGangAttitudeComponent>(Owner, TEXT("ImpactGangAttitude"))->RegisterComponent();
			}
		}
	}

	// -ImpactSansPassants: nobody but the people a bench brings (no passer-by walking into its line of fire).
	if (!bPassersBySpawned)
	{
		bPassersBySpawned = true;
		if (!FParse::Param(FCommandLine::Get(), TEXT("ImpactSansPassants")))
		{
			SpawnPassersBy(Player);
		}
	}
}

void UImpactGameplaySubsystem::SpawnPassersBy(const AAlsCharacter* Player)
{
	UClass* PasserByClass = LoadClass<ACharacter>(nullptr, PasserByClassPath);
	if (PasserByClass == nullptr)
	{
		UE_LOG(LogImpact, Warning, TEXT("[impact] passer-by class not found: %s"), PasserByClassPath);
		return;
	}

	UWorld* World = GetWorld();
	const FRotator Facing(0.0f, Player->GetControlRotation().Yaw, 0.0f);
	const FVector Forward = Facing.Vector();
	const FVector Right = FRotationMatrix(Facing).GetUnitAxis(EAxis::Y);

	FCollisionQueryParams Params(SCENE_QUERY_STAT(ImpactSpawn), false);
	Params.AddIgnoredActor(Player);

	for (int32 i = 0; i < PasserByCount; ++i)
	{
		const float Side = (i - (PasserByCount - 1) * 0.5f) * 250.0f;
		const FVector Wanted = Player->GetActorLocation() + Forward * 750.0f + Right * Side;

		// Stand them on whatever floor is there.
		FHitResult Floor;
		if (!World->LineTraceSingleByChannel(Floor, Wanted + FVector(0, 0, 300), Wanted - FVector(0, 0, 600), ECC_Visibility, Params))
		{
			continue;
		}

		const FVector Location = Floor.ImpactPoint + FVector(0, 0, 95.0f);
		const FRotator Rotation(0.0f, (Player->GetActorLocation() - Location).Rotation().Yaw, 0.0f);

		ACharacter* PasserBy = World->SpawnActorDeferred<ACharacter>(PasserByClass, FTransform(Rotation, Location), nullptr, nullptr,
		                                                             ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn);
		if (PasserBy == nullptr)
		{
			continue;
		}

		PasserBy->AIControllerClass = AImpactCivilianController::StaticClass();
		PasserBy->AutoPossessAI = EAutoPossessAI::Spawned;

		UImpactBodyComponent* Body = NewObject<UImpactBodyComponent>(PasserBy, TEXT("ImpactBody"));
		Body->DisplayName = FString::Printf(TEXT("Passant %d"), i + 1);
		PasserBy->AddInstanceComponent(Body);
		Body->RegisterComponent();

		// -ImpactCorpsActif: the always-physical body instead of the reaction component, not beside it.
		if (UImpactActiveBodyComponent::IsEnabled())
		{
			UImpactActiveBodyComponent* Active = NewObject<UImpactActiveBodyComponent>(PasserBy, TEXT("ImpactCorpsActif"));
			PasserBy->AddInstanceComponent(Active);
			Active->RegisterComponent();
		}
		else
		{
			UImpactReactionComponent* Reaction = NewObject<UImpactReactionComponent>(PasserBy, TEXT("ImpactReaction"));
			PasserBy->AddInstanceComponent(Reaction);
			Reaction->RegisterComponent();
		}

		PasserBy->FinishSpawning(FTransform(Rotation, Location));

		// -ImpactPNJArmes: each passer-by holds a weapon, in turn a rifle, a pistol in one hand, a pistol in two. ALS does the rest:
		// its overlay mode puts the weapon mesh in the hand and the matching arm animation layer on top of the locomotion, so every
		// reaction - hit, stagger, fall, ground posture - is seen on an armed man as it is on an unarmed one.
		static const bool bArmed = FParse::Param(FCommandLine::Get(), TEXT("ImpactPNJArmes"));
		if (bArmed)
		{
			if (AAlsCharacter* Als = Cast<AAlsCharacter>(PasserBy))
			{
				const FGameplayTag Weapons[] = {AlsOverlayModeTags::Rifle, AlsOverlayModeTags::PistolOneHanded, AlsOverlayModeTags::PistolTwoHanded};
				// The three weapons in rotation were meant for an animation bench: one of each, so a hit,
				// a stagger and a fall could be seen on all three. Since -ImpactPNJCouvert gives every
				// passer-by a soldier's brain it also gives three soldiers out of five a handgun, which
				// does not read as soldiers - and ALS only has running and sprinting poses for the rifle,
				// so the pistol carriers run with a frozen standing arm. Soldiers get the rifle; the
				// rotation stays for the bench, where nobody is a soldier.
				// -ImpactAttitudeGang: a gang carries handguns too, one in every two - the pistol held on its side is the first
				// thing that tells a gangster from a soldier, and the attitude layer gives it its running arm.
				const FGameplayTag Weapon = UImpactGangAttitudeComponent::IsEnabled() ? Weapons[i % 2]
				                            : UImpactSoldierBrainComponent::IsEnabled()
					                            ? Weapons[0]
					                            : Weapons[i % UE_ARRAY_COUNT(Weapons)];
				Als->SetOverlayMode(Weapon);
				UE_LOG(LogImpact, Display, TEXT("[impact] Passant %d arme : %s"), i + 1, *Weapon.GetTagName().ToString());
			}
		}

		if (i == 0)
		{
			// The physics asset bodies are what bullets can hit: list them once for reference.
			FString Names;
			for (const FBodyInstance* Instance : PasserBy->GetMesh()->Bodies)
			{
				if (Instance != nullptr && Instance->BodySetup.IsValid())
				{
					Names += Instance->BodySetup->BoneName.ToString() + TEXT(" ");
				}
			}
			UE_LOG(LogImpact, Display, TEXT("[impact] physics bodies: %s"), *Names);
		}
	}

	ConfigureCharacterCollision();
	UE_LOG(LogImpact, Display, TEXT("[impact] %d passers-by placed"), Bodies.Num());
}

ACharacter* UImpactGameplaySubsystem::SpawnTestPerson(const int32 WeaponKind, const int32 Vest, const bool bHelmet)
{
	// Asked for: "rajoute une section test pour les PNJ civils et une section test pour les PNJ avec arme, avec gilet, avec casque,
	// pour que je teste". Made exactly as the passers-by are (SpawnPassersBy), placed in rows of five in front of the player,
	// facing him, and tagged so they can be removed again.
	AAlsCharacter* Player = GetPlayerCharacter();
	UClass* PasserByClass = LoadClass<ACharacter>(nullptr, PasserByClassPath);
	UWorld* World = GetWorld();
	if (Player == nullptr || PasserByClass == nullptr || World == nullptr)
	{
		return nullptr;
	}
	const FRotator Facing(0.0f, Player->GetActorRotation().Yaw, 0.0f);
	const FVector Forward = Facing.Vector();
	const FVector Right = FRotationMatrix(Facing).GetUnitAxis(EAxis::Y);
	const int32 Place = TestPeopleMade % 5;
	const int32 Row = (TestPeopleMade / 5) % 3;
	const FVector Wanted = Player->GetActorLocation() + Forward * (600.0f + Row * 220.0f) + Right * ((Place - 2) * 200.0f);

	FHitResult Floor;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(ImpactTestSpawn), false, Player);
	if (!World->LineTraceSingleByChannel(Floor, Wanted + FVector(0, 0, 300), Wanted - FVector(0, 0, 600), ECC_Visibility, Params))
	{
		return nullptr;
	}
	const FVector Location = Floor.ImpactPoint + FVector(0, 0, 95.0f);
	const FRotator Rotation(0.0f, (Player->GetActorLocation() - Location).Rotation().Yaw, 0.0f);
	ACharacter* Person = World->SpawnActorDeferred<ACharacter>(PasserByClass, FTransform(Rotation, Location), nullptr, nullptr,
	                                                           ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn);
	if (Person == nullptr)
	{
		return nullptr;
	}
	++TestPeopleMade;
	Person->AIControllerClass = AImpactCivilianController::StaticClass();
	Person->AutoPossessAI = EAutoPossessAI::Spawned;
	Person->Tags.Add(TEXT("ImpactPNJTest"));

	const int32 Kind = FMath::Clamp(WeaponKind, 0, 3);
	const int32 Plate = FMath::Clamp(Vest, 0, 3);
	static const TCHAR* WeaponNames[] = {TEXT("civil"), TEXT("fusil"), TEXT("pistolet"), TEXT("pistolet 2 mains")};
	static const TCHAR* VestNames[] = {TEXT(""), TEXT(" - gilet leger"), TEXT(" - gilet moyen"), TEXT(" - gilet lourd")};
	UImpactBodyComponent* Body = NewObject<UImpactBodyComponent>(Person, TEXT("ImpactBody"));
	Body->DisplayName = FString::Printf(TEXT("Test %d - %s%s%s"), TestPeopleMade, WeaponNames[Kind], VestNames[Plate], bHelmet ? TEXT(" - casque") : TEXT(""));
	Person->AddInstanceComponent(Body);
	Body->RegisterComponent();
	if (UImpactActiveBodyComponent::IsEnabled())
	{
		UImpactActiveBodyComponent* Active = NewObject<UImpactActiveBodyComponent>(Person, TEXT("ImpactCorpsActif"));
		Person->AddInstanceComponent(Active);
		Active->RegisterComponent();
	}
	else
	{
		UImpactReactionComponent* Reaction = NewObject<UImpactReactionComponent>(Person, TEXT("ImpactReaction"));
		Person->AddInstanceComponent(Reaction);
		Reaction->RegisterComponent();
	}
	Person->FinishSpawning(FTransform(Rotation, Location));

	if (Kind > 0)
	{
		if (AAlsCharacter* Als = Cast<AAlsCharacter>(Person))
		{
			const FGameplayTag Weapons[] = {AlsOverlayModeTags::Rifle, AlsOverlayModeTags::PistolOneHanded, AlsOverlayModeTags::PistolTwoHanded};
			Als->SetOverlayMode(Weapons[Kind - 1]);
		}
	}
	if (Plate > 0 || bHelmet)
	{
		UImpactEquipmentComponent* Equipment = NewObject<UImpactEquipmentComponent>(Person);
		Person->AddInstanceComponent(Equipment);
		Equipment->RegisterComponent();
		Equipment->SetVest(Plate);
		Equipment->SetHelmet(bHelmet);
	}
	ConfigureCharacterCollision();
	UE_LOG(LogImpact, Display, TEXT("[test-pnj] %s place a %.0f m"), *Body->DisplayName, FVector::Dist(Location, Player->GetActorLocation()) / 100.0f);
	return Person;
}

void UImpactGameplaySubsystem::RunGutTest(const float Time, AAlsCharacter* Player, UImpactWeaponComponent* Weapon)
{
	AImpactCivilianController::bFleeingDisabled = true;
	CurrentTestTarget = 0;
	ACharacter* Target = Bodies.IsValidIndex(0) && Bodies[0].IsValid() ? Cast<ACharacter>(Bodies[0]->GetOwner()) : nullptr;
	if (Target == nullptr)
	{
		return;
	}
	if (TestStep == 0 && Time >= 1.0f)
	{
		Player->SetActorLocation(Target->GetActorLocation() + Target->GetActorForwardVector() * 300.0f, false, nullptr, ETeleportType::TeleportPhysics);
		Player->SetOverlayMode(AlsOverlayModeTags::PistolTwoHanded);
		Player->SetDesiredAiming(true);
		if (AAIController* Brain = Cast<AAIController>(Target->GetController()))
		{
			Brain->ClearFocus(EAIFocusPriority::Gameplay);
			Brain->StopMovement();
		}
		TestStep = 1;
	}
	if (TestStep == 1 && Time >= 2.0f)
	{
		AimAt(FMath::Lerp(GetBonePoint(0, TEXT("pelvis")), GetBonePoint(0, TEXT("spine_01")), 0.7f));
	}
	if (TestStep == 1 && Time >= 4.0f)
	{
		Weapon->SetTriggerHeld(true);
		Weapon->SetTriggerHeld(false);
		UE_LOG(LogImpact, Display, TEXT("[ventre] t=%.1f une balle dans le ventre"), Time);
		TestStep = 2;
	}
	// Twice a second: how far the trunk leans from upright (pelvis to upper chest), how high the pelvis is, and whether he stands.
	GutLogTimer -= GetWorld()->GetDeltaSeconds();
	if (GutLogTimer <= 0.0f && Time >= 2.5f)
	{
		GutLogTimer = 0.5f;
		const USkeletalMeshComponent* Mesh = Target->GetMesh();
		const FVector Trunk = (Mesh->GetSocketLocation(TEXT("spine_03")) - Mesh->GetSocketLocation(TEXT("pelvis"))).GetSafeNormal();
		FHitResult Floor;
		const FVector Hips = Mesh->GetSocketLocation(TEXT("pelvis"));
		FCollisionQueryParams Params(SCENE_QUERY_STAT(ImpactGut), false, Target);
		const float Height = GetWorld()->LineTraceSingleByChannel(Floor, Hips, Hips - FVector(0, 0, 300), ECC_Visibility, Params) ? Hips.Z - Floor.ImpactPoint.Z : -1.0f;
		const UImpactReactionComponent* React = Target->FindComponentByClass<UImpactReactionComponent>();
		UE_LOG(LogImpact, Display, TEXT("[ventre] t=%.1f tronc penche de %.0f deg | bassin a %.0f cm du sol | %s"), Time,
		       FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(Trunk.Z, -1.0f, 1.0f))), Height,
		       React != nullptr && React->IsDown() ? TEXT("a terre") : TEXT("debout"));
	}
}

void UImpactGameplaySubsystem::RunSoldierBench(const float Time, AAlsCharacter* Player, UImpactWeaponComponent* Weapon)
{
	// Nobody is held in place here: the soldiers' brains run as in play.
	if (TestStep == 0 && Time >= 1.0f)
	{
		Player->SetOverlayMode(AlsOverlayModeTags::Rifle);
		Player->SetDesiredAiming(true);
		SoldierBenchOrigin = Player->GetActorLocation();
		TestStep = 1;
	}
	if (TestStep == 0)
	{
		return;
	}
	// Moves every 10 s, round the soldiers.
	static const FVector Spots[] = {FVector(0.0f, 0.0f, 0.0f), FVector(700.0f, 900.0f, 0.0f), FVector(-600.0f, 1000.0f, 0.0f), FVector(400.0f, -500.0f, 0.0f)};
	const int32 Spot = FMath::Min(static_cast<int32>(Time / 10.0f), static_cast<int32>(UE_ARRAY_COUNT(Spots)) - 1);
	if (Spot != SoldierBenchSpot)
	{
		SoldierBenchSpot = Spot;
		FVector Where = SoldierBenchOrigin + Spots[Spot];
		if (UNavigationSystemV1* Nav = FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld()))
		{
			FNavLocation OnNav;
			if (Nav->ProjectPointToNavigation(Where, OnNav, FVector(300.0f, 300.0f, 300.0f)))
			{
				Where = OnNav.Location + FVector(0.0f, 0.0f, Player->GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
			}
		}
		Player->SetActorLocation(Where, false, nullptr, ETeleportType::TeleportPhysics);
		UE_LOG(LogImpact, Display, TEXT("[soldats] t=%.1f le joueur va en %d : %s"), Time, Spot, *Where.ToCompactString());
	}
	// Aims at the nearest soldier still standing, and fires a burst every 6 s.
	int32 Nearest = INDEX_NONE;
	float Best = FLT_MAX;
	for (int32 Index = 0; Index < Bodies.Num(); ++Index)
	{
		const UImpactBodyComponent* Body = Bodies[Index].Get();
		const UImpactReactionComponent* Reaction = Body != nullptr ? Body->GetOwner()->FindComponentByClass<UImpactReactionComponent>() : nullptr;
		if (Body == nullptr || Body->GetPhysiology().IsDead() || (Reaction != nullptr && Reaction->IsDown()))
		{
			continue;
		}
		const float D = FVector::Dist(Body->GetOwner()->GetActorLocation(), Player->GetActorLocation());
		if (D < Best)
		{
			Best = D;
			Nearest = Index;
		}
	}
	if (Nearest != INDEX_NONE)
	{
		CurrentTestTarget = Nearest;
		AimAt(GetBonePoint(Nearest, TEXT("spine_03")));
	}
	const float Phase = FMath::Fmod(Time, 6.0f);
	Weapon->SetTriggerHeld(Time > 3.0f && Phase > 3.0f && Phase < 3.5f);
}

void UImpactGameplaySubsystem::RunGangBench(const float Time, AAlsCharacter* Player, UImpactWeaponComponent* Weapon)
{
	// -ImpactAttitudeGang, seen on photos. For -ImpactGangCalme= seconds (12) the player stands far off (25 m past the farthest of
	// them, beyond the 20 m at which a man seen for a while alerts them), does not aim and does not fire: they hang about. Then he
	// comes back to where he started and the soldiers bench runs as it is, its clock starting there.
	static const float Calm = []
	{
		float Value = 12.0f;
		FParse::Value(FCommandLine::Get(), TEXT("ImpactGangCalme="), Value);
		return FMath::Max(0.0f, Value);
	}();
	// The witness films the man nearest to where the player started (the soldiers bench then films the one it aims at).
	const auto Nearest = [this](const FVector& From)
	{
		int32 Best = INDEX_NONE;
		float BestDistance = FLT_MAX;
		for (int32 Index = 0; Index < Bodies.Num(); ++Index)
		{
			const UImpactBodyComponent* Body = Bodies[Index].Get();
			if (Body != nullptr && !Body->GetPhysiology().IsDead())
			{
				const float Distance = FVector::Dist(Body->GetOwner()->GetActorLocation(), From);
				if (Distance < BestDistance)
				{
					BestDistance = Distance;
					Best = Index;
				}
			}
		}
		return Best;
	};
	if (GangBenchStep == 0 && Time >= 0.5f)
	{
		GangBenchStep = 1;
		GangBenchOrigin = Player->GetActorLocation();
		Player->SetOverlayMode(AlsOverlayModeTags::Rifle);
		Player->SetDesiredAiming(false);
		FVector Centre = FVector::ZeroVector;
		float Spread = 0.0f;
		int32 Count = 0;
		for (const TWeakObjectPtr<UImpactBodyComponent>& Body : Bodies)
		{
			if (Body.IsValid())
			{
				Centre += Body->GetOwner()->GetActorLocation();
				++Count;
			}
		}
		Centre = Count > 0 ? Centre / Count : GangBenchOrigin;
		for (const TWeakObjectPtr<UImpactBodyComponent>& Body : Bodies)
		{
			if (Body.IsValid())
			{
				Spread = FMath::Max(Spread, FVector::Dist2D(Body->GetOwner()->GetActorLocation(), Centre));
			}
		}
		FVector Away = (GangBenchOrigin - Centre).GetSafeNormal2D();
		Away = Away.IsNearlyZero() ? FVector::ForwardVector : Away;
		FVector Where = Centre + Away * (Spread + 2500.0f);
		if (UNavigationSystemV1* Nav = FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld()))
		{
			FNavLocation OnNav;
			if (Nav->ProjectPointToNavigation(Where, OnNav, FVector(600.0f, 600.0f, 400.0f)))
			{
				Where = OnNav.Location + FVector(0.0f, 0.0f, Player->GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
			}
		}
		if (Calm > 0.0f)
		{
			Player->SetActorLocation(Where, false, nullptr, ETeleportType::TeleportPhysics);
		}
		UE_LOG(LogImpact, Display, TEXT("[gang-banc] t=%.1f calme %.0f s : le joueur s'eloigne a %.0f m du groupe (%d hommes, etendue %.0f m)"),
		       Time, Calm, FVector::Dist2D(Where, Centre) / 100.0f, Count, Spread / 100.0f);
	}
	if (GangBenchStep == 1 && Time < Calm)
	{
		Weapon->SetTriggerHeld(false);
		CurrentTestTarget = Nearest(GangBenchOrigin);
		return;
	}
	if (GangBenchStep == 1)
	{
		GangBenchStep = 2;
		Player->SetActorLocation(GangBenchOrigin, false, nullptr, ETeleportType::TeleportPhysics);
		UE_LOG(LogImpact, Display, TEXT("[gang-banc] t=%.1f le joueur revient, banc des soldats"), Time);
	}
	if (GangBenchStep == 2)
	{
		RunSoldierBench(Time - Calm, Player, Weapon);
		// -ImpactGangTete: the player aims at the head instead of the chest, so men die (what a death does to the weapon he held).
		static const bool bHead = FParse::Param(FCommandLine::Get(), TEXT("ImpactGangTete"));
		if (bHead && Bodies.IsValidIndex(CurrentTestTarget))
		{
			AimAt(GetBonePoint(CurrentTestTarget, TEXT("head")));
		}
		// The soldiers bench films the man it fires at, who is soon bleeding and unarmed: here the witness films the nearest
		// other one still standing with his weapon, the one whose attitude is to be seen.
		const int32 Shot = CurrentTestTarget;
		int32 Best = INDEX_NONE;
		float BestDistance = FLT_MAX;
		for (int32 Index = 0; Index < Bodies.Num(); ++Index)
		{
			const UImpactBodyComponent* Body = Bodies[Index].Get();
			const AAlsCharacter* Him = Body != nullptr ? Cast<AAlsCharacter>(Body->GetOwner()) : nullptr;
			const UImpactReactionComponent* Reaction = Him != nullptr ? Him->FindComponentByClass<UImpactReactionComponent>() : nullptr;
			if (Index == Shot || Him == nullptr || Body->GetPhysiology().IsDead() || (Reaction != nullptr && Reaction->IsDown())
			    || Him->GetOverlayMode() == AlsOverlayModeTags::Default)
			{
				continue;
			}
			const float Distance = FVector::Dist(Him->GetActorLocation(), Player->GetActorLocation());
			if (Distance < BestDistance)
			{
				BestDistance = Distance;
				Best = Index;
			}
		}
		if (Best != INDEX_NONE && WitnessFrame < 0)
		{
			CurrentTestTarget = Best;
		}
	}
}

void UImpactGameplaySubsystem::RunGangPoseBench(const float Time, AAlsCharacter* Player, UImpactWeaponComponent* Weapon)
{
	// -ImpactAttitudeGang, close up and without chance: the brains are off, the first pistol man aims at the player 6 m away and
	// makes each move in turn, then the first rifleman. Photos (-ImpactWitness=) at 4, 6.9, 9.8, 12.9, 15.9, 18.8, 24, 26.9, 29.9.
	AImpactCivilianController::bFleeingDisabled = true;
	UImpactGangAttitudeComponent::bBenchMovesOnly = true;
	Weapon->SetTriggerHeld(false);
	const auto Man = [this](const int32 Index) -> AAlsCharacter*
	{
		return Bodies.IsValidIndex(Index) && Bodies[Index].IsValid() ? Cast<AAlsCharacter>(Bodies[Index]->GetOwner()) : nullptr;
	};
	if (TestStep == 0 && Time >= 0.5f)
	{
		TestStep = 1;
		Player->SetOverlayMode(AlsOverlayModeTags::Rifle);
		Player->SetDesiredAiming(false);
		for (int32 Index = 0; Index < Bodies.Num(); ++Index)
		{
			const AAlsCharacter* Him = Man(Index);
			const FGameplayTag Overlay = Him != nullptr ? Him->GetOverlayMode() : FGameplayTag();
			if (GangPoseIndex[0] == INDEX_NONE && (Overlay == AlsOverlayModeTags::PistolOneHanded || Overlay == AlsOverlayModeTags::PistolTwoHanded))
			{
				GangPoseIndex[0] = Index;
			}
			else if (GangPoseIndex[1] == INDEX_NONE && Overlay == AlsOverlayModeTags::Rifle)
			{
				GangPoseIndex[1] = Index;
			}
		}
		UE_LOG(LogImpact, Display, TEXT("[gang-pose] pistolet : passant %d, fusil : passant %d"), GangPoseIndex[0] + 1, GangPoseIndex[1] + 1);
	}
	if (TestStep == 0)
	{
		return;
	}
	const int32 Which = Time < 21.0f ? 0 : 1;
	AAlsCharacter* Him = Man(GangPoseIndex[Which]);
	if (Him == nullptr)
	{
		return;
	}
	CurrentTestTarget = GangPoseIndex[Which];
	// The player 6 m in front of him, once per man.
	if (TestStep == 1 + Which)
	{
		TestStep = 2 + Which;
		const FVector There = Him->GetActorLocation();
		FVector Toward = (Player->GetActorLocation() - There).GetSafeNormal2D();
		Toward = Toward.IsNearlyZero() ? Him->GetActorForwardVector() : Toward;
		const FVector Where = There + Toward * 600.0f;
		Player->SetActorLocation(Where, false, nullptr, ETeleportType::TeleportPhysics);
		Player->SetActorRotation(FRotator(0.0f, (There - Where).Rotation().Yaw, 0.0f));
		UE_LOG(LogImpact, Display, TEXT("[gang-pose] t=%.1f le joueur se place a 6 m de %s"), Time, *Him->GetName());
	}
	Him->SetDesiredAiming(true);
	Him->SetDesiredStance(AlsStanceTags::Standing);
	if (AAIController* AI = Cast<AAIController>(Him->GetController()); AI != nullptr && AI->GetFocusActor() != Player)
	{
		AI->SetFocus(Player);
	}
	// One move every three seconds, each photographed nine tenths of a second in.
	static const int32 PistolMoves[] = {1, 2, 3, 4, 5};
	static const float PistolTimes[] = {6.0f, 9.0f, 12.0f, 15.0f, 18.0f};
	static const int32 RifleMoves[] = {4, 1};
	static const float RifleTimes[] = {26.0f, 29.0f};
	UImpactGangAttitudeComponent* Attitude = Him->FindComponentByClass<UImpactGangAttitudeComponent>();
	const auto Play = [&](const int32* Moves, const float* Times, const int32 Count)
	{
		for (int32 Index = 0; Index < Count; ++Index)
		{
			if (Attitude != nullptr && GangPoseMoveDone == Index + (Which == 0 ? 0 : 10) && Time >= Times[Index])
			{
				GangPoseMoveDone = Index + (Which == 0 ? 0 : 10) + 1;
				Attitude->BenchMove(Moves[Index]);
			}
		}
	};
	if (Which == 0)
	{
		Play(PistolMoves, PistolTimes, UE_ARRAY_COUNT(PistolMoves));
	}
	else
	{
		if (GangPoseMoveDone < 10)
		{
			GangPoseMoveDone = 10;
		}
		Play(RifleMoves, RifleTimes, UE_ARRAY_COUNT(RifleMoves));
	}
}

void UImpactGameplaySubsystem::RunActiveBench(const float Time, AAlsCharacter* Player, UImpactWeaponComponent* Weapon)
{
	// -ImpactActifRafale=: how long the burst lasts, s.
	static const float BurstLength = [] { float Value = 1.0f; FParse::Value(FCommandLine::Get(), TEXT("ImpactActifRafale="), Value); return Value; }();
	// An armed target is a soldier: his brain runs (it does nothing while people are held in place).
	static const bool bArmedTarget = FCString::Strifind(FCommandLine::Get(), TEXT("ImpactActifArme=")) != nullptr;
	AImpactCivilianController::bFleeingDisabled = !bArmedTarget;
	if (TestStep == 0 && Time >= 1.0f)
	{
		Player->SetOverlayMode(AlsOverlayModeTags::Rifle);
		Player->SetDesiredAiming(true);
		// -ImpactActifGilet=N: the same man in a vest, the burst stopped by the plates (no hole: he can get up).
		int32 Vest = 0;
		FParse::Value(FCommandLine::Get(), TEXT("ImpactActifGilet="), Vest);
		// -ImpactActifArme=N: armed (1 rifle, 2 pistol in one hand, 3 in two); -ImpactActifDos: turned away, so a round to the
		// leg drops him on his front.
		int32 Kind = 0;
		FParse::Value(FCommandLine::Get(), TEXT("ImpactActifArme="), Kind);
		ActiveBenchTarget = SpawnTestPerson(Kind, Vest, false);
		if (ActiveBenchTarget.IsValid() && FParse::Param(FCommandLine::Get(), TEXT("ImpactActifDos")))
		{
			ActiveBenchTarget->SetActorRotation((ActiveBenchTarget->GetActorLocation() - Player->GetActorLocation()).GetSafeNormal2D().Rotation());
		}
		TestStep = 1;
	}
	const ACharacter* Who = ActiveBenchTarget.Get();
	const int32 Index = Bodies.IndexOfByPredicate([Who](const TWeakObjectPtr<UImpactBodyComponent>& Body) { return Body.IsValid() && Body->GetOwner() == Who; });
	if (Index == INDEX_NONE)
	{
		return;
	}
	CurrentTestTarget = Index;
	if (TestStep >= 1 && TestStep <= 4)
	{
		// -ImpactActifVisee=<os>: where the burst goes (the chest otherwise).
		static const FString BurstBone = [] { FString Value = TEXT("spine_03"); FParse::Value(FCommandLine::Get(), TEXT("ImpactActifVisee="), Value); return Value; }();
		AimAt(GetBonePoint(Index, TestStep == 2 ? TEXT("spine_01") : TestStep >= 3 ? *BurstBone : TEXT("spine_03")));
	}
	// -ImpactActifRelance=s: once he is down, a round in the arm every s seconds, to send him out of his ground take and back into
	// it again and again.
	static const bool bRelaunchRead = FParse::Value(FCommandLine::Get(), TEXT("ImpactActifRelance="), BenchRelaunch);
	if (bRelaunchRead && TestStep == 5)
	{
		AimAt(GetBonePoint(Index, TEXT("upperarm_r")));
	}
	// -ImpactActifRafaleSeule: the burst only, on a vest the single rounds have not worn yet.
	static const bool bBurstOnly = FParse::Param(FCommandLine::Get(), TEXT("ImpactActifRafaleSeule"));
	if (bBurstOnly && TestStep < 3)
	{
		TestStep = Time >= 3.0f ? 3 : TestStep;
	}
	else if ((TestStep == 1 && Time >= 3.0f) || (TestStep == 2 && Time >= 4.5f))
	{
		Weapon->SetTriggerHeld(true);
		Weapon->SetTriggerHeld(false);
		UE_LOG(LogImpact, Display, TEXT("[actif] t=%.1f une balle"), Time);
		++TestStep;
	}
	else if (TestStep == 3 && Time >= 6.0f && FParse::Param(FCommandLine::Get(), TEXT("ImpactActifRenverse")))
	{
		// -ImpactActifRenverse: instead of the burst, he is knocked over backwards without a wound, to see him get up.
		if (UImpactReactionComponent* React = ActiveBenchTarget.IsValid() ? ActiveBenchTarget->FindComponentByClass<UImpactReactionComponent>() : nullptr)
		{
			React->Collapse(Player->GetActorForwardVector() * 300.0f, false, TEXT("renverse par le banc"));
			UE_LOG(LogImpact, Display, TEXT("[actif] t=%.1f renverse"), Time);
		}
		TestStep = 5;
	}
	else if (TestStep == 3 && Time >= 6.0f)
	{
		Weapon->SetTriggerHeld(true);
		UE_LOG(LogImpact, Display, TEXT("[actif] t=%.1f rafale"), Time);
		TestStep = 4;
	}
	else if (TestStep == 4 && Time >= 6.0f + BurstLength)
	{
		Weapon->SetTriggerHeld(false);
		TestStep = 5;
	}
	else if (TestStep == 5 && Time >= 10.0f && BenchRelaunch > 0.0f && Time >= BenchNextShot)
	{
		BenchNextShot = Time + BenchRelaunch;
		Weapon->SetTriggerHeld(true);
		Weapon->SetTriggerHeld(false);
		UE_LOG(LogImpact, Display, TEXT("[actif] t=%.1f une balle dans le bras du blesse"), Time);
	}
	else if (TestStep == 5 && Time >= 9.0f && FParse::Param(FCommandLine::Get(), TEXT("ImpactActifSurLeDos")))
	{
		// -ImpactActifSurLeDos: once he lies on his belly, turn him over onto his back, to see him roll back before he crawls.
		TestStep = 6;
		USkeletalMeshComponent* Mesh = ActiveBenchTarget.IsValid() ? ActiveBenchTarget->GetMesh() : nullptr;
		if (Mesh != nullptr)
		{
			const FVector Length = (Mesh->GetSocketLocation(TEXT("spine_03")) - Mesh->GetSocketLocation(TEXT("pelvis"))).GetSafeNormal();
			for (FBodyInstance* Body : Mesh->Bodies)
			{
				if (Body != nullptr && Body->IsInstanceSimulatingPhysics())
				{
					Body->SetAngularVelocityInRadians(Length * 13.0f, false);
					Body->SetLinearVelocity(FVector(0.0f, 0.0f, 220.0f), true);
				}
			}
			UE_LOG(LogImpact, Display, TEXT("[actif] t=%.1f retourne sur le dos"), Time);
		}
	}
}

void UImpactGameplaySubsystem::RunCollectorTest(const float Time)
{
	AImpactCivilianController::bFleeingDisabled = true;
	const auto Fall = [this, Time](const int32 Index)
	{
		AAlsCharacter* Who = Bodies.IsValidIndex(Index) && Bodies[Index].IsValid() ? Cast<AAlsCharacter>(Bodies[Index]->GetOwner()) : nullptr;
		UImpactReactionComponent* React = Who != nullptr ? Who->FindComponentByClass<UImpactReactionComponent>() : nullptr;
		if (React != nullptr)
		{
			React->SetMayGetUp(false);
			React->SetHeldDown(true);
			React->Collapse(-Who->GetActorForwardVector() * 150.0f, false, TEXT("banc gc"));
			UE_LOG(LogImpact, Display, TEXT("[gc] t=%.1f %s tombe"), Time, *Bodies[Index]->DisplayName);
		}
	};
	const float Steps[] = {2.0f, 7.0f, 8.0f, 13.0f, 14.0f};
	if (TestStep >= UE_ARRAY_COUNT(Steps) || Time < Steps[TestStep])
	{
		return;
	}
	if (TestStep == 1 || TestStep == 3)
	{
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true);
		UE_LOG(LogImpact, Display, TEXT("[gc] t=%.1f menage de la memoire force"), Time);
	}
	else
	{
		Fall(TestStep / 2);
	}
	++TestStep;
}

void UImpactGameplaySubsystem::RunTestPeopleBench(const float Time, AAlsCharacter* Player, UImpactWeaponComponent* Weapon)
{
	AImpactCivilianController::bFleeingDisabled = true;
	UImpactWeaponPresentationComponent* Menu = Player->FindComponentByClass<UImpactWeaponPresentationComponent>();
	const auto PointOn = [this](const int32 Person, const TCHAR* Bone)
	{
		const ACharacter* Who = TestPeople.IsValidIndex(Person) ? TestPeople[Person].Get() : nullptr;
		const int32 Index = Bodies.IndexOfByPredicate([Who](const TWeakObjectPtr<UImpactBodyComponent>& Body) { return Body.IsValid() && Body->GetOwner() == Who; });
		return Index != INDEX_NONE ? GetBonePoint(Index, Bone) : FVector::ZeroVector;
	};
	if (TestBurstEnd > 0.0f && Time >= TestBurstEnd)
	{
		Weapon->SetTriggerHeld(false);
		TestBurstEnd = -1.0f;
	}
	if (TestStep == 0 && Time >= 2.0f && Menu != nullptr)
	{
		Menu->ShowMenuTab(TEXT("test_armes"));
		TestStep = 1;
	}
	else if (TestStep == 1 && Time >= 3.0f)
	{
		// Two civilians, then a rifle with a heavy vest and a helmet, a pistol with a light vest and a helmet, a pistol in two hands bare.
		TestPeople.Add(SpawnTestPerson(0, 0, false));
		TestPeople.Add(SpawnTestPerson(0, 0, false));
		TestPeople.Add(SpawnTestPerson(1, 3, true));
		TestPeople.Add(SpawnTestPerson(2, 1, true));
		TestPeople.Add(SpawnTestPerson(3, 0, false));
		UE_LOG(LogImpact, Display, TEXT("[test-pnj] t=%.1f PNJ de test en jeu : %d"), Time, CountTestPeople());
		TestStep = 2;
	}
	else if (TestStep == 2 && Time >= 5.0f && Menu != nullptr)
	{
		Menu->CloseInspection();
		Player->SetOverlayMode(AlsOverlayModeTags::Rifle);
		Player->SetDesiredAiming(true);
		TestStep = 3;
	}
	else if (TestStep == 3 && Time >= 7.0f)
	{
		AimAt(PointOn(2, TEXT("spine_03")));
		if (Time >= 7.6f)
		{
			Weapon->SetTriggerHeld(true);
			Weapon->SetTriggerHeld(false);
			UE_LOG(LogImpact, Display, TEXT("[test-pnj] t=%.1f une balle de fusil dans le gilet lourd"), Time);
			TestStep = 4;
		}
	}
	else if (TestStep == 4 && Time >= 9.0f)
	{
		AimAt(PointOn(3, TEXT("head")) + FVector(0, 0, 8));
		if (Time >= 9.6f)
		{
			Weapon->SetTriggerHeld(true);
			Weapon->SetTriggerHeld(false);
			UE_LOG(LogImpact, Display, TEXT("[test-pnj] t=%.1f une balle dans le casque"), Time);
			TestStep = 5;
		}
	}
	else if (TestStep == 5 && Time >= 11.0f)
	{
		AimAt(PointOn(3, TEXT("spine_03")));
		if (Time >= 11.6f)
		{
			Weapon->SetTriggerHeld(true);
			TestBurstEnd = Time + 0.6f;
			UE_LOG(LogImpact, Display, TEXT("[test-pnj] t=%.1f rafale dans le gilet leger"), Time);
			TestStep = 6;
		}
	}
	else if (TestStep == 6 && Time >= 16.0f)
	{
		const int32 Removed = RemoveTestPeople();
		UE_LOG(LogImpact, Display, TEXT("[test-pnj] t=%.1f retires %d, il en reste %d"), Time, Removed, CountTestPeople());
		TestStep = 7;
	}
	else if (TestStep == 7 && Time >= 17.0f)
	{
		UE_LOG(LogImpact, Display, TEXT("[test-pnj] t=%.1f une seconde apres : il en reste %d"), Time, CountTestPeople());
		TestStep = 8;
	}
}

int32 UImpactGameplaySubsystem::RemoveTestPeople()
{
	int32 Removed = 0;
	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		if (It->ActorHasTag(TEXT("ImpactPNJTest")))
		{
			if (const APawn* Pawn = Cast<APawn>(*It))
			{
				if (AController* Brain = Pawn->GetController())
				{
					Brain->Destroy();
				}
			}
			It->Destroy();
			++Removed;
		}
		else if (It->ActorHasTag(TEXT("ImpactArmeLachee")))
		{
			It->Destroy();
		}
	}
	UE_LOG(LogImpact, Display, TEXT("[test-pnj] %d PNJ de test retires"), Removed);
	return Removed;
}

int32 UImpactGameplaySubsystem::CountTestPeople() const
{
	int32 Count = 0;
	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		Count += It->ActorHasTag(TEXT("ImpactPNJTest")) && IsValid(*It) ? 1 : 0;
	}
	return Count;
}

void UImpactGameplaySubsystem::ConfigureCharacterCollision()
{
	// Bullets ignore the capsule and hit the body parts of the mesh instead.
	for (TActorIterator<ACharacter> It(GetWorld()); It; ++It)
	{
		It->GetCapsuleComponent()->SetCollisionResponseToChannel(ECC_ImpactBullet, ECR_Ignore);
		USkeletalMeshComponent* Mesh = It->GetMesh();
		Mesh->SetCollisionResponseToChannel(ECC_ImpactBullet, ECR_Block);
		Mesh->SetReceivesDecals(false);
	}
}

void UImpactGameplaySubsystem::AimAt(const FVector& Target) const
{
	if (WitnessFrame >= 0)
	{
		return;
	}
	APlayerController* Controller = GetWorld()->GetFirstPlayerController();
	FVector ViewLocation;
	FRotator ViewRotation;
	Controller->GetPlayerViewPoint(ViewLocation, ViewRotation);
	Controller->SetControlRotation((Target - ViewLocation).Rotation());
}

void UImpactGameplaySubsystem::RunTest(float DeltaTime)
{
	if (TestName.IsEmpty())
	{
		return;
	}

	const UImpactAutoCapture* Capture = GetWorld()->GetGameInstance()->GetSubsystem<UImpactAutoCapture>();
	const float Time = Capture != nullptr ? Capture->GetTestTime() : -1.0f;
	AAlsCharacter* Player = GetPlayerCharacter();
	UImpactWeaponComponent* Weapon = Player != nullptr ? Player->FindComponentByClass<UImpactWeaponComponent>() : nullptr;
	static const bool bNoPassersBy = FParse::Param(FCommandLine::Get(), TEXT("ImpactSansPassants"));
	if (Time < 0.0f || Weapon == nullptr || (!bNoPassersBy && Bodies.Num() < PasserByCount))
	{
		return;
	}

	if (WitnessFrame >= 0)
	{
		StepWitness();
	}
	else if (NextWitness < WitnessTimes.Num() && Time >= WitnessTimes[NextWitness])
	{
		WitnessIndex = NextWitness++;
		WitnessTarget = CurrentTestTarget;
		WitnessFrame = 0;
		StepWitness();
	}

	// Every measured body keeps computing its pose whether it is on screen or not. Off focus Unreal stops
	// refreshing a skeletal mesh and leaves its transform arrays empty, and something downstream then
	// indexed bone 55 into an array of size 0 and took the whole run down a few seconds after a fall. The
	// same staleness had already produced a frozen pose that read as frozen legs on the course bench.
	for (const TWeakObjectPtr<UImpactBodyComponent>& Body : Bodies)
	{
		const ACharacter* Walker = Body.IsValid() ? Cast<ACharacter>(Body->GetOwner()) : nullptr;
		if (Walker != nullptr && Walker->GetMesh() != nullptr)
		{
			Walker->GetMesh()->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
		}
	}
	if (Player->GetMesh() != nullptr)
	{
		Player->GetMesh()->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
	}

	// A body on the ground: does it act, or is it only acted upon? Measured as how fast the hands, the head
	// and the feet move **in the pelvis's own frame**, so a corpse being dragged or sliding reads as zero
	// and only a body moving its own limbs reads as something. Reported from play: "quand ils sont par
	// terre ils se roulent pas", "ils ont pas l'air de souffrir".
	if (Bodies.IsValidIndex(0) && Bodies[0].IsValid())
	{
		const ACharacter* Down = Cast<ACharacter>(Bodies[0]->GetOwner());
		const UImpactReactionComponent* React = Down != nullptr ? Down->FindComponentByClass<UImpactReactionComponent>() : nullptr;
		if (React != nullptr && React->IsDown() && Down->GetMesh() != nullptr)
		{
			const USkeletalMeshComponent* Skin = Down->GetMesh();
			const FTransform Hips = Skin->GetSocketTransform(TEXT("pelvis"), RTS_World);
			const float StepNow = GetWorld()->GetDeltaSeconds();
			if (!GroundHipsLast.IsZero())
			{
				const float Slid = FVector::Dist2D(Hips.GetLocation(), GroundHipsLast);
				GroundSlideSum += Slid;
				// The peak, not the mean, and that is the point of adding it. "Il glisse comme des nigues
				// ou font des mouvements inhumains pour prendre la pose" is about what happens for a
				// second or two, and a two-second mean divides it away: a posture arrived at calmly and
				// one arrived at by a limb snapping across the body give the same average.
				if (StepNow > 0.0f)
				{
					GroundPeakSlide = FMath::Max(GroundPeakSlide, Slid / StepNow);
				}
			}
			if (StepNow > 0.0f && !GroundHipsLast.IsZero())
			{
				const float Alpha = 1.0f - FMath::Exp(-StepNow / 0.1f);
				GroundSlideSmooth += (FVector::Dist2D(Hips.GetLocation(), GroundHipsLast) / StepNow - GroundSlideSmooth) * Alpha;
				GroundSlideSmoothPeak = FMath::Max(GroundSlideSmoothPeak, GroundSlideSmooth);
			}
			GroundHipsLast = Hips.GetLocation();
			if (GroundHipsOrigin.IsZero())
			{
				GroundHipsOrigin = Hips.GetLocation();
				// -ImpactFilmerChute: the fall itself, filmed from the moment he goes down.
				if (FParse::Param(FCommandLine::Get(), TEXT("ImpactFilmerChute")))
				{
					for (const float Delay : {0.05f, 0.2f, 0.35f, 0.5f, 0.7f, 0.9f, 1.2f, 1.6f})
					{
						WitnessTimes.Add(Time + Delay);
					}
					WitnessTimes.Sort();
				}
			}
			static const TCHAR* Watched[] = {TEXT("hand_l"), TEXT("hand_r"), TEXT("head"), TEXT("foot_l"), TEXT("foot_r")};
			const float Step = GetWorld()->GetDeltaSeconds();
			float Moved = 0.0f;
			for (int32 i = 0; i < 5; ++i)
			{
				const FVector Local = Hips.InverseTransformPosition(Skin->GetSocketLocation(Watched[i]));
				if (!GroundLast[i].IsZero())
				{
					Moved += FVector::Dist(Local, GroundLast[i]);
				}
				GroundLast[i] = Local;
			}
			if (StepNow > 0.0f)
			{
				GroundPeakLimb = FMath::Max(GroundPeakLimb, Moved / StepNow);

				// How fast the joints themselves turn: each child body against its parent. This is what the drives act
				// on, and what reads as "inhuman" when it bursts. Smoothed over ~0.1 s so one noisy frame does not
				// count, the way one bad frame is not what the eye sees.
				float Fastest = 0.0f;
				FName FastestJoint;
				for (const FConstraintInstance* Joint : Skin->Constraints)
				{
					const FBodyInstance* Child = Joint != nullptr ? Skin->GetBodyInstance(Joint->ConstraintBone1) : nullptr;
					const FBodyInstance* Parent = Joint != nullptr ? Skin->GetBodyInstance(Joint->ConstraintBone2) : nullptr;
					if (Child == nullptr || Parent == nullptr)
					{
						continue;
					}
					const float Turn = FMath::RadiansToDegrees(static_cast<float>(
						(Child->GetUnrealWorldAngularVelocityInRadians() - Parent->GetUnrealWorldAngularVelocityInRadians()).Size()));
					if (Turn > Fastest)
					{
						Fastest = Turn;
						FastestJoint = Joint->JointName;
					}
				}
				const float Alpha = 1.0f - FMath::Exp(-StepNow / 0.1f);
				GroundJointSmooth += (Fastest - GroundJointSmooth) * Alpha;
				if (GroundJointSmooth > GroundJointPeak)
				{
					GroundJointPeak = GroundJointSmooth;
					GroundJointPeakName = FastestJoint.ToString();
				}
				GroundJointNowName = FastestJoint.ToString();
			}
			// Over the last window only. A running mean since he went down is swamped by the thump of the
			// fall itself and keeps reporting motion long after the body has settled into a sack.
			if (Step > 0.0f && GroundTime > 0.0f)
			{
				GroundMoveSum += Moved;
				GroundWindow += Step;
			}
			GroundTime += Step;

			// The trajectory into the pose, on its own clock. Windows counted from the fall mixed the end of the
			// tumble with the start of the posture, and the posture starts anywhere from 4 to 12 s after it.
			if (GroundStart < 0.0f && (React->IsGroundPostureActive() || React->GetSettledTime() > 0.5f))
			{
				GroundStart = Time;
				GroundNextSample = Time;
				UE_LOG(LogImpact, Display, TEXT("[art] depart t=%.2f (%s), %.1f s a terre"), Time,
				       React->IsGroundPostureActive() ? TEXT("posture") : TEXT("immobile"), GroundTime);
				if (TestName == TEXT("douleur") && !FParse::Param(FCommandLine::Get(), TEXT("ImpactFilmerPassage"))
				    && !FParse::Param(FCommandLine::Get(), TEXT("ImpactFilmerChute")))
				{
					// Up to 3 s: the way into the pose. 3.5 to 10 s: the handover to the animation (-ImpactSolAnime) and the held take.
					for (const float Delay : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 3.0f, 3.5f, 4.0f, 6.0f, 10.0f})
					{
						WitnessTimes.Add(Time + Delay + 0.05f);
					}
					WitnessTimes.Sort();
				}
			}
			// The handover to the animation (-ImpactSolAnime), filmed close: just before, during the blend and after it.
			static bool bWasLyingFilmed = false;
			if (React->IsLyingDown() && !bWasLyingFilmed && FParse::Param(FCommandLine::Get(), TEXT("ImpactFilmerPassage")))
			{
				for (const float Delay : {0.0f, 0.12f, 0.25f, 0.4f, 0.7f, 1.2f})
				{
					WitnessTimes.Add(Time + Delay);
				}
				WitnessTimes.Sort();
			}
			bWasLyingFilmed = React->IsLyingDown();
			if (GroundStart >= 0.0f && Time - GroundStart <= 6.0f && Time >= GroundNextSample)
			{
				GroundNextSample += 0.1f;
				UE_LOG(LogImpact, Display, TEXT("[art] rel=%.2f art=%.0f (%s) glisse=%.0f posture=%d"), Time - GroundStart, GroundJointSmooth,
				       *GroundJointNowName, GroundSlideSmooth, React->IsGroundPostureActive() ? 1 : 0);
			}
			GroundLogTimer -= Step;
			if (GroundLogTimer <= 0.0f)
			{
				GroundLogTimer = 2.0f;
				// And how far the whole body travels across the floor, which the measurement above cannot
				// see by construction: taking the limbs in the pelvis's own frame was meant to ignore a
				// body being dragged, and it did - including when the muscles themselves were doing the
				// dragging. A take is recorded at the origin of the world; pull hard enough towards it
				// and the body slides there. Reported from play: "ils sont allonges et bougent tout le
				// long du sol tout seuls tres vite".
				UE_LOG(LogImpact, Display, TEXT("[sol] t=%.1f %s : membres %.1f cm/s dans le repere du bassin (PIC %.0f) | GLISSE %.1f cm/s (PIC %.0f) | %s, %.0f s a terre, bassin a %.0f cm | DERIVE %.0f cm"),
				       Time, *Bodies[0]->DisplayName, GroundWindow > 0.0f ? GroundMoveSum / GroundWindow : 0.0f,
				       GroundPeakLimb, GroundWindow > 0.0f ? GroundSlideSum / GroundWindow : 0.0f, GroundPeakSlide,
				       React->IsWrithing() ? TEXT("se tord") : TEXT("inerte"), GroundTime,
				       Hips.GetLocation().Z - Down->GetActorLocation().Z + 90.0f,
				       FVector::Dist2D(Hips.GetLocation(), GroundHipsOrigin));
				// Whether the body took the posture, not merely whether it twitched. Curled up, the head
				// comes near the knees and the hands near the belly; sprawled, both stay far. This is the
				// thing the new ground loops are for - their own motion is only a few degrees, so what
				// they buy is a shape, and a shape has to be measured as a shape.
				const USkeletalMeshComponent* Sk = Down->GetMesh();
				const float HeadToKnees = FVector::Dist(Sk->GetSocketLocation(TEXT("head")),
				                                        (Sk->GetSocketLocation(TEXT("calf_l")) + Sk->GetSocketLocation(TEXT("calf_r"))) * 0.5f);
				const float HandsToBelly = (FVector::Dist(Sk->GetSocketLocation(TEXT("hand_l")), Sk->GetSocketLocation(TEXT("spine_01")))
				                            + FVector::Dist(Sk->GetSocketLocation(TEXT("hand_r")), Sk->GetSocketLocation(TEXT("spine_01")))) * 0.5f;
				// And whether he is in a T-pose, which a shape metric alone does not see: the hands of a man lying curled are
				// tens of centimetres apart, those of the reference pose about 160.
				const float HandsApart = FVector::Dist(Sk->GetSocketLocation(TEXT("hand_l")), Sk->GetSocketLocation(TEXT("hand_r")));
				const AAlsCharacter* DownAls = Cast<AAlsCharacter>(Down);
				const FGameplayTag Action = DownAls != nullptr ? DownAls->GetLocomotionAction() : FGameplayTag();
				UE_LOG(LogImpact, Display, TEXT("[posture] t=%.1f tete-genoux %.0f cm | mains-ventre %.0f cm | mains-ecart %.0f cm | %s"),
				       Time, HeadToKnees, HandsToBelly, HandsApart, Action.IsValid() ? *Action.GetTagName().ToString() : TEXT("aucune action"));
				// Where he lies and which way he points, so a slow crawl or a slow turn in the held pose can be read
				// as a distance and an angle, not inferred from speeds.
				const FVector Along = Sk->GetSocketLocation(TEXT("head")) - Hips.GetLocation();
				// And how close the player stands: a body that lands against him is shoved by PushedByPawns with
				// his speed, and his speed can be the shove the body itself gave him.
				const ACharacter* Watcher = UGameplayStatics::GetPlayerCharacter(GetWorld(), 0);
				const float WatcherDistance = Watcher != nullptr ? FVector::Dist2D(Watcher->GetActorLocation(), Hips.GetLocation()) : -1.0f;
				const float WatcherSpeed = Watcher != nullptr ? Watcher->GetVelocity().Size2D() : -1.0f;
				UE_LOG(LogImpact, Display, TEXT("[place] t=%.1f x=%.1f y=%.1f cap=%.1f joueur=%.0f cm a %.0f cm/s"), Time,
				       Hips.GetLocation().X - GroundHipsOrigin.X, Hips.GetLocation().Y - GroundHipsOrigin.Y,
				       FMath::RadiansToDegrees(FMath::Atan2(Along.Y, Along.X)), WatcherDistance, WatcherSpeed);
				UE_LOG(LogImpact, Display, TEXT("[lisse] t=%.1f %.0f s a terre | ARTICULATION PIC %.0f deg/s (%s) | GLISSE PIC %.0f cm/s"),
				       Time, GroundTime, GroundJointPeak, *GroundJointPeakName, GroundSlideSmoothPeak);
				GroundJointPeak = 0.0f;
				GroundSlideSmoothPeak = 0.0f;
				GroundSlideSum = 0.0f;
				GroundMoveSum = 0.0f;
				GroundWindow = 0.0f;
				GroundPeakLimb = 0.0f;
				GroundPeakSlide = 0.0f;
			}
		}
		else
		{
			for (int32 i = 0; i < 5; ++i)
			{
				GroundLast[i] = FVector::ZeroVector;
			}
			GroundMoveSum = 0.0f;
			GroundSlideSum = 0.0f;
			GroundHipsLast = FVector::ZeroVector;
			GroundHipsOrigin = FVector::ZeroVector;
			GroundWindow = 0.0f;
			GroundTime = 0.0f;
			GroundStart = -1.0f;
			GroundJointSmooth = 0.0f;
			GroundSlideSmooth = 0.0f;
		}
	}

	if (TestName == TEXT("anatomy"))
	{
		RunAnatomyTest(Time, Player, Weapon);
		return;
	}
	if (TestName == TEXT("flee"))
	{
		RunFleeTest(Time, Player, Weapon);
		return;
	}
	if (TestName == TEXT("wound"))
	{
		RunWoundTest(Time, Player, Weapon);
		return;
	}
	if (TestName == TEXT("limp"))
	{
		RunLimpTest(Time, Player, Weapon);
		return;
	}
	if (TestName == TEXT("crawl"))
	{
		RunCrawlTest(Time, Player, Weapon);
		return;
	}
	if (TestName == TEXT("control"))
	{
		RunControlTest(Time);
		return;
	}
	if (TestName == TEXT("zones"))
	{
		RunZonesTest(Time, Player, Weapon);
		return;
	}
	if (TestName == TEXT("legs"))
	{
		RunLegsTest(Time, Player, Weapon);
		return;
	}
	if (TestName == TEXT("criblage"))
	{
		RunRiddleTest(Time, Player, Weapon);
		return;
	}
	if (TestName == TEXT("douleur") || TestName == TEXT("rechute"))
	{
		RunPainTest(Time, Player, Weapon);
		return;
	}
	if (TestName == TEXT("partie"))
	{
		RunGameTest(Time, Player, Weapon);
		return;
	}
	if (TestName == TEXT("bourrade"))
	{
		RunBodyShoveTest(Time, Player, Weapon);
		return;
	}
	if (TestName == TEXT("bras"))
	{
		RunArmTest(Time, Player, Weapon);
		return;
	}
	if (TestName == TEXT("course") || TestName == TEXT("fuite"))
	{
		RunCourseTest(Time, Player, Weapon);
		return;
	}
	if (TestName == TEXT("push"))
	{
		RunPushTest(Time, Player, Weapon);
		return;
	}
	if (TestName == TEXT("pnjtest"))
	{
		RunTestPeopleBench(Time, Player, Weapon);
		return;
	}
	if (TestName == TEXT("gc"))
	{
		RunCollectorTest(Time);
		return;
	}
	if (TestName == TEXT("actif"))
	{
		RunActiveBench(Time, Player, Weapon);
		return;
	}
	if (TestName == TEXT("soldats"))
	{
		RunSoldierBench(Time, Player, Weapon);
		return;
	}
	if (TestName == TEXT("gang"))
	{
		RunGangBench(Time, Player, Weapon);
		return;
	}
	if (TestName == TEXT("gangpose"))
	{
		RunGangPoseBench(Time, Player, Weapon);
		return;
	}
	if (TestName == TEXT("ventre"))
	{
		RunGutTest(Time, Player, Weapon);
		return;
	}

	const auto BonePoint = [this](const int32 Index, const TCHAR* BoneA, const TCHAR* BoneB, const float Alpha)
	{
		const ACharacter* Character = Bodies.IsValidIndex(Index) && Bodies[Index].IsValid() ? Cast<ACharacter>(Bodies[Index]->GetOwner()) : nullptr;
		if (Character == nullptr)
		{
			return FVector::ZeroVector;
		}
		const FVector A = Character->GetMesh()->GetSocketLocation(BoneA);
		return BoneB != nullptr ? FMath::Lerp(A, Character->GetMesh()->GetSocketLocation(BoneB), Alpha) : A;
	};

	// Where to aim during each window: passer-by index and point between two bones.
	struct FAim
	{
		float From;
		float To;
		int32 Target;
		const TCHAR* BoneA;
		const TCHAR* BoneB;
		float Alpha;
	};
	static const FAim Aims[] = {
		{1.2f, 3.4f, 0, TEXT("spine_03"), nullptr, 0.0f},
		{3.8f, 5.8f, 1, TEXT("thigh_l"), TEXT("calf_l"), 0.45f},
		{6.0f, 7.8f, 2, TEXT("upperarm_l"), TEXT("lowerarm_l"), 0.5f},
		{8.0f, 9.4f, 3, TEXT("head"), nullptr, 0.0f},
		{9.6f, 10.6f, 4, TEXT("thigh_l"), TEXT("calf_l"), 0.45f},
		{10.6f, 11.6f, 4, TEXT("calf_r"), TEXT("foot_r"), 0.4f},
	};
	for (const FAim& Aim : Aims)
	{
		if (Time >= Aim.From && Time < Aim.To)
		{
			AimAt(BonePoint(Aim.Target, Aim.BoneA, Aim.BoneB, Aim.Alpha));
			CurrentTestTarget = Aim.Target;
		}
	}

	enum class EAct : uint8 { Rifle, Pistol, TriggerOn, TriggerOff, Tap, End };
	struct FAct
	{
		float At;
		EAct Act;
		const TCHAR* What;
	};
	static const FAct Acts[] = {
		{1.0f, EAct::Rifle, TEXT("fusil, visée")},
		{2.0f, EAct::TriggerOn, TEXT("rafale sur Passant 1 (poitrine)")},
		{3.0f, EAct::TriggerOff, TEXT("fin de rafale")},
		{3.6f, EAct::Pistol, TEXT("pistolet")},
		{4.4f, EAct::Tap, TEXT("Passant 2 cuisse gauche")},
		{5.0f, EAct::Tap, TEXT("Passant 2 cuisse gauche encore")},
		{6.6f, EAct::Tap, TEXT("Passant 3 bras gauche")},
		{7.2f, EAct::Tap, TEXT("Passant 3 bras gauche encore")},
		{8.8f, EAct::Tap, TEXT("Passant 4 tête")},
		{10.2f, EAct::Tap, TEXT("Passant 5 cuisse gauche")},
		{11.2f, EAct::Tap, TEXT("Passant 5 tibia droit")},
		{12.5f, EAct::End, TEXT("fin")},
	};

	if (TestStep >= UE_ARRAY_COUNT(Acts) || Time < Acts[TestStep].At)
	{
		return;
	}

	const FAct& Act = Acts[TestStep++];
	switch (Act.Act)
	{
	case EAct::Rifle:
		Player->SetOverlayMode(AlsOverlayModeTags::Rifle);
		Player->SetDesiredAiming(true);
		break;
	case EAct::Pistol:
		Player->SetOverlayMode(AlsOverlayModeTags::PistolTwoHanded);
		break;
	case EAct::TriggerOn:
		Weapon->SetTriggerHeld(true);
		break;
	case EAct::TriggerOff:
		Weapon->SetTriggerHeld(false);
		break;
	case EAct::Tap:
		Weapon->SetTriggerHeld(true);
		Weapon->SetTriggerHeld(false);
		break;
	case EAct::End:
		Player->SetDesiredAiming(false);
		break;
	}
	UE_LOG(LogImpact, Display, TEXT("[test] t=%.2f %s (%d tirs)"), Time, Act.What, Weapon->GetShotCount());
}

void UImpactGameplaySubsystem::RunAnatomyTest(const float Time, AAlsCharacter* Player, UImpactWeaponComponent* Weapon)
{
	// Each passer-by gets one bullet aimed at one organ: heart, femur, lumbar cord from behind, femoral artery, brain.
	const auto OrganPoint = [this](const int32 Index, const TCHAR* OrganName)
	{
		const UImpactBodyComponent* Body = Bodies.IsValidIndex(Index) ? Bodies[Index].Get() : nullptr;
		const ACharacter* Character = Body != nullptr ? Cast<ACharacter>(Body->GetOwner()) : nullptr;
		if (Character == nullptr)
		{
			return FVector::ZeroVector;
		}
		const TArray<FImpactOrgan>& Organs = Body->GetAnatomy().GetOrgans();
		for (int32 i = 0; i < Organs.Num(); ++i)
		{
			if (Organs[i].Name == OrganName)
			{
				const USkeletalMeshComponent* Mesh = Character->GetMesh();
				return (Body->GetAnatomy().GetOrganWorldA(Mesh, i) + Body->GetAnatomy().GetOrganWorldB(Mesh, i)) * 0.5f;
			}
		}
		return Character->GetActorLocation();
	};

	struct FShot
	{
		float At;
		int32 Target;
		const TCHAR* Organ;
		bool bRifle;
		const TCHAR* What;
	};
	static const FShot Shots[] = {
		{3.0f, 0, TEXT("cœur"), false, TEXT("Passant 1 : cœur (pistolet)")},
		{5.0f, 1, TEXT("fémur gauche"), true, TEXT("Passant 2 : fémur gauche (fusil)")},
		{7.0f, 2, TEXT("moelle lombaire"), true, TEXT("Passant 3 : bas du dos, moelle (fusil, de dos)")},
		{9.0f, 3, TEXT("artère fémorale gauche"), false, TEXT("Passant 4 : artère fémorale gauche (pistolet)")},
		{11.0f, 4, TEXT("cerveau"), false, TEXT("Passant 5 : tête (pistolet)")},
	};

	if (TestStep == 0 && Time >= 1.0f)
	{
		// Passant 3 turns his back to the shooter.
		if (Bodies.IsValidIndex(2) && Bodies[2].IsValid())
		{
			AActor* Third = Bodies[2]->GetOwner();
			Third->SetActorRotation(Third->GetActorRotation() + FRotator(0, 180, 0));
		}
		Player->SetDesiredAiming(true);
		TestStep = 1;
		UE_LOG(LogImpact, Display, TEXT("[test] t=%.2f anatomie : visée"), Time);
	}

	for (int32 i = 0; i < UE_ARRAY_COUNT(Shots); ++i)
	{
		const FShot& Shot = Shots[i];
		if (Time >= Shot.At - 1.2f && Time < Shot.At + 0.6f)
		{
			Player->SetOverlayMode(Shot.bRifle ? AlsOverlayModeTags::Rifle : AlsOverlayModeTags::PistolTwoHanded);
			AimAt(OrganPoint(Shot.Target, Shot.Organ));
			CurrentTestTarget = Shot.Target;
		}
		if (TestStep == i + 1 && Time >= Shot.At)
		{
			Weapon->SetTriggerHeld(true);
			Weapon->SetTriggerHeld(false);
			UE_LOG(LogImpact, Display, TEXT("[test] t=%.2f %s"), Time, Shot.What);
			++TestStep;
		}
	}

	// After the shots, the witness camera visits each person in turn.
	if (Time > 12.0f && TestStep > UE_ARRAY_COUNT(Shots))
	{
		CurrentTestTarget = FMath::FloorToInt((Time - 12.0f) / 6.0f) % PasserByCount;
	}
}


FVector UImpactGameplaySubsystem::GetOrganPoint(const int32 Index, const TCHAR* OrganName) const
{
	const UImpactBodyComponent* Body = Bodies.IsValidIndex(Index) ? Bodies[Index].Get() : nullptr;
	const ACharacter* Character = Body != nullptr ? Cast<ACharacter>(Body->GetOwner()) : nullptr;
	if (Character == nullptr)
	{
		return FVector::ZeroVector;
	}
	const TArray<FImpactOrgan>& Organs = Body->GetAnatomy().GetOrgans();
	for (int32 i = 0; i < Organs.Num(); ++i)
	{
		if (Organs[i].Name == OrganName)
		{
			const USkeletalMeshComponent* Mesh = Character->GetMesh();
			return (Body->GetAnatomy().GetOrganWorldA(Mesh, i) + Body->GetAnatomy().GetOrganWorldB(Mesh, i)) * 0.5f;
		}
	}
	return Character->GetActorLocation();
}

FVector UImpactGameplaySubsystem::GetBonePoint(const int32 Index, const TCHAR* Bone) const
{
	const UImpactBodyComponent* Body = Bodies.IsValidIndex(Index) ? Bodies[Index].Get() : nullptr;
	const ACharacter* Character = Body != nullptr ? Cast<ACharacter>(Body->GetOwner()) : nullptr;
	return Character != nullptr ? Character->GetMesh()->GetSocketLocation(Bone) : FVector::ZeroVector;
}

void UImpactGameplaySubsystem::RunFleeTest(const float Time, AAlsCharacter* Player, UImpactWeaponComponent* Weapon)
{
	// One pistol shot into the air: everybody around should run away from the shooter.
	static TArray<FVector> Starts;
	if (TestStep == 0 && Time >= 1.0f)
	{
		Starts.Reset();
		for (const TWeakObjectPtr<UImpactBodyComponent>& Body : Bodies)
		{
			Starts.Add(Body.IsValid() ? Body->GetOwner()->GetActorLocation() : FVector::ZeroVector);
		}
		UNavigationSystemV1* Navigation = FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
		int32 OnMesh = 0;
		for (const FVector& Start : Starts)
		{
			FNavLocation Projected;
			if (Navigation != nullptr && Navigation->ProjectPointToNavigation(Start, Projected, FVector(100.0f, 100.0f, 250.0f)))
			{
				++OnMesh;
			}
		}
		UE_LOG(LogImpact, Display, TEXT("[test] t=%.2f navigation : %s, %d zones, construction %s, %d passants sur le maillage"), Time,
		       Navigation != nullptr && Navigation->GetDefaultNavDataInstance(FNavigationSystem::DontCreate) != nullptr ? TEXT("présente") : TEXT("absente"),
		       Navigation != nullptr ? Navigation->GetNavigationBounds().Num() : 0,
		       Navigation != nullptr && Navigation->IsNavigationBuildInProgress() ? TEXT("en cours") : TEXT("finie"), OnMesh);
		Player->SetOverlayMode(AlsOverlayModeTags::PistolTwoHanded);
		Player->SetDesiredAiming(true);
		TestStep = 1;
	}
	if (Time >= 1.2f && Time < 2.2f)
	{
		AimAt(Player->GetActorLocation() + Player->GetActorForwardVector() * 1000.0f + FVector(0, 0, 900.0f));
	}
	if (TestStep == 1 && Time >= 2.0f)
	{
		Weapon->SetTriggerHeld(true);
		Weapon->SetTriggerHeld(false);
		UE_LOG(LogImpact, Display, TEXT("[test] t=%.2f coup de feu en l'air"), Time);
		TestStep = 2;
	}
	if (Time > 2.0f)
	{
		CurrentTestTarget = FMath::FloorToInt(Time - 2.0f) % PasserByCount;
	}
	if (TestStep == 2 && Time >= 8.0f)
	{
		for (int32 i = 0; i < Bodies.Num() && i < Starts.Num(); ++i)
		{
			if (Bodies[i].IsValid())
			{
				const FVector Now = Bodies[i]->GetOwner()->GetActorLocation();
				UE_LOG(LogImpact, Display, TEXT("[test] t=%.2f %s : a parcouru %.0f cm, s'est éloigné du tireur de %.0f cm"), Time,
				       *Bodies[i]->DisplayName, FVector::Dist2D(Now, Starts[i]),
				       FVector::Dist2D(Now, Player->GetActorLocation()) - FVector::Dist2D(Starts[i], Player->GetActorLocation()));
			}
		}
		TestStep = 3;
	}
}

void UImpactGameplaySubsystem::RunWoundTest(const float Time, AAlsCharacter* Player, UImpactWeaponComponent* Weapon)
{
	// People stay in place; each gets one pistol bullet in flesh: belly, arm, thigh (outer side), chest.
	// -ImpactWoundFlee lets them run instead, to watch a hand hold a wound on a body that is moving.
	AImpactCivilianController::bFleeingDisabled = !FParse::Param(FCommandLine::Get(), TEXT("ImpactWoundFlee"));

	struct FShot
	{
		float At;
		int32 Target;
		const TCHAR* Organ; // or nullptr: use the bone points below
		const TCHAR* BoneA;
		const TCHAR* BoneB;
		float Outward;      // cm moved away from the body axis (to stay in muscle)
		const TCHAR* What;
	};
	static const FShot Shots[] = {
		{3.0f, 0, TEXT("intestins"), nullptr, nullptr, 0.0f, TEXT("Passant 1 : ventre")},
		{5.0f, 1, nullptr, TEXT("upperarm_l"), TEXT("lowerarm_l"), 3.5f, TEXT("Passant 2 : bras gauche (muscle)")},
		{7.0f, 2, nullptr, TEXT("thigh_r"), TEXT("calf_r"), 6.0f, TEXT("Passant 3 : cuisse droite (muscle)")},
		{9.0f, 3, TEXT("poumon droit"), nullptr, nullptr, 0.0f, TEXT("Passant 4 : poitrine (poumon droit)")},
	};

	const auto Point = [this](const FShot& Shot)
	{
		if (Shot.Organ != nullptr)
		{
			return GetOrganPoint(Shot.Target, Shot.Organ);
		}
		const FVector Middle = FMath::Lerp(GetBonePoint(Shot.Target, Shot.BoneA), GetBonePoint(Shot.Target, Shot.BoneB), 0.45f);
		const FVector Axis = GetBonePoint(Shot.Target, TEXT("pelvis"));
		return Middle + (Middle - Axis).GetSafeNormal2D() * Shot.Outward;
	};

	if (TestStep == 0 && Time >= 1.0f)
	{
		Player->SetOverlayMode(AlsOverlayModeTags::PistolTwoHanded);
		Player->SetDesiredAiming(true);
		TestStep = 1;
	}

	for (int32 i = 0; i < UE_ARRAY_COUNT(Shots); ++i)
	{
		const FShot& Shot = Shots[i];
		if (Time >= Shot.At - 0.8f && Time < Shot.At + 0.3f)
		{
			AimAt(Point(Shot));
		}
		if (Time >= Shot.At && Time < Shot.At + 1.2f)
		{
			CurrentTestTarget = Shot.Target; // the witness films whoever was just hit
		}
		if (TestStep == i + 1 && Time >= Shot.At)
		{
			Weapon->SetTriggerHeld(true);
			Weapon->SetTriggerHeld(false);
			UE_LOG(LogImpact, Display, TEXT("[test] t=%.2f %s"), Time, Shot.What);
			++TestStep;
		}
	}

	// Passant 3 walks away sideways on the hurt thigh: the limp should show.
	if (TestStep == UE_ARRAY_COUNT(Shots) + 1 && Time >= 11.0f && Bodies.IsValidIndex(2) && Bodies[2].IsValid())
	{
		APawn* Third = Cast<APawn>(Bodies[2]->GetOwner());
		if (AImpactCivilianController* Brain = Cast<AImpactCivilianController>(Third->GetController()))
		{
			if (AAlsCharacter* Als = Cast<AAlsCharacter>(Third))
			{
				Als->SetDesiredGait(AlsGaitTags::Walking);
				Als->SetDesiredRotationMode(AlsRotationModeTags::VelocityDirection);
			}
			Brain->ClearFocus(EAIFocusPriority::Gameplay);
			Brain->MoveToLocation(Third->GetActorLocation() + Third->GetActorRightVector() * 900.0f, 50.0f, true, true, false, false);
			UE_LOG(LogImpact, Display, TEXT("[test] t=%.2f Passant 3 marche sur le côté"), Time);
		}
		++TestStep;
	}
	if (Time >= 11.0f)
	{
		CurrentTestTarget = Time < 16.0f ? 2 : FMath::FloorToInt(Time - 16.0f) % 4;
	}
}

void UImpactGameplaySubsystem::RunLimpTest(const float Time, AAlsCharacter* Player, UImpactWeaponComponent* Weapon)
{
	// One person takes a bullet in the right thigh and is then walked in a straight line, so the gait can be
	// watched from the side and measured. Nobody flees: a limp is only readable on someone walking calmly.
	AImpactCivilianController::bFleeingDisabled = true;
	CurrentTestTarget = 0;

	if (TestStep == 0 && Time >= 1.0f)
	{
		// Stand in front of the one being measured, with nobody in between. The shot of this bench was landing
		// on whichever passer-by happened to be in the line of fire, so every limp figure it produced may have
		// described a different person than the one intended.
		if (Bodies.IsValidIndex(0) && Bodies[0].IsValid())
		{
			if (const ACharacter* Target = Cast<ACharacter>(Bodies[0]->GetOwner()))
			{
				const FVector Front = Target->GetActorForwardVector();
				Player->SetActorLocation(Target->GetActorLocation() + Front * 330.0f, false, nullptr, ETeleportType::TeleportPhysics);
				UE_LOG(LogImpact, Display, TEXT("[test] t=%.2f tireur place face a Passant 1"), Time);
			}
		}
		Player->SetOverlayMode(AlsOverlayModeTags::PistolTwoHanded);
		Player->SetDesiredAiming(true);
		TestStep = 1;
	}

	if (Time >= 2.2f && Time < 3.3f)
	{
		// Outer side of the thigh, between hip and knee: muscle, so the leg is weakened but not destroyed.
		const FVector Middle = FMath::Lerp(GetBonePoint(0, TEXT("thigh_r")), GetBonePoint(0, TEXT("calf_r")), 0.45f);
		const FVector Axis = GetBonePoint(0, TEXT("pelvis"));
		AimAt(Middle + (Middle - Axis).GetSafeNormal2D() * 6.0f);
	}
	if (TestStep == 1 && Time >= 3.0f)
	{
		Weapon->SetTriggerHeld(true);
		Weapon->SetTriggerHeld(false);
		UE_LOG(LogImpact, Display, TEXT("[test] t=%.2f Passant 1 : cuisse droite"), Time);
		TestStep = 2;
	}

	if (Time < 4.5f || !Bodies.IsValidIndex(0) || !Bodies[0].IsValid())
	{
		return;
	}

	AAlsCharacter* Walker = Cast<AAlsCharacter>(Bodies[0]->GetOwner());
	if (Walker == nullptr)
	{
		return;
	}

	if (LimpWalker != 0)
	{
		LimpWalker = 0;
		LimpDirection = (Walker->GetActorLocation() - Player->GetActorLocation()).GetSafeNormal2D();
		if (AAIController* Brain = Cast<AAIController>(Walker->GetController()))
		{
			Brain->ClearFocus(EAIFocusPriority::Gameplay);
			Brain->StopMovement();
		}
		UE_LOG(LogImpact, Display, TEXT("[test] t=%.2f Passant 1 marche tout droit"), Time);
	}

	Walker->SetDesiredAiming(false);
	Walker->SetDesiredGait(AlsGaitTags::Walking);
	Walker->SetDesiredRotationMode(AlsRotationModeTags::VelocityDirection);
	Walker->AddMovementInput(LimpDirection, 1.0f);
	MeasureGait(Time);
}

void UImpactGameplaySubsystem::MeasureGait(const float Time)
{
	if (!Bodies.IsValidIndex(LimpWalker) || !Bodies[LimpWalker].IsValid())
	{
		return;
	}
	const ACharacter* Walker = Cast<ACharacter>(Bodies[LimpWalker]->GetOwner());
	if (Walker == nullptr)
	{
		return;
	}
	const USkeletalMeshComponent* Mesh = Walker->GetMesh();

	// Heights are taken against the capsule, not the ground, so a slope does not read as a limp.
	const float Reference = Walker->GetActorLocation().Z;
	const float Pelvis = Mesh->GetSocketLocation(TEXT("pelvis")).Z - Reference;
	const float LeftFoot = Mesh->GetSocketLocation(TEXT("foot_l")).Z - Reference;
	const float RightFoot = Mesh->GetSocketLocation(TEXT("foot_r")).Z - Reference;
	const float Speed = Walker->GetVelocity().Size2D();
	const float Delta = GetWorld()->GetDeltaSeconds();

	// A limp is an asymmetry: the body spends less time on the hurt leg, and drops further while it is on it.
	// The foot carrying the weight is the lower one.
	if (Speed > 20.0f && Delta > 0.0f)
	{
		if (LeftFoot < RightFoot)
		{
			GaitLeftStance += Delta;
			GaitLeftDipSum += Pelvis * Delta;
		}
		else
		{
			GaitRightStance += Delta;
			GaitRightDipSum += Pelvis * Delta;
		}
		GaitPelvisHigh = FMath::Max(GaitPelvisHigh, Pelvis);
		GaitPelvisLow = FMath::Min(GaitPelvisLow, Pelvis);
	}

	// One line per frame: a walking cycle lasts about a second, so a once-a-second sample aliases it away
	// and cannot show which leg is cut short. Parsed afterwards from the log.
	if (Speed > 20.0f)
	{
		const UImpactReactionComponent* Reaction = Walker->FindComponentByClass<UImpactReactionComponent>();
		const float Lean = Reaction != nullptr ? Reaction->GetLimpLean() : 0.0f;
		const float Dip = Reaction != nullptr ? Reaction->GetLimpDip() : 0.0f;
		UE_LOG(LogImpact, Display, TEXT("[pas] %.3f %.2f %.2f %.2f %.0f %.2f %.2f"), Time, Pelvis, LeftFoot, RightFoot, Speed, Dip, Lean);
	}

	GaitLogTimer -= Delta;
	if (GaitLogTimer > 0.0f)
	{
		return;
	}
	GaitLogTimer = 1.0f;

	const float LeftMean = GaitLeftStance > 0.0f ? GaitLeftDipSum / GaitLeftStance : 0.0f;
	const float RightMean = GaitRightStance > 0.0f ? GaitRightDipSum / GaitRightStance : 0.0f;
	const float Share = GaitLeftStance + GaitRightStance > 0.0f
		                    ? GaitRightStance / (GaitLeftStance + GaitRightStance)
		                    : 0.5f;
	UE_LOG(LogImpact, Display,
	       TEXT("[démarche] t=%.1f vitesse %.0f cm/s | bassin %.1f (bas %.1f haut %.1f) | appui G %.2fs moy %.2f | appui D %.2fs moy %.2f | part droite %.0f%% | creux G-D %.2f cm"),
	       Time, Speed, Pelvis, GaitPelvisLow, GaitPelvisHigh, GaitLeftStance, LeftMean, GaitRightStance, RightMean,
	       Share * 100.0f, LeftMean - RightMean);
}

void UImpactGameplaySubsystem::RunCrawlTest(const float Time, AAlsCharacter* Player, UImpactWeaponComponent* Weapon)
{
	// One rifle round through the lower spinal cord from behind: the legs stop answering, the person goes down
	// and should then drag himself away on his arms. Nobody else moves, so the crawl is the only thing happening.
	AImpactCivilianController::bFleeingDisabled = true;
	CurrentTestTarget = 0;

	if (TestStep == 0 && Time >= 1.0f)
	{
		if (Bodies.IsValidIndex(0) && Bodies[0].IsValid())
		{
			AActor* Target = Bodies[0]->GetOwner();
			Target->SetActorRotation(Target->GetActorRotation() + FRotator(0.0f, 180.0f, 0.0f));
		}
		Player->SetOverlayMode(AlsOverlayModeTags::Rifle);
		Player->SetDesiredAiming(true);
		TestStep = 1;
	}

	if (Time >= 1.8f && Time < 3.4f)
	{
		AimAt(GetOrganPoint(0, TEXT("moelle lombaire")));
	}
	if (TestStep == 1 && Time >= 3.0f)
	{
		Weapon->SetTriggerHeld(true);
		Weapon->SetTriggerHeld(false);
		UE_LOG(LogImpact, Display, TEXT("[test] t=%.2f Passant 1 : bas du dos, moelle (fusil, de dos)"), Time);
		TestStep = 2;
	}

	// Putting a round through a spinal cord is its own aiming problem: one shot hit the aorta instead and the
	// legs kept working, so nothing could be learned about the crawl. -ImpactParalyse takes the legs away
	// directly, which separates "can we hit the cord" from "does the crawl work".
	if (TestStep == 2 && Time >= 4.0f && FParse::Param(FCommandLine::Get(), TEXT("ImpactParalyse")))
	{
		if (Bodies.IsValidIndex(0) && Bodies[0].IsValid())
		{
			Bodies[0]->GetPhysiology().ForceLimbLoss(EImpactLimb::LeftLeg, 0.0f, TEXT("paralysie (test)"));
			Bodies[0]->GetPhysiology().ForceLimbLoss(EImpactLimb::RightLeg, 0.0f, TEXT("paralysie (test)"));
			UE_LOG(LogImpact, Display, TEXT("[test] t=%.2f jambes retirées directement (-ImpactParalyse)"), Time);
		}
		TestStep = 3;
	}
}

void UImpactGameplaySubsystem::RunZonesTest(const float Time, AAlsCharacter* Player, UImpactWeaponComponent* Weapon)
{
	// One bullet each, in a different place on each person, to check that a wound ends up where the bullet went.
	// Reported from play: a shot in the shoulder made someone hold his belly. This test shows the mapping.
	AImpactCivilianController::bFleeingDisabled = true;

	struct FZone
	{
		float At;
		int32 Target;
		const TCHAR* BoneA;
		const TCHAR* BoneB;
		float Alpha;
		float Outward;
		bool bRifle;
		const TCHAR* What;
	};
	static const FZone Zones[] = {
		{3.0f, 0, TEXT("upperarm_r"), nullptr, 0.0f, 4.0f, false, TEXT("épaule droite (pistolet)")},
		{5.0f, 1, TEXT("upperarm_r"), nullptr, 0.0f, 4.0f, true, TEXT("épaule droite (fusil)")},
		{7.0f, 2, TEXT("upperarm_r"), TEXT("lowerarm_r"), 0.4f, 4.0f, true, TEXT("bras droit (fusil)")},
		{9.0f, 3, TEXT("spine_03"), nullptr, 0.0f, 3.0f, true, TEXT("poitrine (fusil)")},
		{11.0f, 4, TEXT("thigh_l"), TEXT("calf_l"), 0.45f, 6.0f, true, TEXT("cuisse gauche (fusil)")},
	};

	const auto Point = [this](const FZone& Zone)
	{
		const FVector A = GetBonePoint(Zone.Target, Zone.BoneA);
		const FVector Middle = Zone.BoneB != nullptr ? FMath::Lerp(A, GetBonePoint(Zone.Target, Zone.BoneB), Zone.Alpha) : A;
		const FVector Axis = GetBonePoint(Zone.Target, TEXT("pelvis"));
		return Middle + (Middle - Axis).GetSafeNormal2D() * Zone.Outward;
	};

	if (TestStep == 0 && Time >= 1.0f)
	{
		Player->SetOverlayMode(AlsOverlayModeTags::PistolTwoHanded);
		Player->SetDesiredAiming(true);
		TestStep = 1;
	}

	for (int32 i = 0; i < UE_ARRAY_COUNT(Zones); ++i)
	{
		const FZone& Zone = Zones[i];
		if (Time >= Zone.At - 0.9f && Time < Zone.At + 0.3f)
		{
			Player->SetOverlayMode(Zone.bRifle ? AlsOverlayModeTags::Rifle : AlsOverlayModeTags::PistolTwoHanded);
			AimAt(Point(Zone));
		}
		if (Time >= Zone.At && Time < Zone.At + 1.4f)
		{
			CurrentTestTarget = Zone.Target;
		}
		if (TestStep == i + 1 && Time >= Zone.At)
		{
			Weapon->SetTriggerHeld(true);
			Weapon->SetTriggerHeld(false);
			UE_LOG(LogImpact, Display, TEXT("[test] t=%.2f visé : %s sur Passant %d"), Time, Zone.What, Zone.Target + 1);
			++TestStep;
		}
	}
}

void UImpactGameplaySubsystem::RunLegsTest(const float Time, AAlsCharacter* Player, UImpactWeaponComponent* Weapon)
{
	// Reproduces what was reported from play: standing right next to someone and shooting one thigh, then the
	// other, several times. Every shot is logged, so the miss rate on the second leg can be counted instead of
	// guessed. The shooter is placed close on purpose: the complaint was about point blank range.
	AImpactCivilianController::bFleeingDisabled = true;
	CurrentTestTarget = 0;

	if (!Bodies.IsValidIndex(0) || !Bodies[0].IsValid())
	{
		return;
	}
	ACharacter* Victim = Cast<ACharacter>(Bodies[0]->GetOwner());
	if (Victim == nullptr)
	{
		return;
	}

	if (TestStep == 0 && Time >= 1.0f)
	{
		// Stand two metres away, facing him.
		const FVector Side = Victim->GetActorForwardVector();
		Player->SetActorLocation(Victim->GetActorLocation() + Side * 200.0f, false, nullptr, ETeleportType::TeleportPhysics);
		Player->SetOverlayMode(AlsOverlayModeTags::PistolTwoHanded);
		Player->SetDesiredAiming(true);
		TestStep = 1;
		UE_LOG(LogImpact, Display, TEXT("[test] t=%.2f tireur placé à 2 m"), Time);
	}

	// Alternate thighs: one shot every 0.7 s, right first then left over and over.
	const float Start = 2.5f;
	if (Time < Start)
	{
		return;
	}
	const int32 Shot = FMath::FloorToInt((Time - Start) / 0.7f);
	if (Shot > 11)
	{
		return;
	}
	const bool bRight = Shot < 2; // two at the right thigh, then every following one at the left
	const TCHAR* BoneA = bRight ? TEXT("thigh_r") : TEXT("thigh_l");
	const TCHAR* BoneB = bRight ? TEXT("calf_r") : TEXT("calf_l");
	const FVector Middle = FMath::Lerp(GetBonePoint(0, BoneA), GetBonePoint(0, BoneB), 0.45f);
	AimAt(Middle);

	if (TestStep == Shot + 1)
	{
		Weapon->SetTriggerHeld(true);
		Weapon->SetTriggerHeld(false);
		UE_LOG(LogImpact, Display, TEXT("[test] t=%.2f coup %d visé sur %s"), Time, Shot + 1, BoneA);
		++TestStep;
	}
}

void UImpactGameplaySubsystem::RunGameTest(const float Time, AAlsCharacter* Player, UImpactWeaponComponent* Weapon)
{
	// A minute of play, as it was played when it was reported "bugge de partout": rounds at whoever is still standing or
	// crawling, at random places on the body, one every 1.6 s. Nothing is held down and nothing is staged; DetectBugs
	// writes what goes wrong.
	AImpactCivilianController::bFleeingDisabled = false;
	// Bursts, not single rounds: "arrete les essais a 1 tir, dans un jeu on rafale". The rifle fires as long as the trigger is
	// held, so each burst is the trigger held for 0.5 to 1.2 s.
	if (TestStep == 0 && Time >= 1.0f)
	{
		Player->SetOverlayMode(AlsOverlayModeTags::Rifle);
		Player->SetDesiredAiming(true);
		TestStep = 1;
	}
	if (Time > 62.0f)
	{
		Weapon->SetTriggerHeld(false);
		return;
	}
	static float BurstEnd = -1.0f;
	if (BurstEnd > 0.0f && Time >= BurstEnd)
	{
		Weapon->SetTriggerHeld(false);
		BurstEnd = -1.0f;
		GameBone = NAME_None;
	}
	static const TCHAR* Places[] = {TEXT("spine_01"), TEXT("spine_02"), TEXT("spine_03"), TEXT("pelvis"), TEXT("thigh_l"),
	                                TEXT("thigh_r"), TEXT("calf_l"), TEXT("upperarm_l"), TEXT("upperarm_r"), TEXT("spine_01")};
	// -ImpactPartieZone=jambes: the knees and thighs only - the fall that was reported leaving the pistol in the air.
	static const TCHAR* Legs[] = {TEXT("calf_l"), TEXT("calf_r"), TEXT("thigh_l"), TEXT("thigh_r")};
	static FString PlaceZone;
	static bool bPlaceZoneRead = false;
	if (!bPlaceZoneRead)
	{
		bPlaceZoneRead = true;
		FParse::Value(FCommandLine::Get(), TEXT("ImpactPartieZone="), PlaceZone);
	}
	const bool bLegsOnly = PlaceZone == TEXT("jambes");
	if (Time >= GameNextShot - 0.8f && GameBone.IsNone() && BurstEnd < 0.0f)
	{
		// The next one still alive, in turn.
		for (int32 Try = 0; Try < Bodies.Num(); ++Try)
		{
			GameTarget = (GameTarget + 1) % FMath::Max(1, Bodies.Num());
			if (Bodies.IsValidIndex(GameTarget) && Bodies[GameTarget].IsValid() && !Bodies[GameTarget]->GetPhysiology().IsDead())
			{
				break;
			}
		}
		GameBone = bLegsOnly ? Legs[FMath::RandRange(0, UE_ARRAY_COUNT(Legs) - 1)] : Places[FMath::RandRange(0, UE_ARRAY_COUNT(Places) - 1)];
		CurrentTestTarget = GameTarget;
	}
	if (!GameBone.IsNone() && Bodies.IsValidIndex(GameTarget) && Bodies[GameTarget].IsValid())
	{
		AimAt(GetBonePoint(GameTarget, *GameBone.ToString()));
	}
	if (Time >= GameNextShot && !GameBone.IsNone() && BurstEnd < 0.0f)
	{
		Weapon->SetTriggerHeld(true);
		BurstEnd = Time + FMath::FRandRange(0.5f, 1.2f);
		++GameShots;
		UE_LOG(LogImpact, Display, TEXT("[partie] t=%.1f rafale %d sur %s (%s), %.1f s"), Time, GameShots,
		       Bodies.IsValidIndex(GameTarget) && Bodies[GameTarget].IsValid() ? *Bodies[GameTarget]->DisplayName : TEXT("?"), *GameBone.ToString(),
		       BurstEnd - Time);
		GameNextShot = BurstEnd + 1.2f;
	}
}

void UImpactGameplaySubsystem::DetectBugs(const float DeltaTime)
{
	// The bugs reported from play, looked for every frame on every passer-by:
	//  0 T-pose     both hands out level with the shoulders and far apart: the reference pose showing
	//  1 saut       the pelvis moving more than 80 cm in one frame: a teleport, or an animation snapping back
	//  2 arme       a dropped weapon still hanging in the air three seconds later
	//  3 releve     a man back on his feet with a hole in the trunk
	//  4 marche     the same man walking again
	//  5 arme loin  the weapon a man still holds drawn more than 30 cm from both his hands: left hanging in the air while he falls
	//               (reported from play: "quand on tire dans le genou et qu'il tombe, il lache le pistolet au milieu du vide")
	//  6 arme saute the weapon moving more than 40 cm against the right hand in one frame: what handing it from bone to bone could do
	//  7 fusil      a rifle still held by a man on the floor ("ca sert a rien qu'il garde le fusil quand il tombe")
	// A [bug] line each time, and a count every ten seconds, so a play log or a bench says plainly what went wrong.
	if (DeltaTime <= 0.0f)
	{
		return;
	}
	const float Now = GetWorld()->GetTimeSeconds();
	if (Now < 3.0f)
	{
		// The passers-by are placed during the first frames: those moves are not jumps.
		BugLastHips.Reset();
		return;
	}
	if (BugLastHips.Num() != Bodies.Num())
	{
		// Zeroed: FVector's default constructor leaves garbage, and garbage read as "last position" gave 14 m jumps.
		BugLastHips.SetNumZeroed(Bodies.Num());
		BugLastTPose.SetNumZeroed(Bodies.Num());
		BugWasDown.SetNumZeroed(Bodies.Num());
		BugLastFarWeapon.SetNumZeroed(Bodies.Num());
		BugLastWeaponRel.SetNumZeroed(Bodies.Num());
	}
	for (int32 i = 0; i < Bodies.Num(); ++i)
	{
		const UImpactBodyComponent* Body = Bodies[i].Get();
		const AAlsCharacter* Als = Body != nullptr ? Cast<AAlsCharacter>(Body->GetOwner()) : nullptr;
		const UImpactReactionComponent* React = Als != nullptr ? Als->FindComponentByClass<UImpactReactionComponent>() : nullptr;
		const UImpactActiveBodyComponent* Active = Als != nullptr ? Als->FindComponentByClass<UImpactActiveBodyComponent>() : nullptr;
		if (Als == nullptr || (React == nullptr && Active == nullptr) || Als->GetMesh() == nullptr)
		{
			continue;
		}
		const USkeletalMeshComponent* Mesh = Als->GetMesh();
		const FString Action = Als->GetLocomotionAction().IsValid() ? Als->GetLocomotionAction().GetTagName().ToString() : TEXT("debout");
		const FVector HandL = Mesh->GetSocketLocation(TEXT("hand_l"));
		const FVector HandR = Mesh->GetSocketLocation(TEXT("hand_r"));
		const FVector ShoulderL = Mesh->GetSocketLocation(TEXT("upperarm_l"));
		const FVector ShoulderR = Mesh->GetSocketLocation(TEXT("upperarm_r"));
		const bool bTPose = FVector::Dist(HandL, HandR) > 150.0f && FMath::Abs(HandL.Z - ShoulderL.Z) < 20.0f && FMath::Abs(HandR.Z - ShoulderR.Z) < 20.0f
		                    && FMath::Abs(Mesh->GetSocketLocation(TEXT("head")).Z - Mesh->GetSocketLocation(TEXT("pelvis")).Z) > 40.0f;
		if (bTPose && Now - BugLastTPose[i] > 1.0f)
		{
			BugLastTPose[i] = Now;
			++BugCounts[0];
			UE_LOG(LogImpact, Warning, TEXT("[bug] t=%.2f %s : T-POSE (mains a %.0f cm, %s)"), Now, *Body->DisplayName, FVector::Dist(HandL, HandR), *Action);
		}

		const FVector Hips = Mesh->GetSocketLocation(TEXT("pelvis"));
		// Not across a long frame: the witness screenshots stall the game for half a second, and a man falling covers 80 cm in that.
		const bool bShortFrame = DeltaTime < 0.1f;
		if (bShortFrame && !BugLastHips[i].IsZero() && FVector::Dist(Hips, BugLastHips[i]) > 80.0f)
		{
			++BugCounts[1];
			UE_LOG(LogImpact, Warning, TEXT("[bug] t=%.2f %s : SAUT du bassin de %.0f cm en une image (%s)"), Now, *Body->DisplayName,
			       FVector::Dist(Hips, BugLastHips[i]), *Action);
		}
		BugLastHips[i] = Hips;

		// [sol-entree-vu]: the entry into the lying take as it is drawn. Read here, after the frame's physics blend, and not in the
		// reaction's own tick, which runs before it: there the entry frame still holds the previous pose moved with the mesh, a jump
		// nobody sees (27 October). The largest jump of the head, hands, feet or pelvis from one drawn frame to the next, from the last
		// frame before LyingDown to 0,6 s into it, and whether the camera saw him.
		{
			static const TCHAR* EntryBones[] = {TEXT("head"), TEXT("hand_l"), TEXT("hand_r"), TEXT("foot_l"), TEXT("foot_r"), TEXT("pelvis")};
			static const TCHAR* EntryNames[] = {TEXT("tete"), TEXT("main g"), TEXT("main d"), TEXT("pied g"), TEXT("pied d"), TEXT("bassin")};
			if (EntryLast.Num() != Bodies.Num())
			{
				EntryLast.SetNum(Bodies.Num());
				EntryTime.Init(-1.0f, Bodies.Num());
				EntryWorst.SetNumZeroed(Bodies.Num());
				EntryWorstAt.SetNumZeroed(Bodies.Num());
				EntryWasLying.SetNumZeroed(Bodies.Num());
				EntrySeen.SetNumZeroed(Bodies.Num());
			}
			const bool bLying = Als->GetLocomotionAction() == AlsLocomotionActionTags::LyingDown;
			TArray<FVector> NowAt;
			for (const TCHAR* Bone : EntryBones)
			{
				NowAt.Add(Mesh->GetSocketLocation(Bone));
			}
			if (bLying && !EntryWasLying[i])
			{
				EntryTime[i] = 0.0f;
				EntryWorst[i] = 0.0f;
				EntrySeen[i] = false;
			}
			if (EntryTime[i] >= 0.0f)
			{
				if (bShortFrame && EntryLast[i].Num() == NowAt.Num())
				{
					float FrameWorst = 0.0f;
					int32 FrameWorstAt = 0;
					for (int32 Index = 0; Index < NowAt.Num(); ++Index)
					{
						const float Jump = FVector::Dist(NowAt[Index], EntryLast[i][Index]);
						if (Jump > FrameWorst)
						{
							FrameWorst = Jump;
							FrameWorstAt = Index;
						}
						if (Jump > EntryWorst[i])
						{
							EntryWorst[i] = Jump;
							EntryWorstAt[i] = Index;
						}
					}
					// -ImpactSolEntreeImages: frame by frame, with the pelvis body's physics weight (1 = the bodies drawn, 0 = the take).
					static const bool bFrames = FParse::Param(FCommandLine::Get(), TEXT("ImpactSolEntreeImages"));
					if (bFrames)
					{
						const FBodyInstance* PelvisBody = Mesh->GetBodyInstance(TEXT("pelvis"));
						const FBodyInstance* HeadBody = Mesh->GetBodyInstance(TEXT("head"));
						UE_LOG(LogImpact, Display, TEXT("[sol-entree-vu-img] t=%.3f %s %.3f s : +%.0f cm (%s) | tete dessinee a %.0f cm de son corps, poids tete %.2f%s"), Now,
						       *Body->DisplayName, EntryTime[i], FrameWorst, EntryNames[FrameWorstAt],
						       HeadBody != nullptr ? FVector::Dist(HeadBody->GetUnrealWorldTransform().GetLocation(), NowAt[0]) : -1.0f,
						       HeadBody != nullptr ? HeadBody->PhysicsBlendWeight : -1.0f,
						       PelvisBody != nullptr && PelvisBody->IsInstanceSimulatingPhysics() ? TEXT(", bassin simule") : TEXT(""));
					}
				}
				EntrySeen[i] = EntrySeen[i] || Mesh->WasRecentlyRendered(0.1f);
				EntryTime[i] += DeltaTime;
				if (EntryTime[i] >= 0.6f || !bLying)
				{
					UE_LOG(LogImpact, Display, TEXT("[sol-entree-vu] t=%.2f %s : plus grand saut dessine d'une image a l'autre %.0f cm (%s), %s"), Now,
					       *Body->DisplayName, EntryWorst[i], EntryNames[EntryWorstAt[i]], EntrySeen[i] ? TEXT("vu") : TEXT("hors champ"));
					EntryTime[i] = -1.0f;
				}
			}
			EntryLast[i] = MoveTemp(NowAt);
			EntryWasLying[i] = bLying;
		}

		const bool bDown = React != nullptr ? React->IsDown() : Active->IsDown();
		if (BugWasDown[i] && !bDown)
		{
			int32 Trunk = 0;
			for (const FImpactWound& Wound : Body->GetPhysiology().GetWounds())
			{
				const FString Bone = Wound.Bone.ToString();
				Trunk += Wound.bExternal && (Bone.StartsWith(TEXT("spine")) || Bone == TEXT("pelvis")) ? 1 : 0;
			}
			if (Trunk > 0)
			{
				++BugCounts[3];
				UE_LOG(LogImpact, Warning, TEXT("[bug] t=%.2f %s : DEBOUT A NOUVEAU avec %d plaie(s) au tronc, %s"), Now, *Body->DisplayName, Trunk,
				       *Body->GetPhysiology().Describe());
			}
		}
		if (!bDown && Als->GetVelocity().Size2D() > 60.0f && !Body->GetPhysiology().GetWounds().IsEmpty())
		{
			int32 Trunk = 0;
			for (const FImpactWound& Wound : Body->GetPhysiology().GetWounds())
			{
				const FString Bone = Wound.Bone.ToString();
				Trunk += Wound.bExternal && (Bone.StartsWith(TEXT("spine")) || Bone == TEXT("pelvis")) ? 1 : 0;
			}
			static TMap<int32, float> LastWalkReport;
			if (Trunk > 0 && Now - LastWalkReport.FindOrAdd(i, -100.0f) > 3.0f && BugCounts[3] > 0)
			{
				LastWalkReport.FindOrAdd(i) = Now;
				++BugCounts[4];
				UE_LOG(LogImpact, Warning, TEXT("[bug] t=%.2f %s : MARCHE a %.0f cm/s avec une plaie au tronc"), Now, *Body->DisplayName, Als->GetVelocity().Size2D());
			}
		}
		BugWasDown[i] = bDown;

		// A rifle still in the hands of a man on the floor. This watched a rule that has since been
		// reversed for soldiers: -ImpactSolArme keeps the weapon on purpose so he can fire from the
		// ground, and -ImpactPNJCouvert gives every passer-by a soldier's brain, so the exception
		// covers all of them. The watchdog did not know, and it cried on normal behaviour 289 times in
		// four minutes of play - 100 % of the warnings in that session, with the seven other counters
		// of this same function at zero, which left the log unreadable. The guard below is the same
		// condition the code it watches uses (UImpactBodyComponent::UpdateWeaponHold, bSoldierKeeps).
		static const bool bGroundArmed = FParse::Param(FCommandLine::Get(), TEXT("ImpactSolArme"));
		const bool bKeepsOnPurpose = bGroundArmed && Als->FindComponentByClass<UImpactSoldierBrainComponent>() != nullptr;
		if (bDown && !bKeepsOnPurpose && Als->GetOverlayMode() == AlsOverlayModeTags::Rifle)
		{
			static TMap<int32, float> LastRifleReport;
			if (Now - LastRifleReport.FindOrAdd(i, -100.0f) > 2.0f)
			{
				LastRifleReport.FindOrAdd(i) = Now;
				++BugCounts[7];
				UE_LOG(LogImpact, Warning, TEXT("[bug] t=%.2f %s : FUSIL TOUJOURS EN MAIN AU SOL (%s)"), Now, *Body->DisplayName, *Action);
			}
		}

		TArray<USkeletalMeshComponent*> Held;
		Als->GetComponents(Held);
		for (const USkeletalMeshComponent* Weapon : Held)
		{
			if (Weapon == Mesh || Weapon->GetSkeletalMeshAsset() == nullptr || !Weapon->IsVisible())
			{
				continue;
			}
			const FVector Grip = Weapon->GetComponentLocation();
			const float Far = FMath::Min(FVector::Dist(Grip, HandR), FVector::Dist(Grip, HandL));
			const FVector Rel = Grip - HandR;
			if (bShortFrame && !BugLastWeaponRel[i].IsZero() && FVector::Dist(Rel, BugLastWeaponRel[i]) > 40.0f)
			{
				++BugCounts[6];
				UE_LOG(LogImpact, Warning, TEXT("[bug] t=%.2f %s : ARME SAUTE de %.0f cm par rapport a la main (%s)"), Now, *Body->DisplayName,
				       FVector::Dist(Rel, BugLastWeaponRel[i]), *Action);
			}
			BugLastWeaponRel[i] = Rel;
			BugFarWeaponPeak = FMath::Max(BugFarWeaponPeak, Far);
			if (Far > 30.0f && Now - BugLastFarWeapon[i] > 1.0f)
			{
				BugLastFarWeapon[i] = Now;
				++BugCounts[5];
				UE_LOG(LogImpact, Warning, TEXT("[bug] t=%.2f %s : ARME LOIN DE LA MAIN, %.0f cm (accrochee a '%s', %s, arme a %.0f cm au-dessus du bassin)"),
				       Now, *Body->DisplayName, Far, *Weapon->GetAttachSocketName().ToString(), *Action, Grip.Z - Hips.Z);
			}
		}
	}

	// Dropped weapons (tagged by UImpactBodyComponent::DropWeapon): on the floor three seconds later, or a bug.
	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		if (!It->ActorHasTag(TEXT("ImpactArmeLachee")))
		{
			continue;
		}
		float& Since = BugDroppedSeen.FindOrAdd(*It, Now);
		if (Since >= 0.0f && Now - Since > 3.0f)
		{
			FHitResult Floor;
			FCollisionQueryParams Params(SCENE_QUERY_STAT(ImpactBugWeapon), false, *It);
			const FVector At = It->GetActorLocation();
			const float Height = GetWorld()->LineTraceSingleByChannel(Floor, At, At - FVector(0, 0, 500.0f), ECC_Visibility, Params)
				                     ? At.Z - Floor.ImpactPoint.Z : 500.0f;
			if (Height > 25.0f)
			{
				++BugCounts[2];
				UE_LOG(LogImpact, Warning, TEXT("[bug] t=%.2f arme lachee QUI FLOTTE a %.0f cm du sol"), Now, Height);
			}
			Since = -1.0f;
		}
	}

	BugSummaryTimer -= DeltaTime;
	if (BugSummaryTimer <= 0.0f)
	{
		BugSummaryTimer = 10.0f;
		UE_LOG(LogImpact, Display, TEXT("[bugs] t=%.0f T-pose %d | sauts %d | armes qui flottent %d | releves avec plaie au tronc %d | marchent ensuite %d | armes loin de la main %d (pic %.0f cm) | armes qui sautent %d | fusils au sol %d"),
		       Now, BugCounts[0], BugCounts[1], BugCounts[2], BugCounts[3], BugCounts[4], BugCounts[5], BugFarWeaponPeak, BugCounts[6], BugCounts[7]);
	}
}

void UImpactGameplaySubsystem::RunRiddleTest(const float Time, AAlsCharacter* Player, UImpactWeaponComponent* Weapon)
{
	// How many rounds into the chest of a man running away before he stops running. Reported from play:
	// "je crible le torse d'un gars de face, il se tourne normal et il court super vite", "faut pas tirer
	// 90 balles sur un mec pour le voir reagir".
	CurrentTestTarget = 0;

	if (!Bodies.IsValidIndex(0) || !Bodies[0].IsValid())
	{
		return;
	}
	AAlsCharacter* Target = Cast<AAlsCharacter>(Bodies[0]->GetOwner());
	UImpactReactionComponent* React = Target != nullptr ? Target->FindComponentByClass<UImpactReactionComponent>() : nullptr;
	if (Target == nullptr || React == nullptr)
	{
		return;
	}

	if (TestStep == 0 && Time >= 0.8f)
	{
		Player->SetOverlayMode(AlsOverlayModeTags::PistolTwoHanded);
		Player->SetDesiredAiming(true);
		TestStep = 1;
	}
	if (TestStep == 1 && Time >= 1.0f && Time < 1.8f)
	{
		AimAt(Player->GetActorLocation() + Player->GetActorForwardVector() * 60.0f + FVector(0, 0, 3000.0f));
	}
	if (TestStep == 1 && Time >= 1.8f)
	{
		Weapon->SetTriggerHeld(true);
		Weapon->SetTriggerHeld(false);
		UE_LOG(LogImpact, Display, TEXT("[test] t=%.2f coup de feu en l'air, Passant 1 fuit"), Time);
		TestStep = 2;
	}
	if (TestStep < 2)
	{
		return;
	}

	// Behind him between the shoulder blades, or -ImpactDeFace to stand in his path and take the chest
	// head on, which is what was described from play.
	static const bool bFront = FParse::Param(FCommandLine::Get(), TEXT("ImpactDeFace"));
	const FVector Away = (Target->GetActorLocation() - Player->GetActorLocation()).GetSafeNormal2D();
	const FVector Stand = bFront
		                      ? Target->GetActorLocation() + Target->GetActorForwardVector() * 380.0f
		                      : Target->GetActorLocation() - Away * 380.0f;
	Player->SetActorLocation(Stand, false, nullptr, ETeleportType::TeleportPhysics);
	// -ImpactVentre aims at the gut instead of between the shoulder blades: no lung, only pain and blood.
	static const bool bBelly = FParse::Param(FCommandLine::Get(), TEXT("ImpactVentre"));
	AimAt(bBelly
		      // On spine_01 itself, which is the gut. Lower than this is the pelvis, and a fractured pelvis
		      // drops anyone on the first round - correctly, but it is not what a belly shot is.
		      ? FMath::Lerp(GetBonePoint(0, TEXT("spine_01")), GetBonePoint(0, TEXT("spine_02")), 0.25f)
		      : FMath::Lerp(GetBonePoint(0, TEXT("spine_02")), GetBonePoint(0, TEXT("spine_03")), 0.5f));

	const float Speed = Target->GetVelocity().Size2D();
	if (RiddleDown)
	{
		return;
	}
	if (React->IsDown())
	{
		RiddleDown = true;
		UE_LOG(LogImpact, Display, TEXT("[criblage] A TERRE apres %d balles, t=%.1f s"), RiddleShots, Time);
		return;
	}

	// One round every third of a second, which is about as fast as a pistol is fired deliberately.
	RiddleTimer -= GetWorld()->GetDeltaSeconds();
	if (Time >= 3.0f && RiddleTimer <= 0.0f && RiddleShots < 120)
	{
		RiddleTimer = 0.33f;
		Weapon->SetTriggerHeld(true);
		Weapon->SetTriggerHeld(false);
		++RiddleShots;
		const UImpactBodyComponent* Body = Bodies[0].Get();
		UE_LOG(LogImpact, Display, TEXT("[criblage] balle %d : vitesse %.0f cm/s, equilibre %.0f cm, %s"),
		       RiddleShots, Speed, React->GetBalanceCapacity(), *Body->GetPhysiology().Describe());
	}
}

void UImpactGameplaySubsystem::RunPainTest(const float Time, AAlsCharacter* Player, UImpactWeaponComponent* Weapon)
{
	// Someone on the ground, awake, and hurting. That state had no bench at all, which is why nobody had
	// ever measured the one thing that was reported about it: "quand ils sont par terre ils se roulent
	// pas", "ils ont pas l'air de souffrir". The limp bench cannot serve, because the whole point of the
	// work done there was to stop people falling.
	AImpactCivilianController::bFleeingDisabled = true;
	CurrentTestTarget = 0;

	if (!Bodies.IsValidIndex(0) || !Bodies[0].IsValid())
	{
		return;
	}
	AAlsCharacter* Target = Cast<AAlsCharacter>(Bodies[0]->GetOwner());
	UImpactReactionComponent* React = Target != nullptr ? Target->FindComponentByClass<UImpactReactionComponent>() : nullptr;
	if (Target == nullptr || React == nullptr)
	{
		return;
	}

	if (TestStep == 0 && Time >= 1.0f)
	{
		const FVector Front = Target->GetActorForwardVector();
		Player->SetActorLocation(Target->GetActorLocation() + Front * 300.0f, false, nullptr, ETeleportType::TeleportPhysics);
		Player->SetOverlayMode(AlsOverlayModeTags::PistolTwoHanded);
		Player->SetDesiredAiming(true);
		if (AAIController* Brain = Cast<AAIController>(Target->GetController()))
		{
			Brain->ClearFocus(EAIFocusPriority::Gameplay);
			Brain->StopMovement();
		}
		TestStep = 1;
	}

	// One round in the belly: enough pain and enough blood to matter, not enough to kill outright.
	// -ImpactDouleurZone=poitrine or =cuisse puts it in the chest or the thigh instead, which is what picks the
	// two other ground takes (Halete_Au_Sol, Tient_Jambe_Au_Sol) - neither had ever been played on this bench.
	static FString Zone;
	static bool bZoneRead = false;
	if (!bZoneRead)
	{
		bZoneRead = true;
		FParse::Value(FCommandLine::Get(), TEXT("ImpactDouleurZone="), Zone);
	}
	if (TestStep == 1 && Time >= 2.0f && Time < 3.0f)
	{
		AimAt(Zone == TEXT("poitrine") ? GetBonePoint(0, TEXT("spine_03"))
		      : Zone == TEXT("cuisse") ? FMath::Lerp(GetBonePoint(0, TEXT("thigh_l")), GetBonePoint(0, TEXT("calf_l")), 0.4f)
		      : FMath::Lerp(GetBonePoint(0, TEXT("pelvis")), GetBonePoint(0, TEXT("spine_01")), 0.6f));
	}
	if (TestStep == 1 && Time >= 3.0f)
	{
		Weapon->SetTriggerHeld(true);
		Weapon->SetTriggerHeld(false);
		UE_LOG(LogImpact, Display, TEXT("[test] t=%.2f Passant 1 : %s"), Time, Zone.IsEmpty() ? TEXT("ventre") : *Zone);
		TestStep = 2;
	}

	// Put him down and keep him down. He is meant to be measured lying there, not standing up again: what
	// a man does once he is on the ground is the whole question.
	if (TestStep == 2 && Time >= 4.0f)
	{
		React->SetMayGetUp(false);
		React->SetHeldDown(true);
		if (!React->IsDown())
		{
			React->Collapse(Target->GetActorForwardVector() * 150.0f, false, TEXT("banc douleur"));
			UE_LOG(LogImpact, Display, TEXT("[test] t=%.2f Passant 1 mis a terre, conscient"), Time);
		}
	}

	// -ImpactDouleurTirsAuSol: once he lies there, a round in the hips every five seconds from t=14 - each must make him a body
	// again, and he must go back into the take afterwards. That is what real play did to the first attempt.
	static const bool bShootDown = FParse::Param(FCommandLine::Get(), TEXT("ImpactDouleurTirsAuSol"));
	static float NextDownShot = 14.0f;
	if (bShootDown && TestName == TEXT("douleur") && TestStep >= 2)
	{
		if (Time >= NextDownShot - 0.8f && Time < NextDownShot)
		{
			AimAt(GetBonePoint(0, TEXT("pelvis")));
		}
		if (Time >= NextDownShot)
		{
			NextDownShot += 5.0f;
			Weapon->SetTriggerHeld(true);
			Weapon->SetTriggerHeld(false);
			UE_LOG(LogImpact, Display, TEXT("[test] t=%.2f Passant 1 : balle au sol (%s)"), Time,
			       React->IsLyingDown() ? TEXT("allonge") : React->IsDown() ? TEXT("ragdoll") : TEXT("debout"));
		}
	}

	// -ImpactTest=rechute: the same, then he is let up once he has held the posture for three seconds, and put
	// down again - once he has been standing for a second (-ImpactRechute=debout, the default), or while he is
	// still getting up (=releve). Reported: "la posture au sol ne se re-arme pas apres une deuxieme chute".
	if (TestName != TEXT("rechute") || TestStep < 2)
	{
		return;
	}
	static FString Mode;
	static float PostureHeld = 0.0f;
	static float StateTime = 0.0f;
	static float LogClock = 0.0f;
	static float LastTime = 0.0f;
	static int32 Falls = 1;
	if (Mode.IsEmpty())
	{
		Mode = TEXT("debout");
		FParse::Value(FCommandLine::Get(), TEXT("ImpactRechute="), Mode);
		LastTime = Time;
		// =confus needs the bleeding to have made him confused while he is up: the wounds act faster
		// (-ImpactRechuteVitesse=, 4 by default), so it happens within the run.
		if (Mode == TEXT("confus"))
		{
			float Speed = 4.0f;
			FParse::Value(FCommandLine::Get(), TEXT("ImpactRechuteVitesse="), Speed);
			if (IConsoleVariable* WoundSpeed = IConsoleManager::Get().FindConsoleVariable(TEXT("impact.WoundSpeed")))
			{
				WoundSpeed->Set(Speed);
			}
		}
	}
	const float Step = FMath::Max(0.0f, Time - LastTime);
	LastTime = Time;
	const bool bGettingUp = Target->GetLocomotionAction() == AlsLocomotionActionTags::GettingUp;
	const bool bRagdolling = Target->GetLocomotionAction() == AlsLocomotionActionTags::Ragdolling;
	const UImpactBodyComponent* Body = Target->FindComponentByClass<UImpactBodyComponent>();
	const EImpactConsciousness State = Body != nullptr ? Body->GetPhysiology().GetConsciousness() : EImpactConsciousness::Dead;
	const TCHAR* StateName = State == EImpactConsciousness::Conscious ? TEXT("conscient")
		                         : State == EImpactConsciousness::Confused ? TEXT("confus")
		                         : State == EImpactConsciousness::SemiConscious ? TEXT("semi-conscient")
		                         : State == EImpactConsciousness::Unconscious ? TEXT("inconscient") : TEXT("mort");

	LogClock -= Step;
	if (LogClock <= 0.0f)
	{
		LogClock = 0.5f;
		UE_LOG(LogImpact, Display, TEXT("[rechute] t=%.1f chute %d etat=%s ecrit=%d posture=%d action=%s"), Time, Falls, StateName,
		       React->IsWrithing() ? 1 : 0, React->IsGroundPostureActive() ? 1 : 0,
		       bRagdolling ? TEXT("ragdoll") : bGettingUp ? TEXT("se releve") : React->IsLyingDown() ? TEXT("allonge") : TEXT("debout"));
	}

	if (TestStep == 2)
	{
		PostureHeld = React->IsGroundPostureActive() ? PostureHeld + Step : 0.0f;
		if (PostureHeld > 3.0f)
		{
			UE_LOG(LogImpact, Display, TEXT("[rechute] t=%.1f chute 1 : posture tenue 3 s, on le laisse se relever"), Time);
			TestStep = 3;
			StateTime = 0.0f;
		}
		else if (Time > 20.0f)
		{
			UE_LOG(LogImpact, Display, TEXT("[rechute] t=%.1f chute 1 : posture absente, banc arrete"), Time);
			TestStep = 5;
		}
	}
	else if (TestStep == 3)
	{
		React->SetMayGetUp(true);
		React->SetHeldDown(false);
		// =confus: he is put down again only once the bleeding has made him confused - the only second fall on
		// record (log of 20 October 23:13) was a confused man. Run with -ExecCmds="impact.WoundSpeed 4" so it comes
		// before the end. If he goes down by himself first, that fall is the one measured.
		static bool bWasUp = false;
		bWasUp |= !React->IsDown();
		if (Mode == TEXT("confus") && bWasUp && React->IsDown() && !bGettingUp)
		{
			Falls = 2;
			UE_LOG(LogImpact, Display, TEXT("[rechute] t=%.1f chute 2 (tout seul), %s"), Time, StateName);
			React->SetMayGetUp(false);
		React->SetHeldDown(true);
			TestStep = 4;
			StateTime = 0.0f;
			PostureHeld = 0.0f;
			return;
		}
		const bool bReady = Mode == TEXT("releve") ? bGettingUp
		                    : Mode == TEXT("confus") ? !React->IsDown() && State == EImpactConsciousness::Confused
		                    : !React->IsDown();
		StateTime = bReady ? StateTime + Step : 0.0f;
		if (StateTime > (Mode == TEXT("releve") ? 0.3f : 1.0f))
		{
			React->SetMayGetUp(false);
		React->SetHeldDown(true);
			React->Collapse(Target->GetActorForwardVector() * 150.0f, false, TEXT("banc rechute 2"));
			Falls = 2;
			UE_LOG(LogImpact, Display, TEXT("[rechute] t=%.1f chute 2 (%s), %s"), Time, *Mode, StateName);
			TestStep = 4;
			StateTime = 0.0f;
			PostureHeld = 0.0f;
		}
	}
	else if (TestStep == 4)
	{
		React->SetMayGetUp(false);
		React->SetHeldDown(true);
		StateTime += Step;
		PostureHeld = React->IsGroundPostureActive() ? PostureHeld + Step : PostureHeld;
		if (StateTime > 15.0f)
		{
			UE_LOG(LogImpact, Display, TEXT("[rechute] t=%.1f chute 2 : posture %s (%.1f s en posture), %s"), Time,
			       PostureHeld > 0.5f ? TEXT("OUI") : TEXT("NON"), PostureHeld, StateName);
			TestStep = 5;
		}
	}
	else
	{
		React->SetMayGetUp(false);
		React->SetHeldDown(true);
	}
}

void UImpactGameplaySubsystem::FilmCourseShot(const float Time)
{
	// -ImpactFilmerBalle: the runner filmed from the moment the round is fired - it lands a tenth of a second later - through the
	// reaction. The question it answers is the one the numbers cannot: does his trunk snap into a standing pose while his legs run.
	if (FParse::Param(FCommandLine::Get(), TEXT("ImpactFilmerBalle")))
	{
		for (const float Delay : {0.08f, 0.14f, 0.2f, 0.28f, 0.38f, 0.5f, 0.7f, 1.0f})
		{
			WitnessTimes.Add(Time + Delay);
		}
		WitnessTimes.Sort();
	}
}

void UImpactGameplaySubsystem::RunCourseTest(const float Time, AAlsCharacter* Player, UImpactWeaponComponent* Weapon)
{
	// The exact failure that was reported: a man running takes a round in the shoulder, keeps his momentum,
	// and his legs freeze while his chest acts out a standing reaction. So this bench runs him, shoots him,
	// and measures both halves at once — the legs against their own speed before the shot, the trunk against
	// its own swing before the shot. Either number alone proves nothing.
	// Two ways to get a man running. "course" drives him from the bench, which is repeatable. "fuite" lets
	// him panic and run on his own, which is what was being watched when the legs were seen to freeze: a
	// bench that moves the legs itself cannot show them stopping.
	const bool bFlee = TestName == TEXT("fuite");
	AImpactCivilianController::bFleeingDisabled = !bFlee;
	CurrentTestTarget = 0;

	if (!Bodies.IsValidIndex(0) || !Bodies[0].IsValid())
	{
		return;
	}
	AAlsCharacter* Runner = Cast<AAlsCharacter>(Bodies[0]->GetOwner());
	if (Runner == nullptr)
	{
		return;
	}

	if (bFlee)
	{
		if (TestStep == 0 && Time >= 0.8f)
		{
			Runner->GetMesh()->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
			Player->GetMesh()->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
			Player->SetOverlayMode(AlsOverlayModeTags::PistolTwoHanded);
			Player->SetDesiredAiming(true);
			TestStep = 1;
		}
		if (TestStep == 1 && Time >= 1.0f && Time < 1.8f)
		{
			// Straight up, and only a little forward. Aimed at a point a thousand centimetres ahead and
			// nine hundred up, the round was still passing through whoever stood in front: every run
			// logged a reaction on Passant 1 two tenths of a second after the "warning" shot, which put an
			// impact inside the window used as the reference for his normal running. Every ratio measured
			// against that window was understated.
			AimAt(Player->GetActorLocation() + Player->GetActorForwardVector() * 60.0f + FVector(0, 0, 3000.0f));
		}
		if (TestStep == 1 && Time >= 1.8f)
		{
			// One round in the air, nobody hit: everyone runs on their own from here.
			Weapon->SetTriggerHeld(true);
			Weapon->SetTriggerHeld(false);
			UE_LOG(LogImpact, Display, TEXT("[test] t=%.2f coup de feu en l'air, Passant 1 fuit de lui-meme"), Time);
			TestStep = 4;
		}
		if (TestStep == 4)
		{
			// Followed from behind at a fixed distance, so the shot is possible wherever he has run to.
			const FVector Away = (Runner->GetActorLocation() - Player->GetActorLocation()).GetSafeNormal2D();
			Player->SetActorLocation(Runner->GetActorLocation() - Away * 400.0f, false, nullptr, ETeleportType::TeleportPhysics);
			AimAt(FMath::Lerp(GetBonePoint(0, TEXT("spine_02")), GetBonePoint(0, TEXT("spine_03")), 0.5f));
			// Shot only once he is genuinely running, and only after a reference of his own running has
			// been recorded. Firing at a man still getting up to speed measures the acceleration, not the hit.
			if (Runner->GetVelocity().Size2D() > 250.0f && CourseBeforeTime >= 1.2f)
			{
				Weapon->SetTriggerHeld(true);
				Weapon->SetTriggerHeld(false);
				CourseShotTime = Time;
				FilmCourseShot(Time);
				UE_LOG(LogImpact, Display, TEXT("[test] t=%.2f Passant 1 : dos, en pleine fuite a %.0f cm/s"), Time,
				       Runner->GetVelocity().Size2D());
				TestStep = 2;
			}
		}
		if (TestStep == 0)
		{
			return;
		}
	}
	else if (TestStep == 0 && Time >= 0.6f)
	{
		// Behind him, so the shoulder and the back are what the bullet can reach, and his stride stays
		// in frame. He is sent straight away from the shooter and told to run.
		CourseDirection = Runner->GetActorForwardVector().GetSafeNormal2D();
		Player->SetActorLocation(Runner->GetActorLocation() - CourseDirection * 420.0f, false, nullptr, ETeleportType::TeleportPhysics);
		Player->SetOverlayMode(AlsOverlayModeTags::PistolTwoHanded);
		Player->SetDesiredAiming(true);
		if (AAIController* Brain = Cast<AAIController>(Runner->GetController()))
		{
			Brain->ClearFocus(EAIFocusPriority::Gameplay);
			Brain->StopMovement();
		}
		// Off focus, Unreal stops refreshing a skeletal mesh and the pose read back is stale: the first run
		// of this bench reported the same foot heights to the centimetre for a second and a half, which
		// would have read as frozen legs. Measuring requires the pose to be computed whether seen or not.
		Runner->GetMesh()->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
		Player->GetMesh()->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
		UE_LOG(LogImpact, Display, TEXT("[test] t=%.2f Passant 1 part en courant, tireur derriere"), Time);
		TestStep = 1;
	}
	if (TestStep == 0)
	{
		return;
	}

	// Kept running for the whole bench, before and after the shot. If the legs stop, it is not because
	// nobody asked them to move. Not in the fleeing variant: there the legs are his own business, which
	// is the whole point of it.
	if (!bFlee)
	{
		Runner->SetDesiredAiming(false);
		Runner->SetDesiredGait(AlsGaitTags::Running);
		Runner->SetDesiredRotationMode(AlsRotationModeTags::VelocityDirection);
		Runner->AddMovementInput(CourseDirection, 1.0f);
	}

	const USkeletalMeshComponent* Mesh = Runner->GetMesh();
	const float Delta = GetWorld()->GetDeltaSeconds();
	const float Speed = Runner->GetVelocity().Size2D();
	const float Reference = Runner->GetActorLocation().Z;
	const float LeftFoot = Mesh->GetSocketLocation(TEXT("foot_l")).Z - Reference;
	const float RightFoot = Mesh->GetSocketLocation(TEXT("foot_r")).Z - Reference;

	// The trunk measured against the hips, so running forward does not read as bending forward.
	const FQuat Relative = Mesh->GetSocketQuaternion(TEXT("pelvis")).Inverse() * Mesh->GetSocketQuaternion(TEXT("spine_03"));
	if (!bCourseReference && Time >= 2.2f)
	{
		CourseReference = Relative;
		bCourseReference = true;
	}
	float Bend = 0.0f;
	if (bCourseReference)
	{
		FQuat Error = CourseReference.Inverse() * Relative;
		Error.EnforceShortestArcWith(FQuat::Identity);
		Bend = FMath::RadiansToDegrees(2.0f * FMath::Acos(FMath::Clamp(FMath::Abs(Error.W), 0.0f, 1.0f)));
	}

	// What "the legs stop moving" actually looks like when a body still travels: the foot carrying the
	// weight slides along the ground instead of staying planted. A foot that is planted has almost no
	// horizontal speed of its own however fast the body goes, so the ratio of the two is the test.
	// Counting foot crossings was not enough: a frozen leg pose can still cross while the body skates.
	const FVector StanceNow = LeftFoot < RightFoot ? Mesh->GetSocketLocation(TEXT("foot_l")) : Mesh->GetSocketLocation(TEXT("foot_r"));
	const bool bSameFoot = (LeftFoot < RightFoot) == bCourseLeftLower;
	const float StanceSpeed = Delta > 0.0f && bSameFoot && !CourseStanceLast.IsZero()
		                          ? FVector::Dist2D(StanceNow, CourseStanceLast) / Delta
		                          : 0.0f;
	CourseStanceLast = StanceNow;
	const float Skate = Speed > 60.0f ? StanceSpeed / Speed : 0.0f;

	// A running trunk already swings a good thirty degrees, so the angle it reaches says little. What
	// separates a hit from a stride is how abruptly it gets there: the stride is a smooth swing, the hit
	// is a snap. Measured in degrees per second.
	// How far the trunk turned since the last frame, which needs no reference pose and so can be measured
	// from the first frame of the run - including before anything has been fired, on a man simply standing
	// there. That is the only figure that tells a body which shakes from a body which reacted.
	const float Snap = Delta > 0.0f && !CoursePreviousTrunk.Equals(FQuat::Identity)
		                   ? FMath::RadiansToDegrees(CoursePreviousTrunk.AngularDistance(Relative)) / Delta
		                   : 0.0f;
	CoursePreviousTrunk = Relative;

	// At rest, before the shot that starts the panic. A mean, not a maximum: one spike is not a tremor.
	if (Speed < 20.0f && Delta > 0.0f && Snap > 0.0f)
	{
		CourseRestSnapSum += Snap * Delta;
		CourseRestTime += Delta;
	}

	// A stride is counted each time the weight passes from one foot to the other: that is what a frozen leg
	// stops doing. Speed alone is not enough — a body sliding on momentum keeps its speed with still legs.
	const bool bLeftLower = LeftFoot < RightFoot;
	const bool bStride = bLeftLower != bCourseLeftLower && Speed > 60.0f;
	bCourseLeftLower = bLeftLower;

	// Shot at 4 s, after a second and a half of running has been recorded as the reference.
	if (TestStep == 1)
	{
		// Aimed every frame, not once: the first run fired at where the shoulder had been a quarter of a
		// second earlier and the round went into the scenery thirty metres away.
		// On the shoulder itself, between collarbone and arm. Aiming at the bone centre sent rounds through
		// the neck, which is another zone; pushing the point 7 cm outwards sent six rounds out of eight into
		// the scenery forty metres away, and a bench that misses measures nothing.
		AimAt(FMath::Lerp(GetBonePoint(0, TEXT("clavicle_r")), GetBonePoint(0, TEXT("upperarm_r")), 0.75f));
	}
	if (TestStep == 1 && !bFlee && Time >= 5.0f)
	{
		Weapon->SetTriggerHeld(true);
		Weapon->SetTriggerHeld(false);
		CourseShotTime = Time;
		FilmCourseShot(Time);
		UE_LOG(LogImpact, Display, TEXT("[test] t=%.2f Passant 1 : epaule droite, en pleine course a %.0f cm/s"), Time, Speed);
		TestStep = 2;
	}

	// The reference window: his own running, before anything is fired at him.
	if ((TestStep == 1 || TestStep == 4) && (bFlee ? Speed > 250.0f : Time >= 3.4f))
	{
		CourseBeforeSpeedSum += Speed * Delta;
		CourseBeforeTime += Delta;
		CourseBeforePeak = FMath::Max(CourseBeforePeak, Bend);
		CourseBeforeSnap = FMath::Max(CourseBeforeSnap, Snap);
		CourseBeforeSnapSum += Snap * Delta;
		CourseBeforeSkateSum += Skate * Delta;
		CourseBeforeStrides += bStride ? 1 : 0;
	}
	else if (TestStep == 2 && Time < CourseShotTime + 1.6f)
	{
		CourseAfterSpeedSum += Speed * Delta;
		CourseAfterTime += Delta;
		CourseAfterPeak = FMath::Max(CourseAfterPeak, Bend);
		CourseAfterSnap = FMath::Max(CourseAfterSnap, Snap);
		CourseAfterSnapSum += Snap * Delta;
		CourseAfterSkateSum += Skate * Delta;
		CourseAfterStrides += bStride ? 1 : 0;
	}

	if (TestStep >= 1 && (bFlee || Time >= 3.2f) && Time < 11.0f)
	{
		UE_LOG(LogImpact, Display, TEXT("[course] %.3f %.0f %.2f %.2f %.2f %.0f %.2f %d"), Time, Speed, LeftFoot, RightFoot, Bend, Snap, Skate, bStride ? 1 : 0);
	}

	if (!bCourseReported && TestStep == 2 && Time >= CourseShotTime + 1.8f)
	{
		bCourseReported = true;
		const float Before = CourseBeforeTime > 0.0f ? CourseBeforeSpeedSum / CourseBeforeTime : 0.0f;
		const float After = CourseAfterTime > 0.0f ? CourseAfterSpeedSum / CourseAfterTime : 0.0f;
		// Rates, not counts: the two windows do not last the same, and the first run of this bench compared
		// 1,3 s of acceleration from standing against 1,6 s at full speed.
		const float BeforeRate = CourseBeforeTime > 0.0f ? CourseBeforeStrides / CourseBeforeTime : 0.0f;
		const float AfterRate = CourseAfterTime > 0.0f ? CourseAfterStrides / CourseAfterTime : 0.0f;
		UE_LOG(LogImpact, Display,
		       TEXT("[course] JAMBES avant %.0f cm/s, %.1f pas/s, glissement %.2f | apres %.0f cm/s (%.0f%%), %.1f pas/s (%.0f%%), glissement %.2f"),
		       Before, BeforeRate, CourseBeforeTime > 0.0f ? CourseBeforeSkateSum / CourseBeforeTime : 0.0f,
		       After, Before > 0.0f ? After / Before * 100.0f : 0.0f, AfterRate,
		       BeforeRate > 0.0f ? AfterRate / BeforeRate * 100.0f : 0.0f,
		       CourseAfterTime > 0.0f ? CourseAfterSkateSum / CourseAfterTime : 0.0f);
		UE_LOG(LogImpact, Display,
		       TEXT("[course] TORSE angle: course %.1f deg, apres le tir %.1f deg (x%.2f) | brusquerie: course %.0f deg/s, apres le tir %.0f deg/s (x%.2f)"),
		       CourseBeforePeak, CourseAfterPeak, CourseBeforePeak > 0.0f ? CourseAfterPeak / CourseBeforePeak : 0.0f,
		       CourseBeforeSnap, CourseAfterSnap, CourseBeforeSnap > 0.0f ? CourseAfterSnap / CourseBeforeSnap : 0.0f);
		UE_LOG(LogImpact, Display,
		       TEXT("[course] TREPIDATION au repos %.0f deg/s | moyenne en course %.0f deg/s | moyenne apres le tir %.0f deg/s"),
		       CourseRestTime > 0.0f ? CourseRestSnapSum / CourseRestTime : -1.0f,
		       CourseBeforeTime > 0.0f ? CourseBeforeSnapSum / CourseBeforeTime : 0.0f,
		       CourseAfterTime > 0.0f ? CourseAfterSnapSum / CourseAfterTime : 0.0f);
	}
}

void UImpactGameplaySubsystem::RunArmTest(const float Time, AAlsCharacter* Player, UImpactWeaponComponent* Weapon)
{
	// Reported twice, never measured: "les bras font toujours completement n'importe quoi quand le npc
	// court, ils sont plus mou donc il bouge comme des spagethi tout en tenant la blessure et il rentre
	// dans son corp on les vois sortnir par le dos".
	//
	// Two things to find out, and only one of them is about softness. Does the hand go *into* the chest -
	// which is a collision question, answered by the physics asset's disable table and by testing the
	// hand's position against the chest body - and does it thrash, which is the arm's own speed measured
	// in the chest's frame so that running does not count as thrashing.
	AImpactCivilianController::bFleeingDisabled = false;
	CurrentTestTarget = 0;

	if (!Bodies.IsValidIndex(0) || !Bodies[0].IsValid())
	{
		return;
	}
	AAlsCharacter* Target = Cast<AAlsCharacter>(Bodies[0]->GetOwner());
	USkeletalMeshComponent* Mesh = Target != nullptr ? Target->GetMesh() : nullptr;
	if (Mesh == nullptr)
	{
		return;
	}

	// The control, and the whole test is worthless without it. An arm swings when a man runs; a hand
	// moving at a hundred centimetres a second says nothing until an unhurt man's hand has been measured
	// doing the same thing. -ImpactBrasTemoin runs him without shooting him.
	static const bool bControl = FParse::Param(FCommandLine::Get(), TEXT("ImpactBrasTemoin"));

	// Which wound. "Ils sont plus mou donc il bouge comme des spagethi" is about a wounded arm - a belly
	// wound does not soften an arm, it only gives the hands something to hold. So the shoulder is the
	// case to watch, and the belly one is kept because that is what was measured first.
	static const bool bShoulder = FParse::Param(FCommandLine::Get(), TEXT("ImpactBrasEpaule"));

	// Once, up front: is a hand even allowed to collide with the chest?
	if (!bArmTableLogged)
	{
		bArmTableLogged = true;
		const UPhysicsAsset* Asset = Mesh->GetPhysicsAsset();
		if (Asset == nullptr)
		{
			UE_LOG(LogImpact, Display, TEXT("[bras] aucun asset physique"));
		}
		else
		{
			static const FName Pairs[][2] = {
				{TEXT("hand_l"), TEXT("spine_03")}, {TEXT("hand_r"), TEXT("spine_03")},
				{TEXT("lowerarm_l"), TEXT("spine_03")}, {TEXT("lowerarm_r"), TEXT("spine_03")},
				{TEXT("hand_l"), TEXT("spine_01")}, {TEXT("hand_r"), TEXT("spine_01")},
				{TEXT("hand_l"), TEXT("pelvis")}, {TEXT("hand_r"), TEXT("pelvis")},
			};
			for (const FName* Pair : Pairs)
			{
				const int32 A = Asset->FindBodyIndex(Pair[0]);
				const int32 B = Asset->FindBodyIndex(Pair[1]);
				if (A == INDEX_NONE || B == INDEX_NONE)
				{
					UE_LOG(LogImpact, Display, TEXT("[bras] %s <-> %s : un des deux corps n'existe pas (%d, %d)"),
					       *Pair[0].ToString(), *Pair[1].ToString(), A, B);
					continue;
				}
				const bool bDisabled = Asset->CollisionDisableTable.Contains(FRigidBodyIndexPair(A, B));
				UE_LOG(LogImpact, Display, TEXT("[bras] collision %s <-> %s : %s"), *Pair[0].ToString(),
				       *Pair[1].ToString(), bDisabled ? TEXT("DESACTIVEE") : TEXT("active"));
			}
		}
	}

	if (TestStep == 0 && Time >= 1.0f)
	{
		Player->SetActorLocation(Target->GetActorLocation() + Target->GetActorForwardVector() * 320.0f,
		                         false, nullptr, ETeleportType::TeleportPhysics);
		Player->SetOverlayMode(AlsOverlayModeTags::PistolTwoHanded);
		Player->SetDesiredAiming(true);
		TestStep = 1;
	}
	if (TestStep == 1 && !bControl && Time >= 2.0f && Time < 3.0f)
	{
		AimAt(bShoulder
			      ? GetBonePoint(0, TEXT("upperarm_r"))
			      : FMath::Lerp(GetBonePoint(0, TEXT("pelvis")), GetBonePoint(0, TEXT("spine_01")), 0.7f));
	}
	if (TestStep == 1 && Time >= 3.0f)
	{
		if (bControl)
		{
			// Something to run from, without a hole in him: a round into the ground near his feet.
			AimAt(Target->GetActorLocation() - FVector(0.0f, 0.0f, 95.0f) + Target->GetActorForwardVector() * 90.0f);
		}
		Weapon->SetTriggerHeld(true);
		Weapon->SetTriggerHeld(false);
		UE_LOG(LogImpact, Display, TEXT("[bras] t=%.2f %s"), Time,
		       bControl ? TEXT("TEMOIN : tir au sol, il fuit sans blessure") : TEXT("touche au ventre, il doit tenir la plaie et fuir"));
		TestStep = 2;
	}
	if (TestStep < 2 || Time < 5.0f)
	{
		return;
	}

	// He stops running after a few seconds, and three seconds of running is not enough to see an arm do
	// anything. So he is kept moving by a round into the ground well off to one side - two metres away
	// and under him, never at him: a bench that shoots the thing it is measuring was how an earlier set
	// of numbers got spoiled.
	if (Time - PushLogTimer > 3.0f)
	{
		PushLogTimer = Time;
		AimAt(Target->GetActorLocation() + Target->GetActorRightVector() * 200.0f - FVector(0.0f, 0.0f, 95.0f));
		Weapon->SetTriggerHeld(true);
		Weapon->SetTriggerHeld(false);
	}

	// Everything in the chest's own frame, so a man running is not mistaken for a man flailing.
	const FTransform Chest = Mesh->GetSocketTransform(TEXT("spine_03"));
	const float Step = GetWorld()->GetDeltaSeconds();
	static const FName Hands[2] = {TEXT("hand_l"), TEXT("hand_r")};
	float Speed[2] = {0.0f, 0.0f};
	float Depth[2] = {0.0f, 0.0f};
	bool bBehind[2] = {false, false};

	for (int32 i = 0; i < 2; ++i)
	{
		const FVector Hand = Mesh->GetSocketLocation(Hands[i]);
		const FVector Local = Chest.InverseTransformPosition(Hand);
		if (!ArmHandLast[i].IsZero() && Step > 0.0f)
		{
			Speed[i] = FVector::Dist(Local, ArmHandLast[i]) / Step;
		}
		ArmHandLast[i] = Local;

		// How far from the trunk, taking whichever part of it is nearest - a hand on a belly wound is
		// nowhere near the upper chest, and measuring only against spine_03 made it look far out.
		// GetSquaredDistanceToBody answers false when the point is inside the shape.
		static const FName Trunk[] = {TEXT("spine_01"), TEXT("spine_02"), TEXT("spine_03"), TEXT("pelvis")};
		Depth[i] = 1e6f;
		for (const FName& Part : Trunk)
		{
			const FBodyInstance* Body = Mesh->GetBodyInstance(Part);
			if (Body == nullptr)
			{
				continue;
			}
			float Away = 0.0f;
			FVector OnBody;
			if (!Body->GetSquaredDistanceToBody(Hand, Away, OnBody))
			{
				Depth[i] = -1.0f;
				break;
			}
			Depth[i] = FMath::Min(Depth[i], FMath::Sqrt(Away));
		}

		// Behind him: along the way he is facing, not along a spine bone's own axis. The first version
		// used the chest bone's local X, which on this skeleton runs up the spine - so it reported the
		// hand behind the body 100 % of the time, standing still, both hands, every frame. A measurement
		// that never varies is not a measurement.
		bBehind[i] = FVector::DotProduct(Hand - Chest.GetLocation(), Target->GetActorForwardVector()) < -10.0f;
	}

	++ArmFrames;
	if (Depth[0] < 0.0f || Depth[1] < 0.0f) { ++ArmInsideFrames; }
	if (bBehind[0] || bBehind[1]) { ++ArmBehindFrames; }

	ArmLogTimer -= Step;
	if (ArmLogTimer <= 0.0f)
	{
		ArmLogTimer = 1.0f;
		UE_LOG(LogImpact, Display,
		       TEXT("[bras] t=%.1f G: %.0f cm/s, tronc %+.0f cm | D: %.0f cm/s, tronc %+.0f cm | dans le tronc %.0f%% | derriere %.0f%% | course %.0f cm/s"),
		       Time, Speed[0], Depth[0], Speed[1], Depth[1],
		       ArmFrames > 0 ? 100.0f * ArmInsideFrames / ArmFrames : 0.0f,
		       ArmFrames > 0 ? 100.0f * ArmBehindFrames / ArmFrames : 0.0f,
		       Target->GetVelocity().Size2D());
	}
}

void UImpactGameplaySubsystem::RunBodyShoveTest(const float Time, AAlsCharacter* Player, UImpactWeaponComponent* Weapon)
{
	// Does walking into a body move it? Nothing measured that before, and the answer in play was no:
	// "on peut pas les pousser". The push bench next door shoves with Shove() straight into the balance
	// controller, which proves nothing about contact - it never touches anybody.
	//
	// So: put one man down, let him settle, then drive the player into him with real movement input. It
	// has to be real movement, because the force comes from the movement component's own reaction to a
	// blocking hit; teleporting the player through the body produces no hit and no push.
	AImpactCivilianController::bFleeingDisabled = true;
	CurrentTestTarget = 0;

	if (!Bodies.IsValidIndex(0) || !Bodies[0].IsValid())
	{
		return;
	}
	AAlsCharacter* Target = Cast<AAlsCharacter>(Bodies[0]->GetOwner());
	UImpactReactionComponent* React = Target != nullptr ? Target->FindComponentByClass<UImpactReactionComponent>() : nullptr;
	if (Target == nullptr || React == nullptr)
	{
		return;
	}
	const USkeletalMeshComponent* Mesh = Target->GetMesh();

	if (TestStep == 0 && Time >= 1.0f)
	{
		Player->SetActorLocation(Target->GetActorLocation() + Target->GetActorForwardVector() * 300.0f,
		                         false, nullptr, ETeleportType::TeleportPhysics);
		Player->SetOverlayMode(AlsOverlayModeTags::PistolTwoHanded);
		Player->SetDesiredAiming(true);
		if (AAIController* Brain = Cast<AAIController>(Target->GetController()))
		{
			Brain->ClearFocus(EAIFocusPriority::Gameplay);
			Brain->StopMovement();
		}
		TestStep = 1;
	}
	if (TestStep == 1 && Time >= 2.0f && Time < 3.0f)
	{
		AimAt(FMath::Lerp(GetBonePoint(0, TEXT("pelvis")), GetBonePoint(0, TEXT("spine_01")), 0.6f));
	}
	if (TestStep == 1 && Time >= 3.0f)
	{
		Weapon->SetTriggerHeld(true);
		Weapon->SetTriggerHeld(false);
		TestStep = 2;
	}
	if (TestStep == 2 && Time >= 4.0f)
	{
		React->SetMayGetUp(false);
		React->SetHeldDown(true);
		if (!React->IsDown())
		{
			React->Collapse(Target->GetActorForwardVector() * 150.0f, false, TEXT("banc bourrade"));
		}
		else
		{
			TestStep = 3;
			UE_LOG(LogImpact, Display, TEXT("[bourrade] t=%.1f a terre, on laisse reposer"), Time);
			// -ImpactBourradeMort: the same walk into a dead man. Reported from play: "desactive les collisions avec eux et leurs
			// armes quand ils sont morts, sinon ca fait des bugs".
			if (FParse::Param(FCommandLine::Get(), TEXT("ImpactBourradeMort")))
			{
				Bodies[0]->GetPhysiology().KillNow(TEXT("banc bourrade mort"));
				UE_LOG(LogImpact, Display, TEXT("[bourrade] t=%.1f il est mort"), Time);
			}
		}
	}

	// Eight seconds of lying still first. Pushing a body that is still tumbling would measure the tumble,
	// and that is exactly what caught me out once: 75 cm of "push" that was the fall still finishing.
	// So the reference is taken at twelve seconds and the walk only starts at fifteen - whatever the body
	// does in those three seconds is its own, and the log says so before anything touches it.
	if (TestStep == 3 && Time >= 12.0f)
	{
		ShoveHipsStart = Mesh->GetSocketLocation(TEXT("pelvis"));
		TestStep = 4;
	}
	if (TestStep == 4 && Time >= 15.0f)
	{
		TestStep = 5;
		UE_LOG(LogImpact, Display, TEXT("[bourrade] t=%.1f derive propre avant contact : %.1f cm, le joueur marche dedans"),
		       Time, FVector::Dist(Mesh->GetSocketLocation(TEXT("pelvis")), ShoveHipsStart));
		ShoveHipsStart = Mesh->GetSocketLocation(TEXT("pelvis"));
		ShoveBoneStart.Reset();
		for (int32 i = 0; i < Mesh->GetNumBones(); ++i)
		{
			ShoveBoneStart.Add(Mesh->GetBoneTransform(i).GetLocation());
		}
		ShoveWeaponStart.Reset();
		for (TActorIterator<AActor> It(GetWorld()); It; ++It)
		{
			if (It->ActorHasTag(TEXT("ImpactArmeLachee")))
			{
				ShoveWeaponStart.Add(*It, It->GetActorLocation());
			}
		}
	}

	if (TestStep >= 5)
	{
		// Straight at the hips, walking, every frame. Aiming is dropped so he moves at his own speed.
		Player->SetDesiredAiming(false);
		const FVector Hips = Mesh->GetSocketLocation(TEXT("pelvis"));
		const FVector Flat(Hips.X - Player->GetActorLocation().X, Hips.Y - Player->GetActorLocation().Y, 0.0f);
		Player->AddMovementInput(Flat.GetSafeNormal(), 1.0f);

		const float Gap = Flat.Size();
		if (Gap < 60.0f && ShoveContactTime < 0.0f)
		{
			ShoveContactTime = Time;
		}

		ShoveLogTimer -= GetWorld()->GetDeltaSeconds();
		if (ShoveLogTimer <= 0.0f)
		{
			ShoveLogTimer = 0.5f;
			// Every body, not only the hips. Walking into an outflung arm moves the arm, and a bench
			// that only watches the pelvis calls that nothing happening.
			float WeaponMoved = 0.0f;
			for (const TPair<TWeakObjectPtr<AActor>, FVector>& Dropped : ShoveWeaponStart)
			{
				if (Dropped.Key.IsValid())
				{
					WeaponMoved = FMath::Max(WeaponMoved, static_cast<float>(FVector::Dist(Dropped.Key->GetActorLocation(), Dropped.Value)));
				}
			}
			float Worst = 0.0f;
			for (int32 i = 0; i < Mesh->GetNumBones() && i < ShoveBoneStart.Num(); ++i)
			{
				Worst = FMath::Max(Worst, static_cast<float>(
					                   FVector::Dist(Mesh->GetBoneTransform(i).GetLocation(), ShoveBoneStart[i])));
			}
			UE_LOG(LogImpact, Display,
			       TEXT("[bourrade] t=%.1f joueur a %.0f cm du bassin | bassin deplace de %.1f cm | membre le plus deplace %.1f cm | effort joueur %.0f | contact %s | arme au sol deplacee de %.1f cm | vitesse joueur %.0f cm/s"),
			       Time, Gap, FVector::Dist(Hips, ShoveHipsStart), Worst,
			       Player->GetCharacterMovement() != nullptr ? Player->GetCharacterMovement()->GetCurrentAcceleration().Size2D() : 0.0f,
			       ShoveContactTime >= 0.0f ? TEXT("oui") : TEXT("non"), WeaponMoved, Player->GetVelocity().Size());
		}
	}
}

void UImpactGameplaySubsystem::RunPushTest(const float Time, AAlsCharacter* Player, UImpactWeaponComponent* Weapon)
{
	// The measuring stick for balance. One person is shoved harder and harder, with nothing else going on,
	// and what his body does is written down. A balance controller that works must show: small shoves absorbed
	// on the spot, middling ones caught with steps in the direction of the push, and a fall only when the
	// speed is past what feet can catch. Today the middle case barely exists: he stands or he drops.
	AImpactCivilianController::bFleeingDisabled = true;
	CurrentTestTarget = 0;

	if (!Bodies.IsValidIndex(0) || !Bodies[0].IsValid())
	{
		return;
	}
	ACharacter* Victim = Cast<ACharacter>(Bodies[0]->GetOwner());
	UImpactReactionComponent* Reaction = Victim != nullptr ? Victim->FindComponentByClass<UImpactReactionComponent>() : nullptr;
	if (Reaction == nullptr)
	{
		return;
	}

	// Shoves every three seconds, each one harder than the last.
	static const float Pushes[] = {40.0f, 80.0f, 130.0f, 190.0f, 260.0f, 350.0f};
	const float First = 3.0f;
	const int32 Want = FMath::FloorToInt((Time - First) / 3.0f) + 1;
	if (Time >= First && PushStep < Want && PushStep < UE_ARRAY_COUNT(Pushes))
	{
		const FVector From = (Victim->GetActorLocation() - Player->GetActorLocation()).GetSafeNormal2D();
		PushStart = Victim->GetActorLocation();
		Reaction->Shove(From, Pushes[PushStep], TEXT("banc d'essai"));
		UE_LOG(LogImpact, Display, TEXT("[poussee] %d : dV %.0f cm/s"), PushStep + 1, Pushes[PushStep]);
		++PushStep;
	}

	// Per frame: where the centre of mass is relative to the feet, how fast it travels, and how far beyond the
	// support it would land if nothing were done. That last number is the capture point, and it is what a real
	// controller reacts to.
	FVector Centre, Velocity, Support;
	bool bLeft = false, bRight = false;
	if (!Reaction->GetBalanceState(Centre, Velocity, Support, bLeft, bRight))
	{
		return;
	}
	constexpr float Omega = 3.13f;
	const FVector Flat(Centre.X - Support.X, Centre.Y - Support.Y, 0.0f);
	const FVector Capture = Flat + FVector(Velocity.X, Velocity.Y, 0.0f) / Omega;

	PushLogTimer -= GetWorld()->GetDeltaSeconds();
	if (PushLogTimer <= 0.0f)
	{
		PushLogTimer = 0.1f;
		UE_LOG(LogImpact, Display,
		       TEXT("[equilibre] %.2f centre %.1f cm des pieds | vitesse %.0f cm/s | point de rattrapage %.1f cm | appuis %s%s | %s | avance %.0f cm"),
		       Time, Flat.Size(), Velocity.Size2D(), Capture.Size(), bLeft ? TEXT("G") : TEXT("-"), bRight ? TEXT("D") : TEXT("-"),
		       Reaction->IsDown() ? TEXT("A TERRE") : Reaction->IsStaggering() ? TEXT("rattrape") : TEXT("debout"),
		       FVector::Dist2D(Victim->GetActorLocation(), PushStart));
	}
}

void UImpactGameplaySubsystem::StepWitness()
{
	APlayerController* Controller = GetWorld()->GetFirstPlayerController();
	const ACharacter* Character = Bodies.IsValidIndex(WitnessTarget) && Bodies[WitnessTarget].IsValid()
		                              ? Cast<ACharacter>(Bodies[WitnessTarget]->GetOwner())
		                              : nullptr;
	const AAlsCharacter* Player = GetPlayerCharacter();
	if (Controller == nullptr || Character == nullptr || Player == nullptr)
	{
		WitnessFrame = -1;
		return;
	}

	if (WitnessCamera == nullptr)
	{
		WitnessCamera = GetWorld()->SpawnActor<ACameraActor>();
		WitnessCamera->GetCameraComponent()->SetFieldOfView(50.0f);
		WitnessCamera->GetCameraComponent()->bConstrainAspectRatio = false;
	}

	// Views relative to the line of fire: from the shooter's right side, and facing the target.
	const FVector Focus = Character->GetMesh()->GetSocketLocation(TEXT("pelvis")) + FVector(0, 0, 10.0f);
	const FVector Fire = (Focus - Player->GetActorLocation()).GetSafeNormal2D();
	const FVector Side = FVector::CrossProduct(FVector::UpVector, Fire);
	const FString Folder = FPaths::ProjectSavedDir() / TEXT("Screenshots/Witness");

	// Put the camera where it can actually see. Twice today a check was made on a picture that showed a wall,
	// or another passer-by standing in front of the one being filmed, and the wrong conclusion was drawn from
	// it. The camera now walks in along its own line until nothing but the subject is in the way.
	const auto Place = [&](const FVector& Wanted)
	{
		FVector From = Wanted;
		FCollisionQueryParams Params(SCENE_QUERY_STAT(ImpactWitness), false, Character);
		Params.AddIgnoredActor(WitnessCamera);
		for (int32 Try = 0; Try < 8; ++Try)
		{
			FHitResult Blocked;
			if (!GetWorld()->LineTraceSingleByChannel(Blocked, Focus, From, ECC_Visibility, Params))
			{
				break;
			}
			// Something stands between the camera and the subject: come closer along the same line.
			From = Focus + (From - Focus).GetSafeNormal() * FMath::Max(FVector::Dist(Focus, Blocked.ImpactPoint) - 35.0f, 110.0f);
			if (FVector::Dist(Focus, From) <= 115.0f)
			{
				break;
			}
		}
		WitnessCamera->SetActorLocationAndRotation(From, (Focus - From).Rotation());
	};

	switch (WitnessFrame)
	{
	case 0:
		Place(Focus + Side * 330.0f + FVector(0, 0, 30.0f));
		Controller->SetViewTarget(WitnessCamera);
		break;
	case 2:
		FScreenshotRequest::RequestScreenshot(Folder / FString::Printf(TEXT("w_%02d_cote.png"), WitnessIndex), true, false);
		break;
	case 3:
		Place(Focus - Fire * 330.0f + FVector(0, 0, 30.0f));
		break;
	case 5:
		FScreenshotRequest::RequestScreenshot(Folder / FString::Printf(TEXT("w_%02d_face.png"), WitnessIndex), true, false);
		break;
	case 6:
		// -ImpactTest=gang: a third view, from behind his right shoulder towards the player, where a weapon held on its side shows.
		if (TestName.StartsWith(TEXT("gang")))
		{
			// Higher and wider than his shoulder, looking ahead of him at the height of the gun (his back hid it from behind).
			Place(Focus + Fire * 170.0f - Side * 120.0f + FVector(0, 0, 95.0f));
			const FVector Gun = Focus - Fire * 80.0f + FVector(0, 0, 45.0f);
			WitnessCamera->SetActorRotation((Gun - WitnessCamera->GetActorLocation()).Rotation());
			break;
		}
		Controller->SetViewTarget(Controller->GetPawn());
		UE_LOG(LogImpact, Display, TEXT("[witness] %d sur %s"), WitnessIndex, *Bodies[WitnessTarget]->DisplayName);
		WitnessFrame = -1;
		return;
	case 8:
		FScreenshotRequest::RequestScreenshot(Folder / FString::Printf(TEXT("w_%02d_dos.png"), WitnessIndex), true, false);
		break;
	case 9:
		Controller->SetViewTarget(Controller->GetPawn());
		UE_LOG(LogImpact, Display, TEXT("[witness] %d sur %s"), WitnessIndex, *Bodies[WitnessTarget]->DisplayName);
		WitnessFrame = -1;
		return;
	default:
		break;
	}
	++WitnessFrame;
}

void UImpactGameplaySubsystem::DrawOverlay(UCanvas* Canvas, APlayerController* Controller)
{
	if (Canvas == nullptr || Controller == nullptr || Controller->GetWorld() != GetWorld())
	{
		return;
	}

	UFont* Font = GEngine->GetMediumFont();

	// Labels above people.
	for (const TWeakObjectPtr<UImpactBodyComponent>& Body : Bodies)
	{
		const ACharacter* Character = Body.IsValid() ? Cast<ACharacter>(Body->GetOwner()) : nullptr;
		if (Character == nullptr)
		{
			continue;
		}

		const FVector Head = Character->GetMesh()->GetSocketLocation(TEXT("head")) + FVector(0, 0, 35.0f);
		FVector2D Screen;
		if (FVector::DistSquared(Head, Controller->PlayerCameraManager->GetCameraLocation()) > FMath::Square(5000.0f)
		    || !Controller->ProjectWorldLocationToScreen(Head, Screen))
		{
			continue;
		}

		const FString Text = Body->GetLabel();
		float Width = 0.0f;
		float Height = 0.0f;
		Canvas->StrLen(Font, Text, Width, Height);

		FCanvasTileItem Background(FVector2D(Screen.X - Width * 0.5f - 6.0f, Screen.Y - Height - 4.0f), FVector2D(Width + 12.0f, Height + 4.0f),
		                           FLinearColor(0.0f, 0.0f, 0.0f, 0.55f));
		Background.BlendMode = SE_BLEND_Translucent;
		Canvas->DrawItem(Background);
		Canvas->SetDrawColor(FColor::White);
		Canvas->DrawText(Font, Text, Screen.X - Width * 0.5f, Screen.Y - Height - 2.0f);
	}

	// -ImpactPNJTirent: the player's health at the bottom left, the screen red when he is hit, and the word when he is dead.
	if (const UImpactPlayerHealthComponent* Health = Controller->GetPawn() != nullptr ? Controller->GetPawn()->FindComponentByClass<UImpactPlayerHealthComponent>() : nullptr)
	{
		const float Low = 1.0f - Health->GetHealth() / 100.0f;
		const float Red = FMath::Clamp(Health->GetHurtFlash() * 0.35f + Low * Low * 0.3f + (Health->IsDead() ? 0.35f : 0.0f), 0.0f, 0.7f);
		if (Red > 0.01f)
		{
			FCanvasTileItem Tint(FVector2D::ZeroVector, FVector2D(Canvas->ClipX, Canvas->ClipY), FLinearColor(0.6f, 0.0f, 0.0f, Red));
			Tint.BlendMode = SE_BLEND_Translucent;
			Canvas->DrawItem(Tint);
		}
		const FVector2D BarAt(30.0f, Canvas->ClipY - 60.0f);
		FCanvasTileItem Back(BarAt, FVector2D(260.0f, 18.0f), FLinearColor(0.0f, 0.0f, 0.0f, 0.6f));
		Back.BlendMode = SE_BLEND_Translucent;
		Canvas->DrawItem(Back);
		FCanvasTileItem Bar(BarAt + FVector2D(2.0f, 2.0f), FVector2D(256.0f * Health->GetHealth() / 100.0f, 14.0f),
		                    Health->GetHealth() > 35.0f ? FLinearColor(0.85f, 0.85f, 0.85f, 0.9f) : FLinearColor(0.9f, 0.15f, 0.1f, 0.9f));
		Bar.BlendMode = SE_BLEND_Translucent;
		Canvas->DrawItem(Bar);
		if (Health->IsDead())
		{
			const FString Text = TEXT("Tu es mort");
			float Width = 0.0f;
			float Height = 0.0f;
			UFont* Big = GEngine->GetLargeFont();
			Canvas->StrLen(Big, Text, Width, Height);
			Canvas->SetDrawColor(FColor::White);
			Canvas->DrawText(Big, Text, (Canvas->ClipX - Width * 2.0f) * 0.5f, Canvas->ClipY * 0.4f, 2.0f, 2.0f);
		}
	}

	// Crosshair, opening with the spread of the weapon.
	const APawn* Pawn = Controller->GetPawn();
	const UImpactWeaponComponent* Weapon = Pawn != nullptr ? Pawn->FindComponentByClass<UImpactWeaponComponent>() : nullptr;
	if (Weapon == nullptr || Weapon->GetCurrentWeapon() == EImpactWeapon::None)
	{
		return;
	}

	const FVector2D Center(Canvas->ClipX * 0.5f, Canvas->ClipY * 0.5f);
	const float HalfFov = FMath::DegreesToRadians(Controller->PlayerCameraManager->GetFOVAngle() * 0.5f);
	const float Gap = 3.0f + FMath::Tan(FMath::DegreesToRadians(Weapon->GetSpreadDegrees())) / FMath::Tan(HalfFov) * Center.X;
	const float Length = 8.0f;

	const auto Line = [Canvas](const FVector2D& A, const FVector2D& B, const FLinearColor& Color, const float Thickness)
	{
		FCanvasLineItem Item(A, B);
		Item.SetColor(Color);
		Item.LineThickness = Thickness;
		Canvas->DrawItem(Item);
	};

	const FVector2D Directions[] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
	for (const FVector2D& Direction : Directions)
	{
		Line(Center + Direction * Gap, Center + Direction * (Gap + Length), FLinearColor(0, 0, 0, 0.6f), 3.0f);
		Line(Center + Direction * Gap, Center + Direction * (Gap + Length), FLinearColor::White, 1.0f);
	}

	// Hit marker when a bullet lands on a person.
	if (GetWorld()->GetTimeSeconds() - Weapon->GetLastBodyHitTime() < 0.15f)
	{
		const FLinearColor Red(1.0f, 0.15f, 0.1f);
		Line(Center + FVector2D(-12, -12), Center + FVector2D(-5, -5), Red, 2.0f);
		Line(Center + FVector2D(12, -12), Center + FVector2D(5, -5), Red, 2.0f);
		Line(Center + FVector2D(-12, 12), Center + FVector2D(-5, 5), Red, 2.0f);
		Line(Center + FVector2D(12, 12), Center + FVector2D(5, 5), Red, 2.0f);
	}
}

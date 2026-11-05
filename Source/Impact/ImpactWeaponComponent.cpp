#include "ImpactWeaponComponent.h"

#include "AlsCharacter.h"
#include "ImpactAmmoComponent.h"
#include "ImpactWeaponPresentationComponent.h"
#include "ImpactEquipmentComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "GameFramework/PlayerController.h"
#include "Impact.h"
#include "ImpactAudio.h"
#include "ImpactBodyComponent.h"
#include "ImpactFX.h"
#include "ImpactGameplaySubsystem.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "Utility/AlsGameplayTags.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(ImpactWeaponComponent)

namespace
{
	const FName MuzzleBone(TEXT("muzzle"));
}

UImpactWeaponComponent::UImpactWeaponComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PostUpdateWork;
}

void UImpactWeaponComponent::BeginPlay()
{
	Super::BeginPlay();
	if(!GetOwner()->FindComponentByClass<UImpactEquipmentComponent>())
	{
		auto Equipment=NewObject<UImpactEquipmentComponent>(GetOwner(),TEXT("ImpactEquipment"));
		GetOwner()->AddInstanceComponent(Equipment);Equipment->RegisterComponent();
	}
	Ammo = GetOwner()->FindComponentByClass<UImpactAmmoComponent>();
	if (!Ammo)
	{
		Ammo = NewObject<UImpactAmmoComponent>(GetOwner(), TEXT("ImpactAmmo"));
		Ammo->RegisterComponent();
	}
	Presentation = NewObject<UImpactWeaponPresentationComponent>(GetOwner(), TEXT("ImpactWeaponPresentation"));
	Presentation->RegisterComponent();
	BindInput();
}

void UImpactWeaponComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	const APawn* Pawn = Cast<APawn>(GetOwner());
	if (APlayerController* Controller = Pawn != nullptr ? Cast<APlayerController>(Pawn->GetController()) : nullptr)
	{
		if (WeaponInput != nullptr)
		{
			Controller->PopInputComponent(WeaponInput);
		}
		if (UEnhancedInputLocalPlayerSubsystem* Input = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(Controller->GetLocalPlayer()))
		{
			Input->RemoveMappingContext(MappingContext);
		}
	}

	Super::EndPlay(EndPlayReason);
}

void UImpactWeaponComponent::BindInput()
{
	const APawn* Pawn = Cast<APawn>(GetOwner());
	APlayerController* Controller = Pawn != nullptr ? Cast<APlayerController>(Pawn->GetController()) : nullptr;
	if (Controller == nullptr || !Controller->IsLocalController())
	{
		return;
	}

	FireAction = NewObject<UInputAction>(this, TEXT("IA_Impact_Fire"));
	FireAction->ValueType = EInputActionValueType::Boolean;

	MappingContext = NewObject<UInputMappingContext>(this, TEXT("IMC_Impact_Weapon"));
	MappingContext->MapKey(FireAction, EKeys::LeftMouseButton);

	XRayAction = NewObject<UInputAction>(this, TEXT("IA_Impact_XRay"));
	XRayAction->ValueType = EInputActionValueType::Boolean;
	MappingContext->MapKey(XRayAction, EKeys::F3);
	ReloadAction = NewObject<UInputAction>(this, TEXT("IA_Impact_Reload"));
	InspectAction = NewObject<UInputAction>(this, TEXT("IA_Impact_Inspect"));
	CloseInspectionAction = NewObject<UInputAction>(this, TEXT("IA_Impact_CloseInspection"));
	MappingContext->MapKey(ReloadAction, EKeys::R);
	MappingContext->MapKey(InspectAction, EKeys::T);
	LaserAction = NewObject<UInputAction>(this,TEXT("IA_Impact_Laser"));
	MappingContext->MapKey(LaserAction,EKeys::L);
	VisionAction=NewObject<UInputAction>(this,TEXT("IA_Impact_HelmetVision"));
	LampAction=NewObject<UInputAction>(this,TEXT("IA_Impact_HelmetLamp"));
	MappingContext->MapKey(VisionAction,EKeys::N);
	MappingContext->MapKey(LampAction,EKeys::H);
	MappingContext->MapKey(CloseInspectionAction, EKeys::Escape);

	if (UEnhancedInputLocalPlayerSubsystem* Input = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(Controller->GetLocalPlayer()))
	{
		Input->AddMappingContext(MappingContext, 10);
	}

	WeaponInput = NewObject<UEnhancedInputComponent>(GetOwner(), TEXT("ImpactWeaponInput"));
	WeaponInput->bBlockInput = false;
	WeaponInput->Priority = 5;
	WeaponInput->RegisterComponent();
	WeaponInput->BindAction(FireAction, ETriggerEvent::Started, this, &ThisClass::OnFirePressed);
	WeaponInput->BindAction(FireAction, ETriggerEvent::Completed, this, &ThisClass::OnFireReleased);
	WeaponInput->BindAction(XRayAction, ETriggerEvent::Started, this, &ThisClass::OnXRay);
	WeaponInput->BindAction(ReloadAction, ETriggerEvent::Started, this, &ThisClass::OnReload);
	WeaponInput->BindAction(InspectAction, ETriggerEvent::Started, this, &ThisClass::OnInspect);
	WeaponInput->BindAction(LaserAction,ETriggerEvent::Started,this,&ThisClass::OnLaser);
	WeaponInput->BindAction(VisionAction,ETriggerEvent::Started,this,&ThisClass::OnVision);
	WeaponInput->BindAction(LampAction,ETriggerEvent::Started,this,&ThisClass::OnLamp);
	WeaponInput->BindAction(CloseInspectionAction, ETriggerEvent::Started, this, &ThisClass::OnCloseInspection);
	Controller->PushInputComponent(WeaponInput);

	UE_LOG(LogImpact, Display, TEXT("[weapon] input bound on %s"), *GetOwner()->GetName());
}

EImpactWeapon UImpactWeaponComponent::GetCurrentWeapon() const
{
	const AAlsCharacter* Character = Cast<AAlsCharacter>(GetOwner());
	if (Character == nullptr)
	{
		return EImpactWeapon::None;
	}

	const FGameplayTag& Overlay = Character->GetOverlayMode();
	if (Overlay == AlsOverlayModeTags::Rifle)
	{
		return EImpactWeapon::Rifle;
	}
	if (Overlay == AlsOverlayModeTags::PistolOneHanded || Overlay == AlsOverlayModeTags::PistolTwoHanded)
	{
		return EImpactWeapon::Pistol;
	}
	return EImpactWeapon::None;
}

bool UImpactWeaponComponent::IsAiming() const
{
	const AAlsCharacter* Character = Cast<AAlsCharacter>(GetOwner());
	return Character != nullptr && Character->GetRotationMode() == AlsRotationModeTags::Aiming;
}

float UImpactWeaponComponent::GetSpreadDegrees() const
{
	const FImpactWeaponSpec Spec = GetCurrentSpec();
	return (IsAiming() ? Spec.AimSpreadDeg : Spec.HipSpreadDeg) + Bloom;
}

FImpactWeaponSpec UImpactWeaponComponent::GetCurrentSpec() const
{
    return GetCurrentWeapon()==EImpactWeapon::Rifle && Presentation ? Presentation->GetRifleSpec() : GetImpactWeaponSpec(GetCurrentWeapon());
}

void UImpactWeaponComponent::OnLaser() { if (Presentation) Presentation->ToggleLaser(); }
void UImpactWeaponComponent::OnVision(){if(auto E=GetOwner()->FindComponentByClass<UImpactEquipmentComponent>())E->ToggleVision();}
void UImpactWeaponComponent::OnLamp(){if(auto E=GetOwner()->FindComponentByClass<UImpactEquipmentComponent>())E->ToggleLamp();}

void UImpactWeaponComponent::SetTriggerHeld(const bool bHeld)
{
	if (bHeld)
	{
		OnFirePressed();
	}
	else
	{
		OnFireReleased();
	}
}

void UImpactWeaponComponent::OnFirePressed()
{
	if (Presentation && (Presentation->IsInspecting() || Presentation->IsBusy())) return;
	bTriggerHeld = true;
	if (Cooldown <= 0.0f && CanFire())
	{
		FireShot();
	}
}

void UImpactWeaponComponent::OnFireReleased()
{
	bTriggerHeld = false;
}

void UImpactWeaponComponent::OnXRay()
{
	if (UImpactGameplaySubsystem* Gameplay = GetWorld()->GetSubsystem<UImpactGameplaySubsystem>())
	{
		Gameplay->ToggleXRay();
	}
}

void UImpactWeaponComponent::OnReload()
{
	if (Presentation) Presentation->StartReload();
}

void UImpactWeaponComponent::OnInspect()
{
	if (Presentation) Presentation->ToggleInspection();
}

void UImpactWeaponComponent::OnCloseInspection()
{
	if (Presentation) Presentation->CloseInspection();
}

bool UImpactWeaponComponent::CanFire() const
{
	const AAlsCharacter* Character = Cast<AAlsCharacter>(GetOwner());
	return Character != nullptr && GetCurrentWeapon() != EImpactWeapon::None
	       && Ammo && Ammo->CanShoot(GetCurrentWeapon())
	       && (!Presentation || (!Presentation->IsInspecting() && !Presentation->IsBusy()))
	       && !Character->GetLocomotionAction().IsValid(); // not rolling, mantling, ragdolling or getting up
}

USkeletalMeshComponent* UImpactWeaponComponent::FindWeaponMesh() const
{
	if (Presentation && Presentation->GetSourceMesh()) return Presentation->GetSourceMesh();
	const ACharacter* Character = Cast<ACharacter>(GetOwner());
	TInlineComponentArray<USkeletalMeshComponent*> Meshes(GetOwner());
	for (USkeletalMeshComponent* Mesh : Meshes)
	{
		if (Mesh != Character->GetMesh() && Mesh->IsVisible() && Mesh->DoesSocketExist(MuzzleBone))
		{
			return Mesh;
		}
	}
	return nullptr;
}

void UImpactWeaponComponent::TickComponent(const float DeltaTime, const ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	const EImpactWeapon Weapon = GetCurrentWeapon();
	if (Weapon != LastWeapon)
	{
		UE_LOG(LogImpact, Display, TEXT("[weapon] t=%.2f arme : %s"), GetWorld()->GetTimeSeconds(), GetImpactWeaponSpec(Weapon).Name);
		LastWeapon = Weapon;
		Bloom = 0.0f;
	}

	const FImpactWeaponSpec& Spec = GetImpactWeaponSpec(Weapon);
	Cooldown = FMath::Max(0.0f, Cooldown - DeltaTime);
	Bloom = FMath::Max(0.0f, Bloom - Spec.BloomRecoveryDegPerSec * DeltaTime);

	if (bTriggerHeld && Spec.bAutomatic && Cooldown <= 0.0f && CanFire())
	{
		FireShot();
	}

	ApplyRecoil(DeltaTime);
	if (Presentation) Presentation->UpdateSights(DeltaTime);
}

void UImpactWeaponComponent::ApplyRecoil(const float DeltaTime)
{
	const APawn* Pawn = Cast<APawn>(GetOwner());
	AController* Controller = Pawn != nullptr ? Pawn->GetController() : nullptr;
	if (Controller == nullptr)
	{
		return;
	}

	// The kick is spread over a few frames so it reads as a push, not a teleport.
	const float Alpha = FMath::Min(1.0f, DeltaTime * 30.0f);
	float Pitch = PendingRecoilPitch * Alpha;
	const float Yaw = PendingRecoilYaw * Alpha;
	PendingRecoilPitch -= Pitch;
	PendingRecoilYaw -= Yaw;
	RecoilDebtPitch += Pitch;

	// Once the trigger is released the view drifts back down by most of the climb.
	if (GetWorld()->GetTimeSeconds() - LastShotTime > 0.15f && RecoilDebtPitch > 0.0f)
	{
		const float Recover = RecoilDebtPitch * FMath::Min(1.0f, DeltaTime * 6.0f) * 0.7f;
		RecoilDebtPitch -= Recover / 0.7f;
		Pitch -= Recover;
	}

	if (!FMath::IsNearlyZero(Pitch) || !FMath::IsNearlyZero(Yaw))
	{
		Controller->SetControlRotation(Controller->GetControlRotation() + FRotator(Pitch, Yaw, 0.0f));
	}
}

void UImpactWeaponComponent::FireShot()
{
	AAlsCharacter* Character = Cast<AAlsCharacter>(GetOwner());
	APlayerController* Controller = Character != nullptr ? Cast<APlayerController>(Character->GetController()) : nullptr;
	UWorld* World = GetWorld();
	if (Controller == nullptr)
	{
		return;
	}

	const EImpactWeapon Weapon = GetCurrentWeapon();
	const FImpactWeaponSpec Spec = GetCurrentSpec();
	Cooldown = Spec.FireInterval;
	if (!CanFire() || !Ammo->ConsumeRound(Weapon)) return;
	++ShotCount;

	FVector ViewLocation;
	FRotator ViewRotation;
	Controller->GetPlayerViewPoint(ViewLocation, ViewRotation);

	const FVector AimDirection = Random.VRandCone(ViewRotation.Vector(), FMath::DegreesToRadians(GetSpreadDegrees()));

	FCollisionQueryParams Params(SCENE_QUERY_STAT(ImpactShot), false, Character);
	Params.bReturnPhysicalMaterial = true;

	// Where the crosshair points. The trace starts level with the character so nothing between camera and player counts.
	const float StartOffset = FMath::Max(0.0f, FVector::DotProduct(Character->GetActorLocation() - ViewLocation, AimDirection));
	const FVector AimStart = ViewLocation + AimDirection * StartOffset;
	const FVector AimEnd = ViewLocation + AimDirection * Spec.Range;
	FHitResult AimHit;
	const FVector Target = World->LineTraceSingleByChannel(AimHit, AimStart, AimEnd, ECC_ImpactBullet, Params) ? AimHit.ImpactPoint : AimEnd;

	// The real bullet leaves the muzzle: an obstacle right in front of the barrel stops it.
	const USkeletalMeshComponent* WeaponMesh = FindWeaponMesh();
	FVector Muzzle = WeaponMesh != nullptr ? WeaponMesh->GetSocketLocation(MuzzleBone) : AimStart;
	if (Presentation)
	{
		Presentation->GetMuzzle(Muzzle);
		Presentation->EjectCase(Weapon, WeaponMesh ? WeaponMesh->GetComponentTransform() : Character->GetActorTransform());
	}
	const FVector Direction = (Target - Muzzle).GetSafeNormal();
	const FVector BulletEnd = Muzzle + Direction * Spec.Range;

	FHitResult Hit;
	bool bHit = World->LineTraceSingleByChannel(Hit, Muzzle, BulletEnd, ECC_ImpactBullet, Params);
	const FVector End = bHit ? Hit.ImpactPoint : BulletEnd;
	FVector BulletDirection = Direction;

	// The camera and the barrel do not look from the same place, and up close the difference is enormous:
	// measured at 31 degrees two metres from the target. Firing straight from the barrel to the point under the
	// crosshair sent the bullet across whatever hung in between, so aiming at a thigh broke the forearm beside
	// it and the shot read as a miss. The barrel trace is kept for what it was meant for, an obstruction in
	// front of the muzzle, which means something ELSE between the gun and the target. When it lands on the same
	// person the player was aiming at, the shot goes where the crosshair was.
	if (bHit && AimHit.bBlockingHit && Hit.GetActor() == AimHit.GetActor() && Hit.BoneName != AimHit.BoneName)
	{
		Hit = AimHit;
		BulletDirection = AimDirection;
	}
	else if (!bHit && AimHit.bBlockingHit)
	{
		// The barrel line missed entirely (it passes beside a thin limb at an angle): the crosshair decides.
		Hit = AimHit;
		BulletDirection = AimDirection;
		bHit = true;
	}

	// Reported from play: at point blank the second leg is missed three times out of five. The crosshair and the
	// barrel do not look from the same place, so what the aim trace found and what the bullet actually hit are
	// logged apart: that is the only way to tell a miss from a hit on the wrong part.
	{
		const AActor* AimActor = AimHit.GetActor();
		const AActor* HitActor2 = Hit.GetActor();
		UE_LOG(LogImpact, Display, TEXT("[tir] visée: %s/%s à %.0f cm | balle: %s/%s à %.0f cm | écart %.1f°"),
		       AimActor != nullptr ? *AimActor->GetName() : TEXT("rien"), *AimHit.BoneName.ToString(),
		       AimHit.bBlockingHit ? FVector::Dist(AimStart, AimHit.ImpactPoint) : 0.0f,
		       HitActor2 != nullptr ? *HitActor2->GetName() : TEXT("rien"), *Hit.BoneName.ToString(),
		       bHit ? FVector::Dist(Muzzle, Hit.ImpactPoint) : 0.0f,
		       FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(AimDirection | Direction, -1.0f, 1.0f))));
	}

	UImpactGameplaySubsystem* Gameplay = World->GetSubsystem<UImpactGameplaySubsystem>();
	AImpactFX* FX = Gameplay != nullptr ? Gameplay->GetFX() : nullptr;
	if (FX != nullptr)
	{
		FX->MuzzleFlash(Muzzle, Direction, Weapon);
		FX->Tracer(Muzzle, End);
	}
	ImpactAudio::PlayGunshot(World, Weapon, Muzzle, true);

	Bloom = FMath::Min(Bloom + Spec.BloomPerShotDeg, Spec.MaxBloomDeg);
	PendingRecoilPitch += Spec.RecoilPitchDeg * Random.FRandRange(0.8f, 1.2f);
	PendingRecoilYaw += Spec.RecoilYawDeg * Random.FRandRange(-1.0f, 1.0f);
	LastShotTime = World->GetTimeSeconds();
	UE_LOG(LogImpact,Verbose,TEXT("[arme-reglages] vitesse=%.0f recul=%.3f/%.3f dispersion=%.3f energie=%.0fJ"),Spec.MuzzleSpeed*.01f,Spec.RecoilPitchDeg,Spec.RecoilYawDeg,Spec.AimSpreadDeg,.5f*Spec.BulletMassKg*FMath::Square(Spec.MuzzleSpeed*.01f));

	if (Gameplay != nullptr)
	{
		Gameplay->ReportGunshot(Character, Muzzle);
	}

	if (!bHit)
	{
		return;
	}

	FImpactBulletHit Bullet;
	Bullet.Weapon = Weapon;
	Bullet.Hit = Hit;
	Bullet.Direction = BulletDirection;
	Bullet.Speed = Spec.MuzzleSpeed;
	Bullet.MassKg = Spec.BulletMassKg;
	Bullet.Instigator = Character;

	AActor* HitActor = Hit.GetActor();
	if (UImpactBodyComponent* Body = HitActor != nullptr ? HitActor->FindComponentByClass<UImpactBodyComponent>() : nullptr)
	{
		LastBodyHitTime = LastShotTime;
		FImpactBulletResult Out = UImpactEquipmentComponent::DeliverBullet(HitActor,Bullet);

		// A bullet that comes out keeps going with what energy it has left: the person behind, or the wall.
		FCollisionQueryParams Through = Params;
		Through.AddIgnoredActor(HitActor);
		for (int32 Crossed = 1; Out.bExited && Out.EnergyOut > 40.0f && Crossed < 3; ++Crossed)
		{
			FHitResult Next;
			const FVector Start = Out.ExitPoint + Direction;
			const FVector Stop = Start + Direction * Spec.Range;
			if (!World->LineTraceSingleByChannel(Next, Start, Stop, ECC_ImpactBullet, Through))
			{
				break;
			}

			AActor* NextActor = Next.GetActor();
			UImpactBodyComponent* NextBody = NextActor != nullptr ? NextActor->FindComponentByClass<UImpactBodyComponent>() : nullptr;
			if (NextBody == nullptr)
			{
				if (FX != nullptr)
				{
					FX->BulletHole(Next);
				}
				ImpactAudio::PlaySurfaceImpact(World, Next.ImpactPoint);
				break;
			}

			// Same bullet, slower: kinetic energy goes with the square of the speed.
			const float FullEnergy = 0.5f * Bullet.MassKg * FMath::Square(Spec.MuzzleSpeed * 0.01f);
			Bullet.Hit = Next;
			Bullet.Speed = Spec.MuzzleSpeed * FMath::Sqrt(Out.EnergyOut / FMath::Max(1.0f, FullEnergy));
			Through.AddIgnoredActor(NextActor);
			Out = UImpactEquipmentComponent::DeliverBullet(NextActor,Bullet);
		}
		return;
	}

	// The player currently has no anatomy component, but wearable protection still resolves incoming traces.
	if(HitActor&&HitActor->FindComponentByClass<UImpactEquipmentComponent>())
	{
		UImpactEquipmentComponent::DeliverBullet(HitActor,Bullet);
		return;
	}

	if (FX != nullptr)
	{
		FX->BulletHole(Hit);
	}
	ImpactAudio::PlaySurfaceImpact(World, Hit.ImpactPoint);

	// Loose objects (boxes, barrels) get the bullet's momentum.
	UPrimitiveComponent* Component = Hit.GetComponent();
	if (Component != nullptr && Component->IsSimulatingPhysics())
	{
		const float Momentum = Spec.BulletMassKg * Spec.MuzzleSpeed; // kg·cm/s
		Component->AddImpulseAtLocation(Direction * Momentum * 3.0f, Hit.ImpactPoint, Hit.BoneName);
	}
}

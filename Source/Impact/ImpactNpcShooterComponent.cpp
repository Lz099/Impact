#include "ImpactNpcShooterComponent.h"

#include "AlsCharacter.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "Impact.h"
#include "ImpactActiveBodyComponent.h"
#include "ImpactAudio.h"
#include "ImpactBodyComponent.h"
#include "ImpactEquipmentComponent.h"
#include "ImpactFX.h"
#include "ImpactGameplaySubsystem.h"
#include "ImpactPlayerHealthComponent.h"
#include "ImpactReactionComponent.h"
#include "ImpactSoldierBrainComponent.h"
#include "ImpactTypes.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Utility/AlsGameplayTags.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(ImpactNpcShooterComponent)

namespace
{
	EImpactWeapon WeaponOf(const AAlsCharacter* Character)
	{
		const FGameplayTag Overlay = Character->GetOverlayMode();
		return Overlay == AlsOverlayModeTags::Rifle ? EImpactWeapon::Rifle
		       : Overlay == AlsOverlayModeTags::PistolOneHanded || Overlay == AlsOverlayModeTags::PistolTwoHanded ? EImpactWeapon::Pistol
		       : EImpactWeapon::None;
	}

	// Where the round leaves: the weapon's muzzle when its mesh has one, otherwise in front of the head.
	FVector FindMuzzle(const AAlsCharacter* Character, const FVector& Towards)
	{
		TInlineComponentArray<USkeletalMeshComponent*> Meshes(Character);
		for (const USkeletalMeshComponent* Mesh : Meshes)
		{
			if (Mesh != Character->GetMesh() && Mesh->IsVisible() && Mesh->DoesSocketExist(TEXT("muzzle")))
			{
				return Mesh->GetSocketLocation(TEXT("muzzle"));
			}
		}
		const FVector Head = Character->GetMesh()->GetSocketLocation(TEXT("head"));
		return Head + (Towards - Head).GetSafeNormal() * 60.0f - FVector(0.0f, 0.0f, 15.0f);
	}

	// Two men at most fire at the player at the same time; the others keep aiming and wait their turn (otherwise five at once
	// kill him in two seconds, measured).
	constexpr int32 MaxShooters = 2;
	TArray<TWeakObjectPtr<const UImpactNpcShooterComponent>> Firing;

	bool TakeTurn(const UImpactNpcShooterComponent* Shooter)
	{
		Firing.RemoveAll([](const TWeakObjectPtr<const UImpactNpcShooterComponent>& Each) { return !Each.IsValid(); });
		if (Firing.Contains(Shooter))
		{
			return true;
		}
		if (Firing.Num() >= MaxShooters)
		{
			return false;
		}
		Firing.Add(Shooter);
		return true;
	}

	void GiveTurn(const UImpactNpcShooterComponent* Shooter)
	{
		Firing.Remove(Shooter);
	}
}

bool UImpactNpcShooterComponent::IsTurnFree(const UImpactNpcShooterComponent* Shooter)
{
	Firing.RemoveAll([](const TWeakObjectPtr<const UImpactNpcShooterComponent>& Each) { return !Each.IsValid(); });
	return Firing.Contains(Shooter) || Firing.Num() < MaxShooters;
}

FVector UImpactNpcShooterComponent::FindMuzzlePoint(const AAlsCharacter* Character, const FVector& Towards)
{
	return FindMuzzle(Character, Towards);
}

float UImpactNpcShooterComponent::GetMagazineFraction() const
{
	const AAlsCharacter* Me = Cast<AAlsCharacter>(GetOwner());
	const float Full = Me != nullptr && WeaponOf(Me) == EImpactWeapon::Rifle ? 30.0f : 15.0f;
	return Magazine < 0 ? 1.0f : Magazine / Full;
}

void UImpactNpcShooterComponent::ReloadNow()
{
	const AAlsCharacter* Me = Cast<AAlsCharacter>(GetOwner());
	Magazine = Me != nullptr && WeaponOf(Me) == EImpactWeapon::Rifle ? 30 : 15;
	Cooldown = FMath::Max(Cooldown, 2.5f);
	ReloadLeft = 2.5f;
	BurstLeft = 0;
	GiveTurn(this);
	UE_LOG(LogImpact, Display, TEXT("[pnj-tir] t=%.2f %s recharge"), GetWorld()->GetTimeSeconds(), *GetOwner()->GetName());
}

UImpactNpcShooterComponent::UImpactNpcShooterComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
}

bool UImpactNpcShooterComponent::IsEnabled()
{
	return UImpactPlayerHealthComponent::IsEnabled();
}

void UImpactNpcShooterComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	GiveTurn(this);
	Super::EndPlay(EndPlayReason);
}

void UImpactNpcShooterComponent::Alert()
{
	if (!bAlerted)
	{
		bAlerted = true;
		Random.Initialize(GetTypeHash(GetOwner()->GetName()));
		UE_LOG(LogImpact, Display, TEXT("[pnj-tir] t=%.2f %s est en alerte"), GetWorld()->GetTimeSeconds(), *GetOwner()->GetName());
	}
}

bool UImpactNpcShooterComponent::CanShoot(const AAlsCharacter* Me, const APawn* Player) const
{
	if (WeaponOf(Me) == EImpactWeapon::None || Player == nullptr)
	{
		return false;
	}
	const UImpactPlayerHealthComponent* PlayerHealth = Player->FindComponentByClass<UImpactPlayerHealthComponent>();
	if (PlayerHealth != nullptr && PlayerHealth->IsDead())
	{
		return false;
	}
	// On his feet and in his senses - or (-ImpactSolArme) lying on the floor with his weapon pointed, the take aiming.
	const UImpactActiveBodyComponent* Active = Me->FindComponentByClass<UImpactActiveBodyComponent>();
	const UImpactReactionComponent* Reaction = Me->FindComponentByClass<UImpactReactionComponent>();
	const bool bGroundAim = Reaction != nullptr && Reaction->IsLyingDown() && Reaction->IsGroundAimReady();
	if (!bGroundAim && ((Active != nullptr && Active->IsDown()) || (Reaction != nullptr && Reaction->IsDown()) || Me->GetLocomotionAction().IsValid()))
	{
		return false;
	}
	const UImpactBodyComponent* Body = Me->FindComponentByClass<UImpactBodyComponent>();
	return Body == nullptr || Body->GetPhysiology().GetConsciousness() == EImpactConsciousness::Conscious;
}

bool UImpactNpcShooterComponent::SeesPlayer(const AAlsCharacter* Me, const APawn* Player) const
{
	const ACharacter* Target = Cast<ACharacter>(Player);
	if (Target == nullptr)
	{
		return false;
	}
	const FVector Eyes = Me->GetMesh()->GetSocketLocation(TEXT("head"));
	FCollisionQueryParams Params(SCENE_QUERY_STAT(ImpactNpcLook), false, Me);
	for (const TCHAR* Bone : {TEXT("head"), TEXT("spine_02")})
	{
		FHitResult Hit;
		const FVector Point = Target->GetMesh()->GetSocketLocation(Bone);
		if (!GetWorld()->LineTraceSingleByChannel(Hit, Eyes, Point, ECC_Visibility, Params) || Hit.GetActor() == Player)
		{
			return true;
		}
	}
	return false;
}

void UImpactNpcShooterComponent::FireAt(AAlsCharacter* Me, APawn* Player)
{
	UWorld* World = GetWorld();
	const ACharacter* Target = Cast<ACharacter>(Player);
	const EImpactWeapon Weapon = WeaponOf(Me);
	const FImpactWeaponSpec& Spec = GetImpactWeaponSpec(Weapon);

	// The chest, give or take: a man aims at the middle of what he sees.
	FVector Aim = Target->GetMesh()->GetSocketLocation(TEXT("spine_02")) + FVector(0.0f, 0.0f, Random.FRandRange(-10.0f, 15.0f));
	// The first round each time he takes the player under fire goes wide: the crack beside him is the warning.
	if (!bWarned)
	{
		bWarned = true;
		const FVector Side = FVector::CrossProduct((Aim - Me->GetActorLocation()).GetSafeNormal(), FVector::UpVector);
		Aim += Side * (Random.FRandRange(70.0f, 110.0f) * (Random.FRand() < 0.5f ? -1.0f : 1.0f)) + FVector(0.0f, 0.0f, Random.FRandRange(-20.0f, 40.0f));
	}
	const FVector Muzzle = FindMuzzle(Me, Aim);
	const float Distance = FVector::Dist(Muzzle, Aim);
	if (UImpactReactionComponent* Reaction = Me->FindComponentByClass<UImpactReactionComponent>(); Reaction != nullptr && Reaction->IsLyingDown())
	{
		// The take fires with him.
		Reaction->NotifyGroundShot();
	}

	// Not a perfect shot: a cone that opens with each round of a burst, with distance, with the player running, with a wounded arm.
	const UImpactBodyComponent* Body = Me->FindComponentByClass<UImpactBodyComponent>();
	const float Arm = Body != nullptr ? FMath::Min(Body->GetPhysiology().GetLimbCapability(EImpactLimb::LeftArm),
	                                               Body->GetPhysiology().GetLimbCapability(EImpactLimb::RightArm)) : 1.0f;
	static const float Skill = [] { float Value = 1.0f; FParse::Value(FCommandLine::Get(), TEXT("ImpactPNJPrecision="), Value); return FMath::Max(0.1f, Value); }();
	const float Moving = FMath::Min(1.0f, Player->GetVelocity().Size2D() / 400.0f);
	// Lying wounded on the floor he aims worse (measured: from the floor at 7 m he killed a player standing still in 5 s).
	const UImpactReactionComponent* Lying = Me->FindComponentByClass<UImpactReactionComponent>();
	const float Floor = Lying != nullptr && Lying->IsLyingDown() ? 2.5f : 0.0f;
	const float Spread = ((Weapon == EImpactWeapon::Rifle ? 3.0f : 4.0f) + BurstIndex * 0.8f + Distance / 2000.0f + Moving * 2.0f + (1.0f - Arm) * 4.0f + Floor) / Skill;
	const FVector Direction = Random.VRandCone((Aim - Muzzle).GetSafeNormal(), FMath::DegreesToRadians(Spread));
	const FVector End = Muzzle + Direction * Spec.Range;

	FCollisionQueryParams Params(SCENE_QUERY_STAT(ImpactNpcShot), false, Me);
	Params.bReturnPhysicalMaterial = true;
	FHitResult Hit;
	// Not through a comrade: one standing in the line from the muzzle to the player is not fired through (measured on the bench,
	// one round in four hit another soldier).
	{
		FHitResult Line;
		if (World->LineTraceSingleByChannel(Line, Muzzle, Aim, ECC_ImpactBullet, Params) && Line.GetActor() != nullptr && Line.GetActor() != Player
		    && Line.GetActor()->FindComponentByClass<UImpactBodyComponent>() != nullptr)
		{
			UE_LOG(LogImpact, Display, TEXT("[pnj-tir] t=%.2f %s ne tire pas : %s dans la ligne"), World->GetTimeSeconds(), *Me->GetName(), *Line.GetActor()->GetName());
			Cooldown = 0.4f;
			return;
		}
	}
	const bool bHit = World->LineTraceSingleByChannel(Hit, Muzzle, End, ECC_ImpactBullet, Params);
	++ShotsFired;

	UImpactGameplaySubsystem* Gameplay = World->GetSubsystem<UImpactGameplaySubsystem>();
	AImpactFX* FX = Gameplay != nullptr ? Gameplay->GetFX() : nullptr;
	if (FX != nullptr)
	{
		FX->MuzzleFlash(Muzzle, Direction, Weapon);
		FX->Tracer(Muzzle, bHit ? Hit.ImpactPoint : End);
	}
	ImpactAudio::PlayGunshot(World, Weapon, Muzzle, false);
	if (Gameplay != nullptr)
	{
		Gameplay->ReportGunshot(Me, Muzzle);
	}

	AActor* HitActor = bHit ? Hit.GetActor() : nullptr;
	FString What = TEXT("rate");
	if (HitActor != nullptr)
	{
		FImpactBulletHit Bullet;
		Bullet.Weapon = Weapon;
		Bullet.Hit = Hit;
		Bullet.Direction = Direction;
		Bullet.Speed = Spec.MuzzleSpeed;
		Bullet.MassKg = Spec.BulletMassKg;
		Bullet.Instigator = Me;
		if (UImpactPlayerHealthComponent* Health = HitActor->FindComponentByClass<UImpactPlayerHealthComponent>())
		{
			UImpactEquipmentComponent* Equipment = HitActor->FindComponentByClass<UImpactEquipmentComponent>();
			Health->OnBullet(Bullet, Equipment != nullptr && Equipment->Intercept(Bullet));
			++PlayerHits;
			What = FString::Printf(TEXT("joueur %s"), *Hit.BoneName.ToString());
		}
		else if (HitActor->FindComponentByClass<UImpactBodyComponent>() != nullptr)
		{
			UImpactEquipmentComponent::DeliverBullet(HitActor, Bullet);
			What = FString::Printf(TEXT("%s %s"), *HitActor->GetName(), *Hit.BoneName.ToString());
		}
		else
		{
			if (FX != nullptr)
			{
				FX->BulletHole(Hit);
			}
			ImpactAudio::PlaySurfaceImpact(World, Hit.ImpactPoint);
			if (UPrimitiveComponent* Component = Hit.GetComponent(); Component != nullptr && Component->IsSimulatingPhysics())
			{
				Component->AddImpulseAtLocation(Direction * Spec.BulletMassKg * Spec.MuzzleSpeed * 3.0f, Hit.ImpactPoint, Hit.BoneName);
			}
			What = TEXT("decor");
		}
	}
	UE_LOG(LogImpact, Display, TEXT("[pnj-tir] t=%.2f %s tire a %.0f m (cone %.1f deg) : %s"), World->GetTimeSeconds(), *Me->GetName(),
	       Distance / 100.0f, Spread, *What);
}

void UImpactNpcShooterComponent::TickComponent(const float DeltaTime, const ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	AAlsCharacter* Me = Cast<AAlsCharacter>(GetOwner());
	APawn* Player = UGameplayStatics::GetPlayerPawn(this, 0);
	if (Me == nullptr || !CanShoot(Me, Player))
	{
		React = -1.0f;
		BurstLeft = 0;
		bWarned = false;
		GiveTurn(this);
		return;
	}
	const EImpactWeapon Weapon = WeaponOf(Me);
	if (Magazine < 0)
	{
		Magazine = Weapon == EImpactWeapon::Rifle ? 30 : 15;
	}
	ReloadLeft = FMath::Max(0.0f, ReloadLeft - DeltaTime);
	// -ImpactPNJCouvert: the soldier's brain says when (behind cover he holds fire, up or out he fires).
	const UImpactSoldierBrainComponent* Soldier = Me->FindComponentByClass<UImpactSoldierBrainComponent>();
	const bool bGate = Soldier == nullptr || Soldier->MayFire();

	// Looking costs a trace: four times a second is enough.
	LookTimer -= DeltaTime;
	if (LookTimer <= 0.0f)
	{
		LookTimer = 0.25f;
		bSees = SeesPlayer(Me, Player);
	}
	if (!bAlerted || !bSees || !bGate)
	{
		GateOpenTime = 0.0f;
		bHeldLogged = false;
		React = -1.0f;
		BurstLeft = 0;
		// The wide first round once per sighting, not once per peek.
		if (!bSees)
		{
			bWarned = false;
		}
		GiveTurn(this);
		return;
	}
	// A moment to react each time he finds the player again (short when he has come up from cover to fire), and the weapon must
	// point near him.
	// Only between bursts, never inside one. React was being drawn again after EVERY round, so each
	// round waited its own 0,38 to 1,8 s of "time to react" on a rifle whose cadence is 0,08 s: the
	// burst did not exist, and five men together put 0,37 rounds a second downrange. That is the main
	// reason the player never died. A man reacts when he finds his target again, not between two
	// rounds of the same burst.
	if (React < 0.0f && BurstLeft <= 0)
	{
		React = Soldier != nullptr ? Soldier->GetPeekReact() : Random.FRandRange(1.0f, 2.0f);
	}
	React -= DeltaTime;
	const FVector ToPlayer = (Player->GetActorLocation() - Me->GetActorLocation()).GetSafeNormal2D();
	// Lying, the body's facing says nothing: the barrel itself.
	const UImpactReactionComponent* Reaction = Me->FindComponentByClass<UImpactReactionComponent>();
	const bool bLying = Reaction != nullptr && Reaction->IsLyingDown();
	// Standing, the barrel too when there is one: an ALS rifleman aiming stands turned from where he aims (measured 40 deg on the
	// soldier bench, and he never fired); the barrel is what has to point.
	bool bFacing = FVector::DotProduct(Me->GetActorForwardVector().GetSafeNormal2D(), ToPlayer) > 0.94f;
	if (bLying)
	{
		bFacing = Reaction->GetGroundAimError() < 20.0f;
	}
	else
	{
		TInlineComponentArray<USkeletalMeshComponent*> Meshes(Me);
		for (const USkeletalMeshComponent* Mesh : Meshes)
		{
			if (Mesh != Me->GetMesh() && Mesh->IsVisible() && Mesh->DoesSocketExist(TEXT("muzzle")))
			{
				const FVector Muzzle = Mesh->GetSocketLocation(TEXT("muzzle"));
				const FVector Barrel = (Muzzle - Mesh->GetComponentLocation()).GetSafeNormal();
				const ACharacter* Target = Cast<ACharacter>(Player);
				const FVector Chest = Target != nullptr ? Target->GetMesh()->GetSocketLocation(TEXT("spine_02")) : Player->GetActorLocation();
				// 25 deg: ALS's aim brings a rifle within 14 to 24 deg of the chest (measured), and the round goes along the line
				// from the muzzle anyway.
				bFacing = FVector::DotProduct(Barrel, (Chest - Muzzle).GetSafeNormal()) > 0.906f;
				break;
			}
		}
	}
	Cooldown -= DeltaTime;
	// Why a soldier allowed to fire holds it for over a second (once per opening).
	GateOpenTime += DeltaTime;
	if (Soldier != nullptr && GateOpenTime > 1.0f && !bHeldLogged && (React > 0.0f || !bFacing || Cooldown > 0.0f))
	{
		bHeldLogged = true;
		float BarrelOff = -1.0f;
		TInlineComponentArray<USkeletalMeshComponent*> Meshes(Me);
		for (const USkeletalMeshComponent* Mesh : Meshes)
		{
			if (Mesh != Me->GetMesh() && Mesh->IsVisible() && Mesh->DoesSocketExist(TEXT("muzzle")))
			{
				const FVector Muzzle = Mesh->GetSocketLocation(TEXT("muzzle"));
				const ACharacter* Target = Cast<ACharacter>(Player);
				const FVector Chest = Target != nullptr ? Target->GetMesh()->GetSocketLocation(TEXT("spine_02")) : Player->GetActorLocation();
				BarrelOff = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct((Muzzle - Mesh->GetComponentLocation()).GetSafeNormal(),
				                                                                               (Chest - Muzzle).GetSafeNormal()), -1.0f, 1.0f)));
				break;
			}
		}
		UE_LOG(LogImpact, Display, TEXT("[pnj-tir] t=%.2f %s retient son tir : reaction %.2f s, canon a %.0f deg du joueur, attente %.2f s"), GetWorld()->GetTimeSeconds(),
		       *Me->GetName(), React, BarrelOff, Cooldown);
	}
	if (React > 0.0f || !bFacing || Cooldown > 0.0f)
	{
		return;
	}

	if (Magazine <= 0)
	{
		// Reloading.
		Magazine = Weapon == EImpactWeapon::Rifle ? 30 : 15;
		Cooldown = 2.5f;
		ReloadLeft = 2.5f;
		BurstLeft = 0;
		GiveTurn(this);
		UE_LOG(LogImpact, Display, TEXT("[pnj-tir] t=%.2f %s recharge"), GetWorld()->GetTimeSeconds(), *Me->GetName());
		return;
	}
	if (BurstLeft <= 0)
	{
		if (!TakeTurn(this))
		{
			Cooldown = 0.3f;
			return;
		}
		BurstLeft = Weapon == EImpactWeapon::Rifle ? Random.RandRange(3, 5) : Random.RandRange(1, 3);
		BurstIndex = 0;
	}
	FireAt(Me, Player);
	--Magazine;
	--BurstLeft;
	++BurstIndex;
	const FImpactWeaponSpec& Spec = GetImpactWeaponSpec(Weapon);
	if (BurstLeft > 0)
	{
		Cooldown = Weapon == EImpactWeapon::Rifle ? Spec.FireInterval : Random.FRandRange(0.35f, 0.6f);
	}
	else
	{
		// Between bursts he gives the turn to another.
		Cooldown = Random.FRandRange(1.2f, 2.2f);
		++BurstsDone;
		GiveTurn(this);
	}
}

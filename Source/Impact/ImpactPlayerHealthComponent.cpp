#include "ImpactPlayerHealthComponent.h"

#include "AlsCharacter.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Impact.h"
#include "ImpactAudio.h"
#include "ImpactTypes.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(ImpactPlayerHealthComponent)

UImpactPlayerHealthComponent::UImpactPlayerHealthComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
}

bool UImpactPlayerHealthComponent::IsEnabled()
{
	static const bool bEnabled = FParse::Param(FCommandLine::Get(), TEXT("ImpactPNJTirent"));
	return bEnabled;
}

void UImpactPlayerHealthComponent::OnBullet(const FImpactBulletHit& Bullet, const bool bStoppedByArmour)
{
	if (IsDead())
	{
		return;
	}
	// Game tuning, not anatomy: the head nearly kills, the trunk a fifth (five rifle rounds), a limb less.
	const FString Bone = Bullet.Hit.BoneName.ToString();
	const bool bHead = Bone == TEXT("head") || Bone.StartsWith(TEXT("neck"));
	const bool bTrunk = Bone.StartsWith(TEXT("spine")) || Bone == TEXT("pelvis") || Bone.StartsWith(TEXT("clavicle"));
	float Damage = bHead ? 70.0f : bTrunk ? 20.0f : 10.0f;
	Damage *= Bullet.Weapon == EImpactWeapon::Pistol ? 0.75f : 1.0f;
	if (bStoppedByArmour)
	{
		Damage = 5.0f;
	}
	Health = FMath::Max(0.0f, Health - Damage);
	SinceHit = 0.0f;
	Flash = 1.0f;
	ImpactAudio::PlayFleshImpact(GetWorld(), Bullet.Hit.ImpactPoint);

	// The view kicks with the hit.
	AAlsCharacter* Character = Cast<AAlsCharacter>(GetOwner());
	APlayerController* Controller = Character != nullptr ? Cast<APlayerController>(Character->GetController()) : nullptr;
	if (Controller != nullptr)
	{
		Controller->AddPitchInput(FMath::FRandRange(-1.2f, -0.4f));
		Controller->AddYawInput(FMath::FRandRange(-0.8f, 0.8f));
	}
	UE_LOG(LogImpact, Display, TEXT("[joueur] t=%.2f touche %s par %s%s : -%.0f, reste %.0f"), GetWorld()->GetTimeSeconds(), *Bone,
	       Bullet.Instigator.IsValid() ? *Bullet.Instigator->GetName() : TEXT("?"), bStoppedByArmour ? TEXT(" (arrete par la protection)") : TEXT(""),
	       Damage, Health);

	if (Health <= 0.0f)
	{
		DeadTime = 0.0f;
		if (Character != nullptr)
		{
			Character->StartRagdolling();
		}
		if (Controller != nullptr)
		{
			Controller->SetIgnoreMoveInput(true);
			Controller->SetIgnoreLookInput(true);
		}
		UE_LOG(LogImpact, Display, TEXT("[joueur] t=%.2f mort"), GetWorld()->GetTimeSeconds());
	}
}

void UImpactPlayerHealthComponent::TickComponent(const float DeltaTime, const ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	Flash = FMath::Max(0.0f, Flash - DeltaTime * 2.5f);
	if (IsDead())
	{
		// The level again from the start, everyone back in place, after a few seconds on the floor.
		DeadTime += DeltaTime;
		if (DeadTime > 5.0f && !bRestarting)
		{
			bRestarting = true;
			UGameplayStatics::OpenLevel(this, FName(*UWorld::RemovePIEPrefix(GetWorld()->GetOutermost()->GetName())));
		}
		return;
	}
	SinceHit += DeltaTime;
	if (SinceHit > 4.0f)
	{
		Health = FMath::Min(100.0f, Health + 20.0f * DeltaTime);
	}
}

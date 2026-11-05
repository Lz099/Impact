#include "ImpactFX.h"

#include "Misc/CommandLine.h"

#include "Components/DecalComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "Impact.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(ImpactFX)

namespace
{
	constexpr int32 FlashPool = 4;
	constexpr int32 TracerPool = 24;
	constexpr int32 DropCapacity = 512;
	constexpr float FlashLife = 0.06f;
	constexpr float TracerSpeed = 45000.0f; // cm/s, slower than a real bullet so the eye can catch it
	constexpr float TracerLength = 220.0f;
	float BloodScale = 1.0f;

	const TCHAR* EmissivePath = TEXT("/Game/Impact/Materials/M_Impact_Emissive.M_Impact_Emissive");
	const TCHAR* BloodPath = TEXT("/Game/Impact/Materials/M_Impact_Blood.M_Impact_Blood");
	const TCHAR* DecalPath = TEXT("/Game/Impact/Materials/M_Impact_DecalSplat.M_Impact_DecalSplat");
	// Real blood is almost black. On screen, at the distance you actually stand from someone, that reads as
	// nothing at all: reported from play as "no blood comes out". This is dark red that can still be seen.
	const FLinearColor BloodColor(0.30f, 0.015f, 0.010f);
}

AImpactFX::AImpactFX()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PostPhysics;

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	UStaticMesh* Sphere = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	UStaticMesh* Cylinder = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));

	for (int32 i = 0; i < FlashPool; ++i)
	{
		UPointLightComponent* Light = CreateDefaultSubobject<UPointLightComponent>(*FString::Printf(TEXT("FlashLight%d"), i));
		Light->SetupAttachment(Root);
		Light->SetMobility(EComponentMobility::Movable);
		Light->SetVisibility(false);
		Light->SetCastShadows(false);
		Light->SetIntensityUnits(ELightUnits::Candelas);
		Light->SetAttenuationRadius(260.0f);
		Light->SetLightColor(FLinearColor(1.0f, 0.62f, 0.3f));
		FlashLights.Add(Light);

		UStaticMeshComponent* Mesh = CreateDefaultSubobject<UStaticMeshComponent>(*FString::Printf(TEXT("FlashMesh%d"), i));
		Mesh->SetupAttachment(Root);
		Mesh->SetStaticMesh(Sphere);
		Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Mesh->SetCastShadow(false);
		Mesh->SetVisibility(false);
		FlashMeshes.Add(Mesh);
	}

	for (int32 i = 0; i < TracerPool; ++i)
	{
		UStaticMeshComponent* Mesh = CreateDefaultSubobject<UStaticMeshComponent>(*FString::Printf(TEXT("Tracer%d"), i));
		Mesh->SetupAttachment(Root);
		Mesh->SetStaticMesh(Cylinder);
		Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Mesh->SetCastShadow(false);
		Mesh->SetVisibility(false);
		TracerMeshes.Add(Mesh);
	}

	Drops = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("Drops"));
	Drops->SetupAttachment(Root);
	Drops->SetStaticMesh(Sphere);
	Drops->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Drops->SetCastShadow(false);
	Drops->SetMobility(EComponentMobility::Movable);
}

UMaterialInterface* AImpactFX::LoadMaterial(const TCHAR* Path) const
{
	UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, Path);
	if (Material == nullptr)
	{
		UE_LOG(LogImpact, Warning, TEXT("[fx] missing material %s (run Tools/make_assets.py)"), Path);
	}
	return Material;
}

int32 AImpactFX::TakeSlot(TArray<float>& SlotTimes, const float Now)
{
	// The oldest slot is reused when all are busy.
	int32 Best = 0;
	for (int32 i = 1; i < SlotTimes.Num(); ++i)
	{
		if (SlotTimes[i] < SlotTimes[Best])
		{
			Best = i;
		}
	}
	SlotTimes[Best] = Now;
	return Best;
}

void AImpactFX::MuzzleFlash(const FVector& Location, const FVector& Direction, const EImpactWeapon Weapon)
{
	if (FlashSlotTimes.IsEmpty())
	{
		FlashSlotTimes.Init(-1.0f, FlashPool);
		if (UMaterialInterface* Emissive = LoadMaterial(EmissivePath))
		{
			for (UStaticMeshComponent* Mesh : FlashMeshes)
			{
				UMaterialInstanceDynamic* Instance = UMaterialInstanceDynamic::Create(Emissive, this);
				Instance->SetVectorParameterValue(TEXT("Color"), FLinearColor(1.0f, 0.55f, 0.18f));
				Instance->SetScalarParameterValue(TEXT("Intensity"), 60.0f);
				Mesh->SetMaterial(0, Instance);
			}
		}
	}

	const int32 Slot = TakeSlot(FlashSlotTimes, GetWorld()->GetTimeSeconds());
	const float Size = Weapon == EImpactWeapon::Rifle ? 1.0f : 0.7f;
	const float Peak = Weapon == EImpactWeapon::Rifle ? 25.0f : 16.0f;

	UPointLightComponent* Light = FlashLights[Slot];
	Light->SetWorldLocation(Location + Direction * 10.0f);
	Light->SetIntensity(Peak);
	Light->SetVisibility(true);

	UStaticMeshComponent* Mesh = FlashMeshes[Slot];
	Mesh->SetWorldLocationAndRotation(Location + Direction * 8.0f * Size, Direction.Rotation());
	Mesh->SetWorldScale3D(FVector(0.22f, 0.07f, 0.07f) * Size);
	Mesh->SetVisibility(true);

	Flashes.RemoveAll([Slot](const FFlash& Flash) { return Flash.Slot == Slot; });
	Flashes.Add({Slot, 0.0f, Peak});
}

void AImpactFX::Tracer(const FVector& From, const FVector& To)
{
	if (TracerSlotTimes.IsEmpty())
	{
		TracerSlotTimes.Init(-1.0f, TracerPool);
		if (UMaterialInterface* Emissive = LoadMaterial(EmissivePath))
		{
			for (UStaticMeshComponent* Mesh : TracerMeshes)
			{
				UMaterialInstanceDynamic* Instance = UMaterialInstanceDynamic::Create(Emissive, this);
				Instance->SetVectorParameterValue(TEXT("Color"), FLinearColor(1.0f, 0.75f, 0.4f));
				Instance->SetScalarParameterValue(TEXT("Intensity"), 25.0f);
				Mesh->SetMaterial(0, Instance);
			}
		}
	}

	const float Length = FVector::Distance(From, To);
	if (Length < 50.0f)
	{
		return;
	}

	const int32 Slot = TakeSlot(TracerSlotTimes, GetWorld()->GetTimeSeconds());
	Tracers.RemoveAll([Slot](const FTracer& Tracer) { return Tracer.Slot == Slot; });

	FTracer Tracer;
	Tracer.Slot = Slot;
	Tracer.From = From;
	Tracer.Direction = (To - From) / Length;
	Tracer.Length = Length;
	Tracers.Add(Tracer);
}

void AImpactFX::BulletHole(const FHitResult& Hit)
{
	if (DecalMaterial == nullptr)
	{
		DecalMaterial = LoadMaterial(DecalPath);
	}
	if (DecalMaterial == nullptr)
	{
		return;
	}

	const FRotator Rotation = FRotationMatrix::MakeFromX(-Hit.ImpactNormal).Rotator() + FRotator(0.0f, 0.0f, FMath::FRandRange(0.0f, 360.0f));
	UDecalComponent* Decal = UGameplayStatics::SpawnDecalAtLocation(this, DecalMaterial, FVector(4.0f, 2.2f, 2.2f), Hit.ImpactPoint, Rotation, 40.0f);
	if (Decal != nullptr)
	{
		UMaterialInstanceDynamic* Instance = Decal->CreateDynamicMaterialInstance();
		Instance->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.012f, 0.011f, 0.01f));
		Instance->SetScalarParameterValue(TEXT("Roughness"), 0.9f);
		Instance->SetScalarParameterValue(TEXT("NoiseAmount"), 0.25f);
		Decal->SetFadeOut(30.0f, 10.0f, false);
		Decal->SetSortOrder(1);
	}
}

void AImpactFX::BloodSpray(const FVector& Location, const FVector& Direction, const float Amount, const float Speed)
{
	if (!bDropMaterialSet)
	{
		bDropMaterialSet = true;
		if (UMaterialInterface* Blood = LoadMaterial(BloodPath))
		{
			UMaterialInstanceDynamic* Instance = UMaterialInstanceDynamic::Create(Blood, this);
			Instance->SetVectorParameterValue(TEXT("Color"), BloodColor);
			Drops->SetMaterial(0, Instance);
		}
	}

	const int32 Count = FMath::Clamp(FMath::RoundToInt(10.0f * Amount), 2, 60);
	for (int32 i = 0; i < Count && DropStates.Num() < DropCapacity; ++i)
	{
		FDrop Drop;
		Drop.Location = Location;
		Drop.Velocity = FMath::VRandCone(Direction, FMath::DegreesToRadians(28.0f)) * Speed * FMath::FRandRange(0.35f, 1.0f);
		Drop.Size = FMath::FRandRange(0.6f, 1.6f);
		DropStates.Add(Drop);
	}
}

void AImpactFX::BloodDrop(const FVector& Location, const FVector& Velocity, const float Size)
{
	if (!bDropMaterialSet)
	{
		bDropMaterialSet = true;
		float Parsed = 1.0f;
		BloodScale = FParse::Value(FCommandLine::Get(), TEXT("ImpactBlood="), Parsed) ? FMath::Clamp(Parsed, 0.2f, 5.0f) : 1.0f;
		if (UMaterialInterface* Blood = LoadMaterial(BloodPath))
		{
			Drops->SetMaterial(0, Blood);
		}
	}
	if (DropStates.Num() < DropCapacity)
	{
		FDrop Drop;
		Drop.Location = Location;
		Drop.Velocity = Velocity;
		Drop.Size = Size;
		DropStates.Add(Drop);
	}
}

UDecalComponent* AImpactFX::BloodSplat(const FVector& Location, const FVector& Normal, const float Size, const bool bPersistent)
{
	if (DecalMaterial == nullptr)
	{
		DecalMaterial = LoadMaterial(DecalPath);
	}
	if (DecalMaterial == nullptr)
	{
		return nullptr;
	}

	const FRotator Rotation = FRotationMatrix::MakeFromX(-Normal).Rotator() + FRotator(0.0f, 0.0f, FMath::FRandRange(0.0f, 360.0f));
	UDecalComponent* Decal = UGameplayStatics::SpawnDecalAtLocation(this, DecalMaterial, FVector(6.0f, Size, Size), Location, Rotation,
	                                                                bPersistent ? 0.0f : 120.0f);
	if (Decal != nullptr)
	{
		UMaterialInstanceDynamic* Instance = Decal->CreateDynamicMaterialInstance();
		Instance->SetVectorParameterValue(TEXT("Color"), BloodColor);
		Instance->SetScalarParameterValue(TEXT("Roughness"), 0.15f);
		Instance->SetScalarParameterValue(TEXT("NoiseAmount"), bPersistent ? 0.45f : 0.8f);
		if (!bPersistent)
		{
			Decal->SetFadeOut(100.0f, 20.0f, false);
		}
		Decal->SetSortOrder(bPersistent ? 1 : 2 + SplatCount++ % 50);
	}
	return Decal;
}

UDecalComponent* AImpactFX::BloodWound(USkeletalMeshComponent* Mesh, const FName Bone, const FVector& Location, const FVector& Normal, const float Size)
{
	if (DecalMaterial == nullptr)
	{
		DecalMaterial = LoadMaterial(DecalPath);
	}
	if (Mesh == nullptr || DecalMaterial == nullptr)
	{
		return nullptr;
	}
	UDecalComponent* Decal = NewObject<UDecalComponent>(Mesh->GetOwner(), NAME_None, RF_Transient);
	Mesh->GetOwner()->AddInstanceComponent(Decal);
	Decal->SetupAttachment(Mesh, Bone);
	Decal->DecalSize = FVector(8.0f, Size, Size);
	Decal->SetDecalMaterial(DecalMaterial);
	Decal->SetFadeScreenSize(0.001f);
	Decal->SetSortOrder(60);
	Decal->RegisterComponent();
	Decal->SetWorldLocationAndRotation(Location, FRotationMatrix::MakeFromX(-Normal).Rotator());
	UMaterialInstanceDynamic* Instance = Decal->CreateDynamicMaterialInstance();
	Instance->SetVectorParameterValue(TEXT("Color"), BloodColor * 0.7f);
	Instance->SetScalarParameterValue(TEXT("Roughness"), 0.2f);
	Instance->SetScalarParameterValue(TEXT("NoiseAmount"), 0.55f);
	return Decal;
}

void AImpactFX::Tick(const float DeltaTime)
{
	Super::Tick(DeltaTime);

	// Muzzle flashes: bright for one or two frames, then gone.
	for (int32 i = Flashes.Num() - 1; i >= 0; --i)
	{
		FFlash& Flash = Flashes[i];
		Flash.Age += DeltaTime;
		const float Fade = 1.0f - Flash.Age / FlashLife;
		if (Fade <= 0.0f)
		{
			FlashLights[Flash.Slot]->SetVisibility(false);
			FlashMeshes[Flash.Slot]->SetVisibility(false);
			Flashes.RemoveAt(i);
			continue;
		}
		FlashLights[Flash.Slot]->SetIntensity(Flash.Peak * Fade);
		if (Flash.Age > 0.03f)
		{
			FlashMeshes[Flash.Slot]->SetVisibility(false);
		}
	}

	// Tracers: a short bright segment travelling from the muzzle to the hit.
	for (int32 i = Tracers.Num() - 1; i >= 0; --i)
	{
		FTracer& Tracer = Tracers[i];
		Tracer.Travelled += TracerSpeed * DeltaTime;

		const float Head = FMath::Min(Tracer.Travelled, Tracer.Length);
		const float Tail = FMath::Max(0.0f, Tracer.Travelled - TracerLength);
		UStaticMeshComponent* Mesh = TracerMeshes[Tracer.Slot];

		if (Tail >= Tracer.Length)
		{
			Mesh->SetVisibility(false);
			Tracers.RemoveAt(i);
			continue;
		}

		const float Segment = FMath::Max(1.0f, Head - Tail);
		const FVector Center = Tracer.From + Tracer.Direction * (Tail + Segment * 0.5f);
		Mesh->SetWorldLocationAndRotation(Center, FRotationMatrix::MakeFromZ(Tracer.Direction).Rotator());
		Mesh->SetWorldScale3D(FVector(0.012f, 0.012f, Segment / 100.0f));
		Mesh->SetVisibility(true);
	}

	// Blood droplets: ballistic flight, a splat where they land.
	if (DropStates.IsEmpty() && Drops->GetInstanceCount() == 0)
	{
		return;
	}

	UWorld* World = GetWorld();
	const FCollisionQueryParams Params(SCENE_QUERY_STAT(ImpactDrop), false);

	for (int32 i = DropStates.Num() - 1; i >= 0; --i)
	{
		FDrop& Drop = DropStates[i];
		Drop.Age += DeltaTime;
		Drop.Velocity.Z -= 980.0f * DeltaTime;
		Drop.Velocity *= FMath::Max(0.0f, 1.0f - 0.6f * DeltaTime);
		const FVector Next = Drop.Location + Drop.Velocity * DeltaTime;

		FHitResult Hit;
		if (World->LineTraceSingleByChannel(Hit, Drop.Location, Next, ECC_Visibility, Params)
		    && (Hit.GetActor() == nullptr || !Hit.GetActor()->IsA<APawn>()))
		{
			BloodSplat(Hit.ImpactPoint, Hit.ImpactNormal, FMath::FRandRange(7.0f, 17.0f) * Drop.Size * BloodScale);
			DropStates.RemoveAtSwap(i);
			continue;
		}

		Drop.Location = Next;
		if (Drop.Age > 4.0f)
		{
			DropStates.RemoveAtSwap(i);
		}
	}

	TArray<FTransform> Transforms;
	Transforms.Reserve(DropStates.Num());
	for (const FDrop& Drop : DropStates)
	{
		// A one-centimetre drop is true to life and invisible past two metres. These are gobs, not droplets.
		const float Stretch = FMath::Clamp(Drop.Velocity.Size() / 350.0f, 1.0f, 4.0f);
		const FVector Scale(0.024f * Drop.Size * Stretch * BloodScale, 0.021f * Drop.Size * BloodScale, 0.021f * Drop.Size * BloodScale);
		Transforms.Add(FTransform(Drop.Velocity.Rotation(), Drop.Location, Scale));
	}

	if (Drops->GetInstanceCount() != Transforms.Num())
	{
		Drops->ClearInstances();
		if (!Transforms.IsEmpty())
		{
			Drops->AddInstances(Transforms, false, true);
		}
	}
	else if (!Transforms.IsEmpty())
	{
		Drops->BatchUpdateInstancesTransforms(0, Transforms, true, true, true);
	}
}

#include "ImpactBodyComponent.h"

#include "Misc/CommandLine.h"

#include "AlsCharacter.h"
#include "Components/BoxComponent.h"
#include "Components/DecalComponent.h"
#include "Engine/CollisionProfile.h"
#include "Components/SkeletalMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "HAL/IConsoleManager.h"
#include "Impact.h"
#include "ImpactAudio.h"
#include "ImpactCivilianController.h"
#include "ImpactFX.h"
#include "ImpactGameplaySubsystem.h"
#include "ImpactReactionComponent.h"
#include "ImpactSoldierBrainComponent.h"
#include "ImpactActiveBodyComponent.h"
#include "ImpactNpcShooterComponent.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "RHIShaderPlatform.h"
#include "Utility/AlsGameplayTags.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(ImpactBodyComponent)

namespace
{
	TAutoConsoleVariable<float> CVarWoundSpeed(
		TEXT("impact.WoundSpeed"), 1.0f,
		TEXT("How fast wounds act: blood loss and lack of oxygen (1 = real time, 2 = twice as fast)."));

	const TCHAR* ConsciousnessName(const EImpactConsciousness State)
	{
		switch (State)
		{
		case EImpactConsciousness::Conscious: return TEXT("conscient");
		case EImpactConsciousness::Confused: return TEXT("confus");
		case EImpactConsciousness::SemiConscious: return TEXT("semi-conscient");
		case EImpactConsciousness::Unconscious: return TEXT("inconscient");
		default: return TEXT("mort");
		}
	}
}

UImpactBodyComponent::UImpactBodyComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PostPhysics;
}

void UImpactBodyComponent::BeginPlay()
{
	Super::BeginPlay();

	if (UImpactGameplaySubsystem* Gameplay = GetWorld()->GetSubsystem<UImpactGameplaySubsystem>())
	{
		Gameplay->RegisterBody(this);
	}

	if (DisplayName.IsEmpty())
	{
		DisplayName = GetOwner()->GetName();
	}

	if (const ACharacter* Character = Cast<ACharacter>(GetOwner()))
	{
		Anatomy.Build(Character->GetMesh());
	}

	// Loaded now, so its shaders are ready before the first wound instead of the body being drawn with the
	// engine's default checkerboard while they compile.
	static const bool bSkinBlood = FParse::Param(FCommandLine::Get(), TEXT("ImpactSangPeau"));
	if (bSkinBlood)
	{
		LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Impact/Materials/M_Impact_BloodSkin.M_Impact_BloodSkin"));
	}
}

void UImpactBodyComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (SkinBloodHandle.IsValid())
	{
		if (const ACharacter* Character = Cast<ACharacter>(GetOwner()))
		{
			Character->GetMesh()->UnregisterOnBoneTransformsFinalizedDelegate(SkinBloodHandle);
			Character->GetMesh()->SetOverlayMaterial(nullptr);
		}
		SkinBloodHandle.Reset();
	}
	Super::EndPlay(EndPlayReason);
}

void UImpactBodyComponent::TrySetupSkinBlood(const float DeltaTime)
{
	// Blood on the skin. A bullet hole bleeds onto the skin round it, and the stain grows with what comes out
	// (FImpactWound::Spread, grown in FImpactPhysiology::Tick). Asked for in play and never wired: the material and
	// the field both existed, nothing joined them. Drawn as the mesh's overlay: a translucent copy of the body laid
	// over it, transparent everywhere except within each wound's stain.
	static const bool bSkinBlood = FParse::Param(FCommandLine::Get(), TEXT("ImpactSangPeau"));
	if (!bSkinBlood || SkinBlood != nullptr || bSkinBloodFailed)
	{
		return;
	}
	bool bAnyStain = false;
	for (const FImpactWound& Wound : Physiology.GetWounds())
	{
		bAnyStain |= Wound.bExternal && Wound.Spread > 0.0f;
	}
	const ACharacter* Character = Cast<ACharacter>(GetOwner());
	if (!bAnyStain || Character == nullptr)
	{
		return;
	}

	UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Impact/Materials/M_Impact_BloodSkin.M_Impact_BloodSkin"));
	UMaterial* Base = Material != nullptr ? Material->GetMaterial() : nullptr;
	if (Base == nullptr)
	{
		UE_LOG(LogImpact, Warning, TEXT("[sang-peau] M_Impact_BloodSkin introuvable"));
		bSkinBloodFailed = true;
		return;
	}
	// A layer over a skinned mesh needs the material to say it may be used on one; in -game the engine will not
	// add that by itself, it drops the layer with a warning.
	if (!Base->GetUsageByFlag(MATUSAGE_SkeletalMesh))
	{
		UE_LOG(LogImpact, Warning, TEXT("[sang-peau] M_Impact_BloodSkin n'est pas marque pour les maillages squelettiques : relancer Tools/make_blood_mat.py"));
		bSkinBloodFailed = true;
		return;
	}
	// Still compiling, or broken: laid over the body now, it would cover the whole man in the default checkerboard.
	if (Base->IsCompilingOrHadCompileError(GMaxRHIShaderPlatform))
	{
		SkinBloodWait += DeltaTime;
		if (SkinBloodWait > 30.0f)
		{
			UE_LOG(LogImpact, Warning, TEXT("[sang-peau] M_Impact_BloodSkin ne compile pas : abandon"));
			bSkinBloodFailed = true;
		}
		return;
	}

	SkinBlood = UMaterialInstanceDynamic::Create(Material, this);
	// The colour of the drops and pools (ImpactFX.cpp), darkened as for the wound decals: blood on skin is thin.
	SkinBlood->SetVectorParameterValue(TEXT("Couleur"), FLinearColor(0.30f, 0.015f, 0.010f) * 0.7f);
	for (int32 Slot = 0; Slot < 8; ++Slot)
	{
		SkinBlood->SetVectorParameterValue(FName(*FString::Printf(TEXT("Plaie%d"), Slot)), FLinearColor(0.0f, 0.0f, -100000.0f, 0.0f));
	}
	USkeletalMeshComponent* Mesh = Character->GetMesh();
	Mesh->SetOverlayMaterial(SkinBlood);
	// Placed on the pose that is drawn, once the bones are final for the frame - in the tick it would trail a frame
	// behind a body that moves, and the stain would slide on the skin.
	SkinBloodHandle = Mesh->RegisterOnBoneTransformsFinalizedDelegate(
		FOnBoneTransformsFinalizedMultiCast::FDelegate::CreateUObject(this, &UImpactBodyComponent::UpdateSkinBlood));
	UpdateSkinBlood();
	UE_LOG(LogImpact, Display, TEXT("[sang-peau] %s : actif"), *DisplayName);
}

void UImpactBodyComponent::UpdateSkinBlood()
{
	if (SkinBlood == nullptr)
	{
		return;
	}

	// One stain per place on the skin: the wounds of one hole (an organ behind it bleeds out through the same
	// hole) share it, and their areas add up.
	struct FStain
	{
		const FImpactWound* First = nullptr;
		float Area = 0.0f;
	};
	TArray<FStain> Stains;
	for (const FImpactWound& Wound : Physiology.GetWounds())
	{
		if (!Wound.bExternal || Wound.Spread <= 0.0f)
		{
			continue;
		}
		FStain* Same = Stains.FindByPredicate([&Wound](const FStain& Stain)
		{
			return Stain.First->Bone == Wound.Bone && FVector::DistSquared(Stain.First->LocalPoint, Wound.LocalPoint) < 9.0f;
		});
		if (Same == nullptr)
		{
			Same = &Stains.AddDefaulted_GetRef();
			Same->First = &Wound;
		}
		Same->Area += FMath::Square(Wound.Spread);
	}
	Stains.Sort([](const FStain& A, const FStain& B) { return A.Area > B.Area; });

	// The material has eight: a body with more holes than that shows its eight largest.
	const int32 Used = FMath::Min(8, Stains.Num());
	SkinBloodSummary.Reset();
	for (int32 Slot = 0; Slot < Used; ++Slot)
	{
		const FVector Centre = GetWoundLocation(*Stains[Slot].First);
		const float Radius = FMath::Sqrt(Stains[Slot].Area);
		SkinBlood->SetVectorParameterValue(FName(*FString::Printf(TEXT("Plaie%d"), Slot)), FLinearColor(Centre.X, Centre.Y, Centre.Z, Radius));
		SkinBloodSummary += FString::Printf(TEXT("%s%s r=%.1f cm"), Slot > 0 ? TEXT(", ") : TEXT(""), *Stains[Slot].First->Bone.ToString(), Radius);
	}
	for (int32 Slot = Used; Slot < SkinBloodSlotsUsed; ++Slot)
	{
		SkinBlood->SetVectorParameterValue(FName(*FString::Printf(TEXT("Plaie%d"), Slot)), FLinearColor(0.0f, 0.0f, -100000.0f, 0.0f));
	}
	SkinBloodSlotsUsed = Used;
	if (Stains.Num() > Used)
	{
		SkinBloodSummary += FString::Printf(TEXT(", %d de plus non montrees"), Stains.Num() - Used);
	}
}

FString UImpactBodyComponent::DescribeBodyPart(const FName BoneName, const bool bFromBehind)
{
	const FString Bone = BoneName.ToString();
	const FString Side = Bone.EndsWith(TEXT("_l")) ? TEXT(" gauche") : Bone.EndsWith(TEXT("_r")) ? TEXT(" droit") : TEXT("");
	const FString SideF = Bone.EndsWith(TEXT("_l")) ? TEXT(" gauche") : Bone.EndsWith(TEXT("_r")) ? TEXT(" droite") : TEXT("");

	if (Bone == TEXT("head")) return TEXT("tête");
	if (Bone.StartsWith(TEXT("neck"))) return TEXT("cou");
	if (Bone == TEXT("spine_03")) return bFromBehind ? TEXT("haut du dos") : TEXT("poitrine");
	if (Bone == TEXT("spine_02")) return bFromBehind ? TEXT("milieu du dos") : TEXT("bas de la poitrine");
	if (Bone == TEXT("spine_01")) return bFromBehind ? TEXT("bas du dos") : TEXT("ventre");
	if (Bone == TEXT("pelvis")) return bFromBehind ? TEXT("fesses") : TEXT("bassin");
	if (Bone.StartsWith(TEXT("clavicle"))) return TEXT("épaule") + SideF;
	if (Bone.StartsWith(TEXT("upperarm"))) return TEXT("bras") + Side;
	if (Bone.StartsWith(TEXT("lowerarm"))) return TEXT("avant-bras") + Side;
	if (Bone.StartsWith(TEXT("hand"))) return TEXT("main") + SideF;
	if (Bone.StartsWith(TEXT("thigh"))) return TEXT("cuisse") + SideF;
	if (Bone.StartsWith(TEXT("calf"))) return TEXT("tibia") + Side;
	if (Bone.StartsWith(TEXT("foot")) || Bone.StartsWith(TEXT("ball"))) return TEXT("pied") + Side;
	return Bone;
}

bool UImpactBodyComponent::UpdateWeaponHold()
{
	// Asked for in play: "le npc vise le joueur avec l'arme tant qu'il le peut, s'il est touche il utilise l'autre bras, et si c'est
	// une blessure grave et une arme a deux mains il lache l'arme pour prendre la blessure a deux mains".
	AAlsCharacter* Als = Cast<AAlsCharacter>(GetOwner());
	if (Als == nullptr)
	{
		return false;
	}
	const FGameplayTag Overlay = Als->GetOverlayMode();
	const bool bRifle = Overlay == AlsOverlayModeTags::Rifle;
	const bool bTwoHandPistol = Overlay == AlsOverlayModeTags::PistolTwoHanded;
	const bool bOneHandPistol = Overlay == AlsOverlayModeTags::PistolOneHanded;
	if (!bRifle && !bTwoHandPistol && !bOneHandPistol)
	{
		return false;
	}
	const int32 Worst = Physiology.GetWorstExternalWound();
	// -ImpactFusilLache. Reported from play: "ceux qui ont des fusils, ca sert a rien qu'il garde le fusil en main quand il tombe ou
	// qu'il est blesse, c'est juste bugge, ils peuvent le lacher". A rifle takes both hands: with one on a wound, or with the man on
	// the floor, it hung off the other one pointing anywhere. So a rifle goes at the first wound or the fall. A pistol stays ("dans
	// un pistolet je comprends").
	static const bool bRifleDrops = FParse::Param(FCommandLine::Get(), TEXT("ImpactFusilLache"));
	// -ImpactSolArme: a soldier keeps it to fire from the floor ("meme a terre, tant qu'ils ont une arme, ils visent"); a wound to the
	// arm that holds it still takes it from him (below).
	static const bool bGroundArmed = FParse::Param(FCommandLine::Get(), TEXT("ImpactSolArme"));
	const bool bSoldierKeeps = bGroundArmed && Als->FindComponentByClass<UImpactSoldierBrainComponent>() != nullptr;
	if (bRifleDrops && bRifle && !bSoldierKeeps)
	{
		const UImpactReactionComponent* React = Als->FindComponentByClass<UImpactReactionComponent>();
		if (React != nullptr && React->IsDown())
		{
			DropWeapon(TEXT("fusil, il tombe"));
			return false;
		}
		if (Worst != INDEX_NONE)
		{
			DropWeapon(TEXT("fusil, il est blesse"));
			return false;
		}
	}
	// A hand that has stopped working lets go. Below this line nothing is evaluated any more once a man
	// is past Confused - the early exit was written to stop re-deciding for someone beyond caring - and
	// the effect was a limp body, and a dead one, holding its rifle perfectly in the palm for ever, in
	// whatever pose it fell. -ImpactSolArme does not cover this: a soldier keeps his weapon to fire
	// from the floor, and an unconscious man fires nothing.
	if (Physiology.GetConsciousness() >= EImpactConsciousness::Unconscious)
	{
		DropWeapon(TEXT("il ne tient plus rien"));
		return false;
	}
	if (Worst == INDEX_NONE || Physiology.GetConsciousness() > EImpactConsciousness::Confused)
	{
		return true;
	}
	const FString Bone = Physiology.GetWounds()[Worst].Bone.ToString();
	const bool bTrunk = Bone.StartsWith(TEXT("spine")) || Bone == TEXT("pelvis");
	const bool bWeaponArm = Bone.EndsWith(TEXT("_r")) && (Bone.StartsWith(TEXT("upperarm")) || Bone.StartsWith(TEXT("lowerarm"))
	                                                       || Bone.StartsWith(TEXT("hand")) || Bone.StartsWith(TEXT("clavicle")));
	// Grave: weak enough that holding the hole shut matters more than holding a gun - a fifth of the blood gone, or the head going.
	// A hole in the trunk alone was the first rule, and it made him drop the rifle on the first round. Reported from play: "il
	// ressaye meme pas de garder l'arme". Now he keeps it in his right hand, holds the wound with his left, and fights on until the
	// bleeding takes that from him.
	const bool bGrave = Physiology.GetBloodLossFraction() > 0.2f || Physiology.GetConsciousness() == EImpactConsciousness::Confused;
	(void)bTrunk;
	if (bWeaponArm)
	{
		DropWeapon(TEXT("le bras qui tient l'arme est touche"));
		return false;
	}
	// A soldier fights on with it (-ImpactSolArme): only the arm that holds it takes it from him (above).
	if (bGrave && (bRifle || bTwoHandPistol) && !bSoldierKeeps)
	{
		DropWeapon(TEXT("blessure grave, arme a deux mains"));
		return false;
	}
	if (bTwoHandPistol)
	{
		// Touched but not badly: the pistol goes to one hand, the other one to the wound.
		Als->SetOverlayMode(AlsOverlayModeTags::PistolOneHanded);
		if (UImpactReactionComponent* React = Als->FindComponentByClass<UImpactReactionComponent>())
		{
			React->HoldWeaponAgain();
		}
		UE_LOG(LogImpact, Display, TEXT("[arme] t=%.2f %s : pistolet dans une main, l'autre sur la plaie"), GetWorld()->GetTimeSeconds(), *DisplayName);
	}
	return true;
}

void UImpactBodyComponent::DropWeapon(const TCHAR* Why)
{
	AAlsCharacter* Als = Cast<AAlsCharacter>(GetOwner());
	if (Als == nullptr)
	{
		return;
	}
	// The weapon ALS shows is a skeletal mesh component of the character other than the body. It leaves the hand as a loose object:
	// a box the size of the weapon falls with physics, and the weapon is drawn on it. The mesh asset has no physics of its own.
	const auto Spawn = [this, Als](USkeletalMesh* Asset, const FTransform& At)
	{
		const FBoxSphereBounds Bounds = Asset->GetBounds();
		AActor* Dropped = GetWorld()->SpawnActor<AActor>(AActor::StaticClass(), At);
		if (Dropped == nullptr)
		{
			return false;
		}
		UBoxComponent* Box = NewObject<UBoxComponent>(Dropped, TEXT("Arme"));
		Box->SetBoxExtent(Bounds.BoxExtent.ComponentMax(FVector(2.0f)));
		Dropped->SetRootComponent(Box);
		Dropped->Tags.Add(TEXT("ImpactArmeLachee"));
		Box->RegisterComponent();
		Box->SetWorldLocationAndRotation(At.TransformPosition(Bounds.Origin), At.GetRotation());
		Box->SetCollisionProfileName(UCollisionProfile::PhysicsActor_ProfileName);
		// -ImpactMortTraversable: on the floor, a dropped weapon is not kicked about by whoever walks there, does not catch in the
		// bodies, and does not push the camera ("desactive les collisions avec eux et leurs armes").
		static const bool bPassThrough = FParse::Param(FCommandLine::Get(), TEXT("ImpactMortTraversable"));
		if (bPassThrough)
		{
			Box->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
			Box->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
			// A ragdoll is a PhysicsBody, not a Pawn (ALS changes it when the ragdoll starts): without this the rifle dropped as he
			// falls is born inside his own arm and chest and is thrown out of them.
			Box->SetCollisionResponseToChannel(ECC_PhysicsBody, ECR_Ignore);
		}
		Box->SetMassOverrideInKg(NAME_None, 3.0f, true);
		Box->SetSimulatePhysics(true);
		USkeletalMeshComponent* Shown = NewObject<USkeletalMeshComponent>(Dropped, TEXT("Visuel"));
		Shown->SetSkeletalMesh(Asset);
		Shown->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Shown->SetupAttachment(Box);
		Shown->RegisterComponent();
		Shown->SetWorldTransform(At);
		Box->SetPhysicsLinearVelocity(Als->GetVelocity() * 0.5f + Als->GetActorForwardVector() * 60.0f);
		// It stays where it fell ("elle doit tomber par terre et y rester"): no life span - it used to go after three minutes.
		return true;
	};

	// The weapon as ALS draws it. In a ragdoll ALS has already taken it off its mesh: the asset is gone and the weapon is no longer
	// drawn. Measured on 30 October: none of the five men who died in a ragdoll dropped anything, and the four whose meshes were
	// logged all had an empty weapon mesh when they let go - so the weapon vanished ("ils lachent leur arme et elle s'evanouit").
	// Then the weapon of his overlay mode is dropped from his gun hand.
	bool bDropped = false;
	TArray<USkeletalMeshComponent*> Meshes;
	Als->GetComponents(Meshes);
	for (const USkeletalMeshComponent* Held : Meshes)
	{
		if (Held != Als->GetMesh() && Held->GetSkeletalMeshAsset() != nullptr && Held->IsVisible())
		{
			bDropped = Spawn(Held->GetSkeletalMeshAsset(), Held->GetComponentTransform());
			break;
		}
	}
	const FGameplayTag Overlay = Als->GetOverlayMode();
	if (!bDropped && (Overlay == AlsOverlayModeTags::Rifle || Overlay == AlsOverlayModeTags::PistolOneHanded || Overlay == AlsOverlayModeTags::PistolTwoHanded))
	{
		USkeletalMesh* Asset = LoadObject<USkeletalMesh>(nullptr, Overlay == AlsOverlayModeTags::Rifle
			                                                          ? TEXT("/ALS/ALS/OverlayObjects/Rifle/SKM_Als_Rifle.SKM_Als_Rifle")
			                                                          : TEXT("/ALS/ALS/OverlayObjects/Pistol/SKM_Als_Pistol.SKM_Als_Pistol"));
		if (Asset != nullptr)
		{
			bDropped = Spawn(Asset, Als->GetMesh()->GetSocketTransform(TEXT("hand_r"), RTS_World));
			UE_LOG(LogImpact, Display, TEXT("[arme-lachee] %s : arme effacee par ALS dans le ragdoll, %s lache depuis la main"), *DisplayName, *Asset->GetName());
		}
	}
	Als->SetOverlayMode(AlsOverlayModeTags::Default);
	Als->SetDesiredAiming(false);
	UE_LOG(LogImpact, Display, TEXT("[arme] t=%.2f %s lache son arme (%s)"), GetWorld()->GetTimeSeconds(), *DisplayName, Why);
}

FVector UImpactBodyComponent::GetWoundLocation(const FImpactWound& Wound) const
{
	const ACharacter* Character = Cast<ACharacter>(GetOwner());
	return Character->GetMesh()->GetSocketTransform(Wound.Bone, RTS_World).TransformPosition(Wound.LocalPoint);
}

bool UImpactBodyComponent::IsStanceLeg(const EImpactLimb Leg) const
{
	const ACharacter* Character = Cast<ACharacter>(GetOwner());
	const USkeletalMeshComponent* Mesh = Character->GetMesh();
	const float Left = Mesh->GetSocketLocation(TEXT("foot_l")).Z;
	const float Right = Mesh->GetSocketLocation(TEXT("foot_r")).Z;
	if (FMath::Abs(Left - Right) < 4.0f || Character->GetVelocity().Size2D() < 60.0f)
	{
		return true; // standing on both feet: both carry weight
	}
	return Leg == EImpactLimb::LeftLeg ? Left < Right : Right < Left;
}

FImpactBulletResult UImpactBodyComponent::ReceiveBullet(const FImpactBulletHit& Bullet)
{
	FImpactBulletResult Result;
	UWorld* World = GetWorld();
	ACharacter* Character = Cast<ACharacter>(GetOwner());
	USkeletalMeshComponent* Mesh = Character->GetMesh();
	const FImpactWeaponSpec& Spec = GetImpactWeaponSpec(Bullet.Weapon);

	FImpactBulletPath Path;
	if (Anatomy.IsBuilt())
	{
		Path = Anatomy.TraceBullet(Mesh, Bullet.Hit.ImpactPoint, Bullet.Direction, Bullet.Hit.BoneName, Bullet.GetEnergy());
	}
	else
	{
		Path.Entry = Path.Exit = Bullet.Hit.ImpactPoint;
		Path.Direction = Bullet.Direction;
		Path.EntryBone = Path.ExitBone = Bullet.Hit.BoneName;
		Path.EnergyIn = Bullet.GetEnergy();
	}

	{
		const FTransform BoneTransform = Mesh->GetSocketTransform(Path.EntryBone, RTS_World);
		FPathRecord& Record = RecentPaths.AddDefaulted_GetRef();
		Record.Bone = Path.EntryBone;
		Record.Entry = BoneTransform.InverseTransformPosition(Path.Entry);
		Record.Exit = BoneTransform.InverseTransformPosition(Path.Exit);
		Record.Before = BoneTransform.InverseTransformPosition(Path.Entry - Path.Direction * 25.0f);
		Record.bExited = Path.bExited;
		for (const FImpactPathHit& Hit : Path.Hits)
		{
			Record.Hits.Add(BoneTransform.InverseTransformPosition(Hit.Point));
		}
	}
	if (RecentPaths.Num() > 12)
	{
		RecentPaths.RemoveAt(0);
	}

	const bool bWasDead = Physiology.IsDead();
	const float LegsBefore[2] = {Physiology.GetLimbCapability(EImpactLimb::LeftLeg), Physiology.GetLimbCapability(EImpactLimb::RightLeg)};
	Physiology.TimeScale = CVarWoundSpeed.GetValueOnGameThread();
	const FString Summary = Physiology.ApplyBullet(Path, Anatomy, Mesh);

	const bool bFromBehind = FVector::DotProduct(Character->GetActorForwardVector(), Bullet.Direction) > 0.0f;
	LastEvent = FString::Printf(TEXT("%s : %s"), *DescribeBodyPart(Bullet.Hit.BoneName, bFromBehind), *Summary);

	UE_LOG(LogImpact, Display, TEXT("[impact] t=%.2f %s, %s : %s : %s. %s (%.0f J -> %.0f J). %s"), World->GetTimeSeconds(), *DisplayName, Spec.Name,
	       *DescribeBodyPart(Bullet.Hit.BoneName, bFromBehind), *Summary, Path.bExited ? TEXT("Ressort") : TEXT("Reste dans le corps"),
	       Path.EnergyIn, Path.EnergyOut, *Physiology.Describe());

	// Blood: a little back towards the shooter, most of it out of the exit hole.
	if (UImpactGameplaySubsystem* Gameplay = World->GetSubsystem<UImpactGameplaySubsystem>())
	{
		if (AImpactFX* FX = Gameplay->GetFX())
		{
			const float Amount = FMath::Clamp((Path.EnergyIn - Path.EnergyOut) / 500.0f, 0.5f, 2.5f);
			FX->BloodSpray(Path.Entry - Bullet.Direction * 2.0f, -Bullet.Direction, Amount * 0.4f, 200.0f);
			if (Path.bExited)
			{
				FX->BloodSpray(Path.Exit + Bullet.Direction * 2.0f, Bullet.Direction, Amount * 1.5f, 450.0f + Path.EnergyOut * 0.2f);
			}
		}
	}
	ImpactAudio::PlayFleshImpact(World, Path.Entry);

	UImpactReactionComponent* Reaction = GetOwner()->FindComponentByClass<UImpactReactionComponent>();
	if (Reaction != nullptr)
	{
		// The push follows the energy the bullet left in the body.
		const float Typical = Bullet.Weapon == EImpactWeapon::Rifle ? 900.0f : 450.0f;
		const float Strength = FMath::Clamp((Path.EnergyIn - Path.EnergyOut) / Typical, 0.5f, 1.5f);
		Reaction->ApplyBulletHit(Bullet, Strength);

		if (!bWasDead && Physiology.IsDead())
		{
			Reaction->SetMayGetUp(false);
			Reaction->Collapse(Bullet.Direction * 200.0f, true, *Physiology.GetCauseOfDeath());
		}

		// A leg that suddenly cannot carry: if the weight is on it, the body drops to that side.
		for (int32 Leg = 0; Leg < 2; ++Leg)
		{
			const EImpactLimb Limb = Leg == 0 ? EImpactLimb::LeftLeg : EImpactLimb::RightLeg;
			if (!Physiology.IsDead() && LegsBefore[Leg] >= 0.25f && Physiology.GetLimbCapability(Limb) < 0.25f && IsStanceLeg(Limb))
			{
				const FVector Right = Character->GetActorRightVector();
				const FVector Side = Limb == EImpactLimb::LeftLeg ? -Right : Right;
				Reaction->Collapse(Side * 180.0f + Bullet.Direction * 120.0f, false, TEXT("la jambe lâche"));
			}
		}
	}

	// -ImpactCorpsActif: the always-physical body takes the round instead.
	if (UImpactActiveBodyComponent* Active = GetOwner()->FindComponentByClass<UImpactActiveBodyComponent>())
	{
		const float Typical = Bullet.Weapon == EImpactWeapon::Rifle ? 900.0f : 450.0f;
		Active->OnBullet(Bullet, FMath::Clamp((Path.EnergyIn - Path.EnergyOut) / Typical, 0.5f, 1.5f));
	}

	if (AImpactCivilianController* Brain = Cast<AImpactCivilianController>(Character->GetController()))
	{
		Brain->Hurt(Bullet.Instigator.Get());
	}
	if (UImpactNpcShooterComponent* Shooter = GetOwner()->FindComponentByClass<UImpactNpcShooterComponent>())
	{
		Shooter->Alert();
	}

	UpdateBody(true);

	Result.bExited = Path.bExited;
	Result.ExitPoint = Path.Exit;
	Result.EnergyOut = Path.EnergyOut;
	return Result;
}

void UImpactBodyComponent::UpdateBody(const bool bNewInjury)
{
	UImpactReactionComponent* Reaction = GetOwner()->FindComponentByClass<UImpactReactionComponent>();
	if (Reaction == nullptr)
	{
		// -ImpactCorpsActif: no reaction component here, only the rule on weapons: a man on the floor lets go of a rifle, which
		// takes both hands; a pistol stays in his hand.
		const UImpactActiveBodyComponent* Active = GetOwner()->FindComponentByClass<UImpactActiveBodyComponent>();
		const AAlsCharacter* Als = Cast<AAlsCharacter>(GetOwner());
		if (Active != nullptr && Active->IsDown() && Als != nullptr && Als->GetOverlayMode() == AlsOverlayModeTags::Rifle)
		{
			DropWeapon(TEXT("fusil, il tombe"));
		}
		return;
	}

	const EImpactConsciousness State = Physiology.GetConsciousness();
	if (State != LastConsciousness)
	{
		UE_LOG(LogImpact, Display, TEXT("[body] t=%.2f %s : %s (%s)"), GetWorld()->GetTimeSeconds(), *DisplayName, ConsciousnessName(State),
		       *Physiology.Describe());
		LastConsciousness = State;
	}

	Reaction->SetControlState(State);
	// Capability and exhaustion are updated before choosing any action, even while on the ground.
	// A hurt arm is not a dead arm. This used to be a cliff: at or above 0.4 of capability the arm worked
	// normally, and one point below it dropped to 0.05, which is no muscle at all - the arm left the body
	// and floated. Reported from play: "on tire dans le bras et directement son bras commence a flotter,
	// ils perdent leur bras super vite".
	//
	// What a shot arm does is stay against the body and stop being useful. It keeps enough tone to carry
	// its own weight and follow the trunk; what it loses is the strength to reach, hold and aim, and that
	// is a separate test (CanUse needs 0.4 of capability). Only a destroyed one hangs.
	//
	// The square root keeps tone in the middle of the range where a damaged arm lives: a first bullet
	// leaves 0.79 of the muscle, a second 0.63, a broken bone 0.37, nothing at all 0.08.
	const auto ArmTone = [](const float Capability)
	{
		return FMath::Clamp(0.08f + 0.92f * FMath::Sqrt(FMath::Clamp(Capability, 0.0f, 1.0f)), 0.08f, 1.0f);
	};
	const bool bLeftArmOk = Physiology.GetLimbCapability(EImpactLimb::LeftArm) >= 0.4f;
	const bool bRightArmOk = Physiology.GetLimbCapability(EImpactLimb::RightArm) >= 0.4f;
	Reaction->SetLimbTone(TEXT("clavicle_l"), ArmTone(Physiology.GetLimbCapability(EImpactLimb::LeftArm)));
	Reaction->SetLimbTone(TEXT("clavicle_r"), ArmTone(Physiology.GetLimbCapability(EImpactLimb::RightArm)));
	const float Spent = FMath::Clamp(Physiology.GetBloodLossFraction() / 0.4f, 0.0f, 1.0f) * 0.6f
	                    + FMath::Clamp(1.0f - Physiology.GetBrainOxygen(), 0.0f, 1.0f) * 0.4f;
	Reaction->SetWeakness(Spent);
	// -ImpactVentreCourbe: a hole in the belly (the lower spine bone or the pelvis), the more holes the deeper he folds.
	int32 GutHoles = 0;
	for (const FImpactWound& Wound : Physiology.GetWounds())
	{
		GutHoles += Wound.bExternal && (Wound.Bone == FName(TEXT("spine_01")) || Wound.Bone == FName(TEXT("pelvis"))) ? 1 : 0;
	}
	Reaction->SetGutWound(GutHoles > 0 ? 0.6f + 0.2f * (GutHoles - 1) : 0.0f);

	switch (State)
	{
	case EImpactConsciousness::Dead:
	case EImpactConsciousness::Unconscious:
		// Everything voluntary stops. Crawling is a voluntary act and was being left running: reported from
		// play as a body marked dead that went on dragging itself along in a loop for ever.
		Reaction->SetLimp(-1, 0.0f);
		Reaction->SetCrawl(FVector::ZeroVector, false, false);
		Reaction->SetMayGetUp(false);
		for (int32 Side = 0; Side < 2; ++Side)
		{
			Reaction->SetHandTarget(Side, FVector::ZeroVector, FVector::ForwardVector, false);
		}
		for (FImpactWound& Wound : Physiology.GetWounds())
		{
			Wound.bPressed = false;
		}
		if (Cast<AAlsCharacter>(GetOwner())->GetLocomotionAction() != AlsLocomotionActionTags::Ragdolling)
		{
			Reaction->Collapse(FVector::ZeroVector, true, State == EImpactConsciousness::Dead ? TEXT("mort") : TEXT("perd connaissance"));
		}
		UpdateHands();
		return;
	case EImpactConsciousness::SemiConscious:
		// Barely there: no crawling either, only the hand that is already on a wound may stay.
		Reaction->SetCrawl(FVector::ZeroVector, false, false);
		Reaction->SetMayGetUp(false);
		if (!Reaction->IsDown())
		{
			Reaction->Collapse(FVector::ZeroVector, false, TEXT("s'effondre"));
		}
		UpdateHands();
		return;
	default:
		break;
	}

	const float Left = Physiology.GetLimbCapability(EImpactLimb::LeftLeg);
	const float Right = Physiology.GetLimbCapability(EImpactLimb::RightLeg);
	// ALS has no one-legged standing/get-up gait. An unusable support cannot be hidden by a normal get-up.
	const bool bCanStand = Left >= 0.25f && Right >= 0.25f;
	// -ImpactResteAuSol: a hole in the belly, the chest or the pelvis, or blood enough gone, and he does not get back up - he stays
	// on the floor holding it. Reported from play: "ils jouent l'anim de mort puis ils remarchent", "ils se relevent normal".
	// Measured on one minute of play: men with a fractured pelvis and up to four holes in the trunk back on their feet within
	// four seconds of the fall, and walking.
	static const bool bStayDown = FParse::Param(FCommandLine::Get(), TEXT("ImpactResteAuSol"));
	bool bSerious = Physiology.GetBloodLossFraction() > 0.15f;
	for (const FImpactWound& Wound : Physiology.GetWounds())
	{
		const FString Bone = Wound.Bone.ToString();
		bSerious |= Bone.StartsWith(TEXT("spine")) || Bone == TEXT("pelvis");
	}
	Reaction->SetMayGetUp(bCanStand && State == EImpactConsciousness::Conscious && Spent < 0.65f && !(bStayDown && bSerious));
	if (!bCanStand)
	{
		Reaction->SetMayGetUp(false);
		if (!Reaction->IsDown())
		{
			Reaction->Collapse(FVector::ZeroVector, false,
			                   Physiology.GetSpinalLesion() != EImpactSpineLevel::None ? TEXT("jambes paralysées") : TEXT("ne tient plus debout"));
		}
	}

	// Legs gone but awake and an arm still works: drag yourself along the ground instead of lying there.
	// The direction is the one the chest already points in, so the body does not have to turn to start.
	if (!bCanStand && Reaction->IsDown() && (bLeftArmOk || bRightArmOk))
	{
		const USkeletalMeshComponent* Mesh = Cast<ACharacter>(GetOwner())->GetMesh();
		FVector Ahead = (Mesh->GetSocketLocation(TEXT("spine_03")) - Mesh->GetSocketLocation(TEXT("pelvis"))).GetSafeNormal2D();
		if (Ahead.IsNearlyZero())
		{
			Ahead = GetOwner()->GetActorForwardVector().GetSafeNormal2D();
		}
		const bool bStarting = !Reaction->IsCrawling();
		Reaction->SetCrawl(Ahead, bLeftArmOk, bRightArmOk);
		if (bStarting)
		{
			UE_LOG(LogImpact, Display, TEXT("[body] t=%.2f %s : rampe sur %s"), GetWorld()->GetTimeSeconds(), *DisplayName,
			       bLeftArmOk && bRightArmOk ? TEXT("les deux bras") : bLeftArmOk ? TEXT("le bras gauche") : TEXT("le bras droit"));
		}
	}
	else if (Reaction->IsCrawling())
	{
		Reaction->SetCrawl(FVector::ZeroVector, false, false);
	}

	// Awake, on the ground, hurting and not busy dragging himself: he writhes. This is the behaviour the
	// physics could never produce on its own — reported from play as "ils ne se roulent pas, leur corps bouge
	// tout seul, ce n'est pas lui qui se bouge".
	const bool bHurts = Physiology.GetWorstExternalWound() != INDEX_NONE || Physiology.GetBloodLossFraction() > 0.08f
	                    || !bCanStand;
	// -ImpactSolReArme: a confused man writhes too. Confusion is what makes a second fall likely (it halves the
	// balance), and it was also what forbade the posture: the only second fall on record, at t=95 in the log of
	// 20 October 23:13, was a confused man who then lay there as a plain ragdoll. He still has voluntary control
	// (the reaction component gives it down to Confused); only this test said no.
	static const bool bReArm = FParse::Param(FCommandLine::Get(), TEXT("ImpactSolReArme"));
	const bool bAwakeEnough = bReArm ? State <= EImpactConsciousness::Confused : State == EImpactConsciousness::Conscious;
	Reaction->SetWrithing(Reaction->IsDown() && !Reaction->IsCrawling() && bHurts && bAwakeEnough);

	// How much push the legs can still absorb with steps - and how much of a body is left above them to
	// keep over those legs. Taking only the legs into account meant a man could be shot twenty-one times
	// through the chest and still have the balance of a healthy one, measured on -ImpactTest=criblage:
	// "equilibre 140 cm" on every round from the first to the last.
	const float Trunk = Physiology.GetTrunkTrauma();
	float Balance = FMath::Min(Left, Right) < 0.25f ? 35.0f : 140.0f * FMath::Min(Left, Right);
	// Half, and the figure is swept rather than guessed. Pistol rounds into the chest of a man running
	// away, front on, three runs per setting, counting the rounds before he goes down:
	//
	//   0,20 -> 30          0,50 -> 20 / 20 / 22        0,70 -> 7 / 2 / 2
	//   0,35 -> 23          0,60 -> 7 / 3 / 20          0,75 -> 2
	//
	// 0,6 sits exactly on the threshold where one unlucky round topples him, which is why it swings from
	// three to twenty; 0,7 and above drop him in two, which is not what the reference footage shows - a
	// man there absorbs a score of rounds and stays up. 0,5 is stable and keeps him standing about as long
	// as he used to.
	//
	// What changed is not how long he lasts but how soon he stops being dangerous: his speed now collapses
	// from 650 to 124 cm/s by the third round instead of the tenth, and his balance falls from 90 to 73 cm
	// where it used to sit at its maximum of 140 through twenty-one rounds. That is the part that was
	// reported: "faut pas tirer 90 balles sur un mec pour le voir reagir".
	static float TrunkWeight = -1.0f;
	if (TrunkWeight < 0.0f)
	{
		float Parsed = 0.5f;
		TrunkWeight = FParse::Value(FCommandLine::Get(), TEXT("ImpactTorsePoids="), Parsed) ? FMath::Clamp(Parsed, 0.0f, 1.0f) : 0.5f;
	}
	Balance *= 1.0f - TrunkWeight * Trunk;
	if (State == EImpactConsciousness::Confused)
	{
		Balance *= 0.5f;
	}
	else if (Physiology.GetBloodLossFraction() > 0.15f)
	{
		Balance *= 0.8f;
	}
	Reaction->SetBalanceCapacity(Balance);

	// What he has left to spend on moving. A man who cannot breathe does not sprint, whatever his legs
	// could still do; the flight controller used to ask for a sprint unconditionally, with a comment
	// claiming ALS would hold it down to "the injury limit" - a limit which was never written.
	Effort = FMath::Clamp((1.0f - Trunk) * (1.0f - 0.7f * Spent) * FMath::Min(1.0f, (Left + Right) * 0.6f), 0.0f, 1.0f);

	// A hurt leg that still carries: limp on it.
	// A leg that is ruined but not both still limps, as hard as the model allows: the old lower bound of 0.25
	// meant a broken femur fell outside the range and the character walked on it as if nothing had happened.
	const float Weakest = FMath::Min(Left, Right);
	if (bCanStand && Weakest < 0.75f)
	{
		// A bullet through a thigh makes someone limp badly, not imperceptibly. Muscle damage puts the leg at
		// 0.55, which the previous mapping turned into 0.7 of the limp: measured on the bench, 2.3 cm of
		// pelvis asymmetry and 49 % of the weight still going through the hurt leg, which is a man walking
		// almost normally. Reported from play: "il essaye meme pas de marcher en trainant la jambe". A leg
		// that has lost nearly half of what it could carry limps as hard as this model goes.
		Reaction->SetLimp(Left < Right ? 0 : 1, FMath::Clamp((1.0f - Weakest) / 0.45f, 0.35f, 1.0f));
	}
	else
	{
		Reaction->SetLimp(-1, 0.0f);
	}

	UpdateHands();
}

void UImpactBodyComponent::UpdateHands()
{
	UImpactReactionComponent* Reaction = GetOwner()->FindComponentByClass<UImpactReactionComponent>();
	const ACharacter* Character = Cast<ACharacter>(GetOwner());
	TArray<FImpactWound>& Wounds = Physiology.GetWounds();
	if (Reaction == nullptr || Character == nullptr)
	{
		return;
	}
	const USkeletalMeshComponent* Mesh = Character->GetMesh();
	const bool bWeaponInRightHand = UpdateWeaponHold();

	// An arm that is dragging the body along cannot also be pressed on a wound.
	if (!Reaction->CanHoldWound())
	{
		for (int32 Side = 0; Side < 2; ++Side)
		{
			Reaction->SetHandTarget(Side, FVector::ZeroVector, FVector::ForwardVector, false);
		}
		for (FImpactWound& Wound : Wounds)
		{
			Wound.bPressed = false;
		}
		if (LastHandSide != INDEX_NONE)
		{
			LastHandSide = INDEX_NONE;
			HandSide = INDEX_NONE;
			UE_LOG(LogImpact, Display, TEXT("[hand] t=%.2f %s : geste interrompu (chute, rampement ou conscience)"), GetWorld()->GetTimeSeconds(), *DisplayName);
		}
		return;
	}

	// How far this body can put a hand: its own arm, plus a little for the shoulder and the lean.
	const FVector ShoulderBones[2] = {Mesh->GetSocketLocation(TEXT("upperarm_l")), Mesh->GetSocketLocation(TEXT("upperarm_r"))};
	const float ArmLength = FVector::Dist(ShoulderBones[1], Mesh->GetSocketLocation(TEXT("lowerarm_r")))
	                        + FVector::Dist(Mesh->GetSocketLocation(TEXT("lowerarm_r")), Mesh->GetSocketLocation(TEXT("hand_r")));
	const float Reach = ArmLength + 22.0f;

	// Which wound the hand holds. Not simply the one bleeding most: a bullet makes a bigger hole where it comes
	// out than where it goes in, so "most blood" always picked the exit. Shot from the front, that hole is in the
	// back, and the arm reached behind the shoulder, failed, and gave up with the hands empty. Someone holds the
	// wound they can feel and reach. Bleeding still decides between two wounds that are equally reachable.
	const float Now = GetWorld()->GetTimeSeconds();
	WoundBlockedUntil.SetNumZeroed(Wounds.Num(), EAllowShrinking::No);
	const auto Score = [&](const int32 Index)
	{
		if (!Wounds.IsValidIndex(Index) || !Wounds[Index].bExternal || Wounds[Index].Bleed <= 0.2f
		    || WoundBlockedUntil[Index] > Now)
		{
			return 0.0f;
		}
		const FVector Point = GetWoundLocation(Wounds[Index]);
		const float Nearest = FMath::Min(FVector::Dist(Point, ShoulderBones[0]), FVector::Dist(Point, ShoulderBones[1]));
		float Factor = Nearest < Reach ? 1.0f : 0.1f;

		// Behind the shoulder line, a hand cannot get there in any way that looks like a person doing it.
		// This used to be a preference and not a rule - the back scored 0,4 against the front's 1,0 - so a
		// hole that bled more still won, and then the arm went round or through the body to reach it.
		// Reported from play, with a photograph: "je tire de devant sur le cote et les mains font des
		// mouvements impossibles pour aller dans le dos ou passer par le dos".
		const FVector Outwards = (Point - Character->GetActorLocation()).GetSafeNormal2D();
		const float Facing = Outwards.IsNearlyZero() ? 1.0f : (Outwards | Character->GetActorForwardVector());
		if (Facing < -0.25f)
		{
			return 0.0f;
		}
		Factor *= FMath::Lerp(0.4f, 1.0f, FMath::Clamp(Facing * 0.5f + 0.5f, 0.0f, 1.0f));
		return Wounds[Index].Bleed * Factor;
	};

	const auto WorthHolding = [&Wounds](const int32 Index)
	{
		return Wounds.IsValidIndex(Index) && Wounds[Index].bExternal && Wounds[Index].Bleed > 0.2f;
	};

	int32 Held = WorthHolding(HandWound) ? HandWound : INDEX_NONE;
	const float HeldScore = Held != INDEX_NONE ? Score(Held) : 0.0f;
	int32 Best = INDEX_NONE;
	float BestScore = 0.0f;
	for (int32 i = 0; i < Wounds.Num(); ++i)
	{
		const float Value = Score(i);
		if (Value > BestScore)
		{
			BestScore = Value;
			Best = i;
		}
	}
	// A wound that has just been taken keeps the hand for a moment, and one actually being held keeps it until
	// it stops mattering. Seen in play: someone with three wounds swapped the hand between them and let go
	// twice a second, because the scores of two equally bad wounds cross every time the body moves.
	const bool bHolding = HandSide != INDEX_NONE && Reaction->IsHandPressing(HandSide);
	const bool bFresh = Held != INDEX_NONE && Now - HandWoundSince < 1.5f;
	if (Held == INDEX_NONE || (!bHolding && !bFresh && Best != INDEX_NONE && Best != Held && BestScore > HeldScore * 1.3f))
	{
		Held = Best;
	}
	if (Held != HandWound)
	{
		HandWound = Held;
		HandWoundSince = Now;
		HandFailedSides = 0;
		HandSide = INDEX_NONE;
	}

	// An arm that pulled towards the wound and never got near it cannot reach that place on this body.
	if (HandSide != INDEX_NONE && !Reaction->IsHandReachable(HandSide))
	{
		HandFailedSides |= 1 << HandSide;
		UE_LOG(LogImpact, Display, TEXT("[hand] t=%.2f %s : la main %s n'atteint pas %s"), GetWorld()->GetTimeSeconds(), *DisplayName,
		       HandSide == 0 ? TEXT("gauche") : TEXT("droite"), Held != INDEX_NONE ? *Wounds[Held].Name : TEXT("la plaie"));
		HandSide = INDEX_NONE;

		// Both arms have now tried this one. Leave it be for a while: without this the next frame scores it top
		// again, picks it again, fails again, and the hands flap between two wounds several times a second.
		if (HandFailedSides == 3 && Wounds.IsValidIndex(Held))
		{
			WoundBlockedUntil.SetNumZeroed(Wounds.Num(), EAllowShrinking::No);
			WoundBlockedUntil[Held] = GetWorld()->GetTimeSeconds() + 6.0f;
			HandFailedSides = 0;
			HandWound = INDEX_NONE;
			UE_LOG(LogImpact, Display, TEXT("[hand] t=%.2f %s : renonce à %s"), GetWorld()->GetTimeSeconds(), *DisplayName, *Wounds[Held].Name);
		}
	}

	int32 BestSide = INDEX_NONE;
	int32 OtherSide = INDEX_NONE;
	FVector Target = FVector::ZeroVector;
	FVector Outward = FVector::ForwardVector;
	if (Held != INDEX_NONE && Physiology.GetConsciousness() <= EImpactConsciousness::Confused)
	{
		// Press on the wound from outside: a few centimetres off the surface, away from the bone it is on.
		const FImpactWound& Wound = Wounds[Held];
		const FVector Point = GetWoundLocation(Wound);
		Outward = Point - Character->GetMesh()->GetSocketLocation(Wound.Bone);
		if (Outward.SizeSquared() < 4.0f)
		{
			Outward = Point - Character->GetActorLocation();
		}
		Outward = Outward.GetSafeNormal();

		// On an arm the other hand grabs the front of it, not the outer side facing away.
		const FString WoundPart = Wound.Bone.ToString();
		if (WoundPart.StartsWith(TEXT("upperarm")) || WoundPart.StartsWith(TEXT("lowerarm")) || WoundPart.StartsWith(TEXT("clavicle")))
		{
			Outward = Character->GetActorForwardVector();
		}
		// How far off the collision shape the hand sits. The old value was three centimetres, chosen on
		// the assumption written here before - that the physics shape may be LARGER than the drawn body,
		// so resting on it would keep the hand clear of the skin. Measured, it is the other way round:
		// a wounded man's holding hand sat at 0 cm from the trunk's shape, pressed flat against it, and
		// the collision capsules are thinner than the body that is drawn over them. A hand on the capsule
		// is therefore a hand inside the chest. Reported from play: "il rentre dans son corp on les vois
		// sortnir par le dos".
		//
		// Nine centimetres: the hand's own shape is about five across, and the rest is the gap between a
		// capsule and the skin stretched over it. That second part is a guess - it has not been measured,
		// only inferred from a hand at zero reading as a hand inside the body - so it is a knob.
		static float Margin = -1.0f;
		if (Margin < 0.0f)
		{
			float Parsed = 9.0f;
			Margin = FParse::Value(FCommandLine::Get(), TEXT("ImpactMainMarge="), Parsed) ? FMath::Max(0.0f, Parsed) : 9.0f;
		}
		Target = Point + Outward * Margin;

		// Rest it on that shape's real surface, which is not where the wound point is: a bullet hole is
		// recorded on the bone, and the shape around that bone can be wide.
		if (const FBodyInstance* Part = Character->GetMesh()->GetBodyInstance(Wound.Bone))
		{
			FHitResult Surface;
			if (Part->LineTrace(Surface, Point + Outward * 40.0f, Point - Outward * 5.0f, false))
			{
				Target = Surface.ImpactPoint + Outward * Margin;
			}
		}

		const FString WoundBone = Wound.Bone.ToString();
		const bool bArmWound = WoundBone.StartsWith(TEXT("upperarm")) || WoundBone.StartsWith(TEXT("lowerarm")) || WoundBone.StartsWith(TEXT("hand"))
		                       || WoundBone.StartsWith(TEXT("clavicle"));
		const auto CanUse = [&](const int32 Try)
		{
			const EImpactLimb Arm = Try == 0 ? EImpactLimb::LeftArm : EImpactLimb::RightArm;
			const bool bSameArm = bArmWound && WoundBone.EndsWith(Try == 0 ? TEXT("_l") : TEXT("_r"));
			// The arm already on the wound is allowed a little further before it is declared out of reach: at the
			// exact limit (a thigh) the body swaying was enough to flip in and out of range several times a second.
			const float Allowed = Try == HandSide || Try == SecondHandSide ? Reach + 12.0f : Reach;
			// A hand does not cross the body to reach a hole plainly on the other side - unless the hole is
			// on an arm, where the far hand is the only one there is: an arm cannot hold itself. Applying
			// the rule to arms as well stopped arm wounds being held at all, which is the opposite of what
			// was asked for ("une blessure a l'epaule, tu la tiens toujours avec l'autre main").
			const FVector Local = Character->GetActorTransform().InverseTransformPosition(Target);
			const bool bCrosses = !bArmWound && FMath::Abs(Local.Y) > 12.0f && ((Local.Y > 0.0f) != (Try == 1));
			const EImpactLimb Near = Local.Y > 0.0f ? EImpactLimb::RightArm : EImpactLimb::LeftArm;
			// A hand holding a weapon counts as gone for this: the other one reaches across, as a man with a gun in his right hand
			// grabs his right thigh with his left.
			const bool bNearArmGone = Physiology.GetLimbCapability(Near) < 0.4f || (Near == EImpactLimb::RightArm && bWeaponInRightHand);

			// The right hand holds the weapon (ALS puts every weapon in it): the wound gets the other one.
			return !(Try == 1 && bWeaponInRightHand) && Physiology.GetLimbCapability(Arm) >= 0.4f && !bSameArm && (HandFailedSides & (1 << Try)) == 0
			       && (!bCrosses || bNearArmGone)
			       && FVector::Dist(Target, ShoulderBones[Try]) < Allowed;
		};

		// The hand already on the wound keeps it as long as it can still do the job. It only hands over when the
		// other arm is clearly better placed and nothing has been touched yet.
		if (HandSide != INDEX_NONE && CanUse(HandSide))
		{
			BestSide = HandSide;
			const int32 Other = 1 - HandSide;
			if (!Reaction->IsHandPressing(HandSide) && Reaction->GetHandGap(HandSide) > 12.0f && CanUse(Other)
			    && FVector::Dist(Target, ShoulderBones[Other]) < FVector::Dist(Target, ShoulderBones[HandSide]) - 15.0f)
			{
				BestSide = Other;
			}
		}
		else
		{
			float BestDistance = Reach;
			for (int32 Try = 0; Try < 2; ++Try)
			{
				const float Distance = FVector::Dist(Target, ShoulderBones[Try]);
				if (CanUse(Try) && Distance < BestDistance)
				{
					BestDistance = Distance;
					BestSide = Try;
				}
			}
		}

		// A hole in the belly or the chest is held with both hands when both are free, because that is
		// what a person does: one hand is what is left when the other cannot reach or no longer works.
		// An arm or a shoulder is different - the wounded side cannot hold itself, so it stays a
		// one-handed job, and CanUse already refuses the arm the wound is on.
		// Once the second hand is down it stays down. Deciding it afresh every frame made it come and go
		// twice a second as the wound drifted across the edge of its reach, which reads as a man patting
		// himself rather than pressing a hole: measured, Passant 4 changed his grip at 9,12 s, 9,16 s,
		// 9,76 s and 10,13 s. It is let go only when the arm itself can no longer do the job.
		const bool bTrunkWound = WoundBone.StartsWith(TEXT("spine")) || WoundBone == TEXT("pelvis");
		if (BestSide != INDEX_NONE && bTrunkWound)
		{
			const int32 Candidate = 1 - BestSide;
			const bool bHadIt = SecondHandSide == Candidate && Now - SecondHandSince < 100.0f;
			if (bHadIt
				    ? Physiology.GetLimbCapability(Candidate == 0 ? EImpactLimb::LeftArm : EImpactLimb::RightArm) >= 0.4f
				    : CanUse(Candidate))
			{
				OtherSide = Candidate;
				if (SecondHandSide != Candidate)
				{
					SecondHandSince = Now;
				}
			}
		}
	}

	// Side by side, not both on the same point: two hands driven at one target fight each other and the
	// arms cross.
	FVector HandTargets[2] = {Target, Target};
	if (OtherSide != INDEX_NONE)
	{
		const FVector Across = Character->GetActorRightVector();
		HandTargets[0] = Target - Across * 6.0f;
		HandTargets[1] = Target + Across * 6.0f;
	}

	HandSide = BestSide;
	SecondHandSide = OtherSide;
	for (int32 Side = 0; Side < 2; ++Side)
	{
		Reaction->SetHandTarget(Side, HandTargets[Side], Outward, Side == BestSide || Side == OtherSide);
	}
	if (BestSide != LastHandSide || OtherSide != LastSecondHandSide)
	{
		LastHandSide = BestSide;
		LastSecondHandSide = OtherSide;
		UE_LOG(LogImpact, Display, TEXT("[hand] t=%.2f %s : %s"), GetWorld()->GetTimeSeconds(), *DisplayName,
		       BestSide == INDEX_NONE
			       ? TEXT("mains libres")
			       : *FString::Printf(TEXT("%s vers %s"),
			                          OtherSide != INDEX_NONE
				                          ? TEXT("les deux mains")
				                          : (BestSide == 0 ? TEXT("main gauche") : TEXT("main droite")),
			                          *Wounds[Held].Name));
	}

	// One-off measurement: which way the palm of this skeleton faces, needed to turn the hand onto the wound.
	if (!bHandAxesLogged && BestSide != INDEX_NONE)
	{
		bHandAxesLogged = true;
		const FTransform HandPose = Mesh->GetSocketTransform(BestSide == 0 ? TEXT("hand_l") : TEXT("hand_r"), RTS_World);
		UE_LOG(LogImpact, Display, TEXT("[hand] axes main %d : X=%s Y=%s Z=%s | acteur avant=%s droite=%s"), BestSide,
		       *HandPose.GetUnitAxis(EAxis::X).ToCompactString(), *HandPose.GetUnitAxis(EAxis::Y).ToCompactString(),
		       *HandPose.GetUnitAxis(EAxis::Z).ToCompactString(), *Character->GetActorForwardVector().ToCompactString(),
		       *Character->GetActorRightVector().ToCompactString());
	}

	// A hand held against the wound slows the bleeding. Being briefly near it is not holding it.
	for (int32 i = 0; i < Wounds.Num(); ++i)
	{
		Wounds[i].bPressed = i == Held && BestSide != INDEX_NONE && Reaction->IsHandPressing(BestSide);
	}
}

void UImpactBodyComponent::TickComponent(const float DeltaTime, const ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	Physiology.TimeScale = CVarWoundSpeed.GetValueOnGameThread();
	Physiology.Tick(DeltaTime);
	UpdateBody(false);
	EmitBlood(DeltaTime);
	TrySetupSkinBlood(DeltaTime);

	LogTimer -= DeltaTime;
	if (LogTimer <= 0.0f && !Physiology.IsDead())
	{
		LogTimer = 2.0f;
		UE_LOG(LogImpact, Display, TEXT("[body] t=%.2f %s : %s, perd %.1f mL/s, oxygène du cerveau %.2f"), GetWorld()->GetTimeSeconds(), *DisplayName,
		       *Physiology.Describe(), Physiology.GetOutflow(), Physiology.GetBrainOxygen());
		if (SkinBlood != nullptr)
		{
			UE_LOG(LogImpact, Display, TEXT("[sang-peau] t=%.2f %s : %s"), GetWorld()->GetTimeSeconds(), *DisplayName, *SkinBloodSummary);
		}
	}
}

void UImpactBodyComponent::EmitBlood(const float DeltaTime)
{
	UImpactGameplaySubsystem* Gameplay = GetWorld()->GetSubsystem<UImpactGameplaySubsystem>();
	AImpactFX* FX = Gameplay != nullptr ? Gameplay->GetFX() : nullptr;
	ACharacter* Character = Cast<ACharacter>(GetOwner());
	if (FX == nullptr || Character == nullptr)
	{
		return;
	}
	USkeletalMeshComponent* Mesh = Character->GetMesh();
	const UImpactReactionComponent* Reaction = GetOwner()->FindComponentByClass<UImpactReactionComponent>();
	const bool bDown = Reaction != nullptr && Reaction->IsDown();
	const TArray<FImpactWound>& Wounds = Physiology.GetWounds();
	WoundDripBudgets.SetNumZeroed(Wounds.Num(), EAllowShrinking::No);

	// Persistent marks follow the injured bone. These are surface stains, not a destructible skin mesh.
	for (; ProcessedVisualWounds < Wounds.Num(); ++ProcessedVisualWounds)
	{
		const FImpactWound& Wound = Wounds[ProcessedVisualWounds];
		if (!Wound.bExternal || MarkedWounds.Num() >= 16)
		{
			continue;
		}
		const bool bAlreadyMarked = MarkedWounds.ContainsByPredicate([&](const int32 Index)
		{
			return Wounds.IsValidIndex(Index) && Wounds[Index].Bone == Wound.Bone
			       && FVector::DistSquared(Wounds[Index].LocalPoint, Wound.LocalPoint) < 9.0f;
		});
		if (bAlreadyMarked)
		{
			continue;
		}
		const FVector Point = GetWoundLocation(Wound);
		FVector Normal = (Point - Mesh->GetSocketLocation(Wound.Bone)).GetSafeNormal();
		if (Normal.IsNearlyZero()) Normal = Character->GetActorForwardVector();
		if (FX->BloodWound(Mesh, Wound.Bone, Point, Normal, Wound.bArterial ? 6.0f : 3.8f) != nullptr)
		{
			MarkedWounds.Add(ProcessedVisualWounds);
		}
	}

	// Each wound uses the same measured flow as physiology. Internal blood loss cannot create floor pools.
	int32 DropsThisFrame = 0;
	for (int32 Index = 0; Index < Wounds.Num(); ++Index)
	{
		const FImpactWound& Wound = Wounds[Index];
		float& Budget = WoundDripBudgets[Index];
		if (!Wound.bExternal || Wound.CurrentFlow <= 0.0f)
		{
			Budget = 0.0f;
			continue;
		}
		const float Beat = Wound.bArterial && !Wound.bPressed ? Physiology.GetPulse() : 0.0f;
		const float Rhythm = Wound.bArterial && !Wound.bPressed ? 0.25f + 3.0f * Beat : 1.0f;
		Budget = FMath::Min(8.0f, Budget + FMath::Min(120.0f, Wound.CurrentFlow * 1.2f * Rhythm) * DeltaTime);
		const FVector Point = GetWoundLocation(Wound);
		const FVector Outward = (Point - Mesh->GetSocketLocation(Wound.Bone)).GetSafeNormal();
		const FVector InheritedVelocity = Mesh->GetPhysicsLinearVelocity(Wound.Bone);
		while (Budget >= 1.0f && DropsThisFrame < 20)
		{
			Budget -= 1.0f;
			++DropsThisFrame;
			FVector Velocity = InheritedVelocity * 0.9f + FMath::VRand() * 20.0f + FVector(0, 0, -40.0f);
			float Size = FMath::FRandRange(0.7f, 1.3f);
			if (Wound.bArterial && !Wound.bPressed)
			{
				Velocity += Outward * (70.0f + 350.0f * Beat) + FVector(0, 0, 40.0f * Beat);
				Size *= 1.2f + 0.7f * Beat;
			}
			FX->BloodDrop(Point + Outward, Velocity, Size);
		}
	}

	const float ExternalFlow = Physiology.GetExternalOutflow();
	if (bDown && ExternalFlow > 0.0f)
	{
		const int32 Worst = Physiology.GetWorstExternalWound();
		const FVector From = Worst != INDEX_NONE ? GetWoundLocation(Wounds[Worst]) : Character->GetActorLocation();
		// A crawling body leaves successive pools instead of growing its first puddle from metres away.
		if (Pool.IsValid() && FVector::DistSquared2D(Pool->GetComponentLocation(), From) > FMath::Square(55.0f))
		{
			Pool.Reset();
			PoolLostMl = 0.0f;
		}
		PoolLostMl += ExternalFlow * DeltaTime;
		if (!Pool.IsValid() && PoolLostMl > 20.0f)
		{
			FHitResult Ground;
			const FCollisionQueryParams Params(SCENE_QUERY_STAT(ImpactPool), false, GetOwner());
			if (GetWorld()->LineTraceSingleByChannel(Ground, From + FVector(0, 0, 20), From - FVector(0, 0, 150), ECC_Visibility, Params))
			{
				Pool = FX->BloodSplat(Ground.ImpactPoint, Ground.ImpactNormal, 10.0f, true);
			}
		}
		if (Pool.IsValid())
		{
			const float Size = FMath::Min(90.0f, 6.0f + 1.4f * FMath::Sqrt(PoolLostMl));
			Pool->DecalSize = FVector(8.0f, Size, Size);
			Pool->MarkRenderStateDirty();
		}
	}
}
void UImpactBodyComponent::DrawXRay(const float Duration) const
{
	const ACharacter* Character = Cast<ACharacter>(GetOwner());
	UWorld* World = GetWorld();
	if (Character == nullptr || !Anatomy.IsBuilt())
	{
		return;
	}

	Anatomy.DrawOrgans(World, Character->GetMesh(), Duration);

	if (const UImpactReactionComponent* Reaction = GetOwner()->FindComponentByClass<UImpactReactionComponent>())
	{
		for (int32 Side = 0; Side < 2; ++Side)
		{
			FVector Target, Approach;
			bool bApproached = false;
			if (Reaction->GetHandGoal(Side, Target, Approach, bApproached))
			{
				const FVector Hand = Character->GetMesh()->GetSocketLocation(Side == 0 ? TEXT("hand_l") : TEXT("hand_r"));
				DrawDebugSphere(World, Target, 3.0f, 8, FColor::Green, false, Duration, SDPG_Foreground, 0.5f);
				DrawDebugSphere(World, Approach, 2.0f, 8, FColor::Cyan, false, Duration, SDPG_Foreground, 0.5f);
				DrawDebugLine(World, Hand, bApproached ? Target : Approach, FColor::Green, false, Duration, SDPG_Foreground, 0.4f);
			}
		}
	}

	for (const FPathRecord& Record : RecentPaths)
	{
		const FTransform Bone = Character->GetMesh()->GetSocketTransform(Record.Bone, RTS_World);
		const FVector Entry = Bone.TransformPosition(Record.Entry);
		const FColor Color = Record.bExited ? FColor(255, 40, 40) : FColor(255, 160, 0);
		DrawDebugLine(World, Bone.TransformPosition(Record.Before), Entry, FColor(255, 255, 255), false, Duration, SDPG_Foreground, 0.3f);
		DrawDebugLine(World, Entry, Bone.TransformPosition(Record.Exit), Color, false, Duration, SDPG_Foreground, 0.8f);
		for (const FVector& Hit : Record.Hits)
		{
			DrawDebugPoint(World, Bone.TransformPosition(Hit), 8.0f, FColor::White, false, Duration, SDPG_Foreground);
		}
	}
}

FString UImpactBodyComponent::GetLabel() const
{
	const UImpactReactionComponent* Reaction = GetOwner()->FindComponentByClass<UImpactReactionComponent>();
	const UImpactActiveBodyComponent* Active = GetOwner()->FindComponentByClass<UImpactActiveBodyComponent>();
	const FString State = Reaction != nullptr ? Reaction->GetStateText() : Active != nullptr ? Active->GetStateText() : FString();

	FString Text = DisplayName;
	if (!Physiology.GetWounds().IsEmpty())
	{
		Text += TEXT(" · ") + Physiology.Describe();
	}
	if (!State.IsEmpty() && !Physiology.IsDead())
	{
		Text += TEXT(" · ") + State;
	}
	for (const FImpactWound& Wound : Physiology.GetWounds())
	{
		if (Wound.bPressed)
		{
			Text += TEXT(" · main sur la plaie");
			break;
		}
	}
	const APawn* Pawn = Cast<APawn>(GetOwner());
	if (const AImpactCivilianController* Brain = Pawn != nullptr ? Cast<AImpactCivilianController>(Pawn->GetController()) : nullptr)
	{
		const FString Doing = Brain->GetStateText();
		if (!Doing.IsEmpty() && !Physiology.IsDead())
		{
			Text += TEXT(" · ") + Doing;
		}
	}
	return Text;
}

#include "ImpactActiveBodyComponent.h"

#include "AlsCharacter.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "Impact.h"
#include "Kismet/GameplayStatics.h"
#include "ImpactBodyComponent.h"
#include "ImpactTypes.h"
#include "Math/QuatRotationTranslationMatrix.h"
#include "Misc/CommandLine.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/ConstraintInstance.h"
#include "Utility/AlsGameplayTags.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(ImpactActiveBodyComponent)

namespace
{
	struct FJointSpec
	{
		const TCHAR* Bone;
		const TCHAR* Parent;
		float Spring;
	};

	// The muscles are the joints' own drives, between each bone and its parent: they pull one against the other and give the
	// body as a whole nothing, which is what a muscle does. (The first version used UPhysicalAnimationComponent, whose "local"
	// muscles pull towards targets outside the body: measured long ago, they push and spin a body lying on the floor. Seen
	// again at once: "il bouge tout seul".) Parents come before their children: the poses on the ground are built in this order.
	const FJointSpec JointSpecs[] = {
		{TEXT("spine_01"), TEXT("pelvis"), 30000.0f},
		{TEXT("spine_02"), TEXT("spine_01"), 26000.0f},
		{TEXT("spine_03"), TEXT("spine_02"), 22000.0f},
		{TEXT("neck_01"), TEXT("spine_03"), 8000.0f},
		{TEXT("head"), TEXT("neck_01"), 5000.0f},
		{TEXT("clavicle_l"), TEXT("spine_03"), 16000.0f},
		{TEXT("upperarm_l"), TEXT("clavicle_l"), 5000.0f},
		{TEXT("lowerarm_l"), TEXT("upperarm_l"), 3000.0f},
		{TEXT("hand_l"), TEXT("lowerarm_l"), 1000.0f},
		{TEXT("clavicle_r"), TEXT("spine_03"), 16000.0f},
		{TEXT("upperarm_r"), TEXT("clavicle_r"), 5000.0f},
		{TEXT("lowerarm_r"), TEXT("upperarm_r"), 3000.0f},
		{TEXT("hand_r"), TEXT("lowerarm_r"), 1000.0f},
		{TEXT("thigh_l"), TEXT("pelvis"), 40000.0f},
		{TEXT("calf_l"), TEXT("thigh_l"), 30000.0f},
		{TEXT("foot_l"), TEXT("calf_l"), 10000.0f},
		{TEXT("thigh_r"), TEXT("pelvis"), 40000.0f},
		{TEXT("calf_r"), TEXT("thigh_r"), 30000.0f},
		{TEXT("foot_r"), TEXT("calf_r"), 10000.0f},
	};

	// Push given to the part hit, kg.cm/s, before -ImpactActifPoussee=.
	float PushFor(const EImpactWeapon Weapon)
	{
		return Weapon == EImpactWeapon::Rifle ? 3500.0f : Weapon == EImpactWeapon::Pistol ? 5000.0f : 3000.0f;
	}

	// How much shock (about one per round, fading at one a second) he takes standing.
	constexpr float ShockLimit = 4.5f;

	// Damping that stops a joint swinging back and forth: a little under critical for the spring.
	float DampFor(const float Spring)
	{
		return 1.6f * FMath::Sqrt(FMath::Max(0.0f, Spring));
	}

	float CommandLineValue(const TCHAR* Name, const float Default, const float Min)
	{
		float Parsed = Default;
		return FParse::Value(FCommandLine::Get(), Name, Parsed) ? FMath::Max(Min, Parsed) : Default;
	}

	// The up axis tilted towards the front by Degrees (backwards when negative).
	FQuat Flex(const FVector& Up, const FVector& Forward, const float Degrees)
	{
		const float Angle = FMath::DegreesToRadians(Degrees);
		return FQuat::FindBetweenNormals(Up, (Up * FMath::Cos(Angle) + Forward * FMath::Sin(Angle)).GetSafeNormal());
	}

	float AngleBetween(const FQuat& A, const FQuat& B)
	{
		return FMath::RadiansToDegrees(A.AngularDistance(B));
	}
}

UImpactActiveBodyComponent::UImpactActiveBodyComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;
}

bool UImpactActiveBodyComponent::IsEnabled()
{
	static const bool bEnabled = FParse::Param(FCommandLine::Get(), TEXT("ImpactCorpsActif"));
	return bEnabled;
}

AAlsCharacter* UImpactActiveBodyComponent::GetCharacter() const
{
	return Cast<AAlsCharacter>(GetOwner());
}

void UImpactActiveBodyComponent::Setup()
{
	AAlsCharacter* Character = GetCharacter();
	USkeletalMeshComponent* Mesh = Character != nullptr ? Character->GetMesh() : nullptr;
	if (Mesh == nullptr || Mesh->GetPhysicsAsset() == nullptr)
	{
		return;
	}
	// After ALS: it sets its own ragdoll motors in its tick, and ours are set here after it.
	PrimaryComponentTick.AddPrerequisite(Character, Character->PrimaryActorTick);
	Mesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;

	// Measured, swept: the trunk at half the table with the joint hit let go almost entirely and the push at four bends 16 to 19
	// degrees per round and holds its pose between rounds (soft at 0.12 it bent 21 to 27 but sagged). The legs keep the table:
	// they carry him. -ImpactActifRaideur= -ImpactActifBras= change the two shares.
	static const float Stiff = CommandLineValue(TEXT("ImpactActifRaideur="), 0.5f, 0.01f);
	static const float ArmStiff = CommandLineValue(TEXT("ImpactActifBras="), 1.0f, 0.01f);
	for (const FJointSpec& Spec : JointSpecs)
	{
		if (Mesh->FindConstraintInstance(Spec.Bone) != nullptr && Mesh->GetBodyInstance(Spec.Bone) != nullptr)
		{
			const FString Name(Spec.Bone);
			const bool bLeg = Name.StartsWith(TEXT("thigh")) || Name.StartsWith(TEXT("calf")) || Name.StartsWith(TEXT("foot"));
			const bool bArm = Name.StartsWith(TEXT("clavicle")) || Name.StartsWith(TEXT("upperarm")) || Name.StartsWith(TEXT("lowerarm"))
			                  || Name.StartsWith(TEXT("hand"));
			FMuscle& Part = Parts.AddDefaulted_GetRef();
			Part.Bone = Spec.Bone;
			Part.Parent = Spec.Parent;
			Part.Strength = Spec.Spring * (bLeg ? 1.0f : bArm ? ArmStiff : Stiff);
		}
	}
	// PA_Als has no velocity drive, and without one Chaos ignores the damping: switched on, aiming at rest.
	for (FConstraintInstance* Joint : Mesh->Constraints)
	{
		if (Joint != nullptr)
		{
			Joint->SetAngularVelocityTarget(FVector::ZeroVector);
			Joint->SetAngularVelocityDriveTwistAndSwing(true, true);
		}
	}
	SetupPoses(Mesh);
	SetupStanding();
	bReady = true;
	UE_LOG(LogImpact, Display, TEXT("[corps-actif] %s : corps physique, %d articulations musclees%s"), *GetOwner()->GetName(), Parts.Num(),
	       bPosesReady ? TEXT("") : TEXT(", PAS de poses au sol"));
}

void UImpactActiveBodyComponent::SetupPoses(USkeletalMeshComponent* Mesh)
{
	// The reference pose in component space: where each bone is and how it is turned before any animation.
	const USkeletalMesh* Asset = Mesh->GetSkeletalMeshAsset();
	if (Asset == nullptr)
	{
		return;
	}
	const FReferenceSkeleton& Skeleton = Asset->GetRefSkeleton();
	const TArray<FTransform>& Local = Skeleton.GetRefBonePose();
	TArray<FTransform> Space;
	Space.SetNum(Local.Num());
	for (int32 Index = 0; Index < Local.Num(); ++Index)
	{
		const int32 ParentIndex = Skeleton.GetParentIndex(Index);
		Space[Index] = ParentIndex == INDEX_NONE ? Local[Index] : Local[Index] * Space[ParentIndex];
	}
	const auto Find = [&Skeleton](const FName Name) { return Skeleton.FindBoneIndex(Name); };
	const int32 Pelvis = Find(TEXT("pelvis"));
	const int32 Head = Find(TEXT("head"));
	const int32 ThighL = Find(TEXT("thigh_l"));
	const int32 ThighR = Find(TEXT("thigh_r"));
	if (Pelvis == INDEX_NONE || Head == INDEX_NONE || ThighL == INDEX_NONE || ThighR == INDEX_NONE)
	{
		return;
	}
	// The body's own axes, whatever way the mesh faces: up from the pelvis to the head, right from the left hip to the right.
	BodyUp = (Space[Head].GetLocation() - Space[Pelvis].GetLocation()).GetSafeNormal();
	const FVector Across = Space[ThighR].GetLocation() - Space[ThighL].GetLocation();
	BodyForward = FVector::CrossProduct(Across - BodyUp * FVector::DotProduct(Across, BodyUp), BodyUp).GetSafeNormal();
	BodyRight = FVector::CrossProduct(BodyUp, BodyForward);

	for (FMuscle& Part : Parts)
	{
		const FConstraintInstance* Joint = Mesh->FindConstraintInstance(Part.Bone);
		const int32 Bone = Find(Part.Bone);
		if (Joint == nullptr || Bone == INDEX_NONE)
		{
			return;
		}
		Part.PhysicsParent = Joint->GetParentBoneName();
		const int32 ParentBone = Find(Part.PhysicsParent);
		if (ParentBone == INDEX_NONE)
		{
			return;
		}
		Part.ParentPart = Parts.IndexOfByPredicate([&Part](const FMuscle& Each) { return Each.Bone == Part.PhysicsParent; });
		Part.RefParent = Space[ParentBone].GetRotation();
		Part.RefChild = Space[Bone].GetRotation();
		Part.Frame1 = Joint->GetRefFrame(EConstraintFrame::Frame1).GetRotation();
		Part.Frame2 = Joint->GetRefFrame(EConstraintFrame::Frame2).GetRotation();

		const FString Name = Part.Bone.ToString();
		Part.Side = Name.EndsWith(TEXT("_l")) ? -1.0f : Name.EndsWith(TEXT("_r")) ? 1.0f : 0.0f;
		const bool bLeft = Part.Side < 0.0f;
		Part.Group = Name.StartsWith(TEXT("spine")) ? EGroup::Spine
		             : Name.StartsWith(TEXT("neck")) || Name == TEXT("head") ? EGroup::Head
		             : Name.StartsWith(TEXT("thigh")) || Name.StartsWith(TEXT("calf")) || Name.StartsWith(TEXT("foot")) ? (bLeft ? EGroup::LeftLeg : EGroup::RightLeg)
		             : (bLeft ? EGroup::LeftArm : EGroup::RightArm);
		// Which way the bone points, for the limbs: towards the next joint down.
		const FString Suffix = bLeft ? TEXT("_l") : TEXT("_r");
		const TCHAR* Next = Name.StartsWith(TEXT("thigh")) ? TEXT("calf") : Name.StartsWith(TEXT("calf")) ? TEXT("foot")
		                    : Name.StartsWith(TEXT("upperarm")) ? TEXT("lowerarm") : Name.StartsWith(TEXT("lowerarm")) ? TEXT("hand") : nullptr;
		const int32 NextBone = Next != nullptr ? Find(FName(FString(Next) + Suffix)) : INDEX_NONE;
		Part.RefDir = NextBone != INDEX_NONE ? (Space[NextBone].GetLocation() - Space[Bone].GetLocation()).GetSafeNormal() : FVector::ZeroVector;
	}
	PoseCurled.SetNum(Parts.Num());
	PoseOpen.SetNum(Parts.Num());
	Targets.SetNum(Parts.Num());
	bPosesReady = true;
}

void UImpactActiveBodyComponent::SetupStanding()
{
	AAlsCharacter* Character = GetCharacter();
	USkeletalMeshComponent* Mesh = Character != nullptr ? Character->GetMesh() : nullptr;
	if (Mesh == nullptr)
	{
		return;
	}
	Mesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	// Not against his own capsule (nor anyone's): the capsule walks, the body follows it.
	Mesh->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
	// Everything below the pelvis simulated and shown as simulated; the pelvis itself goes where the animation puts it, which
	// is what holds him up and carries him along while he walks.
	Mesh->SetAllBodiesBelowSimulatePhysics(TEXT("pelvis"), true, false);
	Mesh->SetAllBodiesBelowPhysicsBlendWeight(TEXT("pelvis"), 1.0f, false, false);
	// Standing, the upper body is not weighed down: muscles soft enough for a round to show sagged under their own weight
	// (seen: heads hanging, weapons pointing at the floor). Gravity comes back to all of it the moment he falls.
	SetUpperBodyGravity(false);
	bArmsAnimated = false;
	Tone = 1.0f;
	Shock = 0.0f;
	for (FMuscle& Part : Parts)
	{
		Part.Slack = 0.0f;
		Part.Applied = -1.0f;
	}
	UpdateMuscles(0.0f);
}

void UImpactActiveBodyComponent::SetUpperBodyGravity(const bool bEnabled)
{
	AAlsCharacter* Character = GetCharacter();
	USkeletalMeshComponent* Mesh = Character != nullptr ? Character->GetMesh() : nullptr;
	if (Mesh == nullptr)
	{
		return;
	}
	for (FBodyInstance* Body : Mesh->Bodies)
	{
		if (Body == nullptr || !Body->BodySetup.IsValid())
		{
			continue;
		}
		const FString Name = Body->BodySetup->BoneName.ToString();
		const bool bLeg = Name.StartsWith(TEXT("thigh")) || Name.StartsWith(TEXT("calf")) || Name.StartsWith(TEXT("foot")) || Name.StartsWith(TEXT("ball"));
		if (bEnabled || !bLeg)
		{
			Body->SetEnableGravity(bEnabled);
		}
	}
}

void UImpactActiveBodyComponent::OnBullet(const FImpactBulletHit& Bullet, const float Strength)
{
	AAlsCharacter* Character = GetCharacter();
	USkeletalMeshComponent* Mesh = Character != nullptr ? Character->GetMesh() : nullptr;
	if (Mesh == nullptr || State == EState::GettingUp)
	{
		return;
	}
	// The pelvis is the animation's while he stands: a round there is taken by the spine above it.
	FName Hit = Bullet.Hit.BoneName;
	if (Hit == TEXT("pelvis") && State == EState::Standing)
	{
		Hit = TEXT("spine_01");
	}
	static const float PushScale = CommandLineValue(TEXT("ImpactActifPoussee="), 4.0f, 0.0f);
	const FBodyInstance* Body = Mesh->GetBodyInstance(Hit);
	if (Body != nullptr && Body->IsInstanceSimulatingPhysics())
	{
		Mesh->AddImpulseAtLocation(Bullet.Direction.GetSafeNormal() * PushFor(Bullet.Weapon) * PushScale * Strength, Bullet.Hit.ImpactPoint, Hit);
	}
	// The joint hit lets go for a moment, and nearly as much the joints it hangs from, down to the pelvis: a round in the chest
	// bends the whole spine, not one vertebra. The muscles bring it back after.
	float Let = 1.0f;
	for (FName Walk = Hit; !Walk.IsNone() && Let > 0.1f; Let *= 0.85f)
	{
		FMuscle* Part = Parts.FindByPredicate([Walk](const FMuscle& Each) { return Each.Bone == Walk; });
		if (Part == nullptr)
		{
			break;
		}
		Part->Slack = FMath::Max(Part->Slack, Let);
		Walk = Part->Parent;
	}
	Shock += Strength;
	if (State == EState::Standing && BendWatch < 0.0f)
	{
		BendWatch = 0.0f;
		BendPeak = 0.0f;
		BendFrom = (Mesh->GetSocketLocation(TEXT("spine_03")) - Mesh->GetSocketLocation(TEXT("pelvis"))).GetSafeNormal();
	}
	UE_LOG(LogImpact, Display, TEXT("[corps-actif] t=%.2f %s touche %s (force %.2f), choc %.1f / %.1f"), GetWorld()->GetTimeSeconds(),
	       *GetOwner()->GetName(), *Hit.ToString(), Strength, Shock, ShockLimit);
}

void UImpactActiveBodyComponent::Fall(const TCHAR* Why)
{
	// The pelvis let go and the capsule made to follow the body: ALS's ragdolling does both. The body was physical already;
	// it is only no longer held up.
	State = EState::Down;
	DownTime = 0.0f;
	SettleTime = 0.0f;
	SetUpperBodyGravity(true);
	if (AAlsCharacter* Character = GetCharacter())
	{
		Character->StartRagdolling();
	}
	UE_LOG(LogImpact, Display, TEXT("[corps-actif] t=%.2f %s tombe : %s"), GetWorld()->GetTimeSeconds(), *GetOwner()->GetName(), Why);
}

bool UImpactActiveBodyComponent::CanGetUp() const
{
	// Awake, both legs able to carry, and not bleeding out: no open artery, no more than a trickle from the holes, not emptied of
	// blood. A man hit in the vest, an arm or the flesh of the side gets up; one losing his blood stays down holding it.
	const UImpactBodyComponent* Body = GetOwner()->FindComponentByClass<UImpactBodyComponent>();
	if (Body == nullptr)
	{
		return false;
	}
	const FImpactPhysiology& Physiology = Body->GetPhysiology();
	if (Physiology.GetConsciousness() != EImpactConsciousness::Conscious || Physiology.GetBloodLossFraction() > 0.15f
	    || FMath::Min(Physiology.GetLimbCapability(EImpactLimb::LeftLeg), Physiology.GetLimbCapability(EImpactLimb::RightLeg)) < 0.25f)
	{
		return false;
	}
	float Bleeding = 0.0f;
	for (const FImpactWound& Wound : Physiology.GetWounds())
	{
		if (!Wound.bClosed)
		{
			if (Wound.bArterial)
			{
				return false;
			}
			Bleeding += Wound.Bleed;
		}
	}
	return Bleeding < 20.0f;
}

FQuat UImpactActiveBodyComponent::GetCurrentRelative(const USkeletalMeshComponent* Mesh, const FMuscle& Part) const
{
	const FBodyInstance* Child = Mesh->GetBodyInstance(Part.Bone);
	const FBodyInstance* Parent = Mesh->GetBodyInstance(Part.PhysicsParent);
	if (Child == nullptr || Parent == nullptr)
	{
		return FQuat::Identity;
	}
	return Parent->GetUnrealWorldTransform().GetRotation().Inverse() * Child->GetUnrealWorldTransform().GetRotation();
}

void UImpactActiveBodyComponent::CheckTargets(USkeletalMeshComponent* Mesh)
{
	// Once, standing: the engine has just aimed every joint from the animation. The same turn carried into the joint's frames
	// the way DriveWrithe does it must land on the same target; logged as [corps-actif] verif cibles.
	bTargetsChecked = true;
	const USkeletalMesh* Asset = Mesh->GetSkeletalMeshAsset();
	if (Asset == nullptr || !bPosesReady)
	{
		return;
	}
	const FReferenceSkeleton& Skeleton = Asset->GetRefSkeleton();
	const TArray<FTransform> Local = Mesh->GetBoneSpaceTransforms();
	float Sum = 0.0f;
	float Worst = 0.0f;
	int32 Count = 0;
	for (const FMuscle& Part : Parts)
	{
		const FConstraintInstance* Joint = Mesh->FindConstraintInstance(Part.Bone);
		int32 Index = Skeleton.FindBoneIndex(Part.Bone);
		if (Joint == nullptr || Index == INDEX_NONE || !Local.IsValidIndex(Index))
		{
			continue;
		}
		FQuat Relative = Local[Index].GetRotation();
		for (Index = Skeleton.GetParentIndex(Index); Index > 0 && Skeleton.GetBoneName(Index) != Part.PhysicsParent; Index = Skeleton.GetParentIndex(Index))
		{
			Relative = Local[Index].GetRotation() * Relative;
		}
		const FMatrix Body1 = FQuatRotationTranslationMatrix(Part.Frame1, FVector::ZeroVector);
		const FMatrix Body2Inverse = FQuatRotationTranslationMatrix(Part.Frame2, FVector::ZeroVector).InverseFast();
		const FQuat Mine(Body1 * FQuatRotationTranslationMatrix(Relative, FVector::ZeroVector) * Body2Inverse);
		const float Off = AngleBetween(Mine, Joint->GetAngularOrientationTarget().Quaternion());
		Sum += Off;
		Worst = FMath::Max(Worst, Off);
		++Count;
	}
	UE_LOG(LogImpact, Display, TEXT("[corps-actif] verif cibles %s : %d articulations, ecart moyen %.1f deg, pire %.1f deg"), *GetOwner()->GetName(), Count,
	       Sum / FMath::Max(1, Count), Worst);
}

void UImpactActiveBodyComponent::BuildPose(const TFunctionRef<void(const FMuscle&, float&, FVector&)> Shape, TArray<FQuat>& Out) const
{
	// Each limb given the direction it should point in, relative to the pelvis, and the spine a bend forward or back: each joint
	// then aims at that bone turned relative to its parent, so the whole shape goes wherever the pelvis lies.
	Out.SetNum(Parts.Num());
	TArray<FQuat> Delta;
	Delta.Init(FQuat::Identity, Parts.Num());
	for (int32 Index = 0; Index < Parts.Num(); ++Index)
	{
		const FMuscle& Part = Parts[Index];
		const FQuat ParentDelta = Part.ParentPart != INDEX_NONE ? Delta[Part.ParentPart] : FQuat::Identity;
		float Bend = 0.0f;
		FVector Point = FVector::ZeroVector;
		Shape(Part, Bend, Point);
		FQuat Own = FQuat::Identity;
		if (!Point.IsNearlyZero() && !Part.RefDir.IsNearlyZero())
		{
			Own = FQuat::FindBetweenNormals(Part.RefDir, ParentDelta.Inverse().RotateVector(Point.GetSafeNormal()));
		}
		else if (Bend != 0.0f)
		{
			Own = Flex(BodyUp, BodyForward, Bend);
		}
		Delta[Index] = ParentDelta * Own;
		Out[Index] = (Part.RefParent.Inverse() * ParentDelta.Inverse() * Delta[Index] * Part.RefChild).GetNormalized();
	}
}

void UImpactActiveBodyComponent::HoldCurrent(const USkeletalMeshComponent* Mesh)
{
	// From the pose he is in: the joints first aim where they are, then slide towards the new poses.
	for (int32 Index = 0; Index < Parts.Num(); ++Index)
	{
		Parts[Index].Held = GetCurrentRelative(Mesh, Parts[Index]);
		Targets[Index] = Parts[Index].Held;
	}
	WritheBlend = 0.0f;
}

void UImpactActiveBodyComponent::ApplyTargets(USkeletalMeshComponent* Mesh)
{
	for (int32 Index = 0; Index < Parts.Num(); ++Index)
	{
		const FMuscle& Part = Parts[Index];
		if (FConstraintInstance* Joint = Mesh->FindConstraintInstance(Part.Bone))
		{
			// What USkeletalMeshComponent::UpdateRBJointMotors does with an animated bone: the child's turn relative to its
			// parent, carried into the joint's own frames (checked once standing: [corps-actif] verif cibles).
			const FMatrix Body1 = FQuatRotationTranslationMatrix(Part.Frame1, FVector::ZeroVector);
			const FMatrix Body2Inverse = FQuatRotationTranslationMatrix(Part.Frame2, FVector::ZeroVector).InverseFast();
			Joint->SetAngularOrientationTarget(FQuat(Body1 * FQuatRotationTranslationMatrix(Targets[Index], FVector::ZeroVector) * Body2Inverse));
		}
	}
}

FVector UImpactActiveBodyComponent::GetChestFront(const USkeletalMeshComponent* Mesh) const
{
	// The front of the chest in the world: the reference pose's front carried by the upper spine's body.
	const FMuscle* Chest = Parts.FindByPredicate([](const FMuscle& Each) { return Each.Bone == TEXT("spine_03"); });
	const FBodyInstance* Body = Chest != nullptr ? Mesh->GetBodyInstance(Chest->Bone) : nullptr;
	return Body != nullptr ? Body->GetUnrealWorldTransform().GetRotation().RotateVector(Chest->RefChild.Inverse().RotateVector(BodyForward)) : FVector::UpVector;
}

FVector UImpactActiveBodyComponent::GetBodyLength(const USkeletalMeshComponent* Mesh) const
{
	return (Mesh->GetSocketLocation(TEXT("spine_03")) - Mesh->GetSocketLocation(TEXT("pelvis"))).GetSafeNormal();
}

bool UImpactActiveBodyComponent::WantsToCrawl() const
{
	// Awake but unable to get up: he drags himself away instead (-ImpactActifRamper=0 keeps him writhing in place).
	static const bool bCrawl = CommandLineValue(TEXT("ImpactActifRamper="), 1.0f, 0.0f) > 0.5f;
	const UImpactBodyComponent* Body = GetOwner()->FindComponentByClass<UImpactBodyComponent>();
	return bCrawl && Body != nullptr && Body->GetPhysiology().GetConsciousness() == EImpactConsciousness::Conscious && !CanGetUp()
	       && FMath::Max(Body->GetPhysiology().GetLimbCapability(EImpactLimb::LeftArm), Body->GetPhysiology().GetLimbCapability(EImpactLimb::RightArm)) > 0.4f;
}

void UImpactActiveBodyComponent::StartWrithe()
{
	AAlsCharacter* Character = GetCharacter();
	const UImpactBodyComponent* Body = GetOwner()->FindComponentByClass<UImpactBodyComponent>();
	if (Character == nullptr || Body == nullptr || !bPosesReady)
	{
		return;
	}
	// Where it hurts: a leg is drawn up and held, otherwise the hands go to the belly.
	HurtLeg = 0.0f;
	const int32 Worst = Body->GetPhysiology().GetWorstExternalWound();
	if (Worst != INDEX_NONE)
	{
		const FString Bone = Body->GetPhysiology().GetWounds()[Worst].Bone.ToString();
		if (Bone.StartsWith(TEXT("thigh")) || Bone.StartsWith(TEXT("calf")) || Bone.StartsWith(TEXT("foot")))
		{
			HurtLeg = Bone.EndsWith(TEXT("_l")) ? -1.0f : 1.0f;
		}
	}

	// Curled up and stretched out again, worked out from the reference pose.
	const FVector Up = BodyUp;
	const FVector Front = BodyForward;
	const FVector Right = BodyRight;
	for (int32 Pose = 0; Pose < 2; ++Pose)
	{
		const bool bCurled = Pose == 0;
		BuildPose([&](const FMuscle& Part, float& Bend, FVector& Point)
		{
			const FString Name = Part.Bone.ToString();
			const float Side = Part.Side;
			const bool bHurtSide = HurtLeg != 0.0f && Side == HurtLeg;
			if (Name.StartsWith(TEXT("spine")))
			{
				Bend = bCurled ? 16.0f : -6.0f;
			}
			else if (Name.StartsWith(TEXT("neck")))
			{
				Bend = bCurled ? 18.0f : -14.0f;
			}
			else if (Name == TEXT("head"))
			{
				Bend = bCurled ? 8.0f : -6.0f;
			}
			else if (Name.StartsWith(TEXT("upperarm")))
			{
				Point = bHurtSide ? Front * 0.45f - Up * 0.85f + Right * Side * 0.2f
				        : bCurled ? Front * 0.55f - Up * 0.75f + Right * Side * 0.12f
				        : Front * 0.25f - Up * 0.85f + Right * Side * 0.45f;
			}
			else if (Name.StartsWith(TEXT("lowerarm")))
			{
				// Across the belly towards the other side, or down onto the thigh that hurts.
				Point = bHurtSide ? Front * 0.75f - Up * 0.6f + Right * Side * 0.1f
				        : bCurled ? Front * 0.5f + Up * 0.2f - Right * Side * 0.8f
				        : Front * 0.7f - Up * 0.2f - Right * Side * 0.45f;
			}
			else if (Name.StartsWith(TEXT("thigh")))
			{
				Point = bHurtSide ? Front * 0.7f - Up * 0.7f + Right * Side * 0.1f
				        : bCurled ? Front * 0.85f - Up * 0.5f + Right * Side * 0.12f
				        : Front * 0.35f - Up * 0.94f + Right * Side * 0.1f;
			}
			else if (Name.StartsWith(TEXT("calf")))
			{
				Point = bHurtSide ? -Front * 0.5f - Up * 0.85f
				        : bCurled ? -Front * 0.6f - Up * 0.8f
				        : -Front * 0.15f - Up;
			}
		}, bCurled ? PoseCurled : PoseOpen);
	}
	// Dragging himself on his belly: one arm reaching ahead with the hand on the floor, the other pulled in under the shoulder
	// with the elbow out; the head and chest held up off the floor; the legs trailing.
	for (int32 Pose = 0; Pose < 2; ++Pose)
	{
		const bool bLeftReaches = Pose == 0;
		BuildPose([&](const FMuscle& Part, float& Bend, FVector& Point)
		{
			const FString Name = Part.Bone.ToString();
			const float Side = Part.Side;
			const bool bReach = (Side < 0.0f) == bLeftReaches;
			if (Name.StartsWith(TEXT("spine")))
			{
				Bend = -5.0f;
			}
			else if (Name.StartsWith(TEXT("neck")))
			{
				Bend = -25.0f;
			}
			else if (Name == TEXT("head"))
			{
				Bend = -10.0f;
			}
			else if (Name.StartsWith(TEXT("upperarm")))
			{
				Point = bReach ? Up * 0.75f + Right * Side * 0.5f + Front * 0.35f : -Up * 0.1f + Right * Side * 0.85f + Front * 0.5f;
			}
			else if (Name.StartsWith(TEXT("lowerarm")))
			{
				Point = bReach ? Up * 0.6f + Front * 0.75f - Right * Side * 0.15f : Up * 0.55f + Front * 0.6f - Right * Side * 0.55f;
			}
			else if (Name.StartsWith(TEXT("thigh")))
			{
				Point = -Up + Front * 0.05f + Right * Side * 0.12f;
			}
			else if (Name.StartsWith(TEXT("calf")))
			{
				Point = -Up + Front * 0.05f;
			}
		}, bLeftReaches ? PoseCrawlLeft : PoseCrawlRight);
	}
	bWrithing = true;
	StartGround(EGround::Writhe);
	UE_LOG(LogImpact, Display, TEXT("[corps-actif] t=%.2f %s se tord au sol (%s)"), GetWorld()->GetTimeSeconds(), *GetOwner()->GetName(),
	       HurtLeg < 0.0f ? TEXT("tient la jambe gauche") : HurtLeg > 0.0f ? TEXT("tient la jambe droite") : TEXT("se recroqueville"));
}

void UImpactActiveBodyComponent::StartGround(const EGround Next)
{
	AAlsCharacter* Character = GetCharacter();
	USkeletalMeshComponent* Mesh = Character != nullptr ? Character->GetMesh() : nullptr;
	if (Mesh == nullptr)
	{
		return;
	}
	Ground = Next;
	GroundTime = 0.0f;
	WritheTime = 0.0f;
	PullingHand = 0;
	HoldCurrent(Mesh);
	WritheFrom = Mesh->GetSocketLocation(TEXT("pelvis"));
	CrawlFrom = WritheFrom;
	// Crawling, the arms pull hard and the chest is held up; legs that cannot carry trail behind.
	const UImpactBodyComponent* Body = GetOwner()->FindComponentByClass<UImpactBodyComponent>();
	for (FMuscle& Part : Parts)
	{
		float Scale = 1.0f;
		if (Next == EGround::Crawl)
		{
			const bool bLeg = Part.Group == EGroup::LeftLeg || Part.Group == EGroup::RightLeg;
			const float Leg = Body != nullptr ? Body->GetPhysiology().GetLimbCapability(Part.Group == EGroup::LeftLeg ? EImpactLimb::LeftLeg : EImpactLimb::RightLeg) : 1.0f;
			// Measured at four times for the arms: a quarter of the stroke reached, the body seen gliding with the arms tucked under it.
			Scale = Part.Group == EGroup::LeftArm || Part.Group == EGroup::RightArm ? 10.0f : bLeg ? (Leg < 0.25f ? 0.2f : 1.0f) : 4.0f;
		}
		Part.ToneScale = Scale;
	}
	if (Next == EGround::Roll)
	{
		// Over the side he already leans to; flat on his back, towards the leg that does not hurt.
		const float Lean = FVector::DotProduct(FVector::CrossProduct(GetChestFront(Mesh), -FVector::UpVector), GetBodyLength(Mesh));
		RollWay = FMath::Abs(Lean) > 0.15f ? FMath::Sign(Lean) : (HurtLeg != 0.0f ? HurtLeg : 1.0f);
	}
	if (Next != EGround::Writhe)
	{
		UE_LOG(LogImpact, Display, TEXT("[corps-actif] t=%.2f %s %s"), GetWorld()->GetTimeSeconds(), *GetOwner()->GetName(),
		       Next == EGround::Roll ? TEXT("se retourne sur le ventre") : TEXT("rampe"));
	}
}

void UImpactActiveBodyComponent::StopWrithe()
{
	bWrithing = false;
	WritheBlend = 0.0f;
	Ground = EGround::Writhe;
	for (FMuscle& Part : Parts)
	{
		Part.ToneScale = 1.0f;
	}
}

void UImpactActiveBodyComponent::DriveWrithe()
{
	AAlsCharacter* Character = GetCharacter();
	USkeletalMeshComponent* Mesh = Character != nullptr ? Character->GetMesh() : nullptr;
	if (Mesh == nullptr)
	{
		return;
	}
	// Each part of him curls and stretches on its own slow rhythm, never in step: smooth waves, so the joints never have a jump
	// to catch up with. Mostly curled. The leg that hurts stays drawn up. Rolling over, he stays curled: it helps him over.
	static const float Speed[] = {0.9f, 1.1f, 0.8f, 0.75f, 0.7f, 0.85f};
	static const float Phase[] = {0.0f, 1.3f, 2.1f, 0.4f, 3.0f, 1.7f};
	const float Ease = WritheBlend * WritheBlend * (3.0f - 2.0f * WritheBlend);
	for (int32 Index = 0; Index < Parts.Num(); ++Index)
	{
		FMuscle& Part = Parts[Index];
		const int32 Group = static_cast<int32>(Part.Group);
		float Curl = Ground == EGround::Roll ? 1.0f : 0.35f + 0.65f * (0.5f + 0.5f * FMath::Sin(WritheTime * Speed[Group] + Phase[Group]));
		if (HurtLeg != 0.0f && Part.Side == HurtLeg && (Part.Group == EGroup::LeftLeg || Part.Group == EGroup::RightLeg))
		{
			Curl = 0.85f + 0.15f * FMath::Sin(WritheTime * 1.7f);
		}
		const FQuat Wanted = FQuat::Slerp(PoseOpen[Index], PoseCurled[Index], Curl);
		Targets[Index] = FQuat::Slerp(Part.Held, Wanted, Ease).GetNormalized();
	}
	ApplyTargets(Mesh);
}

void UImpactActiveBodyComponent::DriveRoll(const float DeltaTime)
{
	// Onto his belly before he can drag himself: a turn about the length of his body, the trunk turned at a steady speed (the
	// first version of the roll, a push, was seen to throw him over "super vite en sautant").
	AAlsCharacter* Character = GetCharacter();
	USkeletalMeshComponent* Mesh = Character->GetMesh();
	// Measured at 6 x (1.8 - spin) clamped to 10 on the trunk alone: lying flat on his back he did not get over in 3.5 s, twice.
	const FVector Length = GetBodyLength(Mesh);
	for (const TCHAR* Bone : {TEXT("pelvis"), TEXT("spine_01"), TEXT("spine_02"), TEXT("spine_03"), TEXT("clavicle_l"), TEXT("clavicle_r"), TEXT("thigh_l"), TEXT("thigh_r")})
	{
		if (FBodyInstance* Body = Mesh->GetBodyInstance(Bone))
		{
			const float Spin = FVector::DotProduct(Body->GetUnrealWorldAngularVelocityInRadians(), Length);
			Body->AddTorqueInRadians(Length * FMath::Clamp(12.0f * (RollWay * 2.5f - Spin), -30.0f, 30.0f), true, true);
		}
	}
	DriveWrithe();
}

void UImpactActiveBodyComponent::DriveCrawl(const float DeltaTime)
{
	AAlsCharacter* Character = GetCharacter();
	USkeletalMeshComponent* Mesh = Character->GetMesh();
	// One stroke an arm, 0.85 s each: the left arm pulls while the right one comes forward, then the other way round.
	const float Period = 1.7f;
	const float Cycle = FMath::Fmod(GroundTime / Period, 1.0f);
	const int32 Pulling = Cycle < 0.5f ? -1 : 1;
	const float Stroke = FMath::Sin(FMath::Fmod(Cycle * 2.0f, 1.0f) * UE_PI);
	const float Swing = 0.5f - 0.5f * FMath::Cos(Cycle * 2.0f * UE_PI);
	const float Ease = WritheBlend * WritheBlend * (3.0f - 2.0f * WritheBlend);
	for (int32 Index = 0; Index < Parts.Num(); ++Index)
	{
		Targets[Index] = FQuat::Slerp(Parts[Index].Held, FQuat::Slerp(PoseCrawlLeft[Index], PoseCrawlRight[Index], Swing), Ease).GetNormalized();
	}
	ApplyTargets(Mesh);
	if (Ease < 0.5f)
	{
		return;
	}

	// The pulling hand grips the floor where it lies at the start of its stroke.
	const TCHAR* HandName = Pulling < 0 ? TEXT("hand_l") : TEXT("hand_r");
	FBodyInstance* Hand = Mesh->GetBodyInstance(HandName);
	if (Pulling != PullingHand && Hand != nullptr)
	{
		PullingHand = Pulling;
		HandPlant = Hand->GetUnrealWorldTransform().GetLocation();
	}
	if (Hand != nullptr)
	{
		const FVector Off = HandPlant - Hand->GetUnrealWorldTransform().GetLocation();
		const FVector Speed = Hand->GetUnrealWorldVelocity();
		Hand->AddForce(FVector(120.0f * Off.X - 20.0f * Speed.X, 120.0f * Off.Y - 20.0f * Speed.Y, 0.0f), true, true);
	}

	// And the body slides towards it: the whole of him drawn ahead while the arm pulls, as the hand on the floor would. The rest of
	// the time the floor holds him. Towards where his head points, turned slowly away from the player.
	const FVector Length = GetBodyLength(Mesh);
	const FVector Ahead = FVector(Length.X, Length.Y, 0.0f).GetSafeNormal();
	const FBodyInstance* Pelvis = Mesh->GetBodyInstance(TEXT("pelvis"));
	if (Pelvis == nullptr || Ahead.IsNearlyZero())
	{
		return;
	}
	const float Moving = FVector::DotProduct(Pelvis->GetUnrealWorldVelocity(), Ahead);
	// Measured at 12 x (70 - speed): 85 cm in 8 s, the floor holding him back most of the stroke.
	const float Push = FMath::Clamp(30.0f * (Stroke * 90.0f - Moving), 0.0f, 2500.0f);
	for (FBodyInstance* Each : Mesh->Bodies)
	{
		if (Each != nullptr && Each != Hand && Each->IsInstanceSimulatingPhysics())
		{
			Each->AddForce(Ahead * Push, true, true);
		}
	}
	if (const APawn* Player = UGameplayStatics::GetPlayerPawn(this, 0))
	{
		const FVector Away = (Pelvis->GetUnrealWorldTransform().GetLocation() - Player->GetActorLocation()).GetSafeNormal2D();
		const float Turn = FMath::Clamp(FVector::CrossProduct(Ahead, Away).Z, -1.0f, 1.0f);
		for (const TCHAR* Bone : {TEXT("pelvis"), TEXT("spine_01"), TEXT("spine_02"), TEXT("spine_03")})
		{
			if (FBodyInstance* Body = Mesh->GetBodyInstance(Bone))
			{
				Body->AddTorqueInRadians(FVector(0.0f, 0.0f, FMath::Clamp(1.5f * Turn - 1.5f * Body->GetUnrealWorldAngularVelocityInRadians().Z, -1.5f, 1.5f)), true, true);
			}
		}
	}
}

void UImpactActiveBodyComponent::UpdateGround(const float DeltaTime)
{
	AAlsCharacter* Character = GetCharacter();
	USkeletalMeshComponent* Mesh = Character->GetMesh();
	const UImpactBodyComponent* Body = GetOwner()->FindComponentByClass<UImpactBodyComponent>();
	const EImpactConsciousness Mind = Body != nullptr ? Body->GetPhysiology().GetConsciousness() : EImpactConsciousness::Conscious;
	DownTime += DeltaTime;
	SettleTime = Mesh->GetPhysicsLinearVelocity(TEXT("pelvis")).Size() < 30.0f ? SettleTime + DeltaTime : 0.0f;

	if (Mind > EImpactConsciousness::Confused)
	{
		StopWrithe();
		return;
	}
	// Writhing once he has landed and come to rest; the poses come in over two seconds.
	if (!bWrithing && SettleTime > 0.6f)
	{
		StartWrithe();
	}
	if (!bWrithing)
	{
		return;
	}
	WritheTime += DeltaTime;
	GroundTime += DeltaTime;
	WritheBlend = FMath::Min(1.0f, WritheBlend + DeltaTime / (Ground == EGround::Crawl ? 1.0f : 2.0f));

	// Back on his feet when he can, after a few seconds down: ALS's get-up, from the pose the ragdoll is in.
	if (DownTime > 4.0f && SettleTime > 0.5f && CanGetUp())
	{
		StopWrithe();
		Character->StopRagdolling();
		if (Character->GetLocomotionAction() == AlsLocomotionActionTags::GettingUp)
		{
			State = EState::GettingUp;
			UE_LOG(LogImpact, Display, TEXT("[corps-actif] t=%.2f %s se releve"), GetWorld()->GetTimeSeconds(), *GetOwner()->GetName());
		}
		return;
	}

	// Unable to get up: a while writhing, then onto his belly and away, a few metres at a time.
	const bool bProne = GetChestFront(Mesh).Z < -0.6f;
	switch (Ground)
	{
	case EGround::Writhe:
		if (GroundTime > 3.0f && WantsToCrawl())
		{
			StartGround(bProne ? EGround::Crawl : EGround::Roll);
		}
		break;
	case EGround::Roll:
		if (bProne)
		{
			StartGround(EGround::Crawl);
		}
		else if (GroundTime > 3.5f)
		{
			UE_LOG(LogImpact, Display, TEXT("[corps-actif] t=%.2f %s ne parvient pas a se retourner"), GetWorld()->GetTimeSeconds(), *GetOwner()->GetName());
			StartGround(EGround::Writhe);
		}
		break;
	case EGround::Crawl:
		if (!WantsToCrawl() || GroundTime > 8.0f || GetChestFront(Mesh).Z > -0.2f)
		{
			UE_LOG(LogImpact, Display, TEXT("[corps-actif] t=%.2f %s arrete de ramper apres %.0f cm"), GetWorld()->GetTimeSeconds(), *GetOwner()->GetName(),
			       FVector::Dist2D(Mesh->GetSocketLocation(TEXT("pelvis")), CrawlFrom));
			StartGround(EGround::Writhe);
		}
		break;
	}
}

void UImpactActiveBodyComponent::UpdateMuscles(const float DeltaTime)
{
	AAlsCharacter* Character = GetCharacter();
	USkeletalMeshComponent* Mesh = Character != nullptr ? Character->GetMesh() : nullptr;
	if (Mesh == nullptr || State == EState::GettingUp)
	{
		return;
	}
	// Standing, the joints follow the animation. Down they follow no animation at all: ALS would have them reach for its flail
	// pose, and a body on the floor reaching for poses is a body that moves by itself. Writhing, they are aimed from here.
	const bool bStanding = State == EState::Standing;
	Mesh->bUpdateJointsFromAnimation = bStanding;
	// ALS rewrites every motor of a ragdoll in its own tick: down, they are set again each frame after it.
	const bool bForce = !bStanding;
	for (FMuscle& Part : Parts)
	{
		// Let go almost entirely where the round went in, and back over about 0.6 s.
		Part.Slack = FMath::Max(0.0f, Part.Slack - DeltaTime * 1.6f);
		const float Spring = Part.Strength * Tone * Part.ToneScale * (1.0f - 0.98f * Part.Slack);
		if (!bForce && Part.Applied >= 0.0f && FMath::Abs(Spring - Part.Applied) <= Part.Strength * 0.02f)
		{
			continue;
		}
		if (FConstraintInstance* Joint = Mesh->FindConstraintInstance(Part.Bone))
		{
			// Down, the damping stays as a muscle's resistance even when the spring is gone: a limp body is not a rag.
			const float Damp = bStanding ? DampFor(Spring) : FMath::Max(DampFor(Spring), DampFor(Part.Strength) * (State == EState::Dead ? 0.15f : 0.3f));
			Joint->SetAngularDriveParams(Spring, Damp, 0.0f);
		}
		Part.Applied = Spring;
	}
}

void UImpactActiveBodyComponent::TickComponent(const float DeltaTime, const ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	AAlsCharacter* Character = GetCharacter();
	if (Character == nullptr)
	{
		return;
	}
	if (!bReady)
	{
		// Here and not in BeginPlay: ALS sets the mesh up in its own BeginPlay, which may come after ours.
		Setup();
		return;
	}

	const UImpactBodyComponent* Body = GetOwner()->FindComponentByClass<UImpactBodyComponent>();
	const EImpactConsciousness Mind = Body != nullptr ? Body->GetPhysiology().GetConsciousness() : EImpactConsciousness::Conscious;
	const float Legs = Body != nullptr ? FMath::Min(Body->GetPhysiology().GetLimbCapability(EImpactLimb::LeftLeg),
	                                                Body->GetPhysiology().GetLimbCapability(EImpactLimb::RightLeg)) : 1.0f;
	Shock = FMath::Max(0.0f, Shock - DeltaTime);

	if (State != EState::Dead && Mind == EImpactConsciousness::Dead)
	{
		if (State == EState::Standing)
		{
			Fall(TEXT("mort"));
		}
		else if (State == EState::GettingUp)
		{
			Fall(TEXT("mort en se relevant"));
		}
		StopWrithe();
		State = EState::Dead;
	}
	else if (State == EState::Standing)
	{
		if (Mind >= EImpactConsciousness::SemiConscious)
		{
			Fall(TEXT("perd connaissance"));
		}
		else if (Legs < 0.25f)
		{
			Fall(TEXT("les jambes lachent"));
		}
		else if (Shock > ShockLimit)
		{
			Fall(TEXT("trop de balles d'un coup"));
		}
	}
	else if (State == EState::Down)
	{
		UpdateGround(DeltaTime);
	}
	else if (State == EState::GettingUp)
	{
		const FGameplayTag Action = Character->GetLocomotionAction();
		if (Action == AlsLocomotionActionTags::Ragdolling)
		{
			State = EState::Down;
		}
		else if (Action != AlsLocomotionActionTags::GettingUp)
		{
			// Up: an active body again, from the pose the get-up left him in.
			SetupStanding();
			State = EState::Standing;
			UE_LOG(LogImpact, Display, TEXT("[corps-actif] t=%.2f %s debout de nouveau"), GetWorld()->GetTimeSeconds(), *GetOwner()->GetName());
		}
		else if (Mind >= EImpactConsciousness::SemiConscious || Legs < 0.25f)
		{
			Fall(TEXT("retombe en se relevant"));
		}
	}

	// Standing, the full muscle. Down and awake, a twentieth of it, and the writhing tone on top while he writhes
	// (-ImpactActifSol=, a share of the full muscle). Out cold or dead, no spring at all.
	static const float WritheTone = CommandLineValue(TEXT("ImpactActifSol="), 0.1f, 0.0f);
	const bool bAwake = Mind <= EImpactConsciousness::Confused;
	const float Wanted = State == EState::Standing ? 1.0f
	                     : State == EState::Down && bAwake ? FMath::Lerp(0.05f, WritheTone, bWrithing ? WritheBlend : 0.0f)
	                     : State == EState::GettingUp ? 1.0f : 0.0f;
	Tone = State == EState::Standing ? FMath::FInterpTo(Tone, Wanted, DeltaTime, 4.0f) : Wanted;

	// An armed man standing holds his weapon with the animation's arms. Physical arms could not hold an aim: measured, the barrel
	// 15 degrees off the player with a rifle (5 with animated arms) and 45 with a pistol in two hands, whatever their strength.
	// The trunk, head and legs stay physical; down, everything is.
	if (State == EState::Standing)
	{
		const FGameplayTag Overlay = Character->GetOverlayMode();
		const bool bArmed = Overlay == AlsOverlayModeTags::Rifle || Overlay == AlsOverlayModeTags::PistolOneHanded
		                    || Overlay == AlsOverlayModeTags::PistolTwoHanded;
		if (bArmed != bArmsAnimated)
		{
			bArmsAnimated = bArmed;
			USkeletalMeshComponent* Mesh = Character->GetMesh();
			for (const TCHAR* Clavicle : {TEXT("clavicle_l"), TEXT("clavicle_r")})
			{
				Mesh->SetAllBodiesBelowSimulatePhysics(Clavicle, !bArmed, true);
				Mesh->SetAllBodiesBelowPhysicsBlendWeight(Clavicle, 1.0f, false, true);
			}
		}
	}
	UpdateMuscles(DeltaTime);
	if (State == EState::Standing && !bTargetsChecked && GetWorld()->GetTimeSeconds() > 2.0f)
	{
		CheckTargets(Character->GetMesh());
	}
	if (State == EState::Down && bWrithing)
	{
		if (Ground == EGround::Crawl)
		{
			DriveCrawl(DeltaTime);
		}
		else if (Ground == EGround::Roll)
		{
			DriveRoll(DeltaTime);
		}
		else
		{
			DriveWrithe();
		}
	}

	if (BendWatch >= 0.0f)
	{
		const USkeletalMeshComponent* Mesh = Character->GetMesh();
		const FVector Trunk = (Mesh->GetSocketLocation(TEXT("spine_03")) - Mesh->GetSocketLocation(TEXT("pelvis"))).GetSafeNormal();
		BendPeak = FMath::Max(BendPeak, FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct(Trunk, BendFrom), -1.0f, 1.0f))));
		BendWatch += DeltaTime;
		if (BendWatch > 0.7f)
		{
			UE_LOG(LogImpact, Display, TEXT("[corps-actif] reaction %s : torse plie de %.0f deg au plus"), *GetOwner()->GetName(), BendPeak);
			BendWatch = -1.0f;
		}
	}

	LogTimer -= DeltaTime;
	if (LogTimer <= 0.0f)
	{
		LogTimer = 1.0f;
		static const TCHAR* Names[] = {TEXT("debout"), TEXT("a terre"), TEXT("se releve"), TEXT("mort")};
		FString Writhe;
		if (bWrithing)
		{
			// How far the joints are from where they aim, on average, and how far the pelvis has slid since the writhing began.
			const USkeletalMeshComponent* Mesh = Character->GetMesh();
			float Error = 0.0f;
			for (int32 Index = 0; Index < Parts.Num(); ++Index)
			{
				Error += AngleBetween(GetCurrentRelative(Mesh, Parts[Index]), Targets[Index]);
			}
			Writhe = FString::Printf(TEXT(", %s (ecart %.0f deg, bassin %.0f cm)"), Ground == EGround::Crawl ? TEXT("rampe") : Ground == EGround::Roll ? TEXT("se retourne") : TEXT("se tord"),
			                         Error / FMath::Max(1, Parts.Num()),
			                         FVector::Dist2D(Mesh->GetSocketLocation(TEXT("pelvis")), WritheFrom));
		}
		UE_LOG(LogImpact, Display, TEXT("[corps-actif] t=%.1f %s %s, tonus %.2f, choc %.1f%s"), GetWorld()->GetTimeSeconds(), *GetOwner()->GetName(),
		       Names[static_cast<int32>(State)], Tone, Shock, *Writhe);
	}
}

FString UImpactActiveBodyComponent::GetStateText() const
{
	switch (State)
	{
	case EState::Down:
		return !bWrithing ? TEXT("à terre") : Ground == EGround::Crawl ? TEXT("rampe") : Ground == EGround::Roll ? TEXT("se retourne") : TEXT("se tord");
	case EState::GettingUp:
		return TEXT("se relève");
	case EState::Dead:
		return TEXT("inerte");
	default:
		return FString();
	}
}

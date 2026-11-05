#include "ImpactReactionComponent.h"

#include "AIController.h"
#include "AlsCharacter.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/OverlapResult.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Impact.h"
#include "ImpactBodyComponent.h"
#include "ImpactCivilianController.h"
#include "Misc/CommandLine.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsEngine/ConstraintInstance.h"
#include "PhysicsEngine/PhysicalAnimationComponent.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/PhysicsConstraintTemplate.h"
#include "Animation/AnimInstance.h"
#include "AlsAnimationInstance.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/AnimMontage.h"
#include "Animation/Skeleton.h"
#include "Utility/AlsConstants.h"
#include "Utility/AlsGameplayTags.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(ImpactReactionComponent)

namespace
{
	// -ImpactSolMixamo: on the ground, the Mixamo takes (Sol_Mixamo) instead of the made ones, and crawling in a take instead of by
	// forces. -ImpactReleveMixamo: getting up in a Mixamo take instead of ALS's.
	bool SolMixamo()
	{
		static const bool bOn = FParse::Param(FCommandLine::Get(), TEXT("ImpactSolMixamo"));
		return bOn;
	}

	bool ReleveMixamo()
	{
		static const bool bOn = FParse::Param(FCommandLine::Get(), TEXT("ImpactReleveMixamo"));
		return bOn;
	}

	UAnimSequenceBase* LoadMixamoTake(const TCHAR* Name)
	{
		return LoadObject<UAnimSequenceBase>(nullptr, *FString::Printf(TEXT("/Game/Impact/Anims/Sol_Mixamo/%s.%s"), Name, Name), nullptr,
		                                     LOAD_NoWarn | LOAD_Quiet);
	}

	// One bone of a take, in the mesh's space, at a given time: read off the take itself, not off what the graph happens to be
	// showing. A bone the take has no track for keeps its reference pose, as the engine draws it.
	bool TakeBoneInMesh(const UAnimSequenceBase* TakeBase, const float Time, const FName BoneName, FTransform& Out)
	{
		const UAnimSequence* Take = Cast<UAnimSequence>(TakeBase);
		const USkeleton* Skeleton = Take != nullptr ? Take->GetSkeleton() : nullptr;
		if (Skeleton == nullptr)
		{
			return false;
		}
		const FReferenceSkeleton& Ref = Skeleton->GetReferenceSkeleton();
		const int32 Bone = Ref.FindBoneIndex(BoneName);
		if (Bone == INDEX_NONE)
		{
			return false;
		}
		const FAnimExtractContext Context(static_cast<double>(FMath::Clamp(Time, 0.0f, Take->GetPlayLength())));
		Out = FTransform::Identity;
		for (int32 Each = Bone; Each != INDEX_NONE; Each = Ref.GetParentIndex(Each))
		{
			FTransform Local = Ref.GetRefBonePose()[Each];
			Take->GetBoneTransform(Local, FSkeletonPoseBoneIndex(Each), Context, false);
			Out = Out * Local;
		}
		return true;
	}
}

namespace
{
	/**
	 * How hard every muscle pulls, as a multiple of what the physics asset asks for. -ImpactTonus=N sets it.
	 * Four by default, and that figure is measured: a body simulated at one could not keep up with the
	 * animation its muscles are meant to reproduce, so its cadence fell from 5,0 to 3,3 steps a second and
	 * it shook; at four both came back.
	 */
	float MuscleScale()
	{
		static float Scale = -1.0f;
		if (Scale < 0.0f)
		{
			float Parsed = 4.0f;
			Scale = FParse::Value(FCommandLine::Get(), TEXT("ImpactTonus="), Parsed) ? FMath::Max(0.05f, Parsed) : 4.0f;
		}
		return Scale;
	}

	/** Muscle multiplier for someone writhing on the ground (-ImpactDouleurForce=, 2 by default). */
	float GroundPainForce()
	{
		static float PainForce = -1.0f;
		if (PainForce < 0.0f)
		{
			float Parsed = 2.0f;
			PainForce = FParse::Value(FCommandLine::Get(), TEXT("ImpactDouleurForce="), Parsed) ? FMath::Max(0.0f, Parsed) : 2.0f;
		}
		return PainForce;
	}

	/**
	 * Switches that each cut one of the things measured as pulling the body during the ground posture, so they
	 * can be measured one at a time. All off by default: the game does not change until one has been seen.
	 *   -ImpactSolSansMuscles  the PhysicalAnimation muscles fade out as the joint drives take over. They are
	 *                          anchored to kinematic actors, so their pull is an outside force on the whole body,
	 *                          six times the joint drives on the trunk, and neither ramped nor paced.
	 *   -ImpactSolMainsLibres  no hand springs during the posture: a hand that cannot reach the wound keeps
	 *                          pulling with an outside force of several hundred newtons.
	 *   -ImpactSolAmortiActif  the joint damping actually applied. PA_Als has no velocity drive, and Chaos then
	 *                          ignores the damping given to SetAngularDriveParams.
	 */
	bool GroundFlag(const TCHAR* Name)
	{
		return FParse::Param(FCommandLine::Get(), Name);
	}

	/**
	 * -ImpactAnimsPretes. Whether an animation's compressed data is there to be read. Ours are compressed when the game loads
	 * them, in the background; loaded at the first round (LoadObject) and read at once (TakeMovesTheTrunk samples it), a take
	 * whose compression had not finished was read as empty, and the engine stopped on "Array index out of bounds: 49 into an
	 * array of size 0" - the crash that came back every few games, three and a half seconds in, at the first hit. One not ready
	 * is not played (the reaction goes without it); UImpactGameplaySubsystem compresses them all at the start.
	 */
	bool AnimReady(const UAnimSequenceBase* Take)
	{
		static const bool bCheck = FParse::Param(FCommandLine::Get(), TEXT("ImpactAnimsPretes"));
		const UAnimSequence* Sequence = Cast<UAnimSequence>(Take);
		if (!bCheck || Sequence == nullptr)
		{
			return Take != nullptr;
		}
		const bool bReady = Sequence->IsCompressedDataValid();
		if (!bReady)
		{
			UE_LOG(LogImpact, Warning, TEXT("[anims] %s pas encore prete (compression en cours) : pas jouee"), *Sequence->GetName());
		}
		return bReady;
	}

	/**
	 * The muscle a man writhing on the ground has before the ground posture takes over.
	 * -ImpactSolCalmeAvant (with -ImpactSolRagdoll only) keeps the tone of the fall (-ImpactSolForceAvant=, 0.32 by
	 * default) instead of the pain force. Those muscles push each bone towards the take with no push back on its
	 * parent, so on a body lying on a floor they are an outside force, and it travels: measured with the pain force,
	 * 124 and 489 cm across the floor between the end of the fall and the start of the posture, 230 cm/s at worst.
	 */
	float WritheMuscleForce()
	{
		static float Calm = -2.0f;
		if (Calm < -1.0f)
		{
			Calm = -1.0f;
			if (GroundFlag(TEXT("ImpactSolCalmeAvant")) && GroundFlag(TEXT("ImpactSolRagdoll")))
			{
				float Parsed = 0.32f;
				Calm = FParse::Value(FCommandLine::Get(), TEXT("ImpactSolForceAvant="), Parsed) ? FMath::Max(0.0f, Parsed) : 0.32f;
			}
		}
		return Calm >= 0.0f ? Calm : GroundPainForce();
	}
}

namespace
{
	// Whether a take actually moves its trunk, or only holds a pose. This exists because for a while every
	// retargeted Mixamo take did the latter: 79 bone tracks in which every bone - spine, arms, legs alike -
	// held the same value at every time. Played whole-body that read as a body snapping into a frozen
	// posture, which is what was reported as ridiculous; read for its trunk displacement it yielded exactly
	// zero degrees.
	//
	// The cause was in the retargeter, not in the takes. Since UE 5.6 an IK Retargeter is a stack of ops
	// and each op carries its own chain mapping; set_ik_rig() only fills in the global rigs, so the ops kept
	// ik_rig_asset = None and an empty chains_to_retarget. auto_map_chains then had nothing to map and said
	// nothing about it, and the retargeter emitted the target's retarget pose, the same at every time.
	// Tools/retarget_setup.py now calls assign_ik_rig_to_all_ops() and refuses to go on unless the mapping
	// came out non-empty; Tools/retarget_batch.py redoes the fourteen takes; Tools/check_anims.py measures
	// them. All fourteen now carry between 5 and 31 degrees of spine travel.
	//
	// The check is kept as a guard, not as a crutch: if a take ever comes back still, this says so in the
	// log with its name, and that take is not played.
	bool TakeMovesTheTrunk(UAnimSequence* Take)
	{
		static TMap<TWeakObjectPtr<UAnimSequence>, bool> Known;
		if (const bool* Cached = Known.Find(Take))
		{
			return *Cached;
		}

		bool bMoves = false;
		const USkeleton* Skeleton = Take != nullptr ? Take->GetSkeleton() : nullptr;
		if (Skeleton != nullptr)
		{
			const FReferenceSkeleton& Ref = Skeleton->GetReferenceSkeleton();
			const float Length = Take->GetPlayLength();
			for (const TCHAR* Name : {TEXT("spine_01"), TEXT("spine_02"), TEXT("spine_03")})
			{
				const int32 Index = Ref.FindBoneIndex(FName(Name));
				if (Index == INDEX_NONE)
				{
					continue;
				}
				FTransform First;
				Take->GetBoneTransform(First, FSkeletonPoseBoneIndex(Index), FAnimExtractContext(0.0), false);
				for (int32 Step = 1; Step <= 6; ++Step)
				{
					FTransform Later;
					const double When = Length * Step / 7.0;
					Take->GetBoneTransform(Later, FSkeletonPoseBoneIndex(Index), FAnimExtractContext(When), false);
					if (FMath::RadiansToDegrees(First.GetRotation().AngularDistance(Later.GetRotation())) > 1.5f)
					{
						bMoves = true;
						break;
					}
				}
				if (bMoves)
				{
					break;
				}
			}
		}

		if (!bMoves)
		{
			UE_LOG(LogImpact, Warning, TEXT("[anim] %s ne bouge pas : son torse garde la meme pose du debut a la fin, "
				       "le reciblage n'a rien produit. Elle n'est pas utilisee."), *GetNameSafe(Take));
		}
		Known.Add(Take, bMoves);
		return bMoves;
	}
}


namespace
{
	struct FBoneSpec
	{
		const TCHAR* Name;
		const TCHAR* Parent; // nullptr: attached to the (animated) pelvis
		float Strength;      // muscle strength of a healthy body
	};

	// Bodies of the ALS physics asset that react. Pelvis and root stay animated: the capsule carries the body.
	const FBoneSpec BoneSpecs[] = {
		{TEXT("spine_01"), nullptr, 3000.0f},
		{TEXT("spine_02"), TEXT("spine_01"), 2600.0f},
		{TEXT("spine_03"), TEXT("spine_02"), 2300.0f},
		{TEXT("neck_01"), TEXT("spine_03"), 1200.0f},
		{TEXT("head"), TEXT("neck_01"), 700.0f},
		{TEXT("clavicle_l"), TEXT("spine_03"), 3500.0f},
		{TEXT("upperarm_l"), TEXT("clavicle_l"), 2400.0f},
		{TEXT("lowerarm_l"), TEXT("upperarm_l"), 1400.0f},
		{TEXT("hand_l"), TEXT("lowerarm_l"), 500.0f},
		{TEXT("clavicle_r"), TEXT("spine_03"), 3500.0f},
		{TEXT("upperarm_r"), TEXT("clavicle_r"), 2400.0f},
		{TEXT("lowerarm_r"), TEXT("upperarm_r"), 1400.0f},
		{TEXT("hand_r"), TEXT("lowerarm_r"), 500.0f},
		{TEXT("thigh_l"), nullptr, 3200.0f},
		{TEXT("calf_l"), TEXT("thigh_l"), 2400.0f},
		{TEXT("foot_l"), TEXT("calf_l"), 1000.0f},
		{TEXT("thigh_r"), nullptr, 3200.0f},
		{TEXT("calf_r"), TEXT("thigh_r"), 2400.0f},
		{TEXT("foot_r"), TEXT("calf_r"), 1000.0f},
	};

	constexpr float BodyMassKg = 70.0f;
	constexpr float BalanceOmega = 3.13f;   // sqrt(g / centre of mass height) for a 1 m high centre of mass
	constexpr float StepGain = 2.5f;        // game tuning: how much of the capture distance becomes actual steps
	constexpr float BlendOutTime = 0.35f;   // physics fades back into animation
	constexpr float ToneRecoveryTime = 0.35f;

	/** Push given to the hit bone, kg·cm/s. Far above a real bullet's momentum: this is how the hit reads on screen. */
	float ReactionImpulse(const EImpactWeapon Weapon)
	{
		switch (Weapon)
		{
		case EImpactWeapon::Rifle:
			return 2200.0f;
		case EImpactWeapon::Pistol:
			return 3800.0f;
		default:
			return 2000.0f;
		}
	}
}

UImpactReactionComponent::UImpactReactionComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;
}

AAlsCharacter* UImpactReactionComponent::GetCharacter() const
{
	return Cast<AAlsCharacter>(GetOwner());
}

void UImpactReactionComponent::BeginPlay()
{
	Super::BeginPlay();

	AAlsCharacter* Character = GetCharacter();
	if (Character == nullptr)
	{
		return;
	}

	// Run after the character so ALS has set its pose and ragdoll motors before we adjust them.
	PrimaryComponentTick.AddPrerequisite(Character, Character->PrimaryActorTick);

	for (const FBoneSpec& Spec : BoneSpecs)
	{
		FBone& Bone = Bones.AddDefaulted_GetRef();
		Bone.Name = Spec.Name;
		Bone.BaseStrength = Spec.Strength;
		Bone.Parent = Spec.Parent != nullptr ? FindBone(Spec.Parent) : INDEX_NONE;
	}

	PhysicalAnimation = NewObject<UPhysicalAnimationComponent>(Character, TEXT("ImpactMuscles"));
	PhysicalAnimation->RegisterComponent();
	PhysicalAnimation->SetSkeletalMeshComponent(Character->GetMesh());

	// Three ticks, one order: the mesh evaluates the animation, we check that the pose came out of it, then
	// the muscles read it. SetSkeletalMeshComponent above already made the muscles wait for the mesh; this
	// slots us in between, which is the only place from which GuardMusclePose can do any good.
	PrimaryComponentTick.AddPrerequisite(Character->GetMesh(), Character->GetMesh()->PrimaryComponentTick);
	PhysicalAnimation->PrimaryComponentTick.AddPrerequisite(this, PrimaryComponentTick);

	PrepareMesh();
	ResetBones();
}

void UImpactReactionComponent::PrepareMesh() const
{
	const AAlsCharacter* Character = GetCharacter();
	USkeletalMeshComponent* Mesh = Character->GetMesh();

	// Bodies need physics collision to be simulated. On his feet the mesh goes back to ignoring pawns,
	// which is what the engine's CharacterMesh profile does and what it is for: upright, the capsule is
	// live and the upper body is physical, so a blocking answer here would have the arms shoving their
	// own capsule about. On the ground it is the opposite - see UpdateDown, where it is set to block.
	Mesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Mesh->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);

	// The physics asset joints have motors aimed at the bind pose (arms spread out). ALS only uses them while ragdolling;
	// during a hit reaction they would pull the arms up, so they are switched off and only our muscles drive the body.
	Mesh->SetAllMotorsAngularDriveParams(0.0f, 0.0f, 0.0f);
}

int32 UImpactReactionComponent::FindBone(const FName Name) const
{
	return Bones.IndexOfByPredicate([Name](const FBone& Bone) { return Bone.Name == Name; });
}

bool UImpactReactionComponent::IsInSubtree(int32 Bone, const int32 Root) const
{
	while (Bone != INDEX_NONE)
	{
		if (Bone == Root)
		{
			return true;
		}
		Bone = Bones[Bone].Parent;
	}
	return false;
}

int32 UImpactReactionComponent::Distance(const int32 A, const int32 B) const
{
	TMap<int32, int32> Ancestors;
	int32 Depth = 0;
	for (int32 Bone = A; Bone != INDEX_NONE; Bone = Bones[Bone].Parent)
	{
		Ancestors.Add(Bone, Depth++);
	}

	Depth = 0;
	for (int32 Bone = B; Bone != INDEX_NONE; Bone = Bones[Bone].Parent)
	{
		if (const int32* Found = Ancestors.Find(Bone))
		{
			return *Found + Depth;
		}
		++Depth;
	}
	return 6; // different chains (leg and torso): far apart
}

void UImpactReactionComponent::GuardMusclePose()
{
	// -ImpactSansGarde puts the old behaviour back, crash included. It is how the diagnosis below was
	// proved rather than argued: same binary, same bench, the guard on and off. Do not ship with it.
	static const bool bGuarded = !FParse::Param(FCommandLine::Get(), TEXT("ImpactSansGarde"));

	const AAlsCharacter* Character = GetCharacter();
	USkeletalMeshComponent* Mesh = Character != nullptr ? Character->GetMesh() : nullptr;
	if (!bGuarded || Mesh == nullptr || PhysicalAnimation == nullptr || Mesh->GetSkeletalMeshAsset() == nullptr)
	{
		return;
	}

	// This is the whole of the intermittent crash that was killing the limp bench, reported at about one
	// run in three.
	//
	// UPhysicalAnimationComponent::UpdateTargetActors aims each muscle at the bone's animated pose. For a
	// local-space drive, which is what we use, it reaches that pose through ComputeLocalSpaceTargetTM, and
	// that function does LocalTransforms[BoneIndex] with no bounds check at all, where LocalTransforms is
	// a copy of the mesh's bone space transforms.
	//
	// Those transforms are not always there. While the mesh evaluates its animation on a worker thread the
	// engine swaps the array out into the evaluation context and leaves an empty one behind, and the
	// evaluation context is cleared after every frame, so the array left behind has length zero. It is
	// swapped back when the task completes. The muscles tick inside that window on purpose, and they get
	// away with it because GetBoneSpaceTransforms blocks on the task first -- except that the block is
	// conditional on FTaskBase::IsAwaitable, and on the frames where it does not hold the engine reads the
	// empty array and asserts: "Array index out of bounds: 55 into an array of size 0". 55 is the reference
	// skeleton index of one of the nineteen bones we drive, and the array of size 0 is the pose that was
	// swapped out. Which bone it is was never established and does not matter: every one of them is read
	// the same way.
	//
	// It needs a get-up because that is the one moment the drive is rebuilt from nothing: our tick returns
	// early for the whole of the GettingUp montage, so no bone simulates, and when the montage ends
	// UpdateBones turns the upper body physical again and the muscles create their target actors on a frame
	// that is already busy recreating the mesh physics state.
	//
	// That is measured, not argued. All seven crashes kept in Saved/Crashes print "se releve", then the
	// first line of the resumed tick ("main droite vers plaie cuisse") 1,00 s later -- 1,001 / 1,001 /
	// 1,004 / 1,008 / 1,044 s, a constant, which is the length of the get-up montage -- then the assertion
	// two or three frames after that. And the journal line below caught the state itself on the fourth
	// run of the control campaign: 79 bones expected, zero in the array, one frame after the resume,
	// 0,98 s after the get-up. It never happens on the escape bench because nobody falls there.
	// Saved/_preuves_reprise/PLANTAGE_INTERMITTENT.md has the tables.
	//
	// Reading the view below does the block the engine only sometimes does, unconditionally: it waits for
	// any evaluation still in flight and swaps the result back in. This costs nothing we were not already
	// paying, since UpdateTargetActors asks for the same array one tick function later. If the pose is
	// still not there afterwards, there is nothing for the muscles to aim at this frame, so they do not run
	// -- the tick task rechecks IsTickFunctionEnabled when it fires, so disabling it here is enough.
	const int32 Expected = Mesh->GetSkeletalMeshAsset()->GetRefSkeleton().GetNum();
	const bool bPoseReady = Expected > 0
	                        && Mesh->GetBoneSpaceTransformsView().Num() >= Expected
	                        && Mesh->GetEditableComponentSpaceTransforms().Num() >= Expected
	                        && Mesh->GetComponentSpaceTransforms().Num() >= Expected;

	if (!bPoseReady)
	{
		// Loud on the first few, then quiet: a handful of these is the defect being caught, a stream of
		// them would mean the muscles are off for good and that is worth seeing in the journal.
		++PoseGuardSkips;
		if (PoseGuardSkips <= 8 || PoseGuardSkips % 100 == 0)
		{
			UE_LOG(LogImpact, Warning,
			       TEXT("[muscles] t=%.2f %s pose absente, muscles suspendus pour cette image ")
			       TEXT("(os %d, locaux %d, composante %d/%d, total %d)"),
			       GetWorld() != nullptr ? GetWorld()->GetTimeSeconds() : 0.0f, *GetOwner()->GetName(),
			       Expected, Mesh->GetBoneSpaceTransformsView().Num(),
			       Mesh->GetEditableComponentSpaceTransforms().Num(), Mesh->GetComponentSpaceTransforms().Num(),
			       PoseGuardSkips);
		}
	}

	PhysicalAnimation->SetComponentTickEnabled(bPoseReady);
}

void UImpactReactionComponent::ResetBones()
{
	AAlsCharacter* Character = GetCharacter();
	USkeletalMeshComponent* Mesh = Character->GetMesh();
	for (FBone& Bone : Bones)
	{
		Bone.Tone = Bone.RestTone;
		Bone.Hold = 0.0f;
		Bone.Blend = 0.0f;
		Bone.AppliedStrength = -1.0f;
		if (Bone.bSimulating)
		{
			Mesh->SetBodySimulatePhysics(Bone.Name, false);
			Bone.bSimulating = false;
		}
	}
	StaggerRemaining = 0.0f;
}

bool UImpactReactionComponent::IsDown() const
{
	const AAlsCharacter* Character = GetCharacter();
	return Character != nullptr && (Character->GetLocomotionAction() == AlsLocomotionActionTags::Ragdolling
	                                 || Character->GetLocomotionAction() == AlsLocomotionActionTags::GettingUp
	                                 || Character->GetLocomotionAction() == AlsLocomotionActionTags::LyingDown);
}

bool UImpactReactionComponent::IsLyingDown() const
{
	const AAlsCharacter* Character = GetCharacter();
	return Character != nullptr && Character->GetLocomotionAction() == AlsLocomotionActionTags::LyingDown;
}

FString UImpactReactionComponent::GetStateText() const
{
	const AAlsCharacter* Character = GetCharacter();
	if (Character == nullptr)
	{
		return FString();
	}
	if (Character->GetLocomotionAction() == AlsLocomotionActionTags::Ragdolling)
	{
		return bLimp ? TEXT("inerte") : TEXT("à terre");
	}
	if (Character->GetLocomotionAction() == AlsLocomotionActionTags::LyingDown)
	{
		return TEXT("à terre");
	}
	if (Character->GetLocomotionAction() == AlsLocomotionActionTags::GettingUp)
	{
		return TEXT("se relève");
	}
	if (IsStaggering())
	{
		return TEXT("recule");
	}
	return FString();
}

void UImpactReactionComponent::SetLimbTone(const FName LimbRoot, const float Tone)
{
	const int32 Root = FindBone(LimbRoot);
	if (Root == INDEX_NONE)
	{
		return;
	}
	for (int32 i = 0; i < Bones.Num(); ++i)
	{
		if (IsInSubtree(i, Root))
		{
			Bones[i].RestTone = Tone;
		}
	}
}

void UImpactReactionComponent::ApplyBulletHit(const FImpactBulletHit& Bullet, const float Strength)
{
	// Lying animated: a bullet makes him a body again first, so the impulse below has something to move.
	ReturnToRagdoll(TEXT("balle"), false);
	AAlsCharacter* Character = GetCharacter();
	if (Character == nullptr || IsDown())
	{
		if (Character != nullptr && Character->GetLocomotionAction() == AlsLocomotionActionTags::Ragdolling)
		{
			// Already on the ground: the bullet still moves the limb it hits.
			LastGroundPushTime = GetWorld()->GetTimeSeconds();
			Character->GetMesh()->AddImpulseAtLocation(Bullet.Direction * ReactionImpulse(Bullet.Weapon) * 0.6f * Strength,
			                                           Bullet.Hit.ImpactPoint, Bullet.Hit.BoneName);
		}
		return;
	}

	// [elan]: a man hit while he runs is followed for a second - how much of his speed he keeps, standing or falling. Read off his
	// pelvis, not the movement component: that one is zeroed by a paused path and means nothing once he is a ragdoll.
	if (ElanTime < 0.0f && Character->GetVelocity().Size2D() > 200.0f)
	{
		ElanTime = 0.0f;
		ElanSpeed = Character->GetVelocity().Size2D();
		ElanFrom = Character->GetMesh()->GetSocketLocation(TEXT("pelvis"));
		ElanLast = ElanFrom;
		ElanSmooth = ElanSpeed;
		ElanLowest = ElanSpeed;
	}

	const FString HitName = Bullet.Hit.BoneName.ToString();
	const bool bLeft = HitName.EndsWith(TEXT("_l"));
	FName RegionRoot = TEXT("spine_01");
	bool bArm = false;
	bool bLeg = false;
	if (HitName.StartsWith(TEXT("clavicle")) || HitName.StartsWith(TEXT("upperarm")) || HitName.StartsWith(TEXT("lowerarm")) || HitName.StartsWith(TEXT("hand")))
	{
		RegionRoot = bLeft ? TEXT("clavicle_l") : TEXT("clavicle_r");
		bArm = true;
	}
	else if (HitName.StartsWith(TEXT("thigh")) || HitName.StartsWith(TEXT("calf")) || HitName.StartsWith(TEXT("foot")) || HitName.StartsWith(TEXT("ball")))
	{
		RegionRoot = bLeft ? TEXT("thigh_l") : TEXT("thigh_r");
		bLeg = true;
	}

	const int32 Root = FindBone(RegionRoot);
	int32 HitBone = FindBone(Bullet.Hit.BoneName);
	if (HitBone == INDEX_NONE)
	{
		HitBone = Root; // pelvis: the reaction starts at the lower spine
	}

	// Muscles around the hit go slack, the hit bone most.
	const float Hold = 0.45f + 0.15f * Strength;
	for (int32 i = 0; i < Bones.Num(); ++i)
	{
		FBone& Bone = Bones[i];
		if (IsInSubtree(i, Root))
		{
			// Divided by the muscle scale on purpose: what a bullet does is make the muscle let go, and it
			// lets go to a strength that has nothing to do with how braced the man was a moment earlier.
			// Left proportional, a body toned at four times only slackened to four times the old slack,
			// so the round could not move it: measured, the trunk of a man shot in the back bent 24 deg
			// where his own running bent 32, which is a reaction smaller than no reaction.
			float Dip = FMath::Lerp(0.2f, 0.75f, FMath::Clamp(Distance(i, HitBone) / 4.0f, 0.0f, 1.0f)) / MuscleScale();
			const FString BoneName = Bone.Name.ToString();
			const bool bArmBone = BoneName.Contains(TEXT("arm")) || BoneName.StartsWith(TEXT("hand")) || BoneName.StartsWith(TEXT("clavicle"));
			if (bArmBone && !IsInSubtree(HitBone, i) && !IsInSubtree(i, HitBone))
			{
				// An arm that was not hit keeps most of its tone: it moves with the torso, it does not fly up.
				Dip = FMath::Max(Dip, 0.8f / MuscleScale());
			}
			Bone.Tone = FMath::Min(Bone.Tone, Dip * Bone.RestTone);
			Bone.Hold = FMath::Max(Bone.Hold, Hold);
		}
		else if (bArm && IsInSubtree(i, FindBone(TEXT("spine_01"))))
		{
			// An arm hit still shakes the shoulders a little.
			Bone.Tone = FMath::Min(Bone.Tone, 0.8f * Bone.RestTone);
			Bone.Hold = FMath::Max(Bone.Hold, 0.3f);
		}
		else if (!bArm && !bLeg && HitName == TEXT("pelvis") && Bone.Parent == INDEX_NONE && Bone.Name != TEXT("spine_01"))
		{
			// Pelvis hit: both legs give a little.
			Bone.Tone = FMath::Min(Bone.Tone, 0.6f * Bone.RestTone);
			Bone.Hold = FMath::Max(Bone.Hold, 0.4f);
		}
	}

	// Simulation must be on before the push is applied.
	UpdateBones(0.0f);

	USkeletalMeshComponent* Mesh = Character->GetMesh();
	const float Impulse = ReactionImpulse(Bullet.Weapon) * Strength;
	Mesh->AddImpulseAtLocation(Bullet.Direction * Impulse, Bullet.Hit.ImpactPoint, Bones[HitBone].Name);

	// Someone caught off guard turns towards whoever shot at him and catches the push with his feet. Someone
	// already running away does not: he keeps running. This used to happen on every single hit and was never
	// undone, so a fleeing man turned to face the shooter and carried on walking backwards away from him.
	const AImpactCivilianController* Brain = Cast<AImpactCivilianController>(Character->GetController());
	const bool bAlreadyRunning = Brain != nullptr && Brain->GetState() == EImpactCivilianState::Fleeing;
	// -ImpactElanGarde: nor does anyone already on the move. A soldier running to cover was told to walk and to face the shooter,
	// and during the step that followed nothing told him otherwise (UpdateStagger, which undid it, has no caller; his brain does
	// not think while he staggers): he braked to a walk and pivoted round. Reported from play: "il s'arrete brusquement".
	static const bool bKeepMomentum = FParse::Param(FCommandLine::Get(), TEXT("ImpactElanGarde"));
	const bool bOnTheMove = bKeepMomentum && Character->GetVelocity().Size2D() > 150.0f;
	if (!bAlreadyRunning && !bOnTheMove)
	{
		if (AAIController* Controller = Cast<AAIController>(Character->GetController()))
		{
			if (AActor* Shooter = Bullet.Instigator.Get())
			{
				Controller->SetFocus(Shooter);
			}
		}
		Character->SetDesiredRotationMode(AlsRotationModeTags::ViewDirection);
		Character->SetDesiredGait(AlsGaitTags::Walking);
		bTurnedToThreat = true;
	}

	// A whole-body reaction, played under the physics rather than instead of it. This is the part forces
	// alone cannot do: they move a body, they do not make it act. Which one is played depends on where the
	// bullet landed, and several takes exist for the same place so the same shot never reads twice the same.
	{
		struct FHitAnims
		{
			const TCHAR* Match;
			int32 Count;
			const TCHAR* Paths[5];
		};
		static const FHitAnims Table[] = {
			{TEXT("head"), 5, {TEXT("Big_Hit_To_Head"), TEXT("Big_Hit_To_Head_1"), TEXT("Big_Hit_To_Head_2"),
			                   TEXT("Head_Hit"), TEXT("Hit_On_Side_Of_Head")}},
			{TEXT("neck"), 2, {TEXT("Head_Hit"), TEXT("Hit_On_Side_Of_Head")}},
			{TEXT("spine_01"), 2, {TEXT("Big_Stomach_Hit"), TEXT("Stomach_Hit")}},
			{TEXT("spine_02"), 3, {TEXT("Rib_Hit"), TEXT("Hit_To_Body"), TEXT("Hit_To_Body_1")}},
			{TEXT("spine_03"), 3, {TEXT("Hit_To_Body"), TEXT("Hit_To_Body_1"), TEXT("Impact_Torse_Face_v01")}},
			{TEXT("pelvis"), 1, {TEXT("Hit_On_Side_Of_Body")}},
			{TEXT("clavicle"), 2, {TEXT("Shoulder_Hit_And_Fall"), TEXT("Shoulder_Hit_And_Fall_1")}},
			{TEXT("upperarm"), 2, {TEXT("Shoulder_Hit_And_Fall"), TEXT("Shoulder_Hit_And_Fall_1")}},
		};

		const FString Part = Bones[HitBone].Name.ToString();
		const FHitAnims* Chosen = nullptr;
		for (const FHitAnims& Entry : Table)
		{
			if (Part.StartsWith(Entry.Match))
			{
				Chosen = &Entry;
				break;
			}
		}

		// The take is not played. It is read: only the displacement it puts on the trunk is kept, and the
		// muscles carry it out over whatever the character is already doing. Playing them whole-body froze
		// the legs of a running man while his chest acted out a standing reaction. See UpdateHitPose.
		// -ImpactFullBody plays them whole-body again, the way it was done first. It is kept for one reason:
		// a claim that the new way fixes the frozen legs is worth nothing without the old numbers beside it
		// on the same bench.
		static const bool bWholeBody = FParse::Param(FCommandLine::Get(), TEXT("ImpactFullBody"));
		// Three ways of reacting to a round in the back of a man running at 6,5 m/s, all measured on the
		// bench -ImpactTest=fuite: the slide of his planted foot, and the abruptness of his trunk compared
		// with his own running. These numbers were taken again once the retargeting was fixed, because the
		// first set was taken when every Mixamo take was a still frame and measured a body snapping onto a
		// frozen posture rather than a reaction. Three to four runs each, the spread given.
		//
		//                          slide           trunk angle    trunk abruptness   running jitter
		//   whole-body take        0,59 -> 0,68-0,74  x1,52-1,86  x4,08-4,51         267-274 deg/s
		//   nothing but the push   0,60 -> 0,54-0,56  x0,93-1,22  x1,51-2,84         272-284 deg/s
		//   Spine slot (this)      0,60 -> 0,55-0,57  x1,21-1,56  x2,15-3,56         341-431 deg/s
		//
		// Playing a real take over the whole body makes the planted foot slide at 68 to 74 % of body speed:
		// that is the "his legs stop moving" that was reported, and it is worse with takes that move than
		// it was with still ones. The Spine slot leaves the slide where running alone puts it.
		//
		// Two honest readings of the other columns. The Spine slot's "running jitter" is the highest of the
		// three, but it is not vibration: on this bench the man is hit once at t=2 before the measured shot
		// at t=4, and the trunk is measured against his own hips, so his first reaction still shows inside
		// the reference window. Whole-body does not show it there because it turns the hips with the chest,
		// which is the same thing that makes the foot slide. And the abruptness after the shot, x2,15 to
		// x3,56, is a real snap now: the takes start from a standing stance far from a running trunk and
		// are entered over 0,06 s. Whether that reads as a hit or as a jolt has not been judged by eye.
		//
		// An earlier attempt read the take onto physically simulated trunk bones instead. It fixed the
		// slide and was worse than doing nothing on every other count, because a simulated bone follows a
		// spring towards the animated pose and a spring always lags and attenuates; it also doubled the
		// trunk's jitter while running, which is the vibration that was reported. It is not in the code
		// any more, so its old row is left out rather than quoted beside numbers it cannot be compared to.
		//
		// So the take is played where it belongs: ALS's own Spine slot. AB_Als_Layering masks that slot to
		// spine_01 and three levels below, so the trunk, neck and shoulders take the reaction while the
		// pelvis and the legs stay with locomotion. Nothing goes through physics. The slot is held shut by
		// a ModifyCurve that pins every Layer*Slot curve to zero, so UpdateHitPose opens it for the
		// duration and closes it again. -ImpactFullBody plays the take over the whole body as before and
		// -ImpactNoPose drops it, so the three can still be put side by side.
		static const bool bNoPose = FParse::Param(FCommandLine::Get(), TEXT("ImpactNoPose"));
		// -ImpactCourseSansPose: no take for a man hit at a run. The takes were recorded standing, and their first frame pulled a
		// running trunk into a standing one: "comme s'il jouait une nouvelle animation a la pose zero". Measured on the "fuite" bench
		// (Saved/_preuves_reprise/MELANGE_ANIM_RAGDOLL.md), trunk p95 in the 0,8 s after the round: with the take 600-780 deg/s
		// whatever the blend (0,12 s, 0,35 s, inertial), without it 278-323 - under his own running, ~410. He still reacts: the
		// impulse, the slack muscles, the hand going to the wound. A man standing or walking keeps his take.
		static const bool bNoPoseAtRun = FParse::Param(FCommandLine::Get(), TEXT("ImpactCourseSansPose"));
		const bool bAtRun = bNoPoseAtRun && Character->GetVelocity().Size2D() > 250.0f;

		if (Chosen != nullptr && bVoluntaryControl && !IsDown() && !bNoPose && !bAtRun)
		{
			// Each zone of the body has its own takes again, now that the retargeting carries motion. A
			// take that turns out to be a still frame is dropped rather than substituted: the body then
			// reacts on the impulse and the balance loop alone, which measured better than a frozen pose.
			const TCHAR* Name = Chosen->Paths[FMath::RandHelper(Chosen->Count)];
			const FString Path = FString::Printf(TEXT("/Game/Impact/Anims/%s.%s"), Name, Name);
			UAnimSequence* Source = LoadObject<UAnimSequence>(nullptr, *Path);
			if (Source != nullptr && (!AnimReady(Source) || !TakeMovesTheTrunk(Source)))
			{
				Source = nullptr;
			}

			UAnimInstance* Anim = Character->GetMesh()->GetAnimInstance();
			if (Source != nullptr && Anim != nullptr)
			{
				// A rifle round snaps the body harder and faster than a pistol: the same take, played quicker.
				HitPoseRate = FMath::Clamp(0.85f + Impulse / 6000.0f, 0.85f, 1.45f);
				// Only the reaction, not what follows it. Several of these takes were recorded standing and
				// end in a fall on the ground; whether this man falls is the balance loop's decision, taken
				// frame by frame from his own footing.
				HitPoseLength = FMath::Min(Source->GetPlayLength(), 0.9f);
				HitPoseTime = 0.0f;
				HitPoseWeight = 0.0f;

				if (bWholeBody)
				{
					Anim->PlaySlotAnimationAsDynamicMontage(Source, UAlsConstants::TransitionSlotName(), 0.07f, 0.28f, HitPoseRate, 1);
					UE_LOG(LogImpact, Display, TEXT("[anim] t=%.2f %s : %s sur %s en corps entier (vitesse %.2f)"),
					       GetWorld()->GetTimeSeconds(), *GetOwner()->GetName(), Name, *Part, HitPoseRate);
				}
				else
				{
					HitPoseAnim = Source;
					// Six hundredths of a second is not a blend, it is a cut: the trunk left whatever it was
					// doing and appeared in the take's first frame. Reported from play: "tu tires dessus
					// et d'un coup il retourne a la pose 0 et joue l'anim". -ImpactFonduImpact=N to sweep.
					static float HitBlend = -1.0f;
					if (HitBlend < 0.0f)
					{
						// Swept. The angle the trunk reaches barely moves with this, but the abruptness does,
						// because at six hundredths of a second a good part of what read as "abrupt" was
						// the cut itself:
						//   0,06 -> x1,32    0,12 -> x1,36 · x1,10    0,15 -> x1,20 · x0,74    0,35 -> x0,65
						// Twelve hundredths is twice the old blend and still lands above his own running.
						float Parsed = 0.12f;
						HitBlend = FParse::Value(FCommandLine::Get(), TEXT("ImpactFonduImpact="), Parsed) ? FMath::Max(0.0f, Parsed) : 0.12f;
					}
					// -ImpactImpactInertiel: inertialized instead of crossfaded. A crossfade blends towards the take's first frame,
					// which is a man standing still: the running trunk was pulled into a standing one in a tenth of a second, the
					// "pose zero" reported from play ("comme s'il jouait une nouvelle animation a la pose zero"). Inertialization starts
					// from the pose and the motion the trunk has and lets the difference die out, as ALS does for its turn-in-place.
					// -ImpactFonduInertiel=N for its duration. It needs an Inertialization node downstream of the Spine slot; without
					// one the request is dropped and the take cuts in - which the "fuite" bench would show as more abruptness, not less.
					static const bool bInertial = FParse::Param(FCommandLine::Get(), TEXT("ImpactImpactInertiel"));
					static const float InertialTime = [] { float Value = 0.2f; FParse::Value(FCommandLine::Get(), TEXT("ImpactFonduInertiel="), Value); return FMath::Max(0.0f, Value); }();
					if (bInertial)
					{
						FMontageBlendSettings In(InertialTime);
						In.BlendMode = EMontageBlendMode::Inertialization;
						const FMontageBlendSettings Out(0.3f);
						Anim->PlaySlotAnimationAsDynamicMontage_WithBlendSettings(Source, TEXT("Spine"), In, Out, HitPoseRate, 1);
					}
					else
					{
						Anim->PlaySlotAnimationAsDynamicMontage(Source, TEXT("Spine"), HitBlend, 0.3f, HitPoseRate, 1);
					}
					UE_LOG(LogImpact, Display, TEXT("[pose] t=%.2f %s : %s sur %s, torse seul (vitesse %.2f, %.2f s, %s)"),
					       GetWorld()->GetTimeSeconds(), *GetOwner()->GetName(), Name, *Part, HitPoseRate,
					       HitPoseLength / HitPoseRate, bInertial ? TEXT("inertiel") : TEXT("fondu"));
				}
			}
		}
	}

	// The bullet's momentum goes into the body, and the balance loop decides what happens next, frame by frame.
	// It used to be settled here and now: one test, in the frame of the hit, on a budget that only ever shrank.
	// Measured consequence: the hardest shot in the game was worth 46 % of the threshold, so nobody ever fell
	// to a single bullet and nobody ever stumbled.
	const FVector Push = FVector(Bullet.Direction.X, Bullet.Direction.Y, 0.0f).GetSafeNormal();
	const float DeltaV = Impulse / BodyMassKg * (bLeg ? 0.5f : 1.0f);
	BalanceVelocity += Push * DeltaV;

	UE_LOG(LogImpact, Display, TEXT("[reaction] t=%.2f %s pousse %s, dV %.0f cm/s (vitesse du corps %.0f, capacité %.0f cm)"),
	       GetWorld()->GetTimeSeconds(), *GetOwner()->GetName(), *Bones[HitBone].Name.ToString(), DeltaV,
	       BalanceVelocity.Size(), BalanceCapacity);
}

void UImpactReactionComponent::UpdateElanWatch(const float DeltaTime)
{
	if (ElanTime < 0.0f || DeltaTime <= 0.0f)
	{
		return;
	}
	const FVector Pelvis = GetCharacter()->GetMesh()->GetSocketLocation(TEXT("pelvis"));
	const float Speed = FVector::Dist2D(Pelvis, ElanLast) / DeltaTime;
	ElanLast = Pelvis;
	ElanTime += DeltaTime;
	// A tenth of a second of smoothing: a pelvis read frame by frame bounces with each stride, and a stop is longer than that.
	ElanSmooth += (Speed - ElanSmooth) * FMath::Min(1.0f, DeltaTime / 0.1f);
	ElanLowest = FMath::Min(ElanLowest, ElanSmooth);
	if (ElanTime >= 1.0f)
	{
		const float Mean = FVector::Dist2D(Pelvis, ElanFrom) / ElanTime;
		UE_LOG(LogImpact, Display, TEXT("[elan] t=%.2f %s : %.0f cm/s a la balle, %.0f cm/s en moyenne la seconde suivante (%.0f %%), au plus bas %.0f cm/s, %s"),
		       GetWorld()->GetTimeSeconds(), *GetOwner()->GetName(), ElanSpeed, Mean, ElanSpeed > 0.0f ? Mean / ElanSpeed * 100.0f : 0.0f,
		       ElanLowest, IsDown() ? TEXT("a terre") : TEXT("debout"));
		ElanTime = -1.0f;
	}
}

void UImpactReactionComponent::Shove(const FVector& Direction, const float DeltaV, const TCHAR* Reason)
{
	AAlsCharacter* Character = GetCharacter();
	if (Character != nullptr && IsLyingDown())
	{
		// Lying animated: pushed as a body, not as a man on his feet.
		ReturnToRagdoll(Reason, false);
		LastGroundPushTime = GetWorld()->GetTimeSeconds();
		const FVector Flat = FVector(Direction.X, Direction.Y, 0.0f).GetSafeNormal();
		Character->GetMesh()->AddImpulse(Flat * (DeltaV * BodyMassKg), TEXT("spine_02"), false);
		return;
	}
	if (Character == nullptr || IsDown())
	{
		return;
	}

	const FVector Push = FVector(Direction.X, Direction.Y, 0.0f).GetSafeNormal();
	if (Push.IsNearlyZero())
	{
		return;
	}

	// The shove goes into the body's own momentum. Whether he catches it is decided every frame afterwards by
	// UpdateBalance, not here: that single-frame test was why no one ever stumbled.
	BalanceVelocity += Push * DeltaV;
	Character->GetMesh()->AddImpulse(Push * (DeltaV * BodyMassKg), TEXT("spine_02"), false);

	UE_LOG(LogImpact, Display, TEXT("[equilibre] t=%.2f %s pousse (%s) : dV %.0f cm/s, vitesse du corps %.0f cm/s, capacite %.0f cm"),
	       GetWorld()->GetTimeSeconds(), *GetOwner()->GetName(), Reason, DeltaV, BalanceVelocity.Size(), BalanceCapacity);
}

bool UImpactReactionComponent::GetBalanceState(FVector& OutCentre, FVector& OutVelocity, FVector& OutSupport,
                                               bool& bOutLeftDown, bool& bOutRightDown) const
{
	const AAlsCharacter* Character = GetCharacter();
	if (Character == nullptr)
	{
		return false;
	}
	const USkeletalMeshComponent* Mesh = Character->GetMesh();

	// Centre of mass of the whole body, weighted by what each part actually weighs. Bodies that are following
	// the animation rather than simulating still report their mass and their place, which is what we want.
	// Control test, once: if every body weighs exactly 1 kg the physics asset never had its masses computed,
	// and every centre-of-mass figure below is meaningless. Better to find that out here than to spend a day
	// tuning a balance controller on fiction.
	if (!bMassesChecked)
	{
		bMassesChecked = true;
		const FBodyInstance* Pelvis = Mesh->GetBodyInstance(TEXT("pelvis"));
		const float PelvisMass = Pelvis != nullptr ? Pelvis->GetBodyMass() : -1.0f;
		UE_LOG(LogImpact, Display, TEXT("[masses] bassin %.2f kg, corps entier %.1f kg, %d corps — %s"),
		       PelvisMass, Mesh->GetMass(), Mesh->Bodies.Num(),
		       FMath::IsNearlyEqual(PelvisMass, 1.0f, 0.01f) ? TEXT("VALEURS PAR DEFAUT, mesures inutilisables")
		                                                     : TEXT("masses reelles"));
	}

	FVector Weighted = FVector::ZeroVector;
	float Total = 0.0f;
	for (const FBone& Bone : Bones)
	{
		if (const FBodyInstance* Body = Mesh->GetBodyInstance(Bone.Name))
		{
			const float Mass = FMath::Max(Body->GetBodyMass(), 0.01f);
			Weighted += Body->GetCOMPosition() * Mass;
			Total += Mass;
		}
	}
	if (Total <= 0.0f)
	{
		return false;
	}
	OutCentre = Weighted / Total;

	// Measured, not read from the bodies: most of them are kinematic and report no velocity of their own.
	const float Delta = GetWorld()->GetDeltaSeconds();
	if (bCentreKnown && Delta > KINDA_SMALL_NUMBER)
	{
		const FVector Raw = (OutCentre - LastCentre) / Delta;
		CentreVelocity = FMath::Lerp(CentreVelocity, Raw.GetClampedToMaxSize(2000.0f), FMath::Clamp(Delta * 15.0f, 0.0f, 1.0f));
	}
	LastCentre = OutCentre;
	bCentreKnown = true;
	OutVelocity = CentreVelocity;

	// Which feet are carrying: near the capsule's base and not lifted.
	const FVector Left = Mesh->GetSocketLocation(TEXT("foot_l"));
	const FVector Right = Mesh->GetSocketLocation(TEXT("foot_r"));
	const float Ground = Character->GetActorLocation().Z - Character->GetSimpleCollisionHalfHeight();
	// The foot bone sits at the ankle, a good thirteen centimetres above the sole, so a twelve centimetre
	// threshold said no foot was ever on the ground. A foot carries when it is low AND not lifted above
	// the other one: that second half is what tells a stance from a swing.
	const float Lowest = FMath::Min(Left.Z, Right.Z);
	bOutLeftDown = Left.Z - Ground < 24.0f && Left.Z - Lowest < 7.0f;
	bOutRightDown = Right.Z - Ground < 24.0f && Right.Z - Lowest < 7.0f;

	OutSupport = bOutLeftDown && bOutRightDown ? (Left + Right) * 0.5f : bOutLeftDown ? Left : Right;
	OutSupport.Z = Ground;
	return true;
}

void UImpactReactionComponent::Collapse(const FVector& PushVelocity, const bool bInLimp, const TCHAR* Reason)
{
	AAlsCharacter* Character = GetCharacter();
	if (Character == nullptr)
	{
		return;
	}
	if (IsLyingDown())
	{
		// Already on the floor: not a new fall (no count, no hands out), only back to the ragdoll and the push.
		if (bInLimp)
		{
			bLimp = true;
			bVoluntaryControl = false;
		}
		ReturnToRagdoll(Reason, bInLimp);
		Character->GetMesh()->AddImpulse(PushVelocity * 0.6f, TEXT("pelvis"), true);
		return;
	}

	if (bInLimp)
	{
		bLimp = true;
		bVoluntaryControl = false;
	}
	CancelHandCommands();
	LastReason = Reason;

	// A fall only counts against getting up again while it is recent: someone who went down once a minute
	// ago is not someone who cannot stand.
	const float Now = GetWorld()->GetTimeSeconds();
	RecentFalls = Now - LastFallTime > 25.0f ? 1 : RecentFalls + 1;
	LastFallTime = Now;

	// In what state does a body actually hit the ground? The ground postures only exist for someone who
	// is down and still awake, and it is worth knowing how often that is anyone at all.
	if (const UImpactBodyComponent* Body = GetOwner()->FindComponentByClass<UImpactBodyComponent>())
	{
		const EImpactConsciousness State = Body->GetPhysiology().GetConsciousness();
		UE_LOG(LogImpact, Display, TEXT("[etat] %s tombe (%s) : %s, sang %.0f %%, cerveau %.2f"),
		       *GetOwner()->GetName(), Reason,
		       State == EImpactConsciousness::Conscious ? TEXT("CONSCIENT")
			       : State == EImpactConsciousness::Confused ? TEXT("confus")
			       : State == EImpactConsciousness::SemiConscious ? TEXT("semi-conscient")
			       : State == EImpactConsciousness::Unconscious ? TEXT("inconscient") : TEXT("mort"),
		       Body->GetPhysiology().GetBloodFraction() * 100.0f, Body->GetPhysiology().GetBrainOxygen());
	}

	// Back to being a ragdoll: the shoulders must have the asset's own limits again, or a falling body folds
	// its arms in ways a body does not.
	SetArmReaching(0, false);
	SetArmReaching(1, false);

	// Anyone still awake puts his hands out when the ground comes up. Someone who has passed out does not,
	// which is most of the difference between a person falling and a sack falling.
	// -ImpactSansAmorti: no hands out, to measure what the brace does to the fall.
	static const bool bNoBrace = GroundFlag(TEXT("ImpactSansAmorti"));
	BraceTime = !bNoBrace && bVoluntaryControl && (IsArmUsable(0) || IsArmUsable(1)) ? 1.1f : 0.0f;

	if (Character->GetLocomotionAction() != AlsLocomotionActionTags::Ragdolling)
	{
		ResetBones();

		// -ImpactSolReArme: every new fall starts the writhing over, take included. Writhing is only switched on
		// on the way down and off once he is up, and "down" covers getting up too, so a man who fell again while
		// still getting up kept the take chosen for his first wound and the fall's weak muscle. Not through
		// SetWrithing(false), which would also change the muscle in the middle of the fall: the body component
		// sees him down and not writhing on its next tick and switches it back on itself, as on a first fall.
		static const bool bReArm = GroundFlag(TEXT("ImpactSolReArme"));
		if (bReArm)
		{
			bWrithing = false;
			GroundTake = nullptr;
			bBraced = false;
		}

		// Someone who is knocked off balance still has muscle; only the dead and the unconscious go slack.
		// This was unconditional, so every fall looked like a corpse dropping. The amount a conscious body keeps
		// is a judgement call about how it looks, so it stays tunable (-ImpactFallTone=). It was left at zero
		// until someone had watched it; reported from play: "il tombe en full ragdoll, tous les membres vont
		// dans tous les sens, c'est archi pas realiste". That was this, on every fall in the game.
		static float FallTone = -1.0f;
		if (FallTone < 0.0f)
		{
			float Parsed = 0.0f;
			FallTone = FParse::Value(FCommandLine::Get(), TEXT("ImpactFallTone="), Parsed) ? FMath::Clamp(Parsed, 0.0f, 1.0f) : 0.32f;
		}
		PhysicalAnimation->SetStrengthMultiplyer(bInLimp ? 0.0f : FallTone);
		static const bool bAnimatedFall = GroundFlag(TEXT("ImpactChuteAnimee"));
		static const bool bBlendedFallOn = GroundFlag(TEXT("ImpactChuteMelee"));
		const FTransform StandingFrame = Character->GetMesh()->GetComponentTransform();
		if (bAnimatedFall && !bBlendedFallOn && !bInLimp && bVoluntaryControl && TryStartAnimatedFall(PushVelocity, Reason))
		{
			DownTime = 0.0f;
			GroundContactTime = 0.0f;
			UE_LOG(LogImpact, Display, TEXT("[reaction] t=%.2f %s tombe (%s), chute animee"), GetWorld()->GetTimeSeconds(), *GetOwner()->GetName(), Reason);
			return;
		}
		// -ImpactChuteElan: his velocity as he goes down, before the ragdoll takes the movement component's away. Not the one he had
		// when the round arrived: a step may have turned or stopped him since (relecture du 27 octobre).
		RunVelocityAtFall = Character->GetVelocity();
		Character->StartRagdolling();
		// -ImpactChuteTenue: the joints keep aiming at the pose he had when he went down (ALS set them from it just now), instead of
		// following A_Als_Flail frame by frame. See UpdateDown for how that hold fades.
		static const bool bHeldFall = GroundFlag(TEXT("ImpactChuteTenue"));
		if (bHeldFall && !bInLimp && !bBlendedFallOn)
		{
			Character->GetMesh()->bUpdateJointsFromAnimation = false;
		}
		DownTime = 0.0f;
		GroundContactTime = 0.0f;

		USkeletalMeshComponent* Mesh = Character->GetMesh();
		Mesh->AddImpulse(PushVelocity, TEXT("spine_03"), true);
		Mesh->AddImpulse(PushVelocity * 0.6f, TEXT("pelvis"), true);
		if (bBlendedFallOn && !bInLimp && bVoluntaryControl)
		{
			StartBlendedFall(PushVelocity, Reason, StandingFrame);
		}
	}

	UE_LOG(LogImpact, Display, TEXT("[reaction] t=%.2f %s tombe (%s)%s"), GetWorld()->GetTimeSeconds(), *GetOwner()->GetName(), Reason,
	       bLimp ? TEXT(", inerte") : TEXT(""));
}

void UImpactReactionComponent::TickComponent(const float DeltaTime, const ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	ActiveForceCount = 0;

	if (GetCharacter() == nullptr || PhysicalAnimation == nullptr)
	{
		return;
	}

	// First, and before any of the early returns below: the muscles tick right after us whatever we decide.
	GuardMusclePose();

	// Guard every force producer, including transitions that happen between the body's post-physics ticks.
	if (const UImpactBodyComponent* Body = GetOwner()->FindComponentByClass<UImpactBodyComponent>())
	{
		SetControlState(Body->GetPhysiology().GetConsciousness());
	}
	UpdateElanWatch(DeltaTime);
	UpdateDown(DeltaTime);
	UpdateDeadCollision();
	UpdateFallEnd(DeltaTime);

	// Above the early returns on purpose. This both takes the ground posture and releases it, and the
	// release has to happen on the paths that return - a man who loses consciousness while writhing, or
	// who starts getting up - or the take would go on playing into the ragdoll slot over the get-up.
	UpdateGroundRagdoll(DeltaTime);
	UpdateBlendedFall(DeltaTime);

	// Before the voluntary-control return, always: it is what ends a fall and what catches a take that stopped. Behind it, a man
	// who lost consciousness during his fall was never looked at again - the fall ended, nothing was left playing, and he was
	// drawn standing where the fall began, 1.76 m away, for good. Reported from play: "des anims qui rollback".
	const FGameplayTag Action = GetCharacter()->GetLocomotionAction();
	WatchStandingGhost(Action);
	if (MixamoGetUp != nullptr)
	{
		// The Mixamo get-up carries no ALS notify to end GettingUp, and without one ALS believes he is getting up for ever.
		const UAnimInstance* GetUpAnim = GetCharacter()->GetMesh()->GetAnimInstance();
		if (Action != AlsLocomotionActionTags::GettingUp)
		{
			MixamoGetUp = nullptr;
		}
		else if (GetUpAnim == nullptr || !GetUpAnim->Montage_IsPlaying(MixamoGetUp))
		{
			GetCharacter()->SetLocomotionAction(FGameplayTag::EmptyTag);
			GetCharacter()->GroundMontageOverride = nullptr;
			MixamoGetUp = nullptr;
			UE_LOG(LogImpact, Display, TEXT("[releve-mixamo] t=%.2f %s debout"), GetWorld()->GetTimeSeconds(), *GetOwner()->GetName());
		}
	}
	KeepWeaponInHand(Action == AlsLocomotionActionTags::Ragdolling || Action == AlsLocomotionActionTags::LyingDown
	                 || Action == AlsLocomotionActionTags::GettingUp);
	if (Action == AlsLocomotionActionTags::LyingDown)
	{
		UpdateLyingDown(DeltaTime);
		return;
	}

	if (!bVoluntaryControl)
	{
		return;
	}
	if (Action == AlsLocomotionActionTags::GettingUp)
	{
		CancelHandCommands();
		SetCrawl(FVector::ZeroVector, false, false);
		BraceTime = 0.0f;
		return;
	}
	if (Action == AlsLocomotionActionTags::Ragdolling)
	{
		// Mutually exclusive arm owners. Fading wound springs must not fight a brace or a crawl.
		if (BraceTime > 0.0f)
		{
			CancelHandCommands();
			UpdateBrace(DeltaTime);
		}
		else if (bCrawling)
		{
			CancelHandCommands();
			UpdateCrawl(DeltaTime);
		}
		else
		{
			UpdateHands(DeltaTime);
		}

		// Here and not in the branch below: that one only runs for a character on his feet, which is the
		// one case a ground posture is never wanted. Called from the wrong side, the handover could never
		// fire at all.
		UpdateGroundPose();
		return;
	}
	else
	{
		UpdateBones(DeltaTime);
		UpdateHitPose(DeltaTime);
		UpdateBalance(DeltaTime);
		UpdateLimp(DeltaTime);
	}
	UpdateHands(DeltaTime);
}

void UImpactReactionComponent::CancelHandCommands()
{
	for (FHand& Hand : Hands)
	{
		Hand = FHand{};
	}
}

void UImpactReactionComponent::ApplyActiveForce(const FVector& Force, const FName Bone, const bool bAcceleration)
{
	if (bVoluntaryControl)
	{
		GetCharacter()->GetMesh()->AddForce(Force, Bone, bAcceleration);
		++ActiveForceCount;
	}
}

void UImpactReactionComponent::ApplyActiveTorque(const FVector& Torque, const FName Bone, const bool bAcceleration)
{
	if (bVoluntaryControl)
	{
		GetCharacter()->GetMesh()->AddTorqueInRadians(Torque, Bone, bAcceleration);
		++ActiveForceCount;
	}
}

bool UImpactReactionComponent::CanHoldWound() const
{
	static const bool bHandsFree = GroundFlag(TEXT("ImpactSolMainsLibres"));
	if ((bHandsFree && GroundMontage != nullptr) || IsLyingDown())
	{
		// Lying down, the hands are the take's: nothing simulates for a spring to pull on.
		return false;
	}
	const AAlsCharacter* Character = GetCharacter();
	return bVoluntaryControl && BraceTime <= 0.0f && !bCrawling && Character != nullptr
	       && Character->GetLocomotionAction() != AlsLocomotionActionTags::GettingUp;
}

void UImpactReactionComponent::SetControlState(const EImpactConsciousness State)
{
	bVoluntaryControl = State <= EImpactConsciousness::Confused;
	bLimp = State >= EImpactConsciousness::Unconscious;
	bDead = State == EImpactConsciousness::Dead;
	if (bVoluntaryControl)
	{
		return;
	}
	// Out cold or dead while lying animated: back to the ragdoll, limp. The one place every loss of control goes through.
	ReturnToRagdoll(TEXT("perd le controle"), true);
	CancelHandCommands();
	BraceTime = 0.0f;
	bCrawling = false;
	CrawlPhase = 0.0f;
	StaggerRemaining = 0.0f;
	bMayGetUp = false;
	if (AAlsCharacter* Character = GetCharacter())
	{
		SetArmReaching(0, false);
		SetArmReaching(1, false);
		Character->ConsumeMovementInputVector();
		if (AAIController* Controller = Cast<AAIController>(Character->GetController()))
		{
			Controller->StopMovement();
			Controller->ClearFocus(EAIFocusPriority::Gameplay);
		}
		if (Character->GetLocomotionAction() == AlsLocomotionActionTags::Ragdolling)
		{
			// Disable both motor systems; collisions, joint limits and damping are retained.
			Character->GetMesh()->SetAllMotorsAngularDriveParams(0.0f, 0.0f, 0.0f);
		}
	}
	if (PhysicalAnimation != nullptr)
	{
		PhysicalAnimation->SetStrengthMultiplyer(0.0f);
	}
}

void UImpactReactionComponent::SetHandTarget(const int32 Side, const FVector& WorldTarget, const FVector& Outward, const bool bActive)
{
	FHand& Hand = Hands[Side];
	if (!Hand.bActive && bActive)
	{
		Hand.bApproached = false;
		Hand.bClearedTorso = false;
		Hand.Time = 0.0f;
		Hand.ContactTime = 0.0f;
		Hand.bReachable = true;
		Hand.bHadTarget = false;
		Hand.TargetVelocity = FVector::ZeroVector;
	}
	Hand.bActive = bActive && CanHoldWound() && IsArmUsable(Side);
	if (!CanHoldWound() || !IsArmUsable(Side))
	{
		Hand = FHand{};
		return;
	}
	if (bActive)
	{
		Hand.Target = WorldTarget;
		Hand.Outward = Outward;
	}
}

void UImpactReactionComponent::SetArmReaching(const int32 Side, const bool bReaching)
{
	FArmLimits& Limits = ArmLimits[Side];
	if (Limits.bWide == bReaching)
	{
		return;
	}

	USkeletalMeshComponent* Mesh = GetCharacter()->GetMesh();
	FConstraintInstance* Shoulder = Mesh->FindConstraintInstance(Side == 0 ? TEXT("upperarm_l") : TEXT("upperarm_r"));
	FConstraintInstance* Elbow = Mesh->FindConstraintInstance(Side == 0 ? TEXT("lowerarm_l") : TEXT("lowerarm_r"));
	if (Shoulder == nullptr)
	{
		return;
	}

	if (!Limits.bSaved)
	{
		Limits.bSaved = true;
		Limits.Swing1 = static_cast<uint8>(Shoulder->ProfileInstance.ConeLimit.Swing1Motion);
		Limits.Swing2 = static_cast<uint8>(Shoulder->ProfileInstance.ConeLimit.Swing2Motion);
		Limits.Twist = static_cast<uint8>(Shoulder->ProfileInstance.TwistLimit.TwistMotion);
		Limits.Swing1Degrees = Shoulder->ProfileInstance.ConeLimit.Swing1LimitDegrees;
		Limits.Swing2Degrees = Shoulder->ProfileInstance.ConeLimit.Swing2LimitDegrees;
		Limits.TwistDegrees = Shoulder->ProfileInstance.TwistLimit.TwistLimitDegrees;
		if (Elbow != nullptr)
		{
			Limits.ElbowSwing1 = static_cast<uint8>(Elbow->ProfileInstance.ConeLimit.Swing1Motion);
			Limits.ElbowSwing1Degrees = Elbow->ProfileInstance.ConeLimit.Swing1LimitDegrees;
		}
	}

	if (bReaching)
	{
		// A living shoulder reaches far further than a ragdoll is allowed to bend. Measured: with the asset's
		// own 50/40/25 the hand stopped 18 to 20 cm short of the other arm whatever force was applied, to the
		// decimal, because it was against a hard stop and not short of strength.
		Shoulder->SetAngularSwing1Limit(ACM_Limited, 125.0f);
		Shoulder->SetAngularSwing2Limit(ACM_Limited, 100.0f);
		Shoulder->SetAngularTwistLimit(ACM_Limited, 85.0f);
		if (Elbow != nullptr)
		{
			Elbow->SetAngularSwing1Limit(ACM_Limited, 130.0f);
		}
	}
	else
	{
		Shoulder->SetAngularSwing1Limit(static_cast<EAngularConstraintMotion>(Limits.Swing1), Limits.Swing1Degrees);
		Shoulder->SetAngularSwing2Limit(static_cast<EAngularConstraintMotion>(Limits.Swing2), Limits.Swing2Degrees);
		Shoulder->SetAngularTwistLimit(static_cast<EAngularConstraintMotion>(Limits.Twist), Limits.TwistDegrees);
		if (Elbow != nullptr)
		{
			Elbow->SetAngularSwing1Limit(static_cast<EAngularConstraintMotion>(Limits.ElbowSwing1), Limits.ElbowSwing1Degrees);
		}
	}
	Limits.bWide = bReaching;
}

void UImpactReactionComponent::UpdateHands(const float RealDeltaTime)
{
	if (!CanHoldWound())
	{
		CancelHandCommands();
		return;
	}
	USkeletalMeshComponent* Mesh = GetCharacter()->GetMesh();
	const TCHAR* HandBones[2] = {TEXT("hand_l"), TEXT("hand_r")};

	// A screenshot, a shader hitch or any heavy frame can last a tenth of a second. The spring below is stepped
	// once per frame, so a single long frame threw the arm away from the body: that, not the gesture itself,
	// produced the "hand drifts away" seen in the earlier tests. The real time is still used for the speeds.
	const float DeltaTime = FMath::Min(RealDeltaTime, 1.0f / 30.0f);

	for (int32 Side = 0; Side < 2; ++Side)
	{
		FHand& Hand = Hands[Side];
		Hand.Weight = Hand.bActive ? FMath::Min(1.0f, Hand.Weight + DeltaTime * 2.5f) : FMath::Max(0.0f, Hand.Weight - DeltaTime * 3.0f);
		Hand.Distance = 1.0e6f;
		SetArmReaching(Side, Hand.Weight > 0.0f);
		if (Hand.Weight <= 0.0f)
		{
			Hand.Gap = 1.0e6f;
			Hand.ContactTime = 0.0f;
			Hand.bHadTarget = false;
			continue;
		}

		FBodyInstance* Body = Mesh->GetBodyInstance(HandBones[Side]);
		if (Body == nullptr || !Body->IsInstanceSimulatingPhysics())
		{
			Hand.Gap = 1.0e6f;
			Hand.bHadTarget = false;
			continue;
		}

		// How fast the wound itself is travelling: it is carried by a body that walks, turns and staggers.
		FVector Measured = FVector::ZeroVector;
		if (Hand.bHadTarget && RealDeltaTime > KINDA_SMALL_NUMBER)
		{
			Measured = ((Hand.Target - Hand.LastTarget) / RealDeltaTime).GetClampedToMaxSize(1500.0f);
		}
		Hand.TargetVelocity = FMath::Lerp(Hand.TargetVelocity, Measured, FMath::Clamp(RealDeltaTime * 12.0f, 0.0f, 1.0f));
		Hand.LastTarget = Hand.Target;
		Hand.bHadTarget = true;

		// A damped spring pulls the hand onto the target; it also carries the weight of the soft arm.
		const FVector Position = Body->GetCOMPosition();
		const FVector Velocity = Body->GetUnrealWorldVelocity();
		Hand.Distance = FVector::Distance(Hand.Target, Position);

		// The palm of this skeleton, read from its own finger bones rather than guessed. What is placed on the
		// wound is the palm, not the centre of the fist: aiming the centre left the hand beside the wound or
		// turned the wrong way, and a closed fist floating in front of the belly is not someone holding a wound.
		const FVector Wrist = Mesh->GetSocketLocation(HandBones[Side]);
		const FName FingerBone = Side == 0 ? TEXT("middle_01_l") : TEXT("middle_01_r");
		const FName ThumbBone = Side == 0 ? TEXT("thumb_01_l") : TEXT("thumb_01_r");
		FVector PalmPoint = Position;
		FVector PalmNormal = FVector::ZeroVector;
		if (Mesh->DoesSocketExist(FingerBone) && Mesh->DoesSocketExist(ThumbBone))
		{
			const FVector AlongFingers = Mesh->GetSocketLocation(FingerBone) - Wrist;
			const FVector ToThumb = (Mesh->GetSocketLocation(ThumbBone) - Wrist).GetSafeNormal();
			const FVector Fingers = AlongFingers.GetSafeNormal();
			// Fingers crossed with thumb gives the back of a right hand and the palm of a left one.
			PalmNormal = (FVector::CrossProduct(Fingers, ToThumb) * (Side == 0 ? -1.0f : 1.0f)).GetSafeNormal();
			PalmPoint = Wrist + Fingers * (AlongFingers.Size() * 0.6f) + PalmNormal * 2.0f;
		}

		// Real contact: shortest distance from the shape of the hand to the wound. Never from the centre of the
		// hand, which sits about ten centimetres inside it, so that measure could not reach zero however well
		// the hand was placed. Where the palm is and which way it faces is read out but not driven: pulling the
		// hand by its palm and turning it with a torque fought the wrist and the animation and lost every case.
		FVector PointOnHand = FVector::ZeroVector;
		const float Surface = Body->GetDistanceToBody(Hand.Target, PointOnHand);
		const float SurfaceGap = Surface >= 0.0f ? Surface : Hand.Distance;
		Hand.Gap = SurfaceGap;
		const float PalmGap = FVector::Distance(Hand.Target, PalmPoint);

		// Come from outside the body first (in front of the wound), then press onto it: otherwise the hand
		// slides along the side of the body and gets stuck there.
		const FVector Approach = Hand.Target + Hand.Outward * 14.0f;
		Hand.Time += DeltaTime;
		if (!Hand.bApproached && (FVector::Distance(Approach, Position) < 12.0f || Hand.Gap < ReleaseGap))
		{
			Hand.bApproached = true;
		}

		// The arm has been pulling for a while and is still nowhere near. That place cannot be reached
		// (the middle of the back, a wound behind the shoulder): say so instead of pretending to press on it.
		if (!Hand.bApproached && Hand.bReachable && Hand.Time > 1.6f)
		{
			Hand.bReachable = false;
			UE_LOG(LogImpact, Display, TEXT("[hand] t=%.2f %s main %d : n'atteint pas la plaie (%.0f cm)"),
			       GetWorld()->GetTimeSeconds(), *GetOwner()->GetName(), Side, Hand.Gap);
		}

		// The pressure only counts once the hand has really been held there, and is not dropped on one bad frame.
		const float PalmFacing = PalmNormal.IsNearlyZero() ? 0.0f : (PalmNormal | -Hand.Outward);
		if (Hand.bActive && Hand.bApproached && Hand.Gap <= ContactGap && PalmGap <= 9.0f && PalmFacing > 0.2f)
		{
			Hand.ContactTime += DeltaTime;
		}
		else
		{
			Hand.ContactTime = 0.0f;
		}

		FVector Goal = Hand.bApproached ? Hand.Target : Approach;

		// First bring the hand in front on its own side, then cross towards the wound. The old infinite 42 cm
		// cylinder moved the destination outside reach, even for thigh wounds, without protecting the path.
		const FVector Hips = Mesh->GetSocketLocation(TEXT("pelvis"));
		const FVector Chest = Mesh->GetSocketLocation(TEXT("spine_03"));
		const FVector TrunkAxis = (Chest - Hips).GetSafeNormal();
		const FVector Front = FVector::VectorPlaneProject(GetCharacter()->GetActorForwardVector(), TrunkAxis).GetSafeNormal();
		const FVector Across = FVector::CrossProduct(TrunkAxis, Front).GetSafeNormal();
		const float TargetHeight = (Hand.Target - Hips) | TrunkAxis;
		const bool bUpperBody = TargetHeight > 5.0f;
		if (bUpperBody && !Hand.bClearedTorso && !Hand.bApproached)
		{
			const FVector Waypoint = Hips + TrunkAxis * FMath::Clamp(TargetHeight, 5.0f, FVector::Dist(Hips, Chest))
			                         + Front * 30.0f + Across * (Side == 0 ? -24.0f : 24.0f);
			Hand.bClearedTorso = ((Position - Hips) | Front) >= 24.0f || FVector::Dist(Position, Waypoint) < 9.0f;
			if (!Hand.bClearedTorso)
			{
				Goal = Waypoint;
			}
		}
		HandLogTimer -= DeltaTime;
		if (HandLogTimer <= 0.0f)
		{
			HandLogTimer = 0.25f;
			const TCHAR* Phase = !Hand.bReachable ? TEXT("hors d'atteinte")
			                     : Hand.ContactTime >= ContactHoldTime ? TEXT("comprime")
			                     : Hand.Gap <= ContactGap ? TEXT("touche")
			                     : Hand.bApproached ? TEXT("pose la main") : TEXT("approche");
			// Which way the hand is still off: "dehors" is short of the wound along the approach, "à côté" is
			// beside it. They call for different fixes, so they are measured apart.
			const FVector Miss = Hand.Target - PointOnHand;
			const float Outside = Miss | Hand.Outward;
			const float Beside = (Miss - Hand.Outward * Outside).Size();
			const float Facing = PalmNormal.IsNearlyZero() ? 0.0f : (PalmNormal | -Hand.Outward);
			UE_LOG(LogImpact, Display, TEXT("[hand] t=%.2f %s main %d : %s, contact %.1f cm (dehors %.1f, à côté %.1f), paume %.0f cm tournée %.2f, tenu %.2f s"),
			       GetWorld()->GetTimeSeconds(), *GetOwner()->GetName(), Side, Phase, Hand.Gap, Outside, Beside, PalmGap, Facing, Hand.ContactTime);
		}

		const FVector Error = Goal - Position;

		constexpr float Omega = 14.0f;
		constexpr float ArmMass = 6.0f;
		// The damping is measured against the wound, not against the ground: a hand travelling with the body it
		// holds is not moving as far as the gesture is concerned. Measured against the ground, walking or running
		// left a gap proportional to the speed (30 to 50 cm while fleeing).
		const FVector Relative = Velocity - Hand.TargetVelocity;
		FVector Acceleration = Error * (Omega * Omega) - Relative * (2.0f * Omega * 0.9f) + FVector(0.0f, 0.0f, 900.0f);

		// Easing the push once the hand lands was tried and made things worse: the press is also what keeps the
		// hand where it was put. With it eased the hand wandered three times as far off the wound (measured).
		Acceleration = Acceleration.GetClampedToMaxSize(9000.0f);
		ApplyActiveForce(Acceleration * ArmMass * Hand.Weight, HandBones[Side], false);

		// The whole arm is placed, not just the hand. Where the elbow and the upper arm have to be for this
		// reach is a two-bone problem with one answer, so it is solved rather than guessed, and all three
		// segments are driven to it. Pulling only the hand left the shoulder to be dragged along behind, which
		// is why an arm could never cross the chest to hold the other one: measured, it stalled 18 to 20 cm short.
		const TCHAR* UpperBone = Side == 0 ? TEXT("upperarm_l") : TEXT("upperarm_r");
		const TCHAR* ForearmBone = Side == 0 ? TEXT("lowerarm_l") : TEXT("lowerarm_r");
		FBodyInstance* Upper = Mesh->GetBodyInstance(UpperBone);
		FBodyInstance* Lower = Mesh->GetBodyInstance(ForearmBone);
		if (Upper == nullptr || Lower == nullptr)
		{
			continue;
		}

		const FVector Shoulder = Mesh->GetSocketLocation(UpperBone);
		const FVector ElbowNow = Mesh->GetSocketLocation(ForearmBone);
		const FVector WristNow = Mesh->GetSocketLocation(HandBones[Side]);

		// Bone lengths are fixed, so they can be read from the pose whatever it is doing.
		const float UpperLength = FMath::Max(FVector::Dist(Shoulder, ElbowNow), 1.0f);
		const float LowerLength = FMath::Max(FVector::Dist(ElbowNow, WristNow), 1.0f);

		// The wrist, not the centre of the hand, is what the forearm reaches to.
		const FVector WristGoal = Goal + (WristNow - Position);
		FVector ToGoal = WristGoal - Shoulder;
		const float Span = FMath::Clamp(ToGoal.Size(), FMath::Abs(UpperLength - LowerLength) + 2.0f, UpperLength + LowerLength - 2.0f);
		const FVector Along = ToGoal.GetSafeNormal();
		const float Fold = FMath::Clamp((Span * Span + UpperLength * UpperLength - LowerLength * LowerLength) / (2.0f * Span), 0.0f, UpperLength);
		const float Bend = FMath::Sqrt(FMath::Max(0.0f, UpperLength * UpperLength - Fold * Fold));

		// Elbows hang down and a little away from the body; only the part across the arm can be obeyed.
		const FVector Lateral = (Shoulder - GetCharacter()->GetActorLocation()).GetSafeNormal2D();
		FVector Pole = bUpperBody ? Front * 0.8f + Lateral * 0.35f - TrunkAxis * 0.6f
		                            : Lateral * 0.45f - FVector::UpVector * 0.9f;
		Pole = (Pole - Along * (Pole | Along)).GetSafeNormal();
		if (Pole.IsNearlyZero())
		{
			Pole = (ElbowNow - (Shoulder + Along * Fold)).GetSafeNormal();
		}
		const FVector ElbowGoal = Shoulder + Along * Fold + Pole * Bend;

		// Three times the force changed the reach by nothing at all, to the decimal: that is a hard stop, not a
		// weak pull. Read what the physics asset allows this shoulder and elbow to do, once, to find out.
		if (!bArmLimitsLogged)
		{
			bArmLimitsLogged = true;
			for (const TCHAR* Joint : {UpperBone, ForearmBone})
			{
				if (const FConstraintInstance* Constraint = Mesh->FindConstraintInstance(FName(Joint)))
				{
					const FConeConstraint& Cone = Constraint->ProfileInstance.ConeLimit;
					const FTwistConstraint& Twist = Constraint->ProfileInstance.TwistLimit;
					UE_LOG(LogImpact, Display, TEXT("[butée] %s : swing1 %d (%.0f°), swing2 %d (%.0f°), twist %d (%.0f°)"),
					       Joint, static_cast<int32>(Cone.Swing1Motion), Cone.Swing1LimitDegrees,
					       static_cast<int32>(Cone.Swing2Motion), Cone.Swing2LimitDegrees,
					       static_cast<int32>(Twist.TwistMotion), Twist.TwistLimitDegrees);
				}
				else
				{
					UE_LOG(LogImpact, Display, TEXT("[butée] %s : aucune contrainte trouvée"), Joint);
				}
			}
		}

		// Each segment is pulled to where that answer puts its middle. Accelerations, so the gains do not have
		// to be retuned for the mass of each bone.
		const auto DriveSegment = [&](FBodyInstance* Body, const FVector& From, const FVector& To, const float Gain, const float Cap)
		{
			const FVector SegmentGoal = (From + To) * 0.5f;
			const FVector SegmentError = SegmentGoal - Body->GetCOMPosition();
			const FVector SegmentRelative = Body->GetUnrealWorldVelocity() - Hand.TargetVelocity;
			FVector Pull = SegmentError * (Gain * Gain) - SegmentRelative * (2.0f * Gain * 0.9f) + FVector(0.0f, 0.0f, 980.0f);
			ApplyActiveForce(Pull.GetClampedToMaxSize(Cap) * Hand.Weight, Body == Upper ? UpperBone : ForearmBone, true);
		};
		DriveSegment(Upper, Shoulder, ElbowGoal, 11.0f, 6000.0f);
		DriveSegment(Lower, ElbowGoal, WristGoal, 13.0f, 6000.0f);
	}
}

bool UImpactReactionComponent::IsArmUsable(const int32 Side) const
{
	const int32 Index = FindBone(Side == 0 ? TEXT("clavicle_l") : TEXT("clavicle_r"));
	return Bones.IsValidIndex(Index) && Bones[Index].RestTone >= 0.5f;
}

void UImpactReactionComponent::UpdateBrace(const float DeltaTime)
{
	if (BraceTime <= 0.0f)
	{
		return;
	}
	BraceTime -= DeltaTime;

	USkeletalMeshComponent* Mesh = GetCharacter()->GetMesh();
	const FCollisionQueryParams Params(SCENE_QUERY_STAT(ImpactBrace), false, GetOwner());

	for (int32 Side = 0; Side < 2; ++Side)
	{
		if (!IsArmUsable(Side))
		{
			continue;
		}
		// The shoulders have to open or the arms cannot come out in front at all.
		SetArmReaching(Side, true);

		const TCHAR* HandBone = Side == 0 ? TEXT("hand_l") : TEXT("hand_r");
		FBodyInstance* Hand = Mesh->GetBodyInstance(HandBone);
		if (Hand == nullptr || !Hand->IsInstanceSimulatingPhysics())
		{
			continue;
		}

		// Reach for the ground where the body is heading, not straight down: that is what makes it read as
		// catching yourself rather than dropping your arms.
		const FVector Chest = Mesh->GetSocketLocation(TEXT("spine_03"));
		const FVector Going = GetCharacter()->GetVelocity().GetSafeNormal2D();
		const FVector Shoulder = Mesh->GetSocketLocation(Side == 0 ? TEXT("upperarm_l") : TEXT("upperarm_r"));
		FVector Goal = Shoulder + (Chest - Mesh->GetSocketLocation(TEXT("pelvis"))).GetSafeNormal2D() * 35.0f + Going * 15.0f;

		FHitResult Ground;
		if (GetWorld()->LineTraceSingleByChannel(Ground, Goal + FVector(0.0f, 0.0f, 90.0f), Goal - FVector(0.0f, 0.0f, 150.0f),
		                                         ECC_Visibility, Params))
		{
			Goal = Ground.ImpactPoint + FVector(0.0f, 0.0f, 6.0f);
		}

		const FVector Error = Goal - Hand->GetCOMPosition();
		constexpr float Omega = 17.0f;
		FVector Acceleration = Error * (Omega * Omega) - Hand->GetUnrealWorldVelocity() * (2.0f * Omega * 0.9f)
		                       + FVector(0.0f, 0.0f, 980.0f);
		ApplyActiveForce(Acceleration.GetClampedToMaxSize(9000.0f), HandBone, true);

		// Stiff elbows: a bent arm folds under the weight instead of taking it.
		const TCHAR* ForearmBone = Side == 0 ? TEXT("lowerarm_l") : TEXT("lowerarm_r");
		if (FBodyInstance* Forearm = Mesh->GetBodyInstance(ForearmBone))
		{
			const FVector ElbowGoal = FMath::Lerp(Shoulder, Goal, 0.5f);
			const FVector ElbowError = ElbowGoal - Forearm->GetCOMPosition();
			ApplyActiveForce((ElbowError * 120.0f - Forearm->GetUnrealWorldVelocity() * 16.0f + FVector(0.0f, 0.0f, 980.0f))
			               .GetClampedToMaxSize(6000.0f), ForearmBone, true);
		}

		// Latched, not read on the last frame alone: bracing happens somewhere in the middle of a fall, so a
		// check made only at the end reported a failure every time the hands had already pushed off again.
		if (FMath::Abs(Error.Z) < 16.0f)
		{
			bBraced = true;
		}
	}

	if (BraceTime <= 0.0f)
	{
		UE_LOG(LogImpact, Display, TEXT("[chute] t=%.2f %s : %s"), GetWorld()->GetTimeSeconds(), *GetOwner()->GetName(),
		       bBraced ? TEXT("amorti la chute sur les bras") : TEXT("n'a pas eu le temps de se protéger"));
		if (!bCrawling)
		{
			SetArmReaching(0, false);
			SetArmReaching(1, false);
		}
	}
}

void UImpactReactionComponent::UpdateHitPose(const float DeltaTime)
{
	AAlsCharacter* Character = GetCharacter();
	USkeletalMeshComponent* Mesh = Character != nullptr ? Character->GetMesh() : nullptr;
	UAlsAnimationInstance* Anim = Mesh != nullptr ? Cast<UAlsAnimationInstance>(Mesh->GetAnimInstance()) : nullptr;
	if (Anim == nullptr)
	{
		HitPoseAnim = nullptr;
		return;
	}
	if (HitPoseAnim == nullptr)
	{
		if (HitPoseWeight > 0.0f)
		{
			HitPoseWeight = 0.0f;
			Anim->SetTrunkSlotWeight(0.0f);
		}
		return;
	}

	HitPoseTime += DeltaTime * HitPoseRate;

	// In at once, out over the second half: a body is struck suddenly and recovers gradually.
	HitPoseWeight = HitPoseTime < HitPoseLength * 0.4f
		                ? FMath::Min(1.0f, HitPoseWeight + DeltaTime * 12.0f)
		                : FMath::Max(0.0f, HitPoseWeight - DeltaTime * 2.2f);

	// Only while the character is on his feet and acting. Once he goes down the body belongs to the
	// physics, and a trunk pose fighting a ragdoll reads as a twitch.
	if (HitPoseTime > HitPoseLength || HitPoseWeight <= 0.0f || IsDown() || !bVoluntaryControl)
	{
		HitPoseAnim = nullptr;
		HitPoseWeight = 0.0f;
		Anim->SetTrunkSlotWeight(0.0f);
		return;
	}

	Anim->SetTrunkSlotWeight(HitPoseWeight);
}

FName UImpactReactionComponent::GroundSlotName()
{
	// "Ragdoll" is a node that stock ALS does not have: a Slot inside AB_Als_Ragdolling, between the blend
	// that chooses the ragdoll pose and the output. -ImpactSolSlot= aims this elsewhere, which is how the
	// Transition slot that already exists further up the graph gets tried.
	static FName Name = NAME_None;
	if (Name.IsNone())
	{
		FString Parsed;
		Name = FParse::Value(FCommandLine::Get(), TEXT("ImpactSolSlot="), Parsed) && !Parsed.IsEmpty()
			       ? FName(*Parsed)
			       : FName(TEXT("Ragdoll"));
	}
	return Name;
}

void UImpactReactionComponent::StopGroundSlot(const float BlendOut)
{
	GroundRagdollWeight = 0.0f;
	bGroundHeld = false;
	bGroundAnchored = false;
	ReleasePacedJointTargets();

	if (const AAlsCharacter* Owner = GetCharacter())
	{
		if (bGroundVelocityDrive)
		{
			for (FConstraintInstance* Joint : Owner->GetMesh()->Constraints)
			{
				if (Joint != nullptr)
				{
					Joint->SetAngularVelocityDriveTwistAndSwing(false, false);
				}
			}
			bGroundVelocityDrive = false;
		}
		if (GroundMuscleApplied >= 0.0f && PhysicalAnimation != nullptr)
		{
			// Back to what writhing asks for, or to nothing for someone who has lost control meanwhile.
			PhysicalAnimation->SetStrengthMultiplyer(bWrithing && bVoluntaryControl ? WritheMuscleForce() : (bLimp ? 0.0f : 0.32f));
		}
		GroundMuscleApplied = -1.0f;
	}

	// Give the bodies their own drag back before anything else can read it.
	if (const AAlsCharacter* Owner = GetCharacter(); Owner != nullptr && GroundBaseDamping.Num() > 0)
	{
		USkeletalMeshComponent* Skeleton = Owner->GetMesh();
		for (int32 i = 0; i < Skeleton->Bodies.Num() && i < GroundBaseDamping.Num(); ++i)
		{
			if (FBodyInstance* Body = Skeleton->Bodies[i])
			{
				Body->LinearDamping = GroundBaseDamping[i];
				Body->UpdateDampingProperties();
			}
		}
		GroundBaseDamping.Reset();
	}
	if (GroundMontage == nullptr)
	{
		return;
	}
	const AAlsCharacter* Character = GetCharacter();
	if (UAnimInstance* Anim = Character != nullptr ? Character->GetMesh()->GetAnimInstance() : nullptr)
	{
		// Named, never blanket: a bare Montage_Stop takes the get-up montage down with it, and the get-up
		// is the one montage that must be allowed to finish.
		Anim->Montage_Stop(BlendOut, GroundMontage);
	}
	GroundMontage = nullptr;
}

void UImpactReactionComponent::PushedByPawns(const float DeltaTime)
{
	// Reported from play: "on peut pas les pousser, t'as jamais rajoute cette fonction, ils reagissent pas
	// au joueur comme dans la video". Both halves were true, and they were two different faults.
	//
	// The first was contact. The engine's CharacterMesh profile answers ECR_Ignore on the Pawn channel, so
	// a player walked clean through a body on the floor: measured, the gap from his capsule to the pelvis
	// went 130, 20, 4, 1, 0 cm and he ended up standing inside the hips. That is fixed where the ragdoll
	// starts, by answering blocking - ALS has already switched the ragdoll's own capsule off, so there is
	// nothing of his own left for his limbs to shove.
	//
	// The second is this. Blocking alone turned the body into a wall: the player stopped dead 63 cm from
	// the hips and the hips moved 0,5 cm. Nothing was pushed. The movement component's own physics
	// interaction cannot help, because it scales its force by the speed it has left, and what it has left
	// against a wall is nothing - it stops, so it does not push, so it stays stopped.
	//
	// So the force is taken from the speed the pawn is trying to travel at, not from the collision. Walk
	// into a body and the body is shoved along; stand still against it and nothing happens.
	static const bool bAllowed = !FParse::Param(FCommandLine::Get(), TEXT("ImpactSansBourrade"));
	static const bool bDeadPassThrough = GroundFlag(TEXT("ImpactMortTraversable"));
	if (!bAllowed || DeltaTime <= 0.0f || (bDeadPassThrough && bDead))
	{
		return;
	}

	AAlsCharacter* Character = GetCharacter();
	USkeletalMeshComponent* Mesh = Character->GetMesh();
	const FVector Hips = Mesh->GetSocketLocation(UAlsConstants::PelvisBoneName());

	TArray<FOverlapResult> Found;
	const FCollisionQueryParams Params(SCENE_QUERY_STAT(ImpactBourrade), false, GetOwner());
	// By object type, not by channel. Asking by channel asks "what answers the Pawn channel", and ALS's
	// own capsule does not - characters are meant to walk past each other, not collide. So the query came
	// back with a piece of scenery and nothing else, every frame, while the player stood 63 cm away.
	GetWorld()->OverlapMultiByObjectType(Found, Hips, FQuat::Identity, FCollisionObjectQueryParams(ECC_Pawn),
	                                     FCollisionShape::MakeSphere(220.0f), Params);
	if (Found.Num() == 0)
	{
		return;
	}

	// A velocity change per second, so it does not depend on how heavy a forearm happens to be.
	static float Gain = -1.0f;
	if (Gain < 0.0f)
	{
		float Parsed = 4.0f;
		Gain = FParse::Value(FCommandLine::Get(), TEXT("ImpactBourradeForce="), Parsed) ? FMath::Max(0.0f, Parsed) : 4.0f;
	}

	// One entry per body of every mesh in range, so the same person comes back twenty times over.
	TArray<const ACharacter*> Pawns;
	for (const FOverlapResult& Overlap : Found)
	{
		const ACharacter* Found2 = Cast<ACharacter>(Overlap.OverlapObjectHandle.FetchActor());
		if (Found2 != nullptr && Found2 != GetOwner() && Found2->GetCapsuleComponent() != nullptr)
		{
			Pawns.AddUnique(Found2);
		}
	}

	for (const ACharacter* Pawn : Pawns)
	{
		// What he is trying to do, not what the collision left him with. A man leaning into a body is
		// pushing it even though he is not moving - and against a body that blocks him he is never
		// moving, which is the whole difficulty. His acceleration is the only thing that still says so:
		// GetLastInputVector came back empty here, and his velocity is 1 cm/s because he is stopped dead.
		const UCharacterMovementComponent* Movement = Pawn->GetCharacterMovement();
		if (Movement == nullptr)
		{
			continue;
		}
		const FVector Effort = Movement->GetCurrentAcceleration();
		const float MaxEffort = Movement->GetMaxAcceleration();
		const FVector Dir = (Effort.IsNearlyZero() ? Pawn->GetVelocity() : Effort).GetSafeNormal2D();
		const float Speed = Effort.IsNearlyZero() || MaxEffort <= 0.0f
			                    ? Pawn->GetVelocity().Size2D()
			                    : Movement->GetMaxSpeed() * FMath::Min(1.0f, Effort.Size() / MaxEffort);
		if (Dir.IsNearlyZero() || Speed < 20.0f)
		{
			continue;
		}

		// His own capsule, grown a little, tested against each body for real. Measuring centre-to-centre
		// against a flat radius did not work and could not: whether anything fell inside it depended on
		// how the body had landed. One run had him blocked 57 cm from the hips and the shove moved them
		// 56 cm; the next had him blocked at 63 with nothing in range, so nothing moved at all. That
		// looked like a force problem for a while, and raising the force changed nothing in the runs
		// where nothing was in range - because what holds his capsule out is the collision shape of
		// whichever limb is nearest, which is not at a fixed distance from anything.
		static float Margin = -1.0f;
		if (Margin < 0.0f)
		{
			float Parsed = 12.0f;
			Margin = FParse::Value(FCommandLine::Get(), TEXT("ImpactBourradePortee="), Parsed) ? FMath::Max(0.0f, Parsed) : 12.0f;
		}
		const UCapsuleComponent* Capsule = Pawn->GetCapsuleComponent();
		const FCollisionShape Touch = FCollisionShape::MakeCapsule(
			Capsule->GetScaledCapsuleRadius() + Margin, Capsule->GetScaledCapsuleHalfHeight() + Margin);
		const FVector Feet = Capsule->GetComponentLocation();
		const FQuat Facing = Capsule->GetComponentQuat();
		int32 Pushed = 0;
		float Closest = 1e9f;
		FVector Shove = FVector::ZeroVector;
		for (FBodyInstance* Body : Mesh->Bodies)
		{
			// Lying animated, nothing simulates, but walking into him is still walking into him.
			if (Body == nullptr || (!Body->IsInstanceSimulatingPhysics() && !IsLyingDown()))
			{
				continue;
			}
			FVector Flat = Body->GetUnrealWorldTransform().GetLocation() - Feet;
			Flat.Z = 0.0f;
			Closest = FMath::Min(Closest, static_cast<float>(Flat.Size()));
			if (!Body->OverlapTest(Feet, Facing, Touch))
			{
				continue;
			}
			ReturnToRagdoll(TEXT("bouscule"), false);
			// Along the way he is going, and a little away from him, so a body is shoved aside instead of
			// being dribbled ahead of his feet.
			const FVector Away = (Dir + Flat.GetSafeNormal() * 0.4f).GetSafeNormal();
			Body->AddImpulse(Away * Speed * Gain * DeltaTime, true);
			++Pushed;
			Shove += Away;
		}

		// And the whole man, not only the limb that happened to be in the way. Walking into an outflung
		// arm pushed the arm: seven bodies shoved at 397 cm/s and the hips moved half a centimetre, which
		// is correct physics and the wrong thing to see. A body shoved anywhere travels as one, because
		// what is being moved is a man lying on a floor, not a rag.
		if (Pushed > 0)
		{
			LastGroundPushTime = GetWorld()->GetTimeSeconds();
			const FVector Spread = Shove.GetSafeNormal();
			// -ImpactBourradeBloc: carried along, not nudged. The push above is weaker than the floor's grip on a body - a fraction
			// of the friction a lying man has - so the limbs were shoved and the trunk stayed put: measured with the player walking
			// into the hips for ten seconds, the pelvis moved 0.4 to 8 cm and the limbs 7 to 70. Reported from play: "le corps ne
			// part pas d'un bloc". Here every body is brought towards the same horizontal velocity, a share of the walker's, as long
			// as he keeps walking into it: the body slides along the floor as one piece, at a man's pushing pace.
			static const bool bAsOne = GroundFlag(TEXT("ImpactBourradeBloc"));
			static float Carry = -1.0f;
			if (Carry < 0.0f)
			{
				// A third of his pace: at 0.6 the body slid ahead of a walking man like a sled, 18 to 21 m in ten seconds; at 0.35
				// it goes 9 to 14 m, shoved along and rolling a little, the walker close behind.
				float Parsed = 0.35f;
				Carry = FParse::Value(FCommandLine::Get(), TEXT("ImpactBourradeEntrainement="), Parsed) ? FMath::Clamp(Parsed, 0.0f, 1.5f) : 0.35f;
			}
			const FVector Wanted = Spread * Speed * Carry;
			const float Catch = 1.0f - FMath::Exp(-DeltaTime / 0.15f);
			for (FBodyInstance* Body : Mesh->Bodies)
			{
				if (Body == nullptr || !Body->IsInstanceSimulatingPhysics())
				{
					continue;
				}
				if (bAsOne)
				{
					const FVector Velocity = Body->GetUnrealWorldVelocity();
					const FVector Flat(Velocity.X, Velocity.Y, 0.0f);
					Body->AddImpulse((Wanted - Flat) * Catch, true);
				}
				else
				{
					Body->AddImpulse(Spread * Speed * Gain * 0.35f * DeltaTime, true);
				}
			}
		}
		// Verbose, not Display: a fight with twenty bodies in it would drown the log.
		if (GetWorld()->TimeSince(LastPushedLog) > 1.0f)
		{
			LastPushedLog = GetWorld()->GetTimeSeconds();
			UE_LOG(LogImpact, Log, TEXT("[bourrade] %s pousse %s : %d corps, %.0f cm/s, plus proche %.0f cm"),
			       *Pawn->GetName(), *GetOwner()->GetName(), Pushed, Speed, Closest);
		}
	}
}

void UImpactReactionComponent::UpdateGroundRagdoll(const float DeltaTime)
{
	// A ground posture without leaving the ragdoll.
	//
	// The previous attempt borrowed ALS's get-up to hand the body back to the animation, and that is what
	// broke it: a character in GettingUp with a montage that never ends, inside a state machine built
	// around a get-up that does. This one never leaves the ragdoll, so the state machine is never lied to.
	//
	// What makes it possible is already in ALS, two lines of RefreshRagdolling:
	//
	//     const auto SpeedAmount{Clamp01(RagdollingState.Velocity.Size() / 1000.0f)};
	//     GetMesh()->SetAllMotorsAngularDriveParams(SpeedAmount * 25000.0f, 0.0f, 0.0f);
	//
	// The ragdoll's joints are already driven towards whatever pose the animation graph is handing out -
	// but scaled by speed, so a body that has come to rest has no muscle at all. That is precisely why a
	// settled ragdoll is a sack: not because nothing can pose it, but because ALS switches the drive off.
	// We tick after the character (see BeginPlay), so putting the stiffness back is ours to do.
	//
	// It needs the pose to be the right one, and that needs a node ALS does not have. Without the slot the
	// graph keeps handing out A_Als_Flail and the stiffness below would fold him into a flail on the
	// floor - worse than lying still. So this stays behind a switch until the node exists.
	static const bool bAllowed = FParse::Param(FCommandLine::Get(), TEXT("ImpactSolRagdoll"));
	if (!bAllowed)
	{
		return;
	}

	AAlsCharacter* Character = GetCharacter();
	USkeletalMeshComponent* Mesh = Character->GetMesh();
	UAnimInstance* Anim = Mesh->GetAnimInstance();
	if (Anim == nullptr)
	{
		return;
	}

	// Conscious, in the ragdoll, hurt, and not already using his arms for something else. An unconscious
	// body has no muscle anywhere - UpdateDown zeroes the drives for him, and this must not put them back.
	//
	// Settling is asked once, on the way in, and never again. Measured the other way round, the thing ate
	// itself: the drives move the body, moving the body puts the ragdoll's speed back over the 25 cm/s that
	// counts as settled, SettledTime resets, the posture is dropped, the body comes to rest, the posture
	// starts over. Nineteen to twenty-eight restarts in a twenty-six second run, and at the stiffer setting
	// more of them, so nothing ever held long enough to fold anything. What ends it is leaving the ragdoll,
	// which is a state and does not flicker - and that test has to stay, because this is called before the
	// get-up branch returns and a take left playing over a get-up is exactly the old fault again.
	const bool bRagdolling = Character->GetLocomotionAction() == AlsLocomotionActionTags::Ragdolling;
	// -ImpactSolEntreeTot: the posture starts once he has been lying on the floor for a second, not once he has
	// stopped moving. Waiting for him to be still (under 25 cm/s for half a second) left the body to ALS's flight
	// muscles for as long as it kept moving, and on a floor those keep it moving: the posture started 5 to 9 s
	// after the fall in the runs where the body thrashed, 2 to 3 s in the others. The pacer starts from wherever
	// the body is, so it does not need him to be still.
	static const bool bEarlyEntry = GroundFlag(TEXT("ImpactSolEntreeTot"));
	// -ImpactChuteFinie: not over a fall still playing. The posture plays in the same slot with no blend, so starting it cut the fall
	// dead - 37 falls of 63 never reached their end - and the ground take came in from its first frame over whatever the fall had
	// reached: "a la pose finale d'une animation, il revient a la pose zero d'une autre".
	static const bool bFallFirst = FParse::Param(FCommandLine::Get(), TEXT("ImpactChuteFinie"));
	const bool bWanted = bWrithing && GroundTake != nullptr && bVoluntaryControl && bRagdolling && !(bFallFirst && bBlendedFall)
		&& !bCrawling && BraceTime <= 0.0f && (bGroundHeld || SettledTime > 0.5f || (bEarlyEntry && GroundContactTime > 1.0f));

	if (!bWanted)
	{
		if (GroundMontage != nullptr)
		{
			StopGroundSlot(0.4f);
		}
		return;
	}

	if (GroundMontage == nullptr || !Anim->Montage_IsPlaying(GroundMontage))
	{
		// Slowly. The take loops, and a loop replayed through the joint drives is a swimming stroke: the
		// same pushes against the same floor in the same direction, every five seconds. That is what the
		// sliding was - 50 cm/s held steady for the whole run, in one direction, where a plain ragdoll
		// drifts under 10. Drag alone could not answer it, because drag resists speed and the stroke
		// simply kept supplying more. Playing the loop slowly attacks the stroke itself, and it is also
		// what a man on the ground with a hole in him does: he moves slowly.
		static float Rate = -1.0f;
		if (Rate < 0.0f)
		{
			float Parsed = 0.35f;
			Rate = FParse::Value(FCommandLine::Get(), TEXT("ImpactSolVitesseAnim="), Parsed) ? FMath::Max(0.01f, Parsed) : 0.35f;
		}

		// Sixty loops, not none: a dynamic montage built with no loops comes out zero length, and
		// Montage_Play refuses a montage of zero length without saying so.
		// Under the pacer the take is there from the first frame: the pacer, not the montage blend, carries the
		// body from where it lies. Blending in from the graph meant blending in from a frozen frame of A_Als_Flail
		// (FlailPlayRate is the ragdoll speed over 1000, so it stops at rest), a pose the body was never in.
		static const bool bPacedEntry = FParse::Param(FCommandLine::Get(), TEXT("ImpactSolCible"));
		// -ImpactSolMixamo: the take chosen when he went down was chosen standing; now that he lies on the floor, which way he
		// lies is known.
		if (SolMixamo())
		{
			UAnimSequenceBase* Better = PickMixamoGroundTake();
			if (Better != nullptr && Better != GroundTake && AnimReady(Better))
			{
				UE_LOG(LogImpact, Display, TEXT("[sol-mixamo] t=%.2f %s : couche %s, %s au lieu de %s"), GetWorld()->GetTimeSeconds(),
				       *GetOwner()->GetName(), IsFacingDown() ? TEXT("sur le ventre") : TEXT("sur le dos"), *Better->GetName(),
				       *GroundTake->GetName());
				GroundTake = Better;
				Anim->PlaySlotAnimationAsDynamicMontage(GroundTake, UAlsConstants::TransitionSlotName(), 0.35f, 0.4f, 1.0f, 0);
			}
		}
		GroundMontage = UAnimMontage::CreateSlotAnimationAsDynamicMontage(GroundTake, GroundSlotName(),
		                                                                 bPacedEntry ? 0.0f : 0.5f, 0.4f, Rate, 60);
		if (GroundMontage == nullptr)
		{
			return;
		}
		// [sol-enchaine] / -ImpactSolEnchaine: the take started where its pose is nearest the body's, not at its first frame. The pacer
		// carries the joints from where they are to the take, so the farther the take's pose, the longer and stranger the re-posing
		// (2,06 s for 123 deg measured on the 27th). Turn-free: the pacer's targets are the joints' own, the body's heading is its own.
		float StartTime = 0.0f;
		{
			static const bool bMatch = FParse::Param(FCommandLine::Get(), TEXT("ImpactSolEnchaine"));
			float Yaw = 0.0f, Score = 0.0f, YawZero = 0.0f, ScoreZero = 0.0f, RawZero = 0.0f;
			FVector PelvisInMesh;
			if (MatchTakePose(GroundTake, StartTime, Yaw, Score, YawZero, ScoreZero, RawZero, PelvisInMesh))
			{
				UE_LOG(LogImpact, Display, TEXT("[sol-enchaine] t=%.2f %s : posture %s, ecart de pose a l'image 0 %.0f cm, au meilleur moment (%.2f s) %.0f cm, %s"),
				       GetWorld()->GetTimeSeconds(), *GetOwner()->GetName(), *GroundTake->GetName(), ScoreZero, StartTime, Score,
				       bMatch ? TEXT("part de la") : TEXT("temoin, part de l'image 0"));
			}
			if (!bMatch)
			{
				StartTime = 0.0f;
			}
		}
		Anim->Montage_Play(GroundMontage, 1.0f, EMontagePlayReturnType::MontageLength, StartTime);
		GroundRagdollWeight = 0.0f;
		bGroundHeld = true;
		GroundMuscleApplied = -1.0f;

		static const bool bDampingActive = GroundFlag(TEXT("ImpactSolAmortiActif"));
		if (bDampingActive)
		{
			for (FConstraintInstance* Joint : Mesh->Constraints)
			{
				if (Joint != nullptr)
				{
					Joint->SetAngularVelocityTarget(FVector::ZeroVector);
					Joint->SetAngularVelocityDriveTwistAndSwing(true, true);
				}
			}
			bGroundVelocityDrive = true;
		}
		UE_LOG(LogImpact, Display, TEXT("[sol] %s au sol: %s dans le slot %s"), *GetOwner()->GetName(),
		       *GroundTake->GetName(), *GroundSlotName().ToString());
	}

	// The muscle comes in over a second. A body that has just stopped tumbling and is suddenly stiff looks
	// like a mannequin being set down; the same stiffness arriving slowly looks like someone gathering
	// himself. The montage's own half-second blend is not enough on its own, because the drive is what
	// decides how hard the joints chase that pose.
	static float RampTime = -1.0f;
	if (RampTime < 0.0f)
	{
		float Parsed = 1.0f;
		RampTime = FParse::Value(FCommandLine::Get(), TEXT("ImpactSolMontee="), Parsed) ? FMath::Max(0.05f, Parsed) : 1.0f;
	}
	GroundRagdollWeight = FMath::Min(1.0f, GroundRagdollWeight + DeltaTime / RampTime);

	// Under the pacer the joints aim at where they already are, so full stiffness from the first frame holds the
	// body instead of letting it sag while the muscles are taken away. The ramp only ever softened a jump - and it
	// did not even do that: Chaos closes k.dt2/(1+k.dt2) of the error per step, still 20 % at a tenth of the stiffness.
	static const bool bPacedStiff = FParse::Param(FCommandLine::Get(), TEXT("ImpactSolCible"));
	if (bPacedStiff)
	{
		GroundRagdollWeight = 1.0f;
	}

	static const bool bNoMuscles = GroundFlag(TEXT("ImpactSolSansMuscles"));
	if (bNoMuscles && PhysicalAnimation != nullptr)
	{
		const float Wanted = WritheMuscleForce() * (1.0f - GroundRagdollWeight);
		// Each change rewrites every muscle drive, so only when it is worth it.
		if (GroundMuscleApplied < 0.0f || FMath::Abs(Wanted - GroundMuscleApplied) > 0.01f * FMath::Max(0.1f, WritheMuscleForce()))
		{
			PhysicalAnimation->SetStrengthMultiplyer(Wanted);
			GroundMuscleApplied = Wanted;
		}
	}

	// A quarter of what ALS gives a ragdoll at full flight. Enough to fold a body that is only fighting
	// gravity and friction, far short of what would make it a puppet: pushed, shot again or walked into,
	// it still gives way, which is the whole point of staying in the simulation.
	static float Stiffness = -1.0f;
	static float Damping = -1.0f;
	if (Stiffness < 0.0f)
	{
		float Parsed = 6000.0f;
		Stiffness = FParse::Value(FCommandLine::Get(), TEXT("ImpactSolRaideur="), Parsed) ? FMath::Max(0.0f, Parsed) : 6000.0f;
		Parsed = 400.0f;
		Damping = FParse::Value(FCommandLine::Get(), TEXT("ImpactSolAmorti="), Parsed) ? FMath::Max(0.0f, Parsed) : 400.0f;
	}
	// Only the trunk, the arms and the head. The legs are the thing that pushes against a floor, and with
	// them driven the body did not take a posture, it swam: net drift 201 cm by eight seconds and 223 cm
	// by nineteen, where a plain ragdoll settles 66 cm from where it fell and then stops for good. Three
	// things were tried against that and all three failed - linear drag at 8, 40 and 120, the loop played
	// at 1.0, 0.35 and 0.12, joint damping at 400, 3000 and 8000 - because none of them touched what was
	// doing it. A man curling up on the ground does draw his knees in, so this costs something real; what
	// it buys is a body that stays where it fell. -ImpactSolJambes drives them too, for measuring.
	// Measured, legs driven like the rest: net drift 201 cm by eight seconds and 223 cm by nineteen, still
	// climbing, where a plain ragdoll settles 66 cm from where it fell and then stops for good. Cutting
	// them entirely stopped it dead - drift frozen at 166 and 102 cm, travel 1 to 9 cm/s, which is what a
	// ragdoll does - and cost the fold: head-to-knees 97-106 against 88-93 with them.
	//
	// So the hips and knees keep a share of the drive and the feet get none. The feet are what presses the
	// floor; a knee drawing in does not push anything. A man curling up does bring his knees up, so taking
	// the legs away entirely was giving up something real.
	//
	// Three other remedies were tried against the swimming and all three failed, because none of them
	// touched what was doing it: linear drag at 8, 40 and 120, the loop played at 1.0, 0.35 and 0.12, and
	// joint damping at 400, 3000 and 8000.
	static float LegShare = -1.0f;
	if (LegShare < 0.0f)
	{
		float Parsed = 0.25f;
		LegShare = FParse::Value(FCommandLine::Get(), TEXT("ImpactSolJambes="), Parsed) ? FMath::Clamp(Parsed, 0.0f, 1.0f) : 0.25f;
	}
	// The way into the pose. The drives above run in acceleration mode with no limit at all - a force limit of
	// zero is read by Chaos as "no clamping" (PBDJointCachedSolverGaussSeidel.cpp, FAxisConstraintDatas::SetMaxForce)
	// - and their target is the animation pose, which the slot switches to in half a second. At 6000 and 400 a
	// joint closes most of a radian of error in about a tenth of a second, so every limb snapped across the body to
	// wherever the take had it. Reported from play: "des mouvements inhumains pour prendre la pose".
	// -ImpactSolCible moves the target instead, from the pose the body is really in, at a human pace.
	static const bool bPaced = FParse::Param(FCommandLine::Get(), TEXT("ImpactSolCible"));
	static float JointSpeed = -1.0f;
	static float TorqueCap = -1.0f;
	if (JointSpeed < 0.0f)
	{
		float Parsed = 60.0f;
		JointSpeed = FParse::Value(FCommandLine::Get(), TEXT("ImpactSolVitesseArt="), Parsed) ? FMath::Max(1.0f, Parsed) : 60.0f;
		Parsed = 0.0f;
		// rad/s2 in acceleration mode; 0 keeps the engine's "unlimited".
		TorqueCap = FParse::Value(FCommandLine::Get(), TEXT("ImpactSolCouple="), Parsed) ? FMath::Max(0.0f, Parsed) : 0.0f;
	}
	if (bPaced)
	{
		UpdatePacedJointTargets(DeltaTime, JointSpeed);
	}
	UpdateGroundAnchor(DeltaTime);

	// -ImpactSolAnime: once the body has been brought into the take and has come to rest in it, the take is played for real.
	static const bool bAnimated = GroundFlag(TEXT("ImpactSolAnime"));
	if (bAnimated && bPacedTargets && PacedDuration > 0.0f && PacedTime > PacedDuration + 0.8f
	    && Character->GetRagdollingState().Velocity.Size() < 30.0f && TryEnterLyingDown())
	{
		return;
	}

	const float Spring = Stiffness * GroundRagdollWeight;
	const float Damp = Damping * GroundRagdollWeight;
	for (FConstraintInstance* Constraint : Mesh->Constraints)
	{
		if (Constraint == nullptr)
		{
			continue;
		}
		const FString Joint = Constraint->JointName.ToString();
		const bool bFoot = Joint.StartsWith(TEXT("foot")) || Joint.StartsWith(TEXT("ball"));
		const bool bLeg = Joint.StartsWith(TEXT("thigh")) || Joint.StartsWith(TEXT("calf"));
		// -ImpactSolAnime: the legs get the trunk's stiffness. A quarter of it was to keep a long physical hold from swimming; here the
		// physical part only lasts until the animation takes over, and legs left nearly straight were seen to snap in at the handover.
		static const bool bAnimatedHold = GroundFlag(TEXT("ImpactSolAnime"));
		const float Share = bFoot ? 0.0f : bLeg ? (bAnimatedHold ? 1.0f : LegShare) : 1.0f;
		Constraint->SetAngularDriveParams(Spring * Share, Damp * Share, TorqueCap);
	}

	// And the floor has to hold him while he does it. The drives fold the body - measured, head-to-knees
	// 87-92 cm against 106-110 doing nothing - but a body writhing on a floor pushes against that floor,
	// and the ragdoll turned every bit of that into travel: 38 to 188 cm/s across the ground where lying
	// still gives 2 to 9. He does not take a posture, he swims away in it.
	//
	// Drag, not a speed limit. A limit would also swallow the shove of the next bullet, and being shot
	// while down has to still move him. Drag resists speed in proportion to it, so a short hard push
	// still carries while a slow endless one is cancelled - which is what friction against a floor does.
	static float Drag = -1.0f;
	if (Drag < 0.0f)
	{
		float Parsed = 8.0f;
		Drag = FParse::Value(FCommandLine::Get(), TEXT("ImpactSolTraine="), Parsed) ? FMath::Max(0.0f, Parsed) : 8.0f;
	}
	if (GroundBaseDamping.Num() != Mesh->Bodies.Num())
	{
		GroundBaseDamping.Reset();
		for (const FBodyInstance* Body : Mesh->Bodies)
		{
			GroundBaseDamping.Add(Body != nullptr ? Body->LinearDamping : 0.0f);
		}
	}
	for (int32 i = 0; i < Mesh->Bodies.Num(); ++i)
	{
		FBodyInstance* Body = Mesh->Bodies[i];
		const float Wanted = GroundBaseDamping[i] + Drag * GroundRagdollWeight;
		if (Body != nullptr && !FMath::IsNearlyEqual(Body->LinearDamping, Wanted, 0.01f))
		{
			Body->LinearDamping = Wanted;
			Body->UpdateDampingProperties();
		}
	}
}

void UImpactReactionComponent::UpdatePacedJointTargets(const float DeltaTime, const float DegreesPerSecond)
{
	USkeletalMeshComponent* Mesh = GetCharacter()->GetMesh();
	const UPhysicsAsset* Asset = Mesh->GetPhysicsAsset();
	const USkeletalMesh* Skeleton = Mesh->GetSkeletalMeshAsset();
	if (Asset == nullptr || Skeleton == nullptr || Asset->ConstraintSetup.Num() != Mesh->Constraints.Num())
	{
		return;
	}

	// The engine sets these targets itself each frame from the animation (UpdateRBJointMotors) while
	// bUpdateJointsFromAnimation is on, which ALS turns on for the whole ragdoll. Ours replace them only for the
	// length of the ground posture; ReleasePacedJointTargets hands them back.
	const bool bFirst = !bPacedTargets;
	if (bFirst)
	{
		Mesh->bUpdateJointsFromAnimation = false;
		PacedTargets.Init(FQuat::Identity, Mesh->Constraints.Num());
		PacedStart.Init(FQuat::Identity, Mesh->Constraints.Num());
		PacedTargetsSet.Init(false, Mesh->Constraints.Num());
		PacedTime = 0.0f;
		PacedDuration = -1.0f;
		bPacedTargets = true;
	}
	PacedTime += DeltaTime;

	const FReferenceSkeleton& RefBones = Skeleton->GetRefSkeleton();
	const TArray<FTransform>& Local = Mesh->GetBoneSpaceTransforms();
	const float SafetyStep = FMath::DegreesToRadians(FMath::Max(DegreesPerSecond, 120.0f)) * DeltaTime;

	TArray<FQuat> Animated;
	Animated.Init(FQuat::Identity, Mesh->Constraints.Num());
	TArray<bool> Valid;
	Valid.Init(false, Mesh->Constraints.Num());

	for (int32 i = 0; i < Mesh->Constraints.Num(); ++i)
	{
		const UPhysicsConstraintTemplate* Setup = Asset->ConstraintSetup[i];
		const FConstraintInstance* Joint = Mesh->Constraints[i];
		if (Setup == nullptr || Joint == nullptr || !Joint->IsAngularOrientationDriveEnabled())
		{
			continue;
		}

		const FName Child = Setup->DefaultInstance.ConstraintBone1;
		const FName Parent = Setup->DefaultInstance.ConstraintBone2;
		const int32 BoneIndex = RefBones.FindBoneIndex(Child);
		if (BoneIndex == INDEX_NONE || BoneIndex == 0 || !Local.IsValidIndex(BoneIndex))
		{
			continue;
		}

		// The animated target, computed exactly as USkeletalMeshComponent::UpdateRBJointMotors does
		// (PhysAnim.cpp): the child's local rotation, carried up to the bone that holds the parent body, then
		// expressed between the two constraint frames.
		const FMatrix LocalRotation = FQuatRotationTranslationMatrix(Local[BoneIndex].GetRotation(), FVector::ZeroVector);
		FMatrix ControlToParent = FMatrix::Identity;
		int32 Test = RefBones.GetParentIndex(BoneIndex);
		bool bFound = Test != INDEX_NONE && RefBones.GetBoneName(Test) == Parent;
		while (!bFound && Test > 0 && Local.IsValidIndex(Test))
		{
			FMatrix Relative = Local[Test].ToMatrixNoScale();
			Relative.SetOrigin(FVector::ZeroVector);
			ControlToParent = ControlToParent * Relative;
			Test = RefBones.GetParentIndex(Test);
			bFound = Test != INDEX_NONE && RefBones.GetBoneName(Test) == Parent;
		}
		if (!bFound)
		{
			continue;
		}

		FMatrix Frame1 = Setup->DefaultInstance.GetRefFrame(EConstraintFrame::Frame1).ToMatrixNoScale();
		Frame1.SetOrigin(FVector::ZeroVector);
		FMatrix Frame2 = Setup->DefaultInstance.GetRefFrame(EConstraintFrame::Frame2).ToMatrixNoScale();
		Frame2.SetOrigin(FVector::ZeroVector);
		const FMatrix Frame2Inverse = Frame2.InverseFast();
		Animated[i] = FQuat(Frame1 * (LocalRotation * ControlToParent) * Frame2Inverse).GetNormalized();
		Valid[i] = true;

		// Where the joint really is when the posture starts: the same expression with the two bodies' rotations.
		if (!PacedTargetsSet[i])
		{
			FQuat Actual = Animated[i];
			const FBodyInstance* ChildBody = Mesh->GetBodyInstance(Child);
			const FBodyInstance* ParentBody = Mesh->GetBodyInstance(Parent);
			if (ChildBody != nullptr && ParentBody != nullptr)
			{
				const FQuat Between = ParentBody->GetUnrealWorldTransform().GetRotation().Inverse() * ChildBody->GetUnrealWorldTransform().GetRotation();
				Actual = FQuat(Frame1 * FQuatRotationMatrix(Between) * Frame2Inverse);
			}
			PacedStart[i] = Actual.GetNormalized();
			PacedTargets[i] = PacedStart[i];
			PacedTargetsSet[i] = true;
		}
	}

	// One movement for the whole body, not one per joint. A constant speed per joint made a small joint arrive at
	// once and a large one seconds later, one after the other like a robot. Here every joint starts from where it
	// lies and arrives together, on a minimum-jerk curve (slow start, slow stop), its length set by the largest
	// angle to cover at -ImpactSolVitesseArt=. The trunk leads and the hands and the head follow a little later.
	if (PacedDuration < 0.0f)
	{
		float Largest = 0.0f;
		for (int32 i = 0; i < Animated.Num(); ++i)
		{
			if (Valid[i])
			{
				Largest = FMath::Max(Largest, static_cast<float>(PacedStart[i].AngularDistance(Animated[i])));
			}
		}
		PacedDuration = FMath::Clamp(Largest / FMath::DegreesToRadians(FMath::Max(1.0f, DegreesPerSecond)), 0.8f, 2.5f);
		UE_LOG(LogImpact, Display, TEXT("[sol] %s pose prise en %.2f s (plus grand ecart %.0f deg)"), *GetOwner()->GetName(),
		       PacedDuration, FMath::RadiansToDegrees(Largest));
	}

	// -ImpactSolJambesTenues: once there, the legs keep the fold they arrived in instead of playing the loop. The take
	// loops, and a loop replayed through the leg drives is a stroke against the floor: measured with the trunk and
	// legs both following it, the curled body crawled 75 and 157 cm in twelve seconds, 6 to 11 cm/s, always the same
	// way. Earlier, cutting the legs' drive altogether stopped that dead but undid the fold; holding them keeps it.
	static const bool bLegsHeld = GroundFlag(TEXT("ImpactSolJambesTenues"));
	for (int32 i = 0; i < Animated.Num(); ++i)
	{
		if (!Valid[i])
		{
			continue;
		}
		const FString Name = Mesh->Constraints[i]->JointName.ToString();
		const float Delay = Name.StartsWith(TEXT("hand")) || Name.StartsWith(TEXT("head")) || Name.StartsWith(TEXT("foot")) || Name.StartsWith(TEXT("ball"))
			                    ? 0.3f
			                    : Name.StartsWith(TEXT("lowerarm")) || Name.StartsWith(TEXT("calf")) || Name.StartsWith(TEXT("neck"))
			                    ? 0.2f
			                    : Name.StartsWith(TEXT("upperarm")) || Name.StartsWith(TEXT("thigh"))
			                    ? 0.1f
			                    : 0.0f;
		const float U = FMath::Clamp((PacedTime - Delay) / PacedDuration, 0.0f, 1.0f);
		FQuat& Command = PacedTargets[i];
		if (U < 1.0f)
		{
			const float S = U * U * U * (10.0f - 15.0f * U + 6.0f * U * U);
			Command = FQuat::Slerp(PacedStart[i], Animated[i], S).GetNormalized();
		}
		else if (!(bLegsHeld && (Name.StartsWith(TEXT("thigh")) || Name.StartsWith(TEXT("calf")) || Name.StartsWith(TEXT("foot")) || Name.StartsWith(TEXT("ball")))))
		{
			// Arrived: follow the take as it plays, never faster than a joint turns.
			const float Remaining = static_cast<float>(Command.AngularDistance(Animated[i]));
			if (Remaining > UE_KINDA_SMALL_NUMBER)
			{
				Command = FQuat::Slerp(Command, Animated[i], FMath::Min(1.0f, SafetyStep / Remaining)).GetNormalized();
			}
		}
		Mesh->Constraints[i]->SetAngularOrientationTarget(Command);
	}
}

void UImpactReactionComponent::RefreshPoseNow() const
{
	// -ImpactPoseImmediate. Our switches (out of the ragdoll into a get-up or onto the floor, from standing into a fall) are made
	// from this component's tick, which can come after the mesh has already worked out its pose for the frame. That frame was
	// then drawn with the old pose at the new place: measured, the pelvis 150 to 165 cm away for one frame at every get-up after
	// a fall, then back. Reported from play: "des persos qui se TP", "des anims qui rollback". The pose is recomputed here, at once.
	static const bool bNow = GroundFlag(TEXT("ImpactPoseImmediate"));
	USkeletalMeshComponent* Mesh = GetCharacter() != nullptr ? GetCharacter()->GetMesh() : nullptr;
	if (bNow && Mesh != nullptr)
	{
		Mesh->TickAnimation(0.0f, false);
		Mesh->RefreshBoneTransforms();
	}
}

void UImpactReactionComponent::KeepWeaponInHand(const bool bHold)
{
	// -ImpactArmeEnMain. ALS hangs the weapon on a virtual bone, "VB hand_r_to_ik_hand_gun", which its own animations keep in the
	// palm. The falls and the ground takes come from Mixamo and never move the bone it is made from: through them it stays where a
	// standing man's hand would be. Measured: the pistol 196 to 216 cm from the hand, a metre above the pelvis, for the whole fall.
	// Reported from play: "quand on tire dans le genou et qu'il tombe, il lache le pistolet au milieu du vide, le pistolet tombe pas".
	// While he is down and until he is up again (the get-up starts from the pose the take left), what he holds is carried by the hand
	// bone itself, where it sat in his palm the last time ALS's own animations had him; once he is up, ALS has it back.
	static const bool bOn = GroundFlag(TEXT("ImpactArmeEnMain"));
	USkeletalMeshComponent* Mesh = GetCharacter() != nullptr ? GetCharacter()->GetMesh() : nullptr;
	if (!bOn || Mesh == nullptr)
	{
		return;
	}
	const auto Track = [this, Mesh](USceneComponent* Child) -> FCarried*
	{
		const FName Socket = Child != nullptr ? Child->GetAttachSocketName() : NAME_None;
		const bool bRight = Socket == UAlsConstants::HandRightGunVirtualBoneName();
		if (!bRight && Socket != UAlsConstants::HandLeftGunVirtualBoneName())
		{
			return nullptr;
		}
		FCarried* Item = Carried.FindByPredicate([Child](const FCarried& Known) { return Known.Item.Get() == Child; });
		if (Item == nullptr)
		{
			Item = &Carried.AddDefaulted_GetRef();
			Item->Item = Child;
		}
		Item->Socket = Socket;
		Item->Hand = bRight ? FName(TEXT("hand_r")) : FName(TEXT("hand_l"));
		Item->OnSocket = Child->GetRelativeTransform();
		return Item;
	};

	if (!bHold && !bWeaponInHand)
	{
		// Up, and ALS's own animations in charge: learn where each thing sits in the palm. Never from a fall, a take or a get-up, where
		// the gun bone can be anywhere (a grip taken there would carry the pistol two metres from the hand).
		if (GetCharacter()->GetLocomotionAction().IsValid())
		{
			return;
		}
		const TArray<TObjectPtr<USceneComponent>> Children = Mesh->GetAttachChildren();
		for (USceneComponent* Child : Children)
		{
			if (FCarried* Item = Track(Child))
			{
				const FTransform InHand = Child->GetComponentTransform().GetRelativeTransform(Mesh->GetSocketTransform(Item->Hand));
				if (InHand.GetLocation().Size() < 25.0f)
				{
					Item->InHand = InHand;
					Item->bGripKnown = true;
				}
			}
		}
		return;
	}

	if (bHold && !bWeaponInHand)
	{
		bWeaponInHand = true;
		const TArray<TObjectPtr<USceneComponent>> Children = Mesh->GetAttachChildren();
		for (USceneComponent* Child : Children)
		{
			FCarried* Item = Track(Child);
			if (Item == nullptr)
			{
				continue;
			}
			if (!Item->bGripKnown)
			{
				// Never seen standing: where it is now, if that is in the hand at all, or else on the hand bone itself.
				const FTransform Now = Child->GetComponentTransform().GetRelativeTransform(Mesh->GetSocketTransform(Item->Hand));
				Item->InHand = Now.GetLocation().Size() < 25.0f ? Now : FTransform::Identity;
			}
			Child->AttachToComponent(Mesh, FAttachmentTransformRules::KeepRelativeTransform, Item->Hand);
			Child->SetRelativeTransform(Item->InHand);
			Item->bHeld = true;
			// Carried by a bone of a ragdoll, a weapon that collides is a solid moved into that ragdoll every frame - his own hand pushing
			// his own body. Nothing of his collides with it while he is down.
			UPrimitiveComponent* Solid = Cast<UPrimitiveComponent>(Child);
			Item->Collision = Solid != nullptr ? static_cast<uint8>(Solid->GetCollisionEnabled()) : 0;
			if (Solid != nullptr)
			{
				Solid->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			}
			UE_LOG(LogImpact, Display, TEXT("[arme-main] t=%.2f %s : %s tenu par %s (prise %s, a %.0f cm de l'os, collision %d coupee)"), GetWorld()->GetTimeSeconds(),
			       *GetOwner()->GetName(), *Child->GetName(), *Item->Hand.ToString(), Item->bGripKnown ? TEXT("apprise debout") : TEXT("prise sur place"),
			       Item->InHand.GetLocation().Size(), Item->Collision);
		}
		return;
	}

	if (!bWeaponInHand)
	{
		return;
	}
	for (FCarried& Item : Carried)
	{
		USceneComponent* Child = Item.Item.Get();
		if (Child == nullptr || !Item.bHeld)
		{
			continue;
		}
		if (bHold)
		{
			if (Child->GetAttachParent() != Mesh || Child->GetAttachSocketName() != Item.Hand)
			{
				// Put back on a gun bone meanwhile: ALS does it when the weapon changes (two hands to one on a wound). Where ALS put it is
				// where it goes back to later; for now, into the hand again.
				if (Child->GetAttachParent() == Mesh && Child->GetAttachSocketName() == Item.Socket)
				{
					Item.OnSocket = Child->GetRelativeTransform();
				}
				Child->AttachToComponent(Mesh, FAttachmentTransformRules::KeepRelativeTransform, Item.Hand);
				Child->SetRelativeTransform(Item.InHand);
				UE_LOG(LogImpact, Display, TEXT("[arme-main] t=%.2f %s : %s remis dans la main"), GetWorld()->GetTimeSeconds(), *GetOwner()->GetName(), *Child->GetName());
			}
			// -ImpactArmeVisible: ALS's character takes the weapon off its mesh for as long as the ragdoll lasts, so he fell empty-handed
			// and his weapon came back once he lay still ([arme-effacee], 30 October: in every fall). He holds it all the way down.
			static const bool bKeepDrawn = GroundFlag(TEXT("ImpactArmeVisible"));
			USkeletalMeshComponent* Drawn = Cast<USkeletalMeshComponent>(Child);
			const FGameplayTag Overlay = GetCharacter()->GetOverlayMode();
			const bool bArmed = Overlay == AlsOverlayModeTags::Rifle || Overlay == AlsOverlayModeTags::PistolOneHanded
			                    || Overlay == AlsOverlayModeTags::PistolTwoHanded;
			if (bKeepDrawn && Drawn != nullptr && bArmed && Drawn->GetSkeletalMeshAsset() == nullptr)
			{
				USkeletalMesh* Weapon = LoadObject<USkeletalMesh>(nullptr, Overlay == AlsOverlayModeTags::Rifle
					                                                           ? TEXT("/ALS/ALS/OverlayObjects/Rifle/SKM_Als_Rifle.SKM_Als_Rifle")
					                                                           : TEXT("/ALS/ALS/OverlayObjects/Pistol/SKM_Als_Pistol.SKM_Als_Pistol"));
				if (Weapon != nullptr)
				{
					Drawn->SetSkeletalMeshAsset(Weapon);
					Drawn->SetVisibility(true);
					UE_LOG(LogImpact, Display, TEXT("[arme-main] t=%.2f %s : %s redessinee dans la main (ALS l'avait effacee)"), GetWorld()->GetTimeSeconds(),
					       *GetOwner()->GetName(), *Weapon->GetName());
				}
			}
			continue;
		}
		if (Child->GetAttachParent() == Mesh && Child->GetAttachSocketName() == Item.Hand)
		{
			Child->AttachToComponent(Mesh, FAttachmentTransformRules::KeepRelativeTransform, Item.Socket);
			Child->SetRelativeTransform(Item.OnSocket);
		}
		if (UPrimitiveComponent* Solid = Cast<UPrimitiveComponent>(Child))
		{
			Solid->SetCollisionEnabled(static_cast<ECollisionEnabled::Type>(Item.Collision));
		}
		Item.bHeld = false;
	}
	if (!bHold)
	{
		bWeaponInHand = false;
		UE_LOG(LogImpact, Display, TEXT("[arme-main] t=%.2f %s : arme rendue a ALS"), GetWorld()->GetTimeSeconds(), *GetOwner()->GetName());
	}
}

void UImpactReactionComponent::UpdateDeadCollision()
{
	// -ImpactMortTraversable. Reported from play: "desactive les collisions avec eux et leurs armes quand ils sont morts, sinon ca
	// fait des bugs". A dead body stays a body for the floor, the walls and the bullets, and stops being one for the living: the
	// player and the passers-by walk through it (nothing to shove, climb or be thrown by), and the camera does not bump on it.
	// Their dropped weapons: UImpactBodyComponent::DropWeapon.
	static const bool bOn = GroundFlag(TEXT("ImpactMortTraversable"));
	USkeletalMeshComponent* Mesh = GetCharacter() != nullptr ? GetCharacter()->GetMesh() : nullptr;
	if (!bOn || !bDead || Mesh == nullptr)
	{
		return;
	}
	// Every frame, not once: a ragdoll that starts answers blocking again (UpdateDown).
	if (Mesh->GetCollisionResponseToChannel(ECC_Pawn) != ECR_Ignore || Mesh->GetCollisionResponseToChannel(ECC_Camera) != ECR_Ignore)
	{
		Mesh->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
		Mesh->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
		if (!bDeadCollisionSet)
		{
			bDeadCollisionSet = true;
			UE_LOG(LogImpact, Display, TEXT("[mort] t=%.2f %s : plus de collision avec les vivants ni la camera"), GetWorld()->GetTimeSeconds(), *GetOwner()->GetName());
		}
	}
}

void UImpactReactionComponent::UpdateFallEnd(const float DeltaTime)
{
	if (FallEndWatch < 0.0f && FallEndSettle <= 0.0f)
	{
		return;
	}
	USkeletalMeshComponent* Mesh = GetCharacter() != nullptr ? GetCharacter()->GetMesh() : nullptr;
	if (Mesh == nullptr)
	{
		return;
	}
	if (FallEndSettle > 0.0f)
	{
		FallEndSettle -= DeltaTime;
		// Held for half a second, then let go over the next three tenths: cut at once, the hands were measured whipping at
		// 600 to 1100 deg/s in the frames after the calm ended.
		const float Release = FMath::Clamp((0.3f - FallEndSettle) / 0.3f, 0.0f, 1.0f);
		const float MaxLinear = FMath::Lerp(100.0f, 300.0f, Release);
		const float MaxAngular = FMath::Lerp(3.0f, 10.0f, Release);
		for (FBodyInstance* Part : Mesh->Bodies)
		{
			if (Part == nullptr || !Part->IsInstanceSimulatingPhysics())
			{
				continue;
			}
			const FVector Linear = Part->GetUnrealWorldVelocity();
			if (Linear.Size() > MaxLinear)
			{
				Part->SetLinearVelocity(Linear.GetClampedToMaxSize(MaxLinear), false);
			}
			const FVector Angular = Part->GetUnrealWorldAngularVelocityInRadians();
			if (Angular.Size() > MaxAngular)
			{
				Part->SetAngularVelocityInRadians(Angular.GetClampedToMaxSize(MaxAngular), false);
			}
		}
	}
	if (FallEndWatch >= 0.0f)
	{
		FallEndWatch += DeltaTime;
		FallEndPeakHips = FMath::Max(FallEndPeakHips, static_cast<float>(Mesh->GetPhysicsAngularVelocityInRadians(TEXT("pelvis")).Size()));
		FallEndPeakSpeed = FMath::Max(FallEndPeakSpeed, static_cast<float>(Mesh->GetPhysicsLinearVelocity(TEXT("pelvis")).Size()));
		FallEndMaxZ = FMath::Max(FallEndMaxZ, static_cast<float>(Mesh->GetSocketLocation(TEXT("pelvis")).Z));
		for (const FBodyInstance* Part : Mesh->Bodies)
		{
			if (Part != nullptr && Part->IsInstanceSimulatingPhysics())
			{
				const float Spin = Part->GetUnrealWorldAngularVelocityInRadians().Size();
				if (Spin > FallEndPeakLimb)
				{
					FallEndPeakLimb = Spin;
					FallEndPeakLimbName = Mesh->GetBoneName(Part->InstanceBoneIndex);
				}
			}
		}
		if (FallEndWatch > 0.8f)
		{
			FallEndWatch = -1.0f;
			UE_LOG(LogImpact, Display, TEXT("[fin-chute] t=%.2f %s : bassin %.0f deg/s max | membre le plus rapide %.0f deg/s (%s) | bassin %.0f cm/s max | monte de %.0f cm"),
			       GetWorld()->GetTimeSeconds(), *GetOwner()->GetName(), FMath::RadiansToDegrees(FallEndPeakHips), FMath::RadiansToDegrees(FallEndPeakLimb),
			       *FallEndPeakLimbName.ToString(), FallEndPeakSpeed, FallEndMaxZ - FallEndStartZ);
		}
	}
}

void UImpactReactionComponent::HoldWeaponAgain()
{
	KeepWeaponInHand(bWeaponInHand);
}

UAnimSequenceBase* UImpactReactionComponent::ChooseFallTake(const FVector& PushVelocity, const TCHAR* Reason, const TCHAR*& Chosen, const bool bByPush)
{
	const AAlsCharacter* Character = GetCharacter();
	const FVector Push = PushVelocity.GetSafeNormal2D();
	const float Along = FVector::DotProduct(Push, Character->GetActorForwardVector());
	// bByPush (-ImpactChuteMelee): the fall the push asks for, knees last. Played by a ragdoll, a fall against the push fought
	// the physics, and the fall onto the knees ends kneeling upright: tipped over by the push with its muscles still holding that,
	// the body arched into a bridge (seen on the bench).
	const bool bLeg = !bByPush && FCString::Strstr(Reason, TEXT("jambe")) != nullptr;
	const TCHAR* Order[4];
	if (bByPush && FMath::Abs(Along) <= 0.3f)
	{
		Order[0] = TEXT("Cote"); Order[1] = TEXT("Avant"); Order[2] = TEXT("Arriere"); Order[3] = TEXT("Genoux");
	}
	else if (bByPush)
	{
		Order[0] = Along > 0.0f ? TEXT("Avant") : TEXT("Arriere"); Order[1] = TEXT("Cote"); Order[2] = Along > 0.0f ? TEXT("Arriere") : TEXT("Avant");
		Order[3] = TEXT("Genoux");
	}
	else if (bLeg)
	{
		Order[0] = TEXT("Genoux"); Order[1] = TEXT("Cote"); Order[2] = TEXT("Avant"); Order[3] = TEXT("Arriere");
	}
	else if (Along > 0.3f)
	{
		Order[0] = TEXT("Avant"); Order[1] = TEXT("Genoux"); Order[2] = TEXT("Cote"); Order[3] = TEXT("Arriere");
	}
	else if (Along < -0.3f)
	{
		Order[0] = TEXT("Arriere"); Order[1] = TEXT("Cote"); Order[2] = TEXT("Genoux"); Order[3] = TEXT("Avant");
	}
	else
	{
		Order[0] = TEXT("Cote"); Order[1] = TEXT("Genoux"); Order[2] = TEXT("Avant"); Order[3] = TEXT("Arriere");
	}

	// Looked up once per kind: a missing asset is not looked for again at every fall.
	// And rooted: this list is a static, which the garbage collector does not see. Unrooted, a fall nobody was playing was
	// collected at the next pass (every 60 s) and the next fall that picked it built its montage on freed memory: measured,
	// EXCEPTION_ACCESS_VIOLATION in FAnimMontageInstance::HandleEvents 60.6 s into a game, right after "[chute-anime] ...
	// Chute_Arriere_1", last played at 55.3 s (addresses named from the minidump's module list and the engine's exports).
	// -ImpactSansRacine puts the defect back, for the bench that shows it (-ImpactTest=gc).
	static TMap<FString, TArray<TObjectPtr<UAnimSequenceBase>>> Found;
	static TSet<FString> Searched;
	static const bool bNoRoot = FParse::Param(FCommandLine::Get(), TEXT("ImpactSansRacine"));
	const auto Keep = [](UAnimSequenceBase* Take)
	{
		if (!bNoRoot)
		{
			Take->AddToRoot();
		}
	};
	const auto Variants = [&Keep](const TCHAR* Kind) -> TArray<TObjectPtr<UAnimSequenceBase>>&
	{
		TArray<TObjectPtr<UAnimSequenceBase>>& List = Found.FindOrAdd(Kind);
		if (!Searched.Contains(Kind))
		{
			Searched.Add(Kind);
			for (int32 Index = 1; Index <= 8; ++Index)
			{
				const FString Name = FString::Printf(TEXT("Chute_%s_%d"), Kind, Index);
				const FString Path = FString::Printf(TEXT("/Game/Impact/Anims/Chutes/%s.%s"), *Name, *Name);
				if (UAnimSequenceBase* Take = LoadObject<UAnimSequenceBase>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet))
				{
					Keep(Take);
					List.Add(Take);
				}
			}
			if (FString(Kind) == TEXT("Repli"))
			{
				for (const TCHAR* Name : {TEXT("Shoulder_Hit_And_Fall"), TEXT("Shoulder_Hit_And_Fall_1")})
				{
					const FString Path = FString::Printf(TEXT("/Game/Impact/Anims/%s.%s"), Name, Name);
					if (UAnimSequenceBase* Take = LoadObject<UAnimSequenceBase>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet))
					{
						Keep(Take);
						List.Add(Take);
					}
				}
			}
		}
		return List;
	};
	UAnimSequenceBase* Fall = nullptr;
	Chosen = TEXT("Repli");
	for (const TCHAR* Kind : Order)
	{
		const TArray<TObjectPtr<UAnimSequenceBase>>& List = Variants(Kind);
		if (List.Num() > 0)
		{
			Fall = List[FMath::RandRange(0, List.Num() - 1)];
			Chosen = Kind;
			break;
		}
	}
	if (Fall == nullptr)
	{
		const TArray<TObjectPtr<UAnimSequenceBase>>& List = Variants(TEXT("Repli"));
		if (List.Num() == 0)
		{
			return nullptr;
		}
		Fall = List[FMath::RandRange(0, List.Num() - 1)];
	}

	return Fall;
}

bool UImpactReactionComponent::TryStartAnimatedFall(const FVector& PushVelocity, const TCHAR* Reason)
{
	// The fall, played. Reported from play, after every physical version: "les animations quand il tombe sont toujours n'importe
	// quoi". A ragdoll falling is a ragdoll falling whatever drives it; a fall is something a person does, and an animation is a
	// person doing it. So a conscious man goes to the floor through a fall animation (ALS's LyingDown, no physics), chosen by what
	// threw him: a leg giving way takes him to his knees, a push from the front backwards, from behind forwards. At the end of it he
	// is lying on the floor, and from there it is the usual: a body on the floor, the pose taken, the take played. A bullet or a
	// push during the fall makes him a body at once (ReturnToRagdoll), as anywhere else in LyingDown.
	//
	// The falls are looked for as /Game/Impact/Anims/Chutes/Chute_<Kind>_<n>, Kind = Genoux, Avant, Arriere, Cote, n from 1. Until
	// those exist, the two shoulder-hit falls already in the project stand in for all of them.
	AAlsCharacter* Character = GetCharacter();
	const TCHAR* Chosen = TEXT("Repli");
	UAnimSequenceBase* Fall = ChooseFallTake(PushVelocity, Reason, Chosen);
	if (Fall == nullptr)
	{
		return false;
	}
	if (!AnimReady(Fall))
	{
		return false; // a ragdoll fall instead
	}
	// No blend out: blending out of a fall would blend back towards standing. The end is caught just before (UpdateLyingDown).
	UAnimMontage* Montage = UAnimMontage::CreateSlotAnimationAsDynamicMontage(Fall, TEXT("PostLocomotion"), 0.15f, 0.0f, 1.0f, 1);
	// -ImpactChuteFigee: the fall stops on its last frame instead of ending. UpdateLyingDown hands him to the ragdoll a tenth of a
	// second before the end, but a frame that comes late (a hitch: loading, the bench's photographs) jumps the take straight past
	// its end; it then ended, the next frame drew him standing where he had been before falling, and the ragdoll started from
	// that: measured, the pelvis 175 cm away in one frame ("retour au ragdoll (animation arretee)"). Reported from play long ago as
	// "des anims qui rollback". Held, the take can only wait on its last frame; ALS fades it out once the ragdoll has started.
	static const bool bHoldEnd = GroundFlag(TEXT("ImpactChuteFigee"));
	if (bHoldEnd && Montage != nullptr)
	{
		Montage->bEnableAutoBlendOut = false;
	}
	// Into the hand before the fall moves anything: from its first frame the gun bone is no longer in the palm.
	KeepWeaponInHand(true);
	if (Montage == nullptr || !Character->StartLyingDownFromStanding(Montage))
	{
		return false;
	}
	RefreshPoseNow();
	LyingMontage = Montage;
	LyingTime = 0.0f;
	bFallingAnimated = true;
	BraceTime = 0.0f;
	CancelHandCommands();
	PhysicalAnimation->SetStrengthMultiplyer(0.0f);
	// How far the fall takes the pelvis down, read from the animation itself: a fall that keeps the pelvis at standing height leaves
	// the body lying in the air (a retarget that drops the pelvis translation does exactly that).
	float PelvisStart = 0.0f;
	float PelvisEnd = 0.0f;
	if (const UAnimSequence* Sequence = Cast<UAnimSequence>(Fall))
	{
		if (const USkeleton* Skeleton = Sequence->GetSkeleton())
		{
			const int32 PelvisIndex = Skeleton->GetReferenceSkeleton().FindBoneIndex(TEXT("pelvis"));
			const int32 RootIndex = Skeleton->GetReferenceSkeleton().FindBoneIndex(TEXT("root"));
			if (PelvisIndex != INDEX_NONE)
			{
				FTransform Pelvis0, Pelvis1, Root0, Root1;
				Sequence->GetBoneTransform(Pelvis0, FSkeletonPoseBoneIndex(PelvisIndex), FAnimExtractContext(0.0), false);
				Sequence->GetBoneTransform(Pelvis1, FSkeletonPoseBoneIndex(PelvisIndex), FAnimExtractContext(static_cast<double>(Sequence->GetPlayLength())), false);
				if (RootIndex != INDEX_NONE)
				{
					Sequence->GetBoneTransform(Root0, FSkeletonPoseBoneIndex(RootIndex), FAnimExtractContext(0.0), false);
					Sequence->GetBoneTransform(Root1, FSkeletonPoseBoneIndex(RootIndex), FAnimExtractContext(static_cast<double>(Sequence->GetPlayLength())), false);
				}
				PelvisStart = (Pelvis0 * Root0).GetLocation().Z;
				PelvisEnd = (Pelvis1 * Root1).GetLocation().Z;
			}
		}
	}
	UE_LOG(LogImpact, Display, TEXT("[chute-anime] t=%.2f %s : %s (%s, %.1f s), bassin %.0f -> %.0f cm"), GetWorld()->GetTimeSeconds(), *GetOwner()->GetName(),
	       *Fall->GetName(), Chosen, Fall->GetPlayLength(), PelvisStart, PelvisEnd);
	return true;
}

void UImpactReactionComponent::StartBlendedFall(const FVector& PushVelocity, const TCHAR* Reason, const FTransform& Frame)
{
	// -ImpactChuteMelee. Reported from play with the falls played as animations: "il aurait fallu blend les animations dans les
	// mouvements, la il joue juste les animations, ca se voit, aucun realisme". Here the fall is a ragdoll from its first frame -
	// the round and the push move it, gravity takes it down, the floor stops it - and the fall animation is what its muscles do:
	// it plays in the ragdoll's own slot, so the joints aim at its pose, stiffly at first and less and less as he goes down, and
	// its pelvis path is drawn, weakly and fading, in the frame he stood in. A man knocked over sideways by a burst falls sideways,
	// in the shape of a person falling.
	AAlsCharacter* Character = GetCharacter();
	USkeletalMeshComponent* Mesh = Character->GetMesh();
	UAnimInstance* Anim = Mesh->GetAnimInstance();
	const TCHAR* Chosen = TEXT("Repli");
	UAnimSequenceBase* Fall = ChooseFallTake(PushVelocity, Reason, Chosen, true);
	if (Anim == nullptr || Fall == nullptr || !AnimReady(Fall))
	{
		return;
	}
	BlendedFallMontage = UAnimMontage::CreateSlotAnimationAsDynamicMontage(Fall, GroundSlotName(), 0.1f, 0.3f, 1.0f, 1);
	BlendedFallTake = Fall;
	if (BlendedFallMontage == nullptr)
	{
		return;
	}
	// Held on its last frame: blending out would aim the joints back at ALS's flail.
	BlendedFallMontage->bEnableAutoBlendOut = false;
	// -ImpactChuteLourde: the take played faster. Reported from play, 29 October: "un corps c'est lourd, c'est pas ragdoll moon low
	// gravity". Measured on the gang bench ([chute-temps]): the hips reached the floor 1,3 to 2,0 s after the fall began, and more
	// than 4 s under the 4,3 s Chute_Avant_1, where a free fall from that height takes 0,32 s - the body kept the pace of a theatrical
	// Mixamo death. Faster, its muscles and its guide follow a man who drops.
	static const float FallRate = FParse::Param(FCommandLine::Get(), TEXT("ImpactChuteLourde")) ? 1.4f : 1.0f;
	if (Anim->Montage_Play(BlendedFallMontage, FallRate) <= 0.0f)
	{
		BlendedFallMontage = nullptr;
		return;
	}
	bBlendedFall = true;
	BlendedFallTime = 0.0f;
	BlendedFallLength = Fall->GetPlayLength() / FallRate;
	BlendedFallFrame = Frame;
	// -ImpactChuteElan: a man knocked down at a run falls forward with his run. The pelvis path was drawn where he stood and pulled
	// towards the take's own speed, which starts at zero: the guide braked him at up to 1,5 g. His velocity as he went down (Collapse)
	// is carried into the path instead, dying out (see UpdateBlendedFall).
	static const bool bCarry = FParse::Param(FCommandLine::Get(), TEXT("ImpactChuteElan"));
	BlendedFallCarry = bCarry ? FVector(RunVelocityAtFall.X, RunVelocityAtFall.Y, 0.0f) : FVector::ZeroVector;
	bBlendedFallHasTarget = false;
	BlendedFallDrift = 0.0f;
	Mesh->bUpdateJointsFromAnimation = true;
	// PA_Als has no velocity drive, and without one the joints' damping is ignored.
	for (FConstraintInstance* Joint : Mesh->Constraints)
	{
		if (Joint != nullptr)
		{
			Joint->SetAngularVelocityTarget(FVector::ZeroVector);
			Joint->SetAngularVelocityDriveTwistAndSwing(true, true);
		}
	}
	// The animation already has his arms; the brace and the local muscles would pull them elsewhere.
	BraceTime = 0.0f;
	CancelHandCommands();
	PhysicalAnimation->SetStrengthMultiplyer(0.0f);
	UE_LOG(LogImpact, Display, TEXT("[chute-melee] t=%.2f %s : ragdoll guide par %s (%s, %.1f s)"), GetWorld()->GetTimeSeconds(),
	       *GetOwner()->GetName(), *Fall->GetName(), Chosen, BlendedFallLength);
}

void UImpactReactionComponent::UpdateBlendedFall(const float DeltaTime)
{
	if (!bBlendedFall)
	{
		return;
	}
	AAlsCharacter* Character = GetCharacter();
	USkeletalMeshComponent* Mesh = Character->GetMesh();
	UAnimInstance* Anim = Mesh->GetAnimInstance();
	BlendedFallTime += DeltaTime;
	const float Progress = BlendedFallLength > 0.0f ? FMath::Clamp(BlendedFallTime / BlendedFallLength, 0.0f, 1.0f) : 1.0f;
	const bool bPlaying = Anim != nullptr && BlendedFallMontage != nullptr && Anim->Montage_IsPlaying(BlendedFallMontage);
	if (!bVoluntaryControl || Progress >= 1.0f || !bPlaying || Character->GetLocomotionAction() != AlsLocomotionActionTags::Ragdolling)
	{
		bBlendedFall = false;
		// Without -ImpactChuteFinie the posture can cut the fall (it ticks first, UpdateGroundRagdoll), and then this loop switches off
		// the velocity drives it has just switched on. With the flag the posture waits for the fall, so that frame never comes.
		for (FConstraintInstance* Joint : Mesh->Constraints)
		{
			if (Joint != nullptr)
			{
				Joint->SetAngularVelocityDriveTwistAndSwing(false, false);
			}
		}
		UE_LOG(LogImpact, Display, TEXT("[chute-melee] t=%.2f %s : fin apres %.1f s (%s), bassin au plus a %.0f cm du trajet de l'animation"),
		       GetWorld()->GetTimeSeconds(), *GetOwner()->GetName(), BlendedFallTime,
		       !bVoluntaryControl ? TEXT("perd le controle") : Progress >= 1.0f ? TEXT("animation finie") : TEXT("interrompue"), BlendedFallDrift);
		return;
	}

	// The muscles: the joints aim at the fall's pose (bUpdateJointsFromAnimation, the take in the ragdoll slot), firm at the start,
	// a tenth of that at the end, so he lands as a body and not as a statue. -ImpactChuteMeleeForce= scales them.
	static const float Scale = [] { float Value = 1.0f; FParse::Value(FCommandLine::Get(), TEXT("ImpactChuteMeleeForce="), Value); return FMath::Max(0.0f, Value); }();
	Mesh->bUpdateJointsFromAnimation = true;
	const float Spring = FMath::Lerp(5000.0f, 500.0f, Progress) * Scale;
	Mesh->SetAllMotorsAngularDriveParams(Spring, 1.5f * FMath::Sqrt(Spring), 0.0f, false);

	// The pelvis drawn along the fall's own path, in the frame he stood in when it began, weakly and fading out: what the rounds and
	// the floor do to him stays his. -ImpactChuteMeleeGuide= scales it (0: the muscles alone).
	static const float GuideScale = [] { float Value = 1.0f; FParse::Value(FCommandLine::Get(), TEXT("ImpactChuteMeleeGuide="), Value); return FMath::Max(0.0f, Value); }();
	const USkeletalMesh* Asset = Mesh->GetSkeletalMeshAsset();
	FBodyInstance* Pelvis = Mesh->GetBodyInstance(TEXT("pelvis"));
	if (Asset == nullptr || Pelvis == nullptr || DeltaTime <= 0.0f)
	{
		return;
	}
	const FReferenceSkeleton& RefBones = Asset->GetRefSkeleton();
	const TArray<FTransform>& Local = Mesh->GetBoneSpaceTransforms();
	const int32 PelvisBone = RefBones.FindBoneIndex(TEXT("pelvis"));
	if (PelvisBone == INDEX_NONE || !Local.IsValidIndex(PelvisBone))
	{
		return;
	}
	FTransform PelvisInMesh = Local[PelvisBone];
	for (int32 Bone = RefBones.GetParentIndex(PelvisBone); Bone != INDEX_NONE && Local.IsValidIndex(Bone); Bone = RefBones.GetParentIndex(Bone))
	{
		PelvisInMesh = PelvisInMesh * Local[Bone];
	}
	// -ImpactChuteGuideJuste: the take's own pelvis at the montage's position, read off the take. The mesh's local pose is the take only
	// on the frames the mesh has evaluated before this tick; on the others (off screen, or this component ticking first) the engine has
	// written the physics pose back into it, and the target became the body itself (no guide at all) or the body moved by however far
	// the mesh had been left behind - 10 to 19 m, pulled towards at 1500 cm/s2: the 9-18 m "drifts" (review of 27 October).
	static const bool bTakeTarget = FParse::Param(FCommandLine::Get(), TEXT("ImpactChuteGuideJuste"));
	FTransform TakePelvis;
	const bool bTakePelvis = BlendedFallTake != nullptr && Anim != nullptr && BlendedFallMontage != nullptr
		&& TakeBoneInMesh(BlendedFallTake, Anim->Montage_GetPosition(BlendedFallMontage), TEXT("pelvis"), TakePelvis);
	if (bTakeTarget && bTakePelvis)
	{
		PelvisInMesh = TakePelvis;
	}
	FTransform Target = PelvisInMesh * BlendedFallFrame;
	// The drift is measured against the take's own path read off the take, with or without -ImpactChuteGuideJuste and -ImpactChuteElan, so
	// the number means the same in every version (before the 27th it was measured against the polluted target itself).
	const FVector TakePath = bTakePelvis ? (TakePelvis * BlendedFallFrame).GetLocation() : Target.GetLocation();
	// -ImpactChuteElan: the run carried on along the path, dying out over 0,4 s - at 5 m/s, two metres in all.
	constexpr float CarryTime = 0.4f;
	Target.AddToTranslation(BlendedFallCarry * (CarryTime * (1.0f - FMath::Exp(-BlendedFallTime / CarryTime))));
	const FVector TargetVelocity = bBlendedFallHasTarget ? (Target.GetLocation() - BlendedFallLastTarget) / DeltaTime : FVector::ZeroVector;
	BlendedFallLastTarget = Target.GetLocation();
	bBlendedFallHasTarget = true;
	const FTransform Now = Pelvis->GetUnrealWorldTransform();
	BlendedFallDrift = FMath::Max(BlendedFallDrift, static_cast<float>(FVector::Dist(Now.GetLocation(), TakePath)));
	const float Guide = FMath::Square(1.0f - Progress) * GuideScale;
	const float Kp = 14.0f * Guide;
	FVector Pull = Kp * (Target.GetLocation() - Now.GetLocation()) + 2.0f * FMath::Sqrt(Kp) * (TargetVelocity - Pelvis->GetUnrealWorldVelocity());
	// -ImpactChuteLourde: the guide may steer him and pull him down, never hold him up. Upwards it gave up to 1,5 g - more than gravity
	// itself - wherever the take kept its hips higher than the falling body: that is the floating. A first cap at +150 cm/s2 still
	// lifted one fall of eleven by 11,6 cm in its first 0,4 s ([chute-saut], 30 October: the "petit saut avant de pass out"), which is
	// what that pull gives over that time: none at all now.
	static const bool bHeavy = FParse::Param(FCommandLine::Get(), TEXT("ImpactChuteLourde"));
	if (bHeavy)
	{
		Pull.Z = FMath::Min(Pull.Z, 0.0f);
	}
	Pelvis->AddForce(Pull.GetClampedToMaxSize(1500.0f), true, true);
	FQuat Turn = Target.GetRotation() * Now.GetRotation().Inverse();
	Turn.EnforceShortestArcWith(FQuat::Identity);
	FVector Axis;
	float Angle;
	Turn.ToAxisAndAngle(Axis, Angle);
	const float Kr = 30.0f * Guide;
	const FVector Twist = Axis * (Kr * Angle) - Pelvis->GetUnrealWorldAngularVelocityInRadians() * (2.0f * FMath::Sqrt(Kr));
	Pelvis->AddTorqueInRadians(Twist.GetClampedToMaxSize(40.0f), true, true);
}

bool UImpactReactionComponent::TryEnterLyingDown()
{
	// The ground posture, done the way games do it: the ragdoll for the fall and for whatever hits him, the animation for the time
	// he lies there. Making the ragdoll itself replay the take through its joints was fighting the physics at every step - the
	// pace, the damping, the anchor, the flail - and it never reached the take's shape (head to knees 96-100 cm, the take 84).
	//
	// Not the first attempt either. That one borrowed ALS's get-up, so ALS believed he was getting up, for ever: an action with
	// nothing under it, then the T-pose and five get-ups in a row. This is an action of its own, LyingDown, which only
	// StartRagdolling leaves, and every way out of it (ReturnToRagdoll) goes through that.
	AAlsCharacter* Character = GetCharacter();
	USkeletalMeshComponent* Mesh = Character->GetMesh();
	UAnimInstance* Anim = Mesh->GetAnimInstance();
	const USkeletalMesh* Skeleton = Mesh->GetSkeletalMeshAsset();
	if (Anim == nullptr || Skeleton == nullptr || GroundTake == nullptr || GroundMontage == nullptr || !AnimReady(GroundTake))
	{
		return false;
	}

	// Where the take is in its loop: the montage starts there, so it shows the pose the joints have been following.
	const float Length = FMath::Max(0.01f, GroundTake->GetPlayLength());
	const float TakeTime = FMath::Fmod(FMath::Max(0.0f, Anim->Montage_GetPosition(GroundMontage)), Length);

	// The take's pelvis in the mesh's space, read off the pose the graph is producing now (the take, through the ragdoll slot), and the
	// mesh placed so that pelvis lands on the real one. Without it the take is drawn wherever its root says, rotated and shifted.
	const FReferenceSkeleton& RefBones = Skeleton->GetRefSkeleton();
	const TArray<FTransform>& Local = Mesh->GetBoneSpaceTransforms();
	const int32 Pelvis = RefBones.FindBoneIndex(TEXT("pelvis"));
	if (Pelvis == INDEX_NONE || !Local.IsValidIndex(Pelvis))
	{
		return false;
	}
	FTransform PelvisInMesh = Local[Pelvis];
	for (int32 Bone = RefBones.GetParentIndex(Pelvis); Bone != INDEX_NONE && Local.IsValidIndex(Bone); Bone = RefBones.GetParentIndex(Bone))
	{
		PelvisInMesh = PelvisInMesh * Local[Bone];
	}
	const FTransform GraphPelvisInMesh = PelvisInMesh;
	// -ImpactSolEntreeJuste: the take's own pelvis at TakeTime, read off the take. The graph's is not always the take's: during the
	// ragdoll the mesh is left where the fall began (ALS detaches it) and the pose it holds can carry the body's distance from there -
	// measured 19 m on the 27th ([sol-entree-place]), and the whole body drawn 19 m away at the entry.
	static const bool bEntryFix = FParse::Param(FCommandLine::Get(), TEXT("ImpactSolEntreeJuste"));
	if (bEntryFix)
	{
		FTransform TakePelvis;
		if (TakeBoneInMesh(GroundTake, TakeTime, TEXT("pelvis"), TakePelvis))
		{
			PelvisInMesh = TakePelvis;
		}
	}
	// -ImpactSolEntreeJuste: onto the pelvis body itself. The socket is the body re-expressed through the pose the mesh holds, and that
	// pose lags a frame or more behind a mesh that has just been moved: two entries of 29 placed the take 15-17 m off on the 27th.
	const FBodyInstance* PelvisBody = Mesh->GetBodyInstance(TEXT("pelvis"));
	const FTransform PelvisWorld = bEntryFix && PelvisBody != nullptr ? PelvisBody->GetUnrealWorldTransform() : Mesh->GetSocketTransform(TEXT("pelvis"), RTS_World);
	FTransform MeshWorld = PelvisInMesh.Inverse() * PelvisWorld;
	MeshWorld.SetScale3D(Mesh->GetComponentScale());
	const FTransform MeshWorldMatched = MeshWorld;

	// [sol-entree-place]: the placement above reads the pose the graph shows now - the posture's montage in the ragdoll slot, through
	// ALS's ragdoll branch. LyingDown will show the take in PostLocomotion. If the two differ, the take is drawn off the body from the
	// first frame: the whole body seen jumping 1 to 2 m at the entry (-ImpactSolEntreeImages, 27 October). Compared here with the take
	// itself at TakeTime, and the jump the eye would see predicted, for the pelvis (placement) and for the head (pose).
	{
		FTransform TakePelvis;
		FTransform TakeHead;
		const int32 HeadIndex = RefBones.FindBoneIndex(TEXT("head"));
		if (HeadIndex != INDEX_NONE && Local.IsValidIndex(HeadIndex) && TakeBoneInMesh(GroundTake, TakeTime, TEXT("pelvis"), TakePelvis)
		    && TakeBoneInMesh(GroundTake, TakeTime, TEXT("head"), TakeHead))
		{
			FTransform GraphHead = Local[HeadIndex];
			for (int32 Bone = RefBones.GetParentIndex(HeadIndex); Bone != INDEX_NONE && Local.IsValidIndex(Bone); Bone = RefBones.GetParentIndex(Bone))
			{
				GraphHead = GraphHead * Local[Bone];
			}
			const FVector BodyPelvis = Mesh->GetSocketLocation(TEXT("pelvis"));
			const FVector BodyHead = Mesh->GetSocketLocation(TEXT("head"));
			UE_LOG(LogImpact, Display, TEXT("[sol-entree-place] t=%.2f %s : %s a %.2f s | bassin du graphe a %.0f cm de celui de la prise, tete %.0f cm | si la prise est montree : bassin %.0f cm, tete %.0f cm de la ou ils sont"),
			       GetWorld()->GetTimeSeconds(), *GetOwner()->GetName(), *GroundTake->GetName(), TakeTime,
			       FVector::Dist(GraphPelvisInMesh.GetLocation(), TakePelvis.GetLocation()), FVector::Dist(GraphHead.GetLocation(), TakeHead.GetLocation()),
			       FVector::Dist(MeshWorldMatched.TransformPosition(TakePelvis.GetLocation()), BodyPelvis),
			       FVector::Dist(MeshWorldMatched.TransformPosition(TakeHead.GetLocation()), BodyHead));
		}
		// The bodies themselves, for -ImpactSolEntreeImages: does what is drawn follow them, or the animation?
		EntryBodyStart.Reset();
		for (const TCHAR* Bone : {TEXT("pelvis"), TEXT("head"), TEXT("hand_r")})
		{
			const FBodyInstance* Body = Mesh->GetBodyInstance(Bone);
			EntryBodyStart.Add(Body != nullptr ? Body->GetUnrealWorldTransform().GetLocation() : FVector::ZeroVector);
		}
		EntryComponentStart = Mesh->GetComponentLocation();
		EntryTraceFrames = 0;
	}

	// -ImpactSolEntreeJuste, tried and dropped on the 27th: bringing the mesh onto the body one or more frames before the entry. For
	// a man off screen the engine does not refresh the pose (ALS: OnlyTickMontagesWhenNotRendered), so the pose moved with the mesh and
	// never came back - seven entries of 27 waited the full 15 frames and five jumped 8 to 41 m (series e3). The limbs are put back
	// where the bodies were instead, after the entry (end of this function).


	// -ImpactSolMixamo: the Mixamo takes lie flat on their own floor. The mesh is kept upright, turned so the take's head points
	// where the body's head does, with the take's pelvis over the body's and its floor on the real one. Matching the whole turn of
	// the pelvis instead (right for the made takes, which the pacer has already brought the body into) tilted the take off the
	// floor: seen lying on its side 25 cm up in the air. The montage's blend carries the limbs from the body's pose to the take's.
	const int32 HeadBone = RefBones.FindBoneIndex(TEXT("head"));
	if (SolMixamo() && GroundTake->GetPathName().Contains(TEXT("/Sol_Mixamo/")) && HeadBone != INDEX_NONE && Local.IsValidIndex(HeadBone))
	{
		FTransform HeadInMesh = Local[HeadBone];
		for (int32 Bone = RefBones.GetParentIndex(HeadBone); Bone != INDEX_NONE && Local.IsValidIndex(Bone); Bone = RefBones.GetParentIndex(Bone))
		{
			HeadInMesh = HeadInMesh * Local[Bone];
		}
		// -ImpactSolEntreeJuste: the take's own head, for the same reason as its pelvis above.
		FTransform TakeHeadInMesh;
		if (bEntryFix && TakeBoneInMesh(GroundTake, TakeTime, TEXT("head"), TakeHeadInMesh))
		{
			HeadInMesh = TakeHeadInMesh;
		}
		const FBodyInstance* HeadBody = Mesh->GetBodyInstance(TEXT("head"));
		const FVector BodyPelvis = PelvisWorld.GetLocation();
		const FVector BodyHeadAt = bEntryFix && HeadBody != nullptr ? HeadBody->GetUnrealWorldTransform().GetLocation() : Mesh->GetSocketLocation(TEXT("head"));
		const FVector BodyHead = (BodyHeadAt - BodyPelvis).GetSafeNormal2D();
		const FVector TakeHead = (HeadInMesh.GetLocation() - PelvisInMesh.GetLocation()).GetSafeNormal2D();
		if (!BodyHead.IsNearlyZero() && !TakeHead.IsNearlyZero())
		{
			const FQuat Turn(FVector::UpVector, FMath::DegreesToRadians(BodyHead.Rotation().Yaw - TakeHead.Rotation().Yaw));
			FHitResult Floor;
			FCollisionQueryParams Params(SCENE_QUERY_STAT(ImpactMixamoFloor), false, Character);
			const bool bFloor = GetWorld()->LineTraceSingleByChannel(Floor, BodyPelvis + FVector(0.0f, 0.0f, 50.0f), BodyPelvis - FVector(0.0f, 0.0f, 150.0f),
			                                                         ECC_Visibility, Params);
			const FVector Scale = Mesh->GetComponentScale();
			const FVector PelvisOffset = Turn.RotateVector(PelvisInMesh.GetLocation() * Scale);
			const float FloorZ = bFloor ? Floor.ImpactPoint.Z : BodyPelvis.Z - PelvisInMesh.GetLocation().Z * Scale.Z;
			MeshWorld = FTransform(Turn, FVector(BodyPelvis.X - PelvisOffset.X, BodyPelvis.Y - PelvisOffset.Y, FloorZ), Scale);
		}
	}

	static float Rate = -1.0f;
	static float Fade = -1.0f;
	if (Rate < 0.0f)
	{
		float Parsed = 1.0f;
		Rate = FParse::Value(FCommandLine::Get(), TEXT("ImpactSolAnimeVitesse="), Parsed) ? FMath::Max(0.05f, Parsed) : 1.0f;
		// Long enough for the legs: driven at a quarter of the trunk's stiffness, they are still nearly straight when the take
		// has the knees drawn up, and at 0.25 s they were seen to snap in. Measured on the bench: the pose the graph blends out of
		// is the ragdoll's own (no standing ghost), so a longer blend only slows the drawing-in.
		// -ImpactSolMixamo: no blend at all. ALS's graph knows nothing of LyingDown, so the blend does not start from the ragdoll's
		// pose but from the standing locomotion under the slot: measured by [fantome], the pelvis 83 cm up 0.01 s into the take -
		// reported from play as "il se leve super vite pendant une demi-seconde debout tout droit et refait l'animation".
		const float Default = SolMixamo() ? 0.0f : 0.6f;
		Parsed = Default;
		Fade = FParse::Value(FCommandLine::Get(), TEXT("ImpactSolAnimeFondu="), Parsed) ? FMath::Max(0.0f, Parsed) : Default;
	}
	// The slot ALS's own get-up plays in, made for a whole body over the pose the ragdoll left.
	// Ten thousand loops, not sixty. At sixty the take ran out - after 50 s for a 0.83 s take - and blended out towards the
	// locomotion under it: reported from play, "le perso se leve super vite pendant une demi-seconde debout tout droit et refait
	// direct l'animation" (UpdateLyingDown then sent him back to the ragdoll and the posture took him again).
	UAnimMontage* Montage = UAnimMontage::CreateSlotAnimationAsDynamicMontage(GroundTake, TEXT("PostLocomotion"), Fade, 0.2f, Rate, 10000);
	if (Montage == nullptr)
	{
		return false;
	}
	KeepWeaponInHand(true);
	EntryPoints.Reset();
	EntryWorst = 0.0f;
	EntryWorstAt = 0;
	for (const TCHAR* Bone : {TEXT("head"), TEXT("hand_l"), TEXT("hand_r"), TEXT("foot_l"), TEXT("foot_r"), TEXT("pelvis")})
	{
		EntryPoints.Add(Mesh->GetSocketLocation(Bone));
	}
	// -ImpactSolMixamo: the take takes over from the ragdoll's own pose. With no blend the limbs jumped up to 1.3 m in one frame
	// ([sol-entree]); with the montage's blend the graph started from standing ([fantome]). The bodies are left simulating and the
	// physics faded out into the take instead.
	static const bool bPhysicsFade = SolMixamo();
	const auto SimState = [Mesh]() { const FBodyInstance* P = Mesh->GetBodyInstance(TEXT("pelvis")); return FString::Printf(TEXT("bassin simule %d poids %.2f, composant %d"),
		P != nullptr && P->IsInstanceSimulatingPhysics() ? 1 : 0, P != nullptr ? P->PhysicsBlendWeight : -1.0f, Mesh->IsSimulatingPhysics() ? 1 : 0); };
	static const bool bFrames = FParse::Param(FCommandLine::Get(), TEXT("ImpactSolEntreeImages"));
	if (bFrames) UE_LOG(LogImpact, Display, TEXT("[sol-entree-dbg] avant StartLyingDown : %s"), *SimState());
	// With the fade the mesh starts where the ragdoll pelvis is, turn included, and slides flat over the fade: the pelvis follows the
	// take from the first frame, and turning it at once swung the whole body round it (measured: the right hand 68 cm in a frame).
	LyingFadeFrom = MeshWorldMatched;
	LyingFadeTo = MeshWorld;
	// -ImpactSolEntreeJuste: where every body is before the entry, to put the simulated ones back there afterwards (below).
	TArray<FTransform> BodiesBefore;
	if (bEntryFix)
	{
		BodiesBefore.Reserve(Mesh->Bodies.Num());
		for (const FBodyInstance* Body : Mesh->Bodies)
		{
			BodiesBefore.Add(Body != nullptr ? Body->GetUnrealWorldTransform() : FTransform::Identity);
		}
	}
	if (!Character->StartLyingDown(Montage, TakeTime, bPhysicsFade ? MeshWorldMatched : MeshWorld, bPhysicsFade))
	{
		UE_LOG(LogImpact, Display, TEXT("[sol-anime] t=%.2f %s : refuse par ALS (pas au sol ?)"), GetWorld()->GetTimeSeconds(), *GetOwner()->GetName());
		return false;
	}
	if (bFrames) UE_LOG(LogImpact, Display, TEXT("[sol-entree-dbg] apres StartLyingDown : %s"), *SimState());
	// -ImpactSolEntreeJuste: not under the physics fade. Evaluated here, the take flips the pose buffers itself and the frame's own physics
	// blend is never published: the entry frame was drawn as the bare take while the limbs were still all physics - head and hand
	// 12-104 cm from where they lay, often for two frames (review of 27 October, 19 clean entries of series e2).
	if (!(bEntryFix && bPhysicsFade))
	{
		RefreshPoseNow();
	}
	if (bFrames) UE_LOG(LogImpact, Display, TEXT("[sol-entree-dbg] apres RefreshPoseNow : %s"), *SimState());
	LyingMontage = Montage;
	LyingTime = 0.0f;
	LyingPhysicsFade = bPhysicsFade ? 0.35f : 0.0f;
	bLyingHeadingKnown = false;
	LyingTurnWindow = 0.0f;
	LyingTurnTravel = 0.0f;
	LyingTurnPeak = 0.0f;
	LyingTurnSwitches = 0;
	SwitchWatchTime = -1.0f;
	SwitchFixTimeLeft = 0.0f;
	// -ImpactSolEnchaine: lying down again may be in another take than the one whose gun offset is known (relecture du 27 octobre).
	static const bool bOffsetPerTake = FParse::Param(FCommandLine::Get(), TEXT("ImpactSolEnchaine"));
	if (bOffsetPerTake)
	{
		bGunOffsetKnown = false;
	}

	// The physics half of the posture is over: pace, drives, drag, anchor. Nothing simulates any more - after the fade, when there
	// is one: ResetBones stops the bodies the hit reactions had simulating, the pelvis among them, which ended the fade on its first
	// frame (measured, -ImpactSolEntreeImages: not simulating from the first frame, the right hand 74 cm away in one frame).
	StopGroundSlot(0.0f);
	if (!bPhysicsFade)
	{
		ResetBones();
	}
	else
	{
		// The joints let go for the fade: StopGroundSlot hands them the whole take at once instead of the paced target, and at the
		// posture's stiffness that flung a hand 53 cm in a frame with nothing but physics showing (-ImpactSolEntreeImages).
		Mesh->SetAllMotorsAngularDriveParams(0.0f, 0.0f, 0.0f, false);
	}
	if (bFrames) UE_LOG(LogImpact, Display, TEXT("[sol-entree-dbg] apres StopGroundSlot : %s"), *SimState());
	PhysicalAnimation->SetStrengthMultiplyer(0.0f);
	// -ImpactSolEntreeJuste: the simulated limbs put back where they lay, at rest. Moving the mesh onto the body in StartLyingDown carried
	// them along with it - the head and the right hand 12-13 m from the pelvis, then torn back by the joints, the limb seen flung 1 to
	// 4 m a frame ([sol-entree-corps], 27 October). Put back, the fade starts from the pose he really had.
	if (bEntryFix && BodiesBefore.Num() == Mesh->Bodies.Num())
	{
		int32 PutBack = 0;
		for (int32 Index = 0; Index < Mesh->Bodies.Num(); ++Index)
		{
			FBodyInstance* Body = Mesh->Bodies[Index];
			if (Body != nullptr && Body->IsInstanceSimulatingPhysics())
			{
				Body->SetBodyTransform(BodiesBefore[Index], ETeleportType::TeleportPhysics);
				Body->SetLinearVelocity(FVector::ZeroVector, false);
				Body->SetAngularVelocityInRadians(FVector::ZeroVector, false);
				++PutBack;
			}
		}
		UE_LOG(LogImpact, Display, TEXT("[sol-entree-place] t=%.2f %s : %d corps remis ou ils etaient (%s)"), GetWorld()->GetTimeSeconds(),
		       *GetOwner()->GetName(), PutBack, Mesh->WasRecentlyRendered(0.2f) ? TEXT("vu") : TEXT("hors champ"));
	}
	UE_LOG(LogImpact, Display, TEXT("[sol-anime] t=%.2f %s : allonge, %s a %.2f s (sur %.2f), vitesse %.2f"), GetWorld()->GetTimeSeconds(),
	       *GetOwner()->GetName(), *GroundTake->GetName(), TakeTime, Length, Rate);
	return true;
}

void UImpactReactionComponent::UpdateLyingDown(const float DeltaTime)
{
	LyingTime += DeltaTime;
	AAlsCharacter* Character = GetCharacter();
	if (LyingPhysicsFade > 0.0f)
	{
		// The ragdoll fading out into the take, then the bodies stopped.
		USkeletalMeshComponent* Mesh = Character->GetMesh();
		// Two frames of the ragdoll alone first, while the engine catches up with the mesh having been moved under the bodies.
		const float Hold = 0.03f;
		const float Weight = LyingTime < Hold ? 1.0f : 1.0f - (LyingTime - Hold) / LyingPhysicsFade;
		// A body's and not the component's: a ragdoll's component does not count as simulating. The chest's: the pelvis follows the
		// take during the fade (AAlsCharacter::StartLyingDown).
		const FBodyInstance* Chest = Mesh->GetBodyInstance(TEXT("spine_02"));
		if (Weight > 0.0f && Chest != nullptr && Chest->IsInstanceSimulatingPhysics())
		{
			Mesh->SetAllBodiesPhysicsBlendWeight(Weight * Weight);
			const float Slide = FMath::SmoothStep(0.0f, 1.0f, 1.0f - Weight);
			FTransform Place;
			Place.Blend(LyingFadeFrom, LyingFadeTo, Slide);
			Mesh->SetWorldLocationAndRotationNoPhysics(Place.GetLocation(), Place.Rotator());
		}
		else
		{
			LyingPhysicsFade = 0.0f;
			// [sol-entree-fin]: the fade's end, to line up with the drawn frames of [sol-entree-vu-img] - and why it ended (weight run
			// out, or the chest no longer simulating, which ends it at once whatever the weight).
			UE_LOG(LogImpact, Display, TEXT("[sol-entree-fin] t=%.3f %s : fin du fondu a %.3f s (%s), maillage glisse de %.0f cm"), GetWorld()->GetTimeSeconds(),
			       *GetOwner()->GetName(), LyingTime, Weight <= 0.0f ? TEXT("poids epuise") : TEXT("poitrine plus simulee"),
			       FVector::Dist(Mesh->GetComponentLocation(), LyingFadeTo.GetLocation()));
			Mesh->SetWorldLocationAndRotationNoPhysics(LyingFadeTo.GetLocation(), LyingFadeTo.Rotator());
			ResetBones();
			Character->FinishLyingDownPhysics();
		}
	}
	USkeletalMeshComponent* LyingMesh = Character->GetMesh();
	// -ImpactSolEnchaine: the turn and shift that make the incoming take land on the body, given out over the crossfade as the take
	// comes in, about the pelvis. See SwitchLyingTake.
	if (SwitchFixTimeLeft > 0.0f && LyingPhysicsFade <= 0.0f && !bFallingAnimated && DeltaTime > 0.0f)
	{
		const float Share = FMath::Min(1.0f, DeltaTime / SwitchFixTimeLeft);
		const float Yaw = SwitchYawLeft * Share;
		const FVector Shift = SwitchShiftLeft * Share;
		// About the pelvis as it was at the switch, held in the mesh: the live pelvis slides towards the incoming take's during the
		// crossfade, and turning about it would leave the take off the body (relecture du 27 octobre).
		const FVector Pivot = LyingMesh->GetComponentTransform().TransformPosition(SwitchPivotInMesh);
		const FQuat Turn(FVector::UpVector, FMath::DegreesToRadians(Yaw));
		FRotator Rotation = Character->GetActorRotation();
		Rotation.Yaw += Yaw;
		const FVector Location = Pivot + Turn.RotateVector(Character->GetActorLocation() - Pivot) + Shift;
		Character->SetActorLocationAndRotation(Location, Rotation, false, nullptr, ETeleportType::TeleportPhysics);
		SwitchYawLeft -= Yaw;
		SwitchShiftLeft -= Shift;
		SwitchFixTimeLeft -= DeltaTime;
	}
	// [sol-tourne] and [sol-enchaine]: the body's heading seen from above - what the eye reads as him turning on the floor, whether the
	// actor turned, the crossfade swung the take round, or both.
	if (LyingPhysicsFade <= 0.0f && !bFallingAnimated && DeltaTime > 0.0f)
	{
		const FVector HeadLine = (LyingMesh->GetSocketLocation(TEXT("head")) - LyingMesh->GetSocketLocation(TEXT("pelvis"))).GetSafeNormal2D();
		if (!HeadLine.IsNearlyZero())
		{
			const float Heading = HeadLine.Rotation().Yaw;
			if (bLyingHeadingKnown)
			{
				const float Step = FMath::Abs(FMath::FindDeltaAngleDegrees(LyingHeadingLast, Heading));
				LyingTurnTravel += Step;
				LyingTurnPeak = FMath::Max(LyingTurnPeak, Step / DeltaTime);
				if (SwitchWatchTime >= 0.0f)
				{
					SwitchTurnTravel += Step;
				}
			}
			LyingHeadingLast = Heading;
			bLyingHeadingKnown = true;
			LyingTurnWindow += DeltaTime;
			if (LyingTurnWindow >= 3.0f)
			{
				UE_LOG(LogImpact, Display, TEXT("[sol-tourne] t=%.2f %s : %s, le corps a pivote de %.0f deg en %.1f s (pointe %.0f deg/s), %d changement(s) de prise"),
				       GetWorld()->GetTimeSeconds(), *GetOwner()->GetName(), GroundTake != nullptr ? *GroundTake->GetName() : TEXT("?"), LyingTurnTravel,
				       LyingTurnWindow, LyingTurnPeak, LyingTurnSwitches);
				LyingTurnWindow = 0.0f;
				LyingTurnTravel = 0.0f;
				LyingTurnPeak = 0.0f;
				LyingTurnSwitches = 0;
			}
		}
		if (SwitchWatchTime >= 0.0f && SwitchPoints.Num() == 6)
		{
			static const TCHAR* Watched[] = {TEXT("head"), TEXT("hand_l"), TEXT("hand_r"), TEXT("foot_l"), TEXT("foot_r"), TEXT("pelvis")};
			static const TCHAR* Names[] = {TEXT("tete"), TEXT("main g"), TEXT("main d"), TEXT("pied g"), TEXT("pied d"), TEXT("bassin")};
			for (int32 Index = 0; Index < 6; ++Index)
			{
				const FVector Now = LyingMesh->GetSocketLocation(Watched[Index]);
				const float Jump = FVector::Dist(SwitchPoints[Index], Now);
				if (Jump > SwitchWorst)
				{
					SwitchWorst = Jump;
					SwitchWorstAt = Index;
				}
				SwitchPoints[Index] = Now;
			}
			SwitchWatchTime += DeltaTime;
			if (SwitchWatchTime >= 0.5f)
			{
				UE_LOG(LogImpact, Display, TEXT("[sol-enchaine-vu] t=%.2f %s : %s, plus grand saut d'une image a l'autre %.0f cm (%s), le corps a pivote de %.0f deg (net %.0f deg) en 0,5 s"),
				       GetWorld()->GetTimeSeconds(), *GetOwner()->GetName(), *SwitchLabel, SwitchWorst, Names[SwitchWorstAt], SwitchTurnTravel,
				       FMath::Abs(FMath::FindDeltaAngleDegrees(SwitchHeadingFrom, LyingHeadingLast)));
				SwitchWatchTime = -1.0f;
			}
		}
	}
	if (EntryPoints.Num() == 6)
	{
		// [sol-entree]: the biggest jump of the hands, feet, head or pelvis from one frame to the next while the take takes over -
		// what is seen as a snap - over its first 0.6 s.
		static const TCHAR* Names[] = {TEXT("tete"), TEXT("main g"), TEXT("main d"), TEXT("pied g"), TEXT("pied d"), TEXT("bassin")};
		static const TCHAR* Watched[] = {TEXT("head"), TEXT("hand_l"), TEXT("hand_r"), TEXT("foot_l"), TEXT("foot_r"), TEXT("pelvis")};
		// -ImpactSolEntreeImages: the first entries frame by frame (head and pelvis, relative to where they were the frame before).
		static const bool bFrames = FParse::Param(FCommandLine::Get(), TEXT("ImpactSolEntreeImages"));
		static int32 FramesLeft = 400;
		if (bFrames && FramesLeft > 0)
		{
			--FramesLeft;
			const USkeletalMeshComponent* M = Character->GetMesh();
			UE_LOG(LogImpact, Display, TEXT("[sol-entree-img] t=%.3f %s allonge %.3f poids %.2f simule %d | tete +%.0f cm | bassin +%.0f cm | main d +%.0f cm"),
			       GetWorld()->GetTimeSeconds(), *GetOwner()->GetName(), LyingTime,
			       M->GetBodyInstance(TEXT("pelvis")) != nullptr ? M->GetBodyInstance(TEXT("pelvis"))->PhysicsBlendWeight : -1.0f,
			       M->GetBodyInstance(TEXT("pelvis")) != nullptr && M->GetBodyInstance(TEXT("pelvis"))->IsInstanceSimulatingPhysics() ? 1 : 0, FVector::Dist(EntryPoints[0], M->GetSocketLocation(TEXT("head"))),
			       FVector::Dist(EntryPoints[5], M->GetSocketLocation(TEXT("pelvis"))), FVector::Dist(EntryPoints[2], M->GetSocketLocation(TEXT("hand_r"))));
			// [sol-entree-corps]: the first frames, the bodies against what is drawn. A body that has moved as far as the drawn bone
			// was moved there by the engine (teleported onto the animation); one that has not, and is far from the drawn bone, is
			// still lying where the ragdoll left it while the animation is drawn elsewhere.
			if (EntryBodyStart.Num() == 3 && EntryTraceFrames < 6)
			{
				++EntryTraceFrames;
				static const TCHAR* Traced[] = {TEXT("pelvis"), TEXT("head"), TEXT("hand_r")};
				FString Line;
				for (int32 Index = 0; Index < 3; ++Index)
				{
					const FBodyInstance* Body = M->GetBodyInstance(Traced[Index]);
					const FVector BodyAt = Body != nullptr ? Body->GetUnrealWorldTransform().GetLocation() : FVector::ZeroVector;
					Line += FString::Printf(TEXT(" | %s : corps a bouge de %.0f cm, dessine a %.0f cm du corps (%s)"), Traced[Index],
					                        FVector::Dist(BodyAt, EntryBodyStart[Index]), FVector::Dist(BodyAt, M->GetSocketLocation(Traced[Index])),
					                        Body != nullptr && Body->IsInstanceSimulatingPhysics() ? TEXT("simule") : TEXT("cinematique"));
				}
				UE_LOG(LogImpact, Display, TEXT("[sol-entree-corps] t=%.3f %s image %d, composant deplace de %.0f cm%s"), GetWorld()->GetTimeSeconds(),
				       *GetOwner()->GetName(), EntryTraceFrames, FVector::Dist(M->GetComponentLocation(), EntryComponentStart), *Line);
			}
		}
		for (int32 Index = 0; Index < 6; ++Index)
		{
			const FVector Now = Character->GetMesh()->GetSocketLocation(Watched[Index]);
			const float Jump = FVector::Dist(EntryPoints[Index], Now);
			if (Jump > EntryWorst)
			{
				EntryWorst = Jump;
				EntryWorstAt = Index;
			}
			EntryPoints[Index] = Now;
		}
		if (LyingTime > 0.6f)
		{
			// Seen or not: off screen the engine does not refresh the pose (ALS: OnlyTickMontagesWhenNotRendered), and what is read here is
			// the last rendered pose moved with the mesh - not what anybody could see.
			UE_LOG(LogImpact, Display, TEXT("[sol-entree] t=%.2f %s : plus grand saut d'une image a l'autre %.0f cm (%s), %s"), GetWorld()->GetTimeSeconds(),
			       *GetOwner()->GetName(), EntryWorst, Names[EntryWorstAt], Character->GetMesh()->WasRecentlyRendered(0.2f) ? TEXT("vu") : TEXT("hors champ"));
			EntryPoints.Reset();
		}
	}
	Character->ConsumeMovementInputVector();
	// Never an action with nothing under it. Anything that stops the take - another montage in the same slot group, a blanket stop
	// - and he goes back to being a body instead of standing frozen in whatever pose is left.
	const UAnimInstance* Anim = Character->GetMesh()->GetAnimInstance();
	// The end of a fall: he is on the floor, and from here he is a body like any other until the posture takes him. Caught a little
	// before the end, while the last frame still shows him lying there.
	if (bFallingAnimated && Anim != nullptr && LyingMontage != nullptr && Anim->Montage_IsPlaying(LyingMontage)
	    && Anim->Montage_GetPosition(LyingMontage) >= LyingMontage->GetPlayLength() - 0.1f)
	{
		ReturnToRagdoll(TEXT("fin de chute"), false);
		return;
	}
	if (Anim == nullptr || LyingMontage == nullptr || !Anim->Montage_IsPlaying(LyingMontage))
	{
		ReturnToRagdoll(TEXT("animation arretee"), false);
		return;
	}
	if (SolMixamo())
	{
		UpdateGroundTakeChoice(DeltaTime);
		UpdateGroundAim(DeltaTime);
		UpdateMixamoCrawl(DeltaTime);
	}
}

void UImpactReactionComponent::WatchStandingGhost(const FGameplayTag Action)
{
	// [fantome]: a man who has been on the floor for a while, lying or a ragdoll, drawn with his pelvis at standing height. Reported
	// from play: "il se leve super vite pendant une demi-seconde debout tout droit et refait direct l'animation". Logged with what
	// was playing, the first ten times.
	const bool bDown = Action == AlsLocomotionActionTags::LyingDown || Action == AlsLocomotionActionTags::Ragdolling;
	if (!bDown || DownTime < 1.0f)
	{
		bGhostNow = false;
		return;
	}
	const AAlsCharacter* Character = GetCharacter();
	const USkeletalMeshComponent* Mesh = Character->GetMesh();
	const FVector Pelvis = Mesh->GetSocketLocation(TEXT("pelvis"));
	FHitResult Floor;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(ImpactGhostFloor), false, Character);
	if (!GetWorld()->LineTraceSingleByChannel(Floor, Pelvis + FVector(0.0f, 0.0f, 20.0f), Pelvis - FVector(0.0f, 0.0f, 200.0f), ECC_Visibility, Params))
	{
		return;
	}
	const float Height = Pelvis.Z - Floor.ImpactPoint.Z;
	const bool bGhost = Height > 70.0f;
	if (bGhost)
	{
		++GhostFrames;
	}
	if (bGhost && !bGhostNow && GhostLogged < 10)
	{
		++GhostLogged;
		const UAnimInstance* Anim = Mesh->GetAnimInstance();
		const UAnimMontage* Playing = Anim != nullptr ? Anim->GetCurrentActiveMontage() : nullptr;
		UE_LOG(LogImpact, Warning, TEXT("[fantome] t=%.2f %s debout au sol : bassin a %.0f cm, %s, montage %s a %.2f / %.2f, prise %s, allonge depuis %.2f s"),
		       GetWorld()->GetTimeSeconds(), *GetOwner()->GetName(), Height,
		       Action == AlsLocomotionActionTags::LyingDown ? TEXT("allonge (anime)") : TEXT("ragdoll"),
		       Playing != nullptr ? *Playing->GetName() : TEXT("aucun"), Playing != nullptr ? Anim->Montage_GetPosition(Playing) : 0.0f,
		       Playing != nullptr ? Playing->GetPlayLength() : 0.0f, GroundTake != nullptr ? *GroundTake->GetName() : TEXT("aucune"), LyingTime);
	}
	bGhostNow = bGhost;
}

bool UImpactReactionComponent::IsFacingDown() const
{
	// The way the chest faces, as UpdateCrawl reads it.
	const AAlsCharacter* Character = GetCharacter();
	const USkeletalMeshComponent* Mesh = Character != nullptr ? Character->GetMesh() : nullptr;
	if (Mesh == nullptr)
	{
		return false;
	}
	const FVector ChestAxis = (Mesh->GetSocketLocation(TEXT("spine_03")) - Mesh->GetSocketLocation(TEXT("pelvis"))).GetSafeNormal();
	const FVector Across = (Mesh->GetSocketLocation(TEXT("upperarm_r")) - Mesh->GetSocketLocation(TEXT("upperarm_l"))).GetSafeNormal();
	return (-FVector::CrossProduct(ChestAxis, Across)).Z < -0.3f;
}

void UImpactReactionComponent::UpdateMixamoCrawl(const float DeltaTime)
{
	// -ImpactSolMixamo. The crawl takes crawl in place: he is carried along with them, the way his head points (dragging himself
	// on his front and pushing himself on his back both go head first), at a wounded man's pace, turning slowly towards where
	// he wants to go, and not into a wall.
	// Whenever a crawl take plays, not only when he asked to get away: played in place it was seen crawling on the spot ("la
	// personne avance mais il fait du sur place").
	const bool bCrawlTake = GroundTake != nullptr && (GroundTake->GetName().StartsWith(TEXT("Rampe_")) || GroundTake->GetName() == TEXT("Couche_Rampe"));
	if (!bCrawlTake || LyingTime < 0.8f || (bCrawlGoalOwned && bGroundGoalReached))
	{
		return;
	}
	if (bCrawlGoalOwned)
	{
		// A soldier's crawl: to his goal, and no further (the next take choice lies him still there).
		const FVector To = CrawlGoal - GetCharacter()->GetActorLocation();
		if (To.Size2D() < 45.0f)
		{
			bGroundGoalReached = true;
			bMixamoCrawl = false;
			return;
		}
		CrawlDirection = To.GetSafeNormal2D();
	}
	AAlsCharacter* Character = GetCharacter();
	const USkeletalMeshComponent* Mesh = Character->GetMesh();
	const FVector Head = (Mesh->GetSocketLocation(TEXT("head")) - Mesh->GetSocketLocation(TEXT("pelvis"))).GetSafeNormal2D();
	if (Head.IsNearlyZero())
	{
		return;
	}
	FRotator Rotation = Character->GetActorRotation();
	if (bMixamoCrawl && !CrawlDirection.IsNearlyZero())
	{
		// Turning the actor turns the body lying on it: the head goes round towards the way he wants to go.
		const float Off = FMath::FindDeltaAngleDegrees(Head.Rotation().Yaw, CrawlDirection.Rotation().Yaw);
		const float Turn = bCrawlGoalOwned ? 45.0f : 25.0f;
		Rotation.Yaw += FMath::Clamp(Off, -Turn * DeltaTime, Turn * DeltaTime);
	}
	const FVector Start = Character->GetActorLocation();
	const float HalfHeight = Character->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	const FVector Low = Start - FVector(0.0f, 0.0f, HalfHeight - 25.0f);
	FHitResult Wall;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(ImpactMixamoCrawl), false, Character);
	if (GetWorld()->LineTraceSingleByChannel(Wall, Low, Low + Head * 80.0f, ECC_Visibility, Params))
	{
		Character->SetActorRotation(Rotation);
		return;
	}
	const float Speed = 30.0f * (1.0f - 0.6f * Weakness);
	Character->SetActorLocationAndRotation(Start + Head * Speed * DeltaTime, Rotation, false, nullptr, ETeleportType::None);
	MixamoCrawlDistance += Speed * DeltaTime;
	if (MixamoCrawlDistance - MixamoCrawlLogged >= 100.0f)
	{
		MixamoCrawlLogged = MixamoCrawlDistance;
		UE_LOG(LogImpact, Display, TEXT("[rampe-mixamo] t=%.2f %s : %.0f cm (%s)"), GetWorld()->GetTimeSeconds(), *GetOwner()->GetName(),
		       MixamoCrawlDistance, *GroundTake->GetName());
	}
}

UAnimSequenceBase* UImpactReactionComponent::PickMixamoGroundTake() const
{
	// -ImpactSolMixamo: by where it hurts, as for the made takes, and by which way he lies. A take on his back for a man lying on
	// his front was what turned him over "super vite en sautant": on his front he drags himself on his arms instead, and on his
	// back, if he wants to get away, he pushes himself along it.
	// -ImpactSolArme: with a weapon in his hands, on his front he lies aiming it (firing it when he fires, crawling with it when he
	// crawls); on his back he pushes himself away when he crawls.
	if (WantsGroundAim())
	{
		const bool bCrawl = bCrawlGoalOwned && !bGroundGoalReached;
		const bool bFiring = GetWorld()->GetTimeSeconds() < GroundShotUntil;
		const TCHAR* Armed = IsFacingDown() ? (bCrawl ? TEXT("Couche_Rampe") : bFiring ? TEXT("Couche_Tir") : TEXT("Couche_Attente"))
		                                    : (bCrawl ? TEXT("Rampe_Dos_Recule") : nullptr);
		if (Armed != nullptr)
		{
			if (UAnimSequenceBase* Take = LoadMixamoTake(Armed))
			{
				return Take;
			}
		}
	}
	FString Part;
	if (const UImpactBodyComponent* Body = GetOwner()->FindComponentByClass<UImpactBodyComponent>())
	{
		Part = Body->GetWorstWoundBone().ToString();
	}
	const TCHAR* Name = IsFacingDown() ? TEXT("Rampe_Bras")
	                    : Weakness > 0.5f ? TEXT("Sol_Meurt_Dos")
	                    : bMixamoCrawl ? TEXT("Rampe_Dos_Recule")
	                    : Part.StartsWith(TEXT("spine")) && Part != TEXT("spine_01") ? TEXT("Sol_Souffle_Dos")
	                    : Part.StartsWith(TEXT("thigh")) || Part.StartsWith(TEXT("calf")) ? TEXT("Sol_Gemit_Dos")
	                    : TEXT("Sol_Tord_Dos");
	return LoadMixamoTake(Name);
}

bool UImpactReactionComponent::WantsGroundAim() const
{
	static const bool bOn = GroundFlag(TEXT("ImpactSolArme"));
	const AAlsCharacter* Character = GetCharacter();
	if (!bOn || !SolMixamo() || Character == nullptr || !bVoluntaryControl || Weakness > 0.6f)
	{
		return false;
	}
	const FGameplayTag Overlay = Character->GetOverlayMode();
	return Overlay == AlsOverlayModeTags::Rifle || Overlay == AlsOverlayModeTags::PistolOneHanded || Overlay == AlsOverlayModeTags::PistolTwoHanded;
}

void UImpactReactionComponent::SetGroundGoal(const FVector& World, const bool bOn)
{
	if (bOn && bCrawlGoalOwned && FVector::Dist2D(World, CrawlGoal) < 30.0f)
	{
		return;
	}
	bCrawlGoalOwned = bOn && bVoluntaryControl;
	bMixamoCrawl = bCrawlGoalOwned;
	CrawlGoal = World;
	bGroundGoalReached = false;
}

void UImpactReactionComponent::NotifyGroundShot()
{
	GroundShotUntil = GetWorld()->GetTimeSeconds() + 0.9f;
}

bool UImpactReactionComponent::IsGroundAimReady() const
{
	if (!IsLyingDown() || bFallingAnimated || LyingPhysicsFade > 0.0f || LyingTime < 1.0f || !bGroundAimOn || !WantsGroundAim() || GroundTake == nullptr)
	{
		return false;
	}
	const FString Take = GroundTake->GetName();
	return (Take == TEXT("Couche_Attente") || Take == TEXT("Couche_Tir")) && GroundAimError < 25.0f;
}

namespace
{
	// What two poses on the floor are compared on: what the eye follows on a body lying there.
	const TCHAR* const PoseMatchBones[] = {TEXT("head"), TEXT("spine_03"), TEXT("hand_l"), TEXT("hand_r"), TEXT("lowerarm_l"), TEXT("lowerarm_r"),
	                                       TEXT("foot_l"), TEXT("foot_r"), TEXT("calf_l"), TEXT("calf_r")};
	constexpr int32 PoseMatchCount = UE_ARRAY_COUNT(PoseMatchBones);

	// A take sampled fifteen times a second, once per run: for each sample, its pelvis in the mesh's space and the points relative to it.
	struct FSampledTake
	{
		TArray<float> Times;
		TArray<FVector> Pelvis;
		TArray<FVector> Points;
	};

	TSharedPtr<FSampledTake> SampleTake(const UAnimSequenceBase* TakeBase)
	{
		static TMap<FString, TSharedPtr<FSampledTake>> Cache;
		const FString Key = TakeBase->GetPathName();
		if (const TSharedPtr<FSampledTake>* Found = Cache.Find(Key))
		{
			return *Found;
		}
		TSharedPtr<FSampledTake> Sampled = MakeShared<FSampledTake>();
		Cache.Add(Key, Sampled);
		const UAnimSequence* Take = Cast<UAnimSequence>(TakeBase);
		const USkeleton* Skeleton = Take != nullptr ? Take->GetSkeleton() : nullptr;
		if (Skeleton == nullptr)
		{
			return Sampled;
		}
		const FReferenceSkeleton& Ref = Skeleton->GetReferenceSkeleton();
		const TArray<FTransform>& RefPose = Ref.GetRefBonePose();
		const int32 PelvisBone = Ref.FindBoneIndex(TEXT("pelvis"));
		int32 Bones[PoseMatchCount];
		for (int32 Index = 0; Index < PoseMatchCount; ++Index)
		{
			Bones[Index] = Ref.FindBoneIndex(PoseMatchBones[Index]);
			if (Bones[Index] == INDEX_NONE)
			{
				return Sampled;
			}
		}
		if (PelvisBone == INDEX_NONE)
		{
			return Sampled;
		}
		const float Length = Take->GetPlayLength();
		const int32 Count = FMath::Clamp(FMath::FloorToInt32(Length * 15.0f), 1, 600);
		TMap<int32, FTransform> InMesh;
		for (int32 Sample = 0; Sample < Count; ++Sample)
		{
			const float Time = Length * Sample / Count;
			const FAnimExtractContext Context(static_cast<double>(Time));
			InMesh.Reset();
			// Each bone placed in the mesh's space by walking up to the root once, the bones already placed reused. A bone the take has
			// no track for keeps its reference pose, which is what the engine shows for it.
			const auto Place = [&](const int32 Bone)
			{
				TArray<int32, TInlineAllocator<24>> Chain;
				for (int32 Each = Bone; Each != INDEX_NONE && !InMesh.Contains(Each); Each = Ref.GetParentIndex(Each))
				{
					Chain.Add(Each);
				}
				for (int32 Index = Chain.Num() - 1; Index >= 0; --Index)
				{
					const int32 Each = Chain[Index];
					FTransform Local = RefPose[Each];
					Take->GetBoneTransform(Local, FSkeletonPoseBoneIndex(Each), Context, false);
					const int32 Parent = Ref.GetParentIndex(Each);
					const FTransform Placed = Parent == INDEX_NONE ? Local : Local * InMesh.FindChecked(Parent);
					InMesh.Add(Each, Placed);
				}
				return InMesh.FindChecked(Bone).GetLocation();
			};
			const FVector Pelvis = Place(PelvisBone);
			Sampled->Times.Add(Time);
			Sampled->Pelvis.Add(Pelvis);
			for (int32 Index = 0; Index < PoseMatchCount; ++Index)
			{
				Sampled->Points.Add(Place(Bones[Index]) - Pelvis);
			}
		}
		return Sampled;
	}

	// The turn about Z that best lays the take's points (B) on the body's (A), both relative to their pelvis, and the RMS distance with
	// and without it.
	void AlignAboutZ(const FVector* A, const FVector* B, float& OutYaw, float& OutScore, float& OutRaw)
	{
		double Dot = 0.0;
		double Cross = 0.0;
		for (int32 Index = 0; Index < PoseMatchCount; ++Index)
		{
			Dot += B[Index].X * A[Index].X + B[Index].Y * A[Index].Y;
			Cross += B[Index].X * A[Index].Y - B[Index].Y * A[Index].X;
		}
		const double Angle = FMath::Atan2(Cross, Dot);
		const FQuat Turn(FVector::UpVector, Angle);
		double Turned = 0.0;
		double Raw = 0.0;
		for (int32 Index = 0; Index < PoseMatchCount; ++Index)
		{
			Turned += FVector::DistSquared(A[Index], Turn.RotateVector(B[Index]));
			Raw += FVector::DistSquared(A[Index], B[Index]);
		}
		OutYaw = FMath::RadiansToDegrees(static_cast<float>(Angle));
		OutScore = FMath::Sqrt(static_cast<float>(Turned / PoseMatchCount));
		OutRaw = FMath::Sqrt(static_cast<float>(Raw / PoseMatchCount));
	}
}

bool UImpactReactionComponent::MatchTakePose(const UAnimSequenceBase* Take, float& OutTime, float& OutYaw, float& OutScore, float& OutYawAtZero,
                                             float& OutScoreAtZero, float& OutRawScoreAtZero, FVector& OutPelvisInMesh) const
{
	const AAlsCharacter* Character = GetCharacter();
	if (Take == nullptr || Character == nullptr)
	{
		return false;
	}
	const TSharedPtr<FSampledTake> Sampled = SampleTake(Take);
	if (!Sampled.IsValid() || Sampled->Times.IsEmpty())
	{
		return false;
	}
	// The body's points in the same frame as the take's: relative to the pelvis, in the mesh's heading, upright. Upright and not the
	// mesh's whole turn: a ragdoll's component need not be level, and a take on the floor is.
	const USkeletalMeshComponent* Mesh = Character->GetMesh();
	const FVector Pelvis = Mesh->GetSocketLocation(TEXT("pelvis"));
	const FQuat Upright(FVector::UpVector, FMath::DegreesToRadians(Mesh->GetComponentRotation().Yaw));
	const float Scale = FMath::Max(KINDA_SMALL_NUMBER, static_cast<float>(Mesh->GetComponentScale().Z));
	FVector Now[PoseMatchCount];
	for (int32 Index = 0; Index < PoseMatchCount; ++Index)
	{
		if (Mesh->GetBoneIndex(PoseMatchBones[Index]) == INDEX_NONE)
		{
			return false;
		}
		Now[Index] = Upright.UnrotateVector(Mesh->GetSocketLocation(PoseMatchBones[Index]) - Pelvis) / Scale;
	}
	int32 Best = 0;
	float BestScore = MAX_flt;
	float BestYaw = 0.0f;
	for (int32 Sample = 0; Sample < Sampled->Times.Num(); ++Sample)
	{
		float Yaw = 0.0f;
		float Score = 0.0f;
		float Raw = 0.0f;
		AlignAboutZ(Now, &Sampled->Points[Sample * PoseMatchCount], Yaw, Score, Raw);
		if (Sample == 0)
		{
			OutYawAtZero = Yaw;
			OutScoreAtZero = Score;
			OutRawScoreAtZero = Raw;
		}
		if (Score < BestScore)
		{
			BestScore = Score;
			BestYaw = Yaw;
			Best = Sample;
		}
	}
	OutTime = Sampled->Times[Best];
	OutYaw = BestYaw;
	OutScore = BestScore;
	OutPelvisInMesh = Sampled->Pelvis[Best];
	return true;
}

bool UImpactReactionComponent::SwitchLyingTake(UAnimSequenceBase* Take, const float Blend)
{
	// Another take while lying, crossed over in the same slot: the new one comes in as the old one goes, so nothing of the standing
	// locomotion under them shows (the montage's own blend from LyingDown's start did - [fantome]).
	AAlsCharacter* Character = GetCharacter();
	UAnimInstance* Anim = Character->GetMesh()->GetAnimInstance();
	if (!IsLyingDown() || bFallingAnimated || LyingPhysicsFade > 0.0f || Anim == nullptr || Take == nullptr || Take == GroundTake || !AnimReady(Take))
	{
		return false;
	}
	UAnimMontage* Montage = UAnimMontage::CreateSlotAnimationAsDynamicMontage(Take, TEXT("PostLocomotion"), Blend, 0.2f, 1.0f, 10000);
	if (Montage == nullptr)
	{
		return false;
	}

	// [sol-enchaine] / -ImpactSolEnchaine. The incoming take used to start at its first frame, crossfaded from wherever the outgoing
	// one was, with nothing comparing the two: "a la pose finale d'une animation, il revient a la pose zero d'une autre". And the
	// takes do not lie the same way round in the mesh (Sol_Meurt_Dos's head is 32-39 deg off the other back takes), so the crossfade
	// swung the whole body round on the floor: "il fait des rotations bizarres au sol pour jouer les animations". Now the take starts
	// where its pose is nearest the body's, and the body is turned and shifted about the pelvis, over the crossfade, by what makes
	// that pose land where the body already lies. Both are measured whether the flag is on or not, so the log shows the gap.
	static const bool bMatch = FParse::Param(FCommandLine::Get(), TEXT("ImpactSolEnchaine"));
	// -ImpactSolInertiel: the crossfade replaced by an inertial blend (needs an Inertialization node after PostLocomotion).
	static const bool bInertial = FParse::Param(FCommandLine::Get(), TEXT("ImpactSolInertiel"));
	float StartTime = 0.0f, Yaw = 0.0f, Score = 0.0f, YawZero = 0.0f, ScoreZero = 0.0f, RawZero = 0.0f;
	FVector TakePelvis = FVector::ZeroVector;
	const bool bMatched = MatchTakePose(Take, StartTime, Yaw, Score, YawZero, ScoreZero, RawZero, TakePelvis);
	const float BestTime = StartTime;
	FVector Shift = FVector::ZeroVector;
	if (bMatched && bMatch)
	{
		// Turned about the pelvis by Yaw, the take's pelvis still has to land on the body's: whatever is left is the shift.
		USkeletalMeshComponent* Mesh = Character->GetMesh();
		const FVector Pelvis = Mesh->GetSocketLocation(TEXT("pelvis"));
		const FVector TakePelvisWorld = Mesh->GetComponentTransform().TransformPosition(TakePelvis);
		Shift = -FQuat(FVector::UpVector, FMath::DegreesToRadians(Yaw)).RotateVector(TakePelvisWorld - Pelvis);
		Shift.Z = 0.0f;
		Shift = Shift.GetClampedToMaxSize(80.0f);
	}
	else
	{
		StartTime = 0.0f;
	}

	// Set before playing: UpdateLyingDown sends him back to the ragdoll if LyingMontage is not playing.
	UAnimMontage* Previous = LyingMontage;
	const FString PreviousName = GroundTake != nullptr ? GroundTake->GetName() : TEXT("?");
	LyingMontage = Montage;
	GroundTake = Take;
	float Played;
	if (bInertial)
	{
		FMontageBlendSettings In(Blend);
		In.BlendMode = EMontageBlendMode::Inertialization;
		Played = Anim->Montage_PlayWithBlendSettings(Montage, In, 1.0f, EMontagePlayReturnType::MontageLength, StartTime);
	}
	else
	{
		Played = Anim->Montage_Play(Montage, 1.0f, EMontagePlayReturnType::MontageLength, StartTime);
	}
	if (Played <= 0.0f)
	{
		LyingMontage = Previous;
		ReturnToRagdoll(TEXT("prise refusee"), false);
		return false;
	}
	LastTakeSwitch = GetWorld()->GetTimeSeconds();
	bGunOffsetKnown = false;
	if (bMatch && bMatched)
	{
		SwitchYawLeft = Yaw;
		SwitchShiftLeft = Shift;
		SwitchFixTimeLeft = FMath::Max(Blend, 0.05f);
		USkeletalMeshComponent* Placed = Character->GetMesh();
		SwitchPivotInMesh = Placed->GetComponentTransform().InverseTransformPosition(Placed->GetSocketLocation(TEXT("pelvis")));
	}
	// The half second that follows is watched ([sol-enchaine-vu], UpdateLyingDown).
	USkeletalMeshComponent* Mesh = Character->GetMesh();
	SwitchPoints.Reset();
	for (const TCHAR* Bone : {TEXT("head"), TEXT("hand_l"), TEXT("hand_r"), TEXT("foot_l"), TEXT("foot_r"), TEXT("pelvis")})
	{
		SwitchPoints.Add(Mesh->GetSocketLocation(Bone));
	}
	SwitchWatchTime = 0.0f;
	SwitchWorst = 0.0f;
	SwitchWorstAt = 0;
	SwitchTurnTravel = 0.0f;
	SwitchHeadingFrom = LyingHeadingLast;
	SwitchLabel = FString::Printf(TEXT("%s -> %s"), *PreviousName, *Take->GetName());
	++LyingTurnSwitches;
	UE_LOG(LogImpact, Display, TEXT("[sol-prise] t=%.2f %s : %s"), GetWorld()->GetTimeSeconds(), *GetOwner()->GetName(), *Take->GetName());
	if (bMatched)
	{
		UE_LOG(LogImpact, Display, TEXT("[sol-enchaine] t=%.2f %s : %s, ecart de pose a l'image 0 %.0f cm sans recalage (%.0f cm recale de %.0f deg), au meilleur moment (%.2f s) %.0f cm recale de %.0f deg, %s"),
		       GetWorld()->GetTimeSeconds(), *GetOwner()->GetName(), *SwitchLabel, RawZero, ScoreZero, YawZero, BestTime, Score, Yaw,
		       bMatch ? TEXT("applique") : TEXT("temoin, image 0 sans recalage"));
	}
	return true;
}

void UImpactReactionComponent::UpdateGroundTakeChoice(const float DeltaTime)
{
	TakeChoiceTimer -= DeltaTime;
	if (TakeChoiceTimer > 0.0f)
	{
		return;
	}
	TakeChoiceTimer = 0.25f;
	if (!WantsGroundAim() || LyingTime < 0.8f || LyingPhysicsFade > 0.0f || GetWorld()->TimeSince(LastTakeSwitch) < 0.6f)
	{
		return;
	}
	UAnimSequenceBase* Wanted = PickMixamoGroundTake();
	if (Wanted != nullptr && Wanted != GroundTake)
	{
		SwitchLyingTake(Wanted, 0.25f);
	}
}

void UImpactReactionComponent::UpdateGroundAim(const float DeltaTime)
{
	// Turned on the floor until the barrel points at the target: the prone takes hold the rifle along the body, a few degrees off
	// the line from the pelvis to the head, learnt once per take before he turns.
	AAlsCharacter* Character = GetCharacter();
	const USkeletalMeshComponent* Mesh = Character->GetMesh();
	const USkeletalMeshComponent* Gun = nullptr;
	TInlineComponentArray<USkeletalMeshComponent*> Meshes(Character);
	for (const USkeletalMeshComponent* Each : Meshes)
	{
		if (Each != Mesh && Each->IsVisible() && Each->DoesSocketExist(TEXT("muzzle")))
		{
			Gun = Each;
			break;
		}
	}
	if (Gun == nullptr || !bGroundAimOn)
	{
		GroundAimError = 180.0f;
		return;
	}
	const FVector Muzzle = Gun->GetSocketLocation(TEXT("muzzle"));
	const FVector Barrel = (Muzzle - Gun->GetComponentLocation()).GetSafeNormal();
	const FVector ToTarget = (GroundAimTarget - Muzzle).GetSafeNormal();
	GroundAimError = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct(Barrel, ToTarget), -1.0f, 1.0f)));
	const bool bAimTake = GroundTake != nullptr && GroundTake->GetName().StartsWith(TEXT("Couche_")) && GroundTake->GetName() != TEXT("Couche_Rampe");
	if (!bAimTake || bMixamoCrawl || LyingTime < 0.6f || LyingPhysicsFade > 0.0f)
	{
		return;
	}
	const FVector Head = (Mesh->GetSocketLocation(TEXT("head")) - Mesh->GetSocketLocation(TEXT("pelvis"))).GetSafeNormal2D();
	if (Head.IsNearlyZero())
	{
		return;
	}
	// -ImpactSolEnchaine: the offset is kept per take. Relearnt from zero on every swap, he turned his head line onto the target for
	// 0,4 s, then learnt the offset and turned back by it: a 15-20 deg sweep of the whole body each time Couche_Attente and Couche_Tir
	// swapped, about once a second while he fired (10 swaps in 9 s measured on the 27th). A take never seen yet: he holds still
	// until it is learnt rather than turn towards a wrong aim.
	static const bool bKeepOffset = FParse::Param(FCommandLine::Get(), TEXT("ImpactSolEnchaine"));
	const FName TakeName = GroundTake->GetFName();
	if (bKeepOffset && !bGunOffsetKnown)
	{
		if (const float* Known = GunOffsetByTake.Find(TakeName))
		{
			GunYawOffset = *Known;
			bGunOffsetKnown = true;
		}
	}
	if (!bGunOffsetKnown && GetWorld()->TimeSince(LastTakeSwitch) > 0.4f && FMath::Abs(Barrel.Z) < 0.8f)
	{
		GunYawOffset = FMath::FindDeltaAngleDegrees(Head.Rotation().Yaw, Barrel.Rotation().Yaw);
		bGunOffsetKnown = true;
		GunOffsetByTake.Add(TakeName, GunYawOffset);
	}
	// Nor while the switch turns him onto the incoming take (UpdateLyingDown): two turns at once and the take lands off the body.
	if (bKeepOffset && (!bGunOffsetKnown || SwitchFixTimeLeft > 0.0f))
	{
		return;
	}
	const float Wanted = ToTarget.Rotation().Yaw - (bGunOffsetKnown ? GunYawOffset : 0.0f);
	const float Off = FMath::FindDeltaAngleDegrees(Head.Rotation().Yaw, Wanted);
	if (FMath::Abs(Off) > 3.0f)
	{
		FRotator Rotation = Character->GetActorRotation();
		Rotation.Yaw += FMath::Clamp(Off, -60.0f * DeltaTime, 60.0f * DeltaTime);
		Character->SetActorRotation(Rotation);
	}
	GroundAimLogTimer -= DeltaTime;
	if (GroundAimLogTimer <= 0.0f)
	{
		GroundAimLogTimer = 1.0f;
		UE_LOG(LogImpact, Display, TEXT("[sol-arme] t=%.2f %s : %s, canon a %.0f deg de la cible, decalage %.0f deg"), GetWorld()->GetTimeSeconds(),
		       *GetOwner()->GetName(), *GroundTake->GetName(), GroundAimError, GunYawOffset);
	}
}

UAnimMontage* UImpactReactionComponent::MakeMixamoGetUp()
{
	// -ImpactReleveMixamo. On his back or his front the way ALS tells them apart (the pelvis's roll), slower and more laboured
	// when he is hurt. The takes carry the pelvis's travel in their root (Tools/releve_mixamo_racine.py), so the actor goes with
	// the body and nothing jumps at the end.
	AAlsCharacter* Character = GetCharacter();
	const bool bFacingUp = FMath::UnwindDegrees(Character->GetMesh()->GetSocketTransform(TEXT("pelvis")).Rotator().Roll) <= 0.0f;
	const bool bHurt = Weakness + LimpAmount > 0.4f;
	const TCHAR* Name = !bFacingUp ? TEXT("Releve_Ventre") : bHurt ? TEXT("Releve_Dos_Blesse") : TEXT("Releve_Dos");
	UAnimSequenceBase* Take = LoadMixamoTake(Name);
	if (Take == nullptr || !AnimReady(Take))
	{
		return nullptr;
	}
	MixamoGetUp = UAnimMontage::CreateSlotAnimationAsDynamicMontage(Take, TEXT("PostLocomotion"), 0.3f, 0.4f, 1.0f, 1);
	if (MixamoGetUp != nullptr)
	{
		UE_LOG(LogImpact, Display, TEXT("[releve-mixamo] t=%.2f %s se releve avec %s"), GetWorld()->GetTimeSeconds(), *GetOwner()->GetName(), Name);
	}
	return MixamoGetUp;
}

void UImpactReactionComponent::ReturnToRagdoll(const TCHAR* Reason, const bool bLimpNow)
{
	if (!IsLyingDown())
	{
		return;
	}
	// A fall that has started goes to the floor. Cut short by the next round of a burst, it snapped from the fall into a
	// ragdoll a tenth of a second in (reported from play: "des anims qui rollback"), and doing both in one frame is when the
	// engine's empty-pose crash came back. The wound still counts; the body only stops being animated once it is down.
	if (bFallingAnimated && FCString::Strcmp(Reason, TEXT("fin de chute")) != 0 && FCString::Strcmp(Reason, TEXT("animation arretee")) != 0)
	{
		bLimpAfterFall |= bLimpNow;
		return;
	}
	// Nothing of this lying session carried into the next one: an unfinished switch turn would otherwise turn him on his next fall,
	// and the watches would measure from where he lay last time (relecture du 27 octobre).
	SwitchFixTimeLeft = 0.0f;
	SwitchYawLeft = 0.0f;
	SwitchShiftLeft = FVector::ZeroVector;
	SwitchWatchTime = -1.0f;
	bLyingHeadingKnown = false;
	LyingTurnWindow = 0.0f;
	LyingTurnTravel = 0.0f;
	LyingTurnPeak = 0.0f;
	LyingTurnSwitches = 0;
	const bool bLimpFinal = bLimpNow || bLimpAfterFall;
	bLimpAfterFall = false;
	AAlsCharacter* Character = GetCharacter();
	if (LyingPhysicsFade > 0.0f)
	{
		LyingPhysicsFade = 0.0f;
		Character->GetMesh()->SetAllBodiesPhysicsBlendWeight(1.0f);
	}
	LyingMontage = nullptr;
	bFallingAnimated = false;
	CancelHandCommands();
	SetArmReaching(0, false);
	SetArmReaching(1, false);
	BraceTime = 0.0f;
	bGroundHeld = false;
	// So he lies still again before the posture takes him back, as after a fall.
	SettledTime = 0.0f;
	PhysicalAnimation->SetStrengthMultiplyer(bLimpFinal || !bVoluntaryControl ? 0.0f : bWrithing ? WritheMuscleForce() : 0.32f);
	// ALS leaves any action but Ragdolling for the ragdoll, stops the montages and gives the joints the animated pose as a target.
	Character->StartRagdolling();
	if (FCString::Strcmp(Reason, TEXT("fin de chute")) == 0 || FCString::Strcmp(Reason, TEXT("animation arretee")) == 0)
	{
		// The end of an animated fall. Measured ([fin-chute], and the peaks of [retournement] 0.01 to 0.15 s in): the moment the
		// ragdoll takes over from the take, the body gets a kick of its own - the pelvis turning at 250 to 900 deg/s, the calves
		// at 400 to 1900, the pelvis thrown up 10 to 40 cm - which is what was seen as "il se retourne super vite en sautant, les
		// jambes partent en cacahuete": the turn itself, once it pushes, stays near 180 deg/s. ALS says it in its own code: the
		// ragdoll "can get a much higher initial speed than the character's last speed", and only limits the linear speed.
		// -ImpactFinChuteDouce: the body starts from rest, and for half a second no part of it moves faster than 1 m/s or turns
		// faster than 3 rad/s (170 deg/s).
		USkeletalMeshComponent* Mesh = Character->GetMesh();
		FallEndWatch = 0.0f;
		FallEndPeakHips = FallEndPeakLimb = FallEndPeakSpeed = 0.0f;
		FallEndPeakLimbName = NAME_None;
		FallEndStartZ = FallEndMaxZ = Mesh->GetSocketLocation(TEXT("pelvis")).Z;
		static const bool bGentle = GroundFlag(TEXT("ImpactFinChuteDouce"));
		if (bGentle)
		{
			Mesh->SetAllPhysicsLinearVelocity(FVector::ZeroVector);
			Mesh->SetAllPhysicsAngularVelocityInRadians(FVector::ZeroVector);
			FallEndSettle = 0.8f;
		}
	}
	UE_LOG(LogImpact, Display, TEXT("[sol-anime] t=%.2f %s : retour au ragdoll (%s) apres %.1f s allonge"), GetWorld()->GetTimeSeconds(),
	       *GetOwner()->GetName(), Reason, LyingTime);
}

void UImpactReactionComponent::UpdateGroundAnchor(const float DeltaTime)
{
	// -ImpactSolAncre: the floor's grip, which a ragdoll of capsules does not have. A man lying on a floor touches it
	// with his whole side and his clothes; the ragdoll touches it at a few points on round capsules that roll freely,
	// so the smallest repeated movement walks him across it. Once the pose is taken, the whole body is held near where
	// it ended up, gently, by the same acceleration on every part - so nothing is pulled on, the body is not bent, it
	// simply stops travelling. A bullet or someone walking into him still moves him: the anchor follows him for a
	// second afterwards and never holds him more than a short distance back.
	static const bool bAllowed = GroundFlag(TEXT("ImpactSolAncre"));
	if (!bAllowed || DeltaTime <= 0.0f)
	{
		return;
	}
	static float Omega = -1.0f;
	if (Omega < 0.0f)
	{
		float Parsed = 4.0f;
		Omega = FParse::Value(FCommandLine::Get(), TEXT("ImpactSolAncreRaideur="), Parsed) ? FMath::Max(0.1f, Parsed) : 4.0f;
	}
	constexpr float Leash = 15.0f;        // cm: never held further back than this
	constexpr float MaxAcceleration = 250.0f; // cm/s2: a quarter of gravity, a third of what friction can give
	constexpr float FollowAfterPush = 1.0f;   // s

	// Only once the body is in the pose; on the way into it, rolling over is the whole point.
	const bool bArrived = bPacedTargets ? PacedTime > PacedDuration + 0.3f && PacedDuration > 0.0f : GroundRagdollWeight >= 1.0f;
	USkeletalMeshComponent* Mesh = GetCharacter()->GetMesh();
	if (!bArrived)
	{
		bGroundAnchored = false;
		return;
	}

	float Mass = 0.0f;
	FVector Centre = FVector::ZeroVector;
	FVector Velocity = FVector::ZeroVector;
	for (const FBodyInstance* Body : Mesh->Bodies)
	{
		if (Body != nullptr && Body->IsInstanceSimulatingPhysics())
		{
			const float M = Body->GetBodyMass();
			Mass += M;
			Centre += Body->GetCOMPosition() * M;
			Velocity += Body->GetUnrealWorldVelocity() * M;
		}
	}
	if (Mass <= 0.0f)
	{
		return;
	}
	Centre /= Mass;
	Velocity /= Mass;
	Centre.Z = 0.0f;
	Velocity.Z = 0.0f;

	// Just shot or walked into: no grip at all, not even the damping, or the push would be braked to about 60 % of
	// what it gives without the anchor. The anchor simply moves with him until it is over.
	if (GetWorld()->TimeSince(LastGroundPushTime) < FollowAfterPush)
	{
		GroundAnchor = Centre;
		bGroundAnchored = true;
		return;
	}
	if (!bGroundAnchored)
	{
		GroundAnchor = Centre;
		bGroundAnchored = true;
	}
	const FVector Offset = Centre - GroundAnchor;
	if (Offset.Size() > Leash)
	{
		GroundAnchor = Centre - Offset.GetSafeNormal() * Leash;
	}

	const FVector Accel = (-(Centre - GroundAnchor) * Omega * Omega - Velocity * 1.4f * Omega).GetClampedToMaxSize(MaxAcceleration);
	for (FBodyInstance* Body : Mesh->Bodies)
	{
		if (Body != nullptr && Body->IsInstanceSimulatingPhysics())
		{
			Body->AddForce(Accel, true, true);
		}
	}
}

void UImpactReactionComponent::ReleasePacedJointTargets()
{
	if (!bPacedTargets)
	{
		return;
	}
	bPacedTargets = false;
	PacedTargets.Reset();
	PacedStart.Reset();
	PacedTargetsSet.Reset();
	if (AAlsCharacter* Character = GetCharacter())
	{
		// Still in the ragdoll: ALS keeps the joints on the animation while it lasts (StartRagdollingImplementation).
		// Out of it, ALS has already turned this off itself and it must stay off.
		Character->GetMesh()->bUpdateJointsFromAnimation = Character->GetLocomotionAction() == AlsLocomotionActionTags::Ragdolling;
	}
}

void UImpactReactionComponent::UpdateGroundPose()
{
	// OFF BY DEFAULT, and the reason is what it did in play rather than on the bench.
	//
	// Measured, it looked right: the body curled to 82 cm head-to-knees against 108 sprawled, within two
	// centimetres of the posture the take contains, and slid 0,0 cm/s. What it does in a real fight is
	// another matter. Reported after five rounds into one man: he stopped moving, then went through the
	// get-up five times over, and ended in the skeleton's reference pose - a T-pose, which means the
	// animation stopped evaluating altogether.
	//
	// The cause is the approach, not a value. Taking the posture means borrowing ALS's get-up: it is the
	// only thing that hands a ragdoll back to the animation at the place where it fell. But the character
	// is then in GettingUp with a montage that never ends, inside a state machine built around a get-up
	// that does. Both faults follow from that, and no amount of tuning fixes a state machine that is
	// being lied to.
	//
	// What is needed is a ground state of its own in the animation graph, entered and left on its own
	// terms, instead of a get-up wearing a disguise. -ImpactPostureSol brings this back for measurement.
	static const bool bAllowed = FParse::Param(FCommandLine::Get(), TEXT("ImpactPostureSol"));

	AAlsCharacter* Character = GetCharacter();
	if (!bAllowed || Character == nullptr || bGroundPosed || GroundTake == nullptr || !bWrithing)
	{
		return;
	}

	// Settled, and on the floor. Both matter: ALS refuses the handover in mid-air, and a body still
	// rolling would be snapped into a posture it has not finished falling into.
	if (SettledTime < 0.5f || Character->GetLocomotionAction() != AlsLocomotionActionTags::Ragdolling)
	{
		return;
	}

	UAnimMontage* Ground = UAnimMontage::CreateSlotAnimationAsDynamicMontage(
		// Three quarters of a second in. ALS's own get-up montages blend out of the snapshotted ragdoll
		// pose, and a third of a second is enough for them because they start from a body already lying
		// the way the take expects. These do not.
		GroundTake, UAlsConstants::TransitionSlotName(), 0.75f, 0.4f, 1.0f, 60);
	if (Ground == nullptr)
	{
		return;
	}

	// Where the body actually lies, before anything moves it.
	USkeletalMeshComponent* Mesh = Character->GetMesh();
	const FVector FallenPelvis = Mesh->GetSocketLocation(UAlsConstants::PelvisBoneName());

	bGroundPosed = true;
	Character->GroundMontageOverride = Ground;
	Character->StopRagdolling();

	// Put the body back where it fell. Leaving the ragdoll hands the pose to the animation, and the
	// animation carries its own hip position - each of these ground loops was authored lying in a
	// different spot - so the character jumps to wherever the take says instead of staying where he
	// landed. Reported from play: "tu tires dessus et d'un coup il retourne a la pose 0".
	float Jump = 0.0f;
	if (const UAnimSequence* Take = Cast<UAnimSequence>(GroundTake))
	{
		const USkeleton* Skeleton = Take->GetSkeleton();
		const int32 PelvisIndex = Skeleton != nullptr
			                          ? Skeleton->GetReferenceSkeleton().FindBoneIndex(UAlsConstants::PelvisBoneName())
			                          : INDEX_NONE;
		if (PelvisIndex != INDEX_NONE)
		{
			FTransform TakePelvis;
			Take->GetBoneTransform(TakePelvis, FSkeletonPoseBoneIndex(PelvisIndex), FAnimExtractContext(0.0), false);

			// The pelvis hangs off the root, so the mesh transform is enough to place it.
			const FVector WouldLandAt = Mesh->GetComponentTransform().TransformPosition(TakePelvis.GetLocation());
			const FVector Correction = FallenPelvis - WouldLandAt;
			Jump = Correction.Size();
			Character->SetActorLocation(Character->GetActorLocation() + Correction, false, nullptr, ETeleportType::TeleportPhysics);
		}
	}

	UE_LOG(LogImpact, Display, TEXT("[sol] %s prend la posture %s (%s, recale de %.0f cm)"), *GetOwner()->GetName(),
	       *GroundTake->GetName(),
	       Character->GetLocomotionAction().IsValid() ? *Character->GetLocomotionAction().ToString() : TEXT("echec"), Jump);
}

void UImpactReactionComponent::SetWrithing(const bool bOn)
{
	// -ImpactSansDouleur: never writhe. The control the measurements needed - a body that is down,
	// conscious and hurt but does nothing about it - which is what the game did before this existed.
	static const bool bPainOff = FParse::Param(FCommandLine::Get(), TEXT("ImpactSansDouleur"));
	if (bPainOff && bOn)
	{
		return;
	}
	if (bOn == bWrithing)
	{
		return;
	}
	if (!bOn)
	{
		// He no longer writhes, so the writhing take must not go on playing over him.
		ReturnToRagdoll(TEXT("ne se tord plus"), false);
	}
	bWrithing = bOn;

	AAlsCharacter* Character = GetCharacter();
	USkeletalMeshComponent* Mesh = Character != nullptr ? Character->GetMesh() : nullptr;
	UAnimInstance* Anim = Mesh != nullptr ? Mesh->GetAnimInstance() : nullptr;
	if (Anim == nullptr)
	{
		return;
	}

	if (bOn)
	{
		// The animation says what the body is trying to do; the physics still owns where it actually is, so
		// he writhes against the real ground he is lying on instead of floating through a canned performance.
		// What a man on the ground is trying to do depends on where the hole is. A gut wound curls him up,
		// a chest wound leaves him on his back fighting for air, a leg wound has him reaching for it. One
		// take exists for all of it - Douleur_Au_Sol - so the rest fall back to it and say so in the
		// journal, once each. That log is the shopping list: it names exactly what the game asks for and
		// cannot find, instead of leaving "il n'y a aucune anim" as an impression.
		// The curl, not Douleur_Au_Sol, for wounds elsewhere: that old take was retargeted without its pelvis translation and
		// made the body jump about 90 cm several times a second once it was played for real (-ImpactSolAnime).
		const TCHAR* Wanted = TEXT("Recroqueville_Au_Sol");
		if (const UImpactBodyComponent* Body = GetOwner()->FindComponentByClass<UImpactBodyComponent>())
		{
			const FName Hurt = Body->GetWorstWoundBone();
			const FString Part = Hurt.ToString();
			if (Part == TEXT("spine_01") || Part == TEXT("pelvis"))
			{
				Wanted = TEXT("Recroqueville_Au_Sol");
			}
			else if (Part.StartsWith(TEXT("spine")))
			{
				Wanted = TEXT("Halete_Au_Sol");
			}
			else if (Part.StartsWith(TEXT("thigh")) || Part.StartsWith(TEXT("calf")))
			{
				Wanted = TEXT("Tient_Jambe_Au_Sol");
			}
		}

		// The three ground loops live in their own folder, beside the single take that used to serve for
		// everything. Looked for there first, then flat, so either layout works.
		// -ImpactSolMixamo: the Mixamo takes, chosen as well by which way he lies. A take on his back for a man lying on his front
		// was what turned him over "super vite en sautant": on his front he drags himself on his arms instead.
		// Chosen again once he lies on the floor (UpdateGroundRagdoll): here he may still be standing.
		UAnimSequenceBase* Writhe = SolMixamo() ? PickMixamoGroundTake() : nullptr;
		if (Writhe == nullptr)
		{
			Writhe = LoadObject<UAnimSequenceBase>(nullptr,
				*FString::Printf(TEXT("/Game/Impact/Anims/Sol_Creation_v01/%s.%s"), Wanted, Wanted));
		}
		if (Writhe == nullptr)
		{
			Writhe = LoadObject<UAnimSequenceBase>(nullptr,
				*FString::Printf(TEXT("/Game/Impact/Anims/%s.%s"), Wanted, Wanted));
		}
		if (Writhe == nullptr)
		{
			static TSet<FString> Told;
			if (!Told.Contains(Wanted))
			{
				Told.Add(Wanted);
				UE_LOG(LogImpact, Warning, TEXT("[sol] animation absente : %s — on retombe sur Douleur_Au_Sol"), Wanted);
			}
			Writhe = LoadObject<UAnimSequenceBase>(nullptr, TEXT("/Game/Impact/Anims/Douleur_Au_Sol.Douleur_Au_Sol"));
		}
		if (Writhe != nullptr && AnimReady(Writhe))
		{
			// The take is kept; the handover to it waits until the body has settled. ALS only swaps a
			// ragdoll back to animation when its ground trace succeeds, and asking for it in the same
			// frame as the fall caught the body still in the air: it left the ragdoll into no action at
			// all, which every other system read as "he got up", and the bench knocked him down again one
			// frame later. See UpdateGroundPose.
			GroundTake = Writhe;
			bGroundPosed = false;

			// Until the handover, the muscles pull towards the take while the body is still a ragdoll.
			// Two is the measured limit: below it the movement is lost in the noise of a settling body,
			// above it the body convulses and slides across the floor.
			const float PainForce = WritheMuscleForce();
			Anim->PlaySlotAnimationAsDynamicMontage(Writhe, UAlsConstants::TransitionSlotName(), 0.35f, 0.4f, 1.0f, 0);
			PhysicalAnimation->SetStrengthMultiplyer(PainForce);
			WritheTime = 0.0f;
			UE_LOG(LogImpact, Display, TEXT("[anim] t=%.2f %s : au sol, joue %s"), GetWorld()->GetTimeSeconds(),
			       *GetOwner()->GetName(), *Writhe->GetName());
		}
	}
	else
	{
		Anim->StopSlotAnimation(0.3f, UAlsConstants::TransitionSlotName());
		Character->GroundMontageOverride = nullptr;
		PhysicalAnimation->SetStrengthMultiplyer(bLimp ? 0.0f : 0.32f);
	}
}

void UImpactReactionComponent::SetCrawl(const FVector& WorldDirection, const bool bLeftArm, const bool bRightArm)
{
	const FVector Flat = WorldDirection.GetSafeNormal2D();
	if (SolMixamo() && ((bCrawlGoalOwned && bVoluntaryControl) || WantsGroundAim()))
	{
		// The soldier's brain owns his crawling: he lies aiming his weapon and crawls only where it sends him (the body's own wish
		// to drag himself, legs gone, kept him crawling and never aiming: measured, the barrel 31 deg off for good).
		bMixamoCrawl = bCrawlGoalOwned && !bGroundGoalReached;
		return;
	}
	if (SolMixamo())
	{
		// -ImpactSolMixamo: no crawling by forces on the ragdoll (the turn-over, the legs "en cacahuete"): he drags himself in the
		// crawl take, lying animated (UpdateMixamoCrawl). Only the wish and the way are kept.
		const bool bWas = bMixamoCrawl;
		bMixamoCrawl = bVoluntaryControl && !Flat.IsNearlyZero() && (bLeftArm || bRightArm);
		if (bMixamoCrawl)
		{
			CrawlDirection = Flat;
		}
		if (bMixamoCrawl != bWas)
		{
			UE_LOG(LogImpact, Display, TEXT("[rampe-mixamo] t=%.2f %s %s"), GetWorld()->GetTimeSeconds(), *GetOwner()->GetName(),
			       bMixamoCrawl ? TEXT("veut ramper") : TEXT("ne veut plus ramper"));
		}
	}
	const bool bWanted = !SolMixamo() && bVoluntaryControl && !Flat.IsNearlyZero() && (bLeftArm || bRightArm);
	if (bWanted)
	{
		// Crawling is forces on a simulated body: lying animated, there is none.
		ReturnToRagdoll(TEXT("rampe"), false);
	}
	if (bWanted && !bCrawling)
	{
		CrawlPhase = 0.0f;
		bHasCrawlPlant = false;
		CrawlDistance = 0.0f;
		CrawlArm = bLeftArm ? 0 : 1;
		const AAlsCharacter* Character = GetCharacter();
		CrawlLastLocation = Character != nullptr ? Character->GetActorLocation() : FVector::ZeroVector;
	}
	if (!bWanted && bCrawling)
	{
		SetArmReaching(0, false);
		SetArmReaching(1, false);
	}
	if (!bWanted && bRolling)
	{
		bRolling = false;
		UE_LOG(LogImpact, Display, TEXT("[retournement] t=%.2f %s : interrompu apres %.1f s | bassin %.0f deg/s max | jambes %.0f deg/s max (par rapport au bassin) | bassin monte de %.0f cm, a %.0f cm/s max | pics a %.2f / %.2f / %.2f s"),
		       GetWorld()->GetTimeSeconds(), *GetOwner()->GetName(), RollTime, FMath::RadiansToDegrees(RollPeakHips), FMath::RadiansToDegrees(RollPeakLegs),
		       RollMaxZ - RollStartZ, RollPeakRise, RollPeakHipsAt, RollPeakLegsAt, RollPeakRiseAt);
	}
	bCrawling = bWanted;
	bCrawlArmOk[0] = bLeftArm;
	bCrawlArmOk[1] = bRightArm;
	if (bWanted)
	{
		CrawlDirection = Flat;
	}
}

void UImpactReactionComponent::UpdateCrawl(const float DeltaTime)
{
	if (!bCrawling)
	{
		return;
	}

	AAlsCharacter* Character = GetCharacter();
	USkeletalMeshComponent* Mesh = Character->GetMesh();

	// Which way the chest faces. Someone knocked down lands on his back as often as his front, and you cannot
	// pull yourself along on your arms while lying on your back: he has to turn over first.
	const FVector ChestAxis = (Mesh->GetSocketLocation(TEXT("spine_03")) - Mesh->GetSocketLocation(TEXT("pelvis"))).GetSafeNormal();
	const FVector Across = (Mesh->GetSocketLocation(TEXT("upperarm_r")) - Mesh->GetSocketLocation(TEXT("upperarm_l"))).GetSafeNormal();
	const FVector Front = -FVector::CrossProduct(ChestAxis, Across);

	static const bool bSoft = GroundFlag(TEXT("ImpactRetournementDoux"));
	if (Front.Z > 0.25f || (bSoft && bRolling && Front.Z > -0.3f))
	{
		// How it went, whichever way it is done: how fast the hips turned, how hard the legs whipped about them, how high and how
		// fast the hips went up, and when each of these peaked (from the moment he wanted to turn, which is usually the moment the
		// ragdoll started).
		const FVector HipsAt = Mesh->GetSocketLocation(TEXT("pelvis"));
		if (!bRolling)
		{
			bRolling = true;
			bRollStuckLogged = false;
			RollTime = 0.0f;
			RollPushTime = 0.0f;
			RollPeakHips = RollPeakLegs = RollPeakRise = 0.0f;
			RollPeakHipsAt = RollPeakLegsAt = RollPeakRiseAt = 0.0f;
			RollStartZ = RollMaxZ = HipsAt.Z;
		}
		RollTime += DeltaTime;
		const FVector HipsSpin = Mesh->GetPhysicsAngularVelocityInRadians(TEXT("pelvis"));
		if (HipsSpin.Size() > RollPeakHips)
		{
			RollPeakHips = HipsSpin.Size();
			RollPeakHipsAt = RollTime;
		}
		for (const TCHAR* Calf : {TEXT("calf_l"), TEXT("calf_r")})
		{
			const float Whip = (Mesh->GetPhysicsAngularVelocityInRadians(Calf) - HipsSpin).Size();
			if (Whip > RollPeakLegs)
			{
				RollPeakLegs = Whip;
				RollPeakLegsAt = RollTime;
			}
		}
		const float Rise = Mesh->GetPhysicsLinearVelocity(TEXT("pelvis")).Z;
		if (Rise > RollPeakRise)
		{
			RollPeakRise = Rise;
			RollPeakRiseAt = RollTime;
		}
		RollMaxZ = FMath::Max(RollMaxZ, static_cast<float>(HipsAt.Z));
		if (RollTime > 6.0f && !bRollStuckLogged)
		{
			bRollStuckLogged = true;
			UE_LOG(LogImpact, Display, TEXT("[retournement] t=%.2f %s : toujours sur le dos apres 6 s"), GetWorld()->GetTimeSeconds(), *GetOwner()->GetName());
		}

		if (bSoft)
		{
			// -ImpactRetournementDoux. Reported from play: "quand il se retourne pour ramper, ses jambes partent en cacahuete et il se
			// retourne super vite en sautant". Measured with the old way (below): onto his front in 0.1 s, the pelvis turning at 1400
			// to 1750 deg/s, the calves whipping about it at 800 to 2000 deg/s, the pelvis thrown up 12 to 28 cm at up to 2.5 m/s.
			// A first version that turned every part, legs included, at a set speed stalled on his back and made the legs worse.
			// This one: the trunk alone is turned, as hard as it takes but never faster than about 170 deg/s, and harder the longer he
			// stays stuck; the legs are not driven at all, only kept from whipping about the hips; a little lift on the chest at the
			// very start, none once the hips rise. And it goes on until he is properly on his front (latched by bRolling), so he does
			// not stop on his side and fall back.
			// Not before he has landed: he wants to turn from the very frame the fall ends and the ragdoll takes over, while the body
			// is still settling onto the floor, and the worst turns measured with the second version all began there.
			const float HipsSpeed = Mesh->GetPhysicsLinearVelocity(TEXT("pelvis")).Size();
			if (RollPushTime <= 0.0f && RollTime < 1.5f && (RollTime < 0.6f || HipsSpeed > 60.0f))
			{
				return;
			}
			RollPushTime += DeltaTime;
			const float Rate = Front.Z > 0.0f ? 3.0f : 1.5f;
			const float Boost = 1.0f + 1.5f * FMath::Clamp((RollPushTime - 0.8f) / 1.5f, 0.0f, 1.0f);
			static const TCHAR* Trunk[] = {TEXT("pelvis"), TEXT("spine_01"), TEXT("spine_02"), TEXT("spine_03")};
			for (const TCHAR* Bone : Trunk)
			{
				const FBodyInstance* Part = Mesh->GetBodyInstance(Bone);
				if (Part == nullptr || !Part->IsInstanceSimulatingPhysics())
				{
					continue;
				}
				const float Along = FVector::DotProduct(Part->GetUnrealWorldAngularVelocityInRadians(), ChestAxis);
				const float Turn = Along < Rate ? FMath::Min(200.0f * Boost, (Rate - Along) * 80.0f * Boost) : -(Along - Rate) * 40.0f;
				ApplyActiveTorque(ChestAxis * Turn, Bone, true);
			}
			static const TCHAR* Legs[] = {TEXT("thigh_l"), TEXT("thigh_r"), TEXT("calf_l"), TEXT("calf_r")};
			for (const TCHAR* Bone : Legs)
			{
				const FBodyInstance* Part = Mesh->GetBodyInstance(Bone);
				if (Part != nullptr && Part->IsInstanceSimulatingPhysics())
				{
					ApplyActiveTorque(-(Part->GetUnrealWorldAngularVelocityInRadians() - HipsSpin) * 6.0f, Bone, true);
				}
			}
			if (Front.Z > 0.5f && Rise < 30.0f)
			{
				ApplyActiveForce(FVector(0.0f, 0.0f, 250.0f), TEXT("spine_03"), true);
			}
		}
		else
		{
			// On his back: roll over. Torque about the length of the body, and one arm thrown across to start it.
			const float Push = FMath::Clamp(Front.Z, 0.25f, 1.0f) * 2600.0f;
			ApplyActiveTorque(ChestAxis * Push, TEXT("spine_02"), true);
			ApplyActiveTorque(ChestAxis * (Push * 0.7f), TEXT("pelvis"), true);
			ApplyActiveForce(FVector(0.0f, 0.0f, 400.0f), TEXT("spine_03"), true);
		}

		CrawlLogTimer -= DeltaTime;
		if (CrawlLogTimer <= 0.0f)
		{
			CrawlLogTimer = 0.5f;
			UE_LOG(LogImpact, Display, TEXT("[rampe] t=%.2f %s se retourne (ventre %.2f)"), GetWorld()->GetTimeSeconds(),
			       *GetOwner()->GetName(), Front.Z);
		}
		return;
	}

	if (bRolling)
	{
		bRolling = false;
		UE_LOG(LogImpact, Display, TEXT("[retournement] t=%.2f %s : sur le ventre en %.1f s | bassin %.0f deg/s max | jambes %.0f deg/s max (par rapport au bassin) | bassin monte de %.0f cm, a %.0f cm/s max | pics a %.2f / %.2f / %.2f s"),
		       GetWorld()->GetTimeSeconds(), *GetOwner()->GetName(), RollTime, FMath::RadiansToDegrees(RollPeakHips), FMath::RadiansToDegrees(RollPeakLegs),
		       RollMaxZ - RollStartZ, RollPeakRise, RollPeakHipsAt, RollPeakLegsAt, RollPeakRiseAt);
	}

	// Dragging yourself along is one arm at a time: it reaches ahead, the hand plants, then the whole body is
	// pulled onto it while the legs trail. With one working arm it still goes, slower and off to one side.
	constexpr float ReachShare = 0.45f;

	// Tunable, because once the shoulders were opened the question changed from "can it reach" to "how hard
	// does it pull", and that can only be found by trying values and measuring the distance covered.
	static float Pull = -1.0f;
	static float StrokeTime = -1.0f;
	if (Pull < 0.0f)
	{
		float Parsed = 3200.0f;
		Pull = FParse::Value(FCommandLine::Get(), TEXT("ImpactCrawlPull="), Parsed) ? Parsed : 3200.0f;
		Parsed = 1.25f;
		StrokeTime = FParse::Value(FCommandLine::Get(), TEXT("ImpactCrawlStroke="), Parsed) ? Parsed : 1.25f;
	}

	// Weakness slows both the pull and the rhythm, and the two multiply: at 0.35 and 0.5 a dying man covered
	// 1.4 m in seventeen seconds, which reads as broken rather than as exhausted. Softened on both counts.
	const float Endurance = FMath::Lerp(1.0f, 0.6f, Weakness);
	CrawlPhase += DeltaTime * FMath::Lerp(1.0f, 0.7f, Weakness) / FMath::Max(0.3f, StrokeTime);
	if (CrawlPhase >= 1.0f)
	{
		CrawlPhase -= 1.0f;
		bHasCrawlPlant = false;
		const int32 Other = 1 - CrawlArm;
		if (bCrawlArmOk[Other])
		{
			CrawlArm = Other;
		}
	}
	if (!bCrawlArmOk[CrawlArm])
	{
		CrawlArm = 1 - CrawlArm;
		CrawlPhase = 0.0f;
		bHasCrawlPlant = false;
	}

	// Throwing an arm half a metre ahead of the shoulder is well past the 50 degrees a ragdoll shoulder is
	// allowed, which is the same wall the wound gesture hit. Open both, since they take turns.
	SetArmReaching(0, bCrawlArmOk[0]);
	SetArmReaching(1, bCrawlArmOk[1]);

	const TCHAR* HandBone = CrawlArm == 0 ? TEXT("hand_l") : TEXT("hand_r");
	const TCHAR* ForearmBone = CrawlArm == 0 ? TEXT("lowerarm_l") : TEXT("lowerarm_r");
	const FVector Shoulder = Mesh->GetSocketLocation(CrawlArm == 0 ? TEXT("upperarm_l") : TEXT("upperarm_r"));

	// Where the hand should land: ahead of the shoulder, on whatever ground is there.
	FVector Plant = bHasCrawlPlant ? CrawlPlant : Shoulder + CrawlDirection * 45.0f;
	FHitResult Ground;
	const FCollisionQueryParams Params(SCENE_QUERY_STAT(ImpactCrawl), false, GetOwner());
	const bool bGroundFound = GetWorld()->LineTraceSingleByChannel(Ground, Plant + FVector(0.0f, 0.0f, 60.0f), Plant - FVector(0.0f, 0.0f, 120.0f),
	                                                             ECC_Visibility, Params) && Ground.ImpactNormal.Z > 0.5f;
	if (bGroundFound)
	{
		Plant = Ground.ImpactPoint;
		CrawlPlant = Plant;
		bHasCrawlPlant = true;
	}
	else
	{
		bHasCrawlPlant = false;
		return; // No ground to pull against.
	}

	const bool bReaching = CrawlPhase < ReachShare;
	FBodyInstance* Hand = Mesh->GetBodyInstance(HandBone);
	FVector Closest;
	const float GroundGap = Hand != nullptr ? Hand->GetDistanceToBody(Plant, Closest) : -1.0f;
	const bool bSupported = GroundGap >= 0.0f && GroundGap <= 6.0f;
	if (Hand != nullptr && Hand->IsInstanceSimulatingPhysics())
	{
		// Reaching: lift the hand and carry it forward. Pulling: press it down and hold it where it was put.
		const FVector Goal = bReaching ? Plant + FVector(0.0f, 0.0f, 22.0f) : Plant + FVector(0.0f, 0.0f, 3.0f);
		const FVector Error = Goal - Hand->GetCOMPosition();
		constexpr float Omega = 11.0f;
		FVector Acceleration = Error * (Omega * Omega) - Hand->GetUnrealWorldVelocity() * (2.0f * Omega * 0.9f)
		                       + FVector(0.0f, 0.0f, 980.0f);
		if (!bReaching)
		{
			Acceleration -= CrawlDirection * 1200.0f; // the planted hand pushes back against the ground
		}
		ApplyActiveForce(Acceleration.GetClampedToMaxSize(7000.0f), HandBone, true);

		if (FBodyInstance* Forearm = Mesh->GetBodyInstance(ForearmBone))
		{
			const FVector ElbowGoal = FMath::Lerp(Shoulder, Goal, 0.5f) + FVector(0.0f, 0.0f, bReaching ? 10.0f : 2.0f);
			const FVector ElbowError = ElbowGoal - Forearm->GetCOMPosition();
			const FVector ElbowAcceleration = ElbowError * 90.0f - Forearm->GetUnrealWorldVelocity() * 13.0f
			                                  + FVector(0.0f, 0.0f, 980.0f);
			ApplyActiveForce(ElbowAcceleration.GetClampedToMaxSize(4000.0f), ForearmBone, true);
		}
	}

	// The pull itself: while the hand is down, the hips and chest are dragged towards it.
	if (!bReaching && bSupported)
	{
		const float Effort = FMath::Sin((CrawlPhase - ReachShare) / (1.0f - ReachShare) * PI);
		// A man emptying of blood drags himself more and more slowly.
		const FVector Stroke = CrawlDirection * (Pull * Effort * Endurance);
		ApplyActiveForce(Stroke, TEXT("pelvis"), true);
		ApplyActiveForce(Stroke * 0.8f, TEXT("spine_01"), true);
		ApplyActiveForce(Stroke * 0.6f, TEXT("spine_02"), true);
	}

	// Chest and head up. Reported from play: he crawled with his face in the ground, which read as a zombie
	// rather than someone in pain. Someone dragging himself looks where he is going, and props his chest up.
	ApplyActiveForce(FVector(0.0f, 0.0f, 850.0f * Endurance), TEXT("spine_03"), true);
	ApplyActiveForce(FVector(0.0f, 0.0f, 500.0f * Endurance), TEXT("spine_02"), true);
	if (FBodyInstance* Head = Mesh->GetBodyInstance(TEXT("head")))
	{
		const FVector HeadGoal = Mesh->GetSocketLocation(TEXT("spine_03")) + CrawlDirection * 16.0f + FVector(0.0f, 0.0f, 26.0f);
		const FVector HeadError = HeadGoal - Head->GetCOMPosition();
		ApplyActiveForce((HeadError * 120.0f - Head->GetUnrealWorldVelocity() * 16.0f + FVector(0.0f, 0.0f, 980.0f))
		               .GetClampedToMaxSize(5000.0f), TEXT("head"), true);
	}

	const FVector Now = Character->GetActorLocation();
	CrawlDistance += FVector::Dist2D(Now, CrawlLastLocation) * 0.01f;
	CrawlLastLocation = Now;

	CrawlLogTimer -= DeltaTime;
	if (CrawlLogTimer <= 0.0f)
	{
		CrawlLogTimer = 0.5f;
		UE_LOG(LogImpact, Display, TEXT("[rampe] t=%.2f %s bras %s, %s, parcouru %.2f m (ventre %.2f)"), GetWorld()->GetTimeSeconds(),
		       *GetOwner()->GetName(), CrawlArm == 0 ? TEXT("gauche") : TEXT("droit"),
		       bReaching ? TEXT("tend le bras") : bSupported ? TEXT("tire en appui") : TEXT("cherche l'appui"), CrawlDistance, Front.Z);
	}
}

void UImpactReactionComponent::UpdateLimp(const float DeltaTime)
{
	AAlsCharacter* Character = GetCharacter();
	USkeletalMeshComponent* Mesh = Character->GetMesh();
	// -ImpactSolEntreeJuste: never on a mesh ALS has detached for the ragdoll. UpdateBalance runs just before in the same tick and can
	// make him fall (Collapse -> StartRagdolling detaches the mesh); with no parent the relative location set below is a world
	// location, and the mesh was thrown near the world origin - 10 to 19 m from the body - for the whole fall and until the lying
	// entry. Found by the review of 27 October: the cause of the entry teleports, of the limbs carried away at the entry, and of the
	// blended fall's guide pulling towards nothing.
	static const bool bMeshStays = FParse::Param(FCommandLine::Get(), TEXT("ImpactSolEntreeJuste"));
	if (bMeshStays && Mesh->GetAttachParent() != Character->GetCapsuleComponent())
	{
		return;
	}
	// ALS moves the mesh under the capsule itself: crouching drops it about 34 cm, mantling and standing up from
	// a ragdoll reset it. A base position read once at the start would be wrong after any of those, and the limp
	// offset would be added to a stale origin, which shows as the body jumping.
	BaseMeshLocation = Character->GetBaseTranslationOffset();
	if (!bBaseMeshStored)
	{
		BaseMeshRotation = Mesh->GetRelativeRotation().Quaternion();
		bBaseMeshStored = true;
	}

	// How deep the body drops and how far the trunk leans on the hurt leg. Both are tunable from the command line
	// (-ImpactLimpDip=12 -ImpactLimpLean=8) because the readable amount can only be found by looking at it.
	static float DipDepth = -1.0f;
	static float LeanDegrees = -1.0f;
	if (DipDepth < 0.0f)
	{
		float Parsed = 10.0f;
		DipDepth = FParse::Value(FCommandLine::Get(), TEXT("ImpactLimpDip="), Parsed) ? Parsed : 10.0f;
		Parsed = 7.0f;
		LeanDegrees = FParse::Value(FCommandLine::Get(), TEXT("ImpactLimpLean="), Parsed) ? Parsed : 7.0f;
	}

	// Weight is on the hurt leg when its foot is the lower of the two. A limp is what the body does to spend as
	// little as possible on that leg: it drops onto it, leans the trunk over it to unload the hip, and gets off.
	bool bOnHurtLeg = false;
	if (LimpSide >= 0 && LimpAmount > 0.0f && Character->GetVelocity().Size2D() > 20.0f)
	{
		const float Hurt = Mesh->GetSocketLocation(LimpSide == 0 ? TEXT("foot_l") : TEXT("foot_r")).Z;
		const float Good = Mesh->GetSocketLocation(LimpSide == 0 ? TEXT("foot_r") : TEXT("foot_l")).Z;
		bOnHurtLeg = Hurt <= Good + 1.0f;
	}

	// Each time the hurt leg takes the weight, it gives a little. That jolt goes into the balance loop, which
	// then decides on its own whether a step catches it or not. This is what a limp is made of: not a pose to
	// play, but a leg that keeps nearly failing. Reported from play: "il trebuche presque jamais".
	// Only when he is actually standing on it, and only when the last one is over. A man already lurching
	// does not get an independent second failure on top; measured before this, two jolts landed on
	// consecutive frames and summed to 161 cm/s against a 31 cm step radius.
	const float Settled = (BalanceOffset + BalanceVelocity / FMath::Max(BalanceOmega, 0.01f)).Size();
	if (bOnHurtLeg && !bWasOnHurtLeg && LimpAmount > 0.0f && Settled < 10.0f)
	{
		static float GiveWay = -1.0f;
		if (GiveWay < 0.0f)
		{
			float Parsed = 55.0f;
			GiveWay = FParse::Value(FCommandLine::Get(), TEXT("ImpactLimpGiveWay="), Parsed) ? Parsed : 55.0f;
		}
		const FVector Sideways = Character->GetActorRightVector() * (LimpSide == 0 ? -1.0f : 1.0f);
		const FVector Forwards = Character->GetActorForwardVector();

		// Said directly, because saying it as a fraction of a force kept going wrong: choose where his
		// capture point should land, then give exactly the push that puts it there. The balance loop's own
		// two thresholds decide what that means - under 12 cm the ankles absorb it and nothing shows,
		// between 12 cm and the step radius he saves it with a step, past the step radius nothing reaches
		// and he goes down.
		//
		// Two measured mistakes this replaces. First the push ran from 0,70 to 1,05 of the step radius, so
		// its whole upper half was a fall and a limping man went down every six seconds: reported from play
		// twice as "il fait que de tomber en boucle". Then it was banded, but added on top of whatever
		// imbalance he already carried, so a stumble aimed at 28 cm landed at 38 and fell anyway.
		const float StepRadius = FMath::Max(BalanceCapacity * 0.4f, 6.0f);
		constexpr float AnkleRadius = 12.0f;

		// How often the leg truly fails comes from how little it can still carry: a grazed thigh should
		// hardly ever drop someone, a shattered femur often.
		const float Roll = FMath::FRand();
		const float FailChance = LimpAmount * LimpAmount * 0.025f;
		float Target;
		if (Roll < FailChance)
		{
			Target = StepRadius * FMath::FRandRange(1.15f, 1.50f);
		}
		else if (Roll < FailChance + 0.40f)
		{
			Target = FMath::FRandRange(AnkleRadius * 1.15f, StepRadius * 0.85f);
		}
		else
		{
			Target = FMath::FRandRange(2.0f, AnkleRadius * 0.9f);
		}

		// Only the part he does not already have, so nothing is ever pushed past what was chosen.
		const float Needed = FMath::Max(0.0f, Target - Settled) * BalanceOmega;
		BalanceVelocity += (Sideways * 0.75f + Forwards * 0.35f).GetSafeNormal() * Needed;
	}
	bWasOnHurtLeg = bOnHurtLeg;

	const float DipTarget = bOnHurtLeg ? -DipDepth * LimpAmount : 0.0f;
	const float LeanTarget = bOnHurtLeg ? LeanDegrees * LimpAmount : 0.0f;

	// Dropping onto a painful leg happens fast; coming back off it is what the body hurries to do, so the
	// recovery is faster still. A single symmetric speed read as a gentle sway rather than a limp.
	LimpDip = FMath::FInterpTo(LimpDip, DipTarget, DeltaTime, DipTarget < LimpDip ? 22.0f : 14.0f);
	LimpLean = FMath::FInterpTo(LimpLean, LeanTarget, DeltaTime, LeanTarget > LimpLean ? 20.0f : 13.0f);

	// Someone emptying of blood does not keep walking as if nothing were happening: he sinks onto his legs and
	// stoops forward, more and more. Reported from play: "je le regarde perdre son sang mais sa vitesse, sa
	// respiration, tout est constant". This is the part that shows without needing an animation.
	WeakStoop = FMath::FInterpTo(WeakStoop, Weakness, DeltaTime, 1.5f);
	// -ImpactVentreCourbe. Asked for: "une blessure au ventre devrait faire s'accroupir un peu". A man shot in the belly folds
	// over the hole: the trunk forward, the knees bent (the whole body lowered, ALS's foot placement bends the knees), as if
	// closing round it. Quick to come - he doubles up when it hits - and it stays while the wound is there.
	static const bool bGutFold = GroundFlag(TEXT("ImpactVentreCourbe"));
	GutStoop = FMath::FInterpTo(GutStoop, bGutFold ? GutWound : 0.0f, DeltaTime, 3.0f);
	const float Sink = LimpDip - WeakStoop * 5.0f - GutStoop * 7.0f;
	Mesh->SetRelativeLocation(BaseMeshLocation + FVector(0.0f, 0.0f, Sink));

	// A man favouring a leg bends over it and hunches forward, and he does that with his spine. This used
	// to turn the whole skeletal mesh component - hips, legs and feet with it - and then rely on ALS's foot
	// IK to put the feet back under a body that had been tipped bodily sideways. It read as a model leaning
	// rather than a man leaning. Reported from play: "le boitage est ridicule, ils ont pas l'air de
	// souffrir".
	//
	// The bend now travels as two angles through ALS's own control rig, which spreads them over the pelvis
	// and spine_01..03 and leaves everything below alone: the legs keep walking under a trunk that is bent
	// over them. The channel was added for the hit reactions and went unused when those moved to the Spine
	// animation slot; this is what it is for.
	const float Signed = LimpSide == 0 ? -LimpLean : LimpLean;
	const float Stoop = WeakStoop * 13.0f + GutStoop * 20.0f;
	if (UAlsAnimationInstance* AlsAnim = Cast<UAlsAnimationInstance>(Mesh->GetAnimInstance()))
	{
		AlsAnim->SetTrunkBend(Stoop, Signed);
	}
	if (!BaseMeshRotation.Equals(Mesh->GetRelativeRotation().Quaternion()))
	{
		Mesh->SetRelativeRotation(BaseMeshRotation);
	}
}


void UImpactReactionComponent::UpdateBones(const float DeltaTime)
{
	USkeletalMeshComponent* Mesh = GetCharacter()->GetMesh();
	const float ToneAlpha = DeltaTime > 0.0f ? 1.0f - FMath::Exp(-DeltaTime / ToneRecoveryTime) : 0.0f;

	// An arm reaching for a wound is physical and soft, so the hand can be pulled there.
	for (int32 i = 0; i < Bones.Num(); ++i)
	{
		FBone& Bone = Bones[i];
		const FString Name = Bone.Name.ToString();
		const bool bArmBone = Name.StartsWith(TEXT("upperarm")) || Name.StartsWith(TEXT("lowerarm")) || Name.StartsWith(TEXT("hand"));
		const int32 Side = Name.EndsWith(TEXT("_l")) ? 0 : 1;
		Bone.Cap = 1.0f;
		Bone.bSteered = false;
		if (bArmBone && Hands[Side].Weight > 0.0f)
		{
			// An arm holding a wound still has muscle in it. This used to fall to three per cent, which is
			// no tone at all: the arm was carried only by the force pulling it towards the hole, so the
			// inertia of a running body threw it about and pushed it through the chest. Reported from
			// play: "les bras bougent comme des spaghettis tout en tenant la blessure et ils rentrent
			// dans son corps, on les voit sortir par le dos; quand il arrete de courir ils se stabilisent
			// devant ou il faut". The tone is what has to carry the arm against the body's acceleration;
			// the reach only has to win against the rest.
			// Measured: raising this breaks the hold outright. The muscle pulls the arm back towards the
			// pose the animation wants and the hand never reaches the hole - mean gap 4,1 cm at three per
			// cent, 29,2 cm at thirty-five. The arm must stay slack to be steerable. What it needed was
			// not tone but damping, which is applied separately below.
			Bone.Cap = FMath::Lerp(1.0f, 0.03f, Hands[Side].Weight);
			Bone.bSteered = true;
			Bone.Hold = FMath::Max(Bone.Hold, 0.2f);
		}
	}

	for (int32 i = 0; i < Bones.Num(); ++i)
	{
		FBone& Bone = Bones[i];
		Bone.Tone += (Bone.RestTone - Bone.Tone) * ToneAlpha;
		Bone.Hold = FMath::Max(0.0f, Bone.Hold - DeltaTime);

		// The body above the hips is physical all the time, and the animation is what its muscles aim at
		// rather than what it is. The reference footage behaves this way: across forty-two consecutive
		// frames of a man being shot, no pose repeats, his arms swing from the trunk instead of being
		// placed, and his legs keep stepping throughout.
		//
		// The hips and the legs stay animated, and that is not a compromise but a measured necessity. With
		// them physical too, the planted foot slides at 0,74 of body speed while running against 0,58
		// animated, at every muscle strength tried: ALS plants its feet with a control rig, and a rig
		// cannot place a bone that physics owns.
		//
		// Measured on -ImpactTest=fuite, a man running at 6 m/s shot in the back:
		//
		//                               cadence     foot slide    trunk jitter   reaction
		//   animated (the old default)  5,0 pas/s   0,57-0,60     361-425 deg/s  x1,68-2,77
		//   all physical, muscle x1     3,3 pas/s   0,72-0,73     612-854 deg/s  x1,07-2,26
		//   all physical, muscle x4     5,0 pas/s   0,74          309 deg/s      x3,81
		//   upper body only, muscle x4  4,9-5,0     0,45-0,59     420-641 deg/s  x0,89-3,45
		//
		// The muscle strength was the real lock, not the physics. At x1 a simulated body cannot keep up
		// with the animation it is meant to reproduce, so the cadence collapses and the body shakes; at x4
		// both come back. -ImpactCorpsAnime returns to the old animated body and -ImpactJambesPhysiques
		// puts the legs back in, so the four rows above can still be reproduced.
		static const bool bAlwaysPhysical = !FParse::Param(FCommandLine::Get(), TEXT("ImpactCorpsAnime"));
		const FGameplayTag Overlay = GetCharacter()->GetOverlayMode();
		const bool bArmedAim = GetCharacter()->IsDesiredAiming() && (Overlay == AlsOverlayModeTags::Rifle
			|| Overlay == AlsOverlayModeTags::PistolOneHanded || Overlay == AlsOverlayModeTags::PistolTwoHanded);
		static const bool bAnimatedLegs = !FParse::Param(FCommandLine::Get(), TEXT("ImpactJambesPhysiques"));
		const FString BoneName = Bone.Name.ToString();

		// The pelvis counts as a leg bone here: it is what the legs hang from, and the control rig places
		// the feet relative to it.
		const bool bLowerBody = BoneName == TEXT("pelvis") || BoneName == TEXT("root")
			|| BoneName.StartsWith(TEXT("thigh")) || BoneName.StartsWith(TEXT("calf"))
			|| BoneName.StartsWith(TEXT("foot")) || BoneName.StartsWith(TEXT("ball"));

		// A weak limb stays physical (it hangs); otherwise the bone returns to animation once the hit is absorbed.
		// An armed man aiming is animated above the hips between hits, so the gun points where ALS aims it. Reported from play:
		// "il vise mal le joueur, il vise n'importe comment" - a physical arm only follows the aiming pose at a distance, and the
		// gun with it. A hit still makes the struck part physical (Hold) for the reaction, a hand on a wound keeps its arm physical,
		// and once the reaction is over he aims again.
		const bool bWanted = (bAlwaysPhysical && !bArmedAim && !(bAnimatedLegs && bLowerBody))
			|| Bone.Hold > 0.0f || Bone.RestTone < 0.5f;
		float Blend = bWanted ? 1.0f : FMath::Max(0.0f, Bone.Blend - DeltaTime / BlendOutTime);

		// A simulated parent drags its children: they must be at least as physical.
		if (Bone.Parent != INDEX_NONE)
		{
			Blend = FMath::Max(Blend, Bones[Bone.Parent].Blend);
		}
		Bone.Blend = Blend;

		const bool bSimulate = Blend > 0.001f;
		if (bSimulate != Bone.bSimulating)
		{
			Mesh->SetBodySimulatePhysics(Bone.Name, bSimulate);
			Bone.bSimulating = bSimulate;
			Bone.AppliedStrength = -1.0f;
		}

		// On the ground and still conscious, the body stops being pure physics and lets the animation pose
		// it, with the physics kept for contact with the floor. A muscle pulling a ragdoll towards a pose
		// cannot fold it against gravity and friction at any strength that stays stable: measured, the
		// curl-up loop gave head-to-knees 105 cm where doing nothing at all gave 108, which is to say the
		// body never curled. Blending is what actually poses a body; muscles only push it about.
		// The hips are left alone so nothing drags the body across the floor.
		static float GroundBlend = -1.0f;
		if (GroundBlend < 0.0f)
		{
			float Parsed = 1.0f;
			GroundBlend = FParse::Value(FCommandLine::Get(), TEXT("ImpactSolMelange="), Parsed) ? FMath::Clamp(Parsed, 0.0f, 1.0f) : 1.0f;
		}
		const bool bHips = BoneName == TEXT("pelvis") || BoneName == TEXT("root");
		const float Shown = bWrithing && !bHips ? Blend * GroundBlend : Blend;

		if (FBodyInstance* Body = Mesh->GetBodyInstance(Bone.Name))
		{
			Body->PhysicsBlendWeight = Shown;
		}

		// While he writhes on the ground the hips get no muscle at all. A take is recorded at the origin of
		// the world, and a muscle on the pelvis pulls the whole body towards where that take was recorded:
		// measured at a strength of five, the body slid across the floor at 24 cm/s and its limbs thrashed
		// at 637 cm/s. Reported from play: "ils sont allonges et bougent tout le long du sol tout seuls
		// tres vite". The limbs and the spine may be driven; what the body as a whole does belongs to the
		// physics and the ground it is lying on. Same lesson as the walk: the pelvis is not ours to move.
		// Only the hips, and only the hips. Cutting the whole lower body stopped the sliding but also took
		// the legs away, and a man cannot curl up without bringing his knees in: measured, head-to-knees
		// 103 cm with the curl-up loop playing against 108 cm with no loop at all, which is to say the
		// body never curled. What drags a body across the floor is a muscle on the pelvis pulling towards
		// where the take was recorded; the legs pull on the pelvis, not on the world.
		// -ImpactSolBassin lets the hips turn while he writhes. Their position is never driven anywhere
		// (PositionStrength is zero for every bone), so what this costs is only what a body rolling on
		// the floor costs: friction turns rotation into travel, which is what a real body does too.
		// Without it he cannot roll onto his side, and curling up starts with rolling onto your side.
		static const bool bHipsTurn = FParse::Param(FCommandLine::Get(), TEXT("ImpactSolBassin"));
		const bool bNoMuscle = bWrithing && bHips && !bHipsTurn;
		const float Tone = bNoMuscle ? 0.0f : FMath::Min(Bone.Tone, Bone.Cap);
		const float Strength = Bone.BaseStrength * Tone * MuscleScale();
		if (bSimulate && FMath::Abs(Strength - Bone.AppliedStrength) > Bone.BaseStrength * MuscleScale() * 0.02f)
		{
			FPhysicalAnimationData Data;
			Data.BodyName = Bone.Name;
			Data.bIsLocalSimulation = true;
			Data.OrientationStrength = Strength;

			// Damping is normally a quarter of the pull, which is fine while a bone is being held to a
			// pose. An arm steering itself onto a wound is deliberately left without pull, and that took
			// its damping away with it: nothing then resisted the arm's own velocity, so the inertia of a
			// running body set it swinging and it oscillated through the chest. Damping costs nothing in
			// steering - it resists speed, not position - so a steered arm keeps it in absolute terms.
			static float SteerDamp = -1.0f;
			if (SteerDamp < 0.0f)
			{
				float Parsed = 0.0f;
				SteerDamp = FParse::Value(FCommandLine::Get(), TEXT("ImpactBrasAmorti="), Parsed) ? FMath::Max(0.0f, Parsed) : 0.0f;
			}
			Data.AngularVelocityStrength = Bone.bSteered && SteerDamp > 0.0f
				                               ? FMath::Max(Strength * 0.25f, Bone.BaseStrength * SteerDamp * MuscleScale())
				                               : Strength * 0.25f;
			Data.PositionStrength = 0.0f;
			Data.VelocityStrength = 0.0f;
			Data.MaxLinearForce = 0.0f;
			Data.MaxAngularForce = 0.0f;
			PhysicalAnimation->ApplyPhysicalAnimationSettings(Bone.Name, Data);
			Bone.AppliedStrength = Strength;
		}
	}
}

void UImpactReactionComponent::UpdateBalance(const float DeltaTime)
{
	AAlsCharacter* Character = GetCharacter();
	FVector Centre, Velocity, Support;
	bool bLeft = false, bRight = false;
	if (Character == nullptr || !GetBalanceState(Centre, Velocity, Support, bLeft, bRight))
	{
		return;
	}

	const float Dt = FMath::Min(DeltaTime, 1.0f / 60.0f);
	const float Height = FMath::Max(Centre.Z - Support.Z, 40.0f);
	const float Omega = FMath::Sqrt(980.0f / Height);

	// An inverted pendulum runs away from its support: the further out it already is, the harder it pulls.
	// Nothing here is measured from the mesh, because a kinematic skeleton cannot fall over by itself.
	BalanceVelocity += BalanceOffset * (Omega * Omega) * Dt;
	BalanceOffset += BalanceVelocity * Dt;
	BalanceOffset.Z = 0.0f;
	BalanceVelocity.Z = 0.0f;

	// Where the feet would have to land to stop all of it. This is the whole decision.
	const FVector Capture = BalanceOffset + BalanceVelocity / Omega;
	const float Reach = Capture.Size();

	// What those legs can still do. A healthy pair catches about 55 cm; a ruined one catches nothing.
	const float StepRadius = FMath::Max(BalanceCapacity * 0.4f, 6.0f);
	constexpr float AnkleRadius = 12.0f;

	if (StepTimer > 0.0f)
	{
		// A step is in the air. Drive the body towards where it must land, harder the further it has to go.
		StepTimer -= DeltaTime;
		const FVector Towards = StepTarget.GetSafeNormal2D();
		if (!Towards.IsNearlyZero())
		{
			Character->AddMovementInput(Towards, FMath::Clamp(StepTarget.Size() / 45.0f, 0.25f, 1.0f));
		}
		if (StepTimer <= 0.0f)
		{
			// The foot is down: the support has moved under the body, so most of the imbalance is gone.
			// Landing a little short is what makes someone take two or three steps instead of one neat one.
			BalanceOffset -= StepTarget * 0.9f;
			++StepsTaken;
		}
	}
	else if (Reach > StepRadius)
	{
		// Beyond what a step can catch. He goes down the way he was already going.
		const FVector Going = BalanceVelocity.GetSafeNormal2D();
		Collapse(Going * (BalanceVelocity.Size2D() * 1.5f + 120.0f), false,
		         StepsTaken > 0 ? TEXT("ne rattrape plus") : TEXT("déséquilibre"));
		BalanceOffset = FVector::ZeroVector;
		BalanceVelocity = FVector::ZeroVector;
		StepsTaken = 0;
	}
	else if (Reach > AnkleRadius)
	{
		// Worth a step. It takes a fifth of a second to get the foot there, and the capture point keeps
		// running away during that time, so aim at where it will be, not where it is.
		StepTarget = Capture * FMath::Exp(Omega * 0.22f);
		StepTarget = StepTarget.GetClampedToMaxSize(StepRadius);
		StepTimer = 0.22f;
	}
	else
	{
		// Small enough to hold with the ankles and a lean. It bleeds off on its own.
		const float Settle = FMath::Exp(-Dt * 3.5f);
		BalanceOffset *= Settle;
		BalanceVelocity *= Settle;
		if (Reach < 1.0f)
		{
			StepsTaken = 0;
		}
	}

	// The rest of the code still asks "is he catching himself" through this. Kept meaningful, and kept at
	// zero when he is not, so the passer-by brain does not think he is staggering for ever and freeze.
	StaggerRemaining = StepTimer > 0.0f ? Reach : 0.0f;
	StaggerDirection = Capture.GetSafeNormal2D();

	BalanceLogTimer -= DeltaTime;
	if (BalanceLogTimer <= 0.0f && (Reach > 2.0f || StepsTaken > 0))
	{
		BalanceLogTimer = 0.2f;
		UE_LOG(LogImpact, Display, TEXT("[equilibre] t=%.2f %s : ecart %.0f cm, vitesse %.0f cm/s, rattrapage %.0f cm (limite %.0f), %s, pas %d"),
		       GetWorld()->GetTimeSeconds(), *GetOwner()->GetName(), BalanceOffset.Size(), BalanceVelocity.Size(),
		       Reach, StepRadius, StepTimer > 0.0f ? TEXT("fait un pas") : Reach > AnkleRadius ? TEXT("va devoir") : TEXT("tient"),
		       StepsTaken);
	}
}

void UImpactReactionComponent::UpdateStagger(const float DeltaTime)
{
	if (StaggerRemaining <= 1.0f)
	{
		StaggerRemaining = 0.0f;

		// Done flinching: stop staring at the shooter and face wherever he is going again. Without this the
		// character kept the threat in view for the rest of his life and moved backwards everywhere.
		if (bTurnedToThreat)
		{
			bTurnedToThreat = false;
			if (AAIController* Controller = Cast<AAIController>(GetCharacter()->GetController()))
			{
				Controller->ClearFocus(EAIFocusPriority::Gameplay);
			}
			GetCharacter()->SetDesiredRotationMode(AlsRotationModeTags::VelocityDirection);
		}
		return;
	}

	AAlsCharacter* Character = GetCharacter();
	Character->AddMovementInput(StaggerDirection, 1.0f);

	const float Speed = Character->GetCharacterMovement()->Velocity.Size2D();
	StaggerRemaining -= FMath::Max(Speed, 40.0f) * DeltaTime;
}

void UImpactReactionComponent::UpdateDown(const float DeltaTime)
{
	AAlsCharacter* Character = GetCharacter();
	const bool bRagdolling = Character->GetLocomotionAction() == AlsLocomotionActionTags::Ragdolling;

	// A man holding a ground posture is still on the ground, even though he left the simulation to take
	// it. Counting only the ragdoll froze his time on the floor the moment he curled up, so the test that
	// lets him stand again could never come true: three runs, three falls, three postures taken, not one
	// man back on his feet.
	const bool bLying = IsLyingDown();
	const bool bOnTheFloor = bRagdolling || bGroundPosed || bLying;

	if (bOnTheFloor)
	{
		DownTime += DeltaTime;
		FHitResult Ground;
		const FVector Pelvis = Character->GetMesh()->GetSocketLocation(TEXT("pelvis"));
		const FCollisionQueryParams Params(SCENE_QUERY_STAT(ImpactGetUp), false, GetOwner());
		const bool bOnGround = GetWorld()->LineTraceSingleByChannel(Ground, Pelvis + FVector(0, 0, 10), Pelvis - FVector(0, 0, 65),
		                                                           ECC_Visibility, Params) && Ground.ImpactNormal.Z > 0.65f;
		// Already posed means already settled: there is no ragdoll velocity left to read.
		SettledTime = bGroundPosed || bLying || (bOnGround && Character->GetRagdollingState().Velocity.Size() < 25.0f)
			              ? SettledTime + DeltaTime
			              : 0.0f;
		GroundContactTime = bOnGround ? GroundContactTime + DeltaTime : 0.0f;

		// -ImpactChuteTenue. Reported from play, again: "les animations quand il tombe sont toujours n'importe quoi". What the body
		// does in the fall is set by two things that both aim at A_Als_Flail, ALS's arm-windmilling for a body thrown through the air:
		// its joint motors, and our PhysicalAnimation muscles - which a wounded man gets at the pain force, six times the fall tone,
		// from the very frame he goes down (SetWrithing). So a man shot in the belly fell windmilling. Here, while he falls and until
		// the ground posture takes him, the muscles are off, and the joints hold the pose he went down in - firmly at first, the legs
		// barely, all of it fading to nothing in a second: he crumples, knees first, instead of flailing. The hands still go out to
		// the floor (UpdateBrace).
		static const bool bHeldFall = GroundFlag(TEXT("ImpactChuteTenue"));
		const bool bHeldFallActive = bHeldFall && bRagdolling && GroundMontage == nullptr;
		// Checked every frame, not once: SetWrithing puts the pain force back the frame he goes down, and that was the somersault -
		// those muscles push each bone with nothing pushing back on its parent, so at the pain force they spun the whole body: filmed,
		// head down and legs up 0.9 s into a fall, with or without the hands going out.
		if (bHeldFallActive && PhysicalAnimation->StrengthMultiplyer != 0.0f)
		{
			PhysicalAnimation->SetStrengthMultiplyer(0.0f);
		}
		// And nothing after the hold until the posture takes over: ALS's own motors would aim at the same frozen standing pose with a
		// stiffness from the speed, and they stood him back up like a puppet - filmed, crouching at 0.5 s and upright again at 1.2 s.
		if (bHeldFallActive && bVoluntaryControl && !bCrawling)
		{
			const float Fade = GroundContactTime < 0.4f ? FMath::Clamp(1.0f - DownTime / 1.0f, 0.0f, 1.0f) : 0.0f;
			for (FConstraintInstance* Joint : Character->GetMesh()->Constraints)
			{
				if (Joint == nullptr)
				{
					continue;
				}
				const FString Name = Joint->JointName.ToString();
				const bool bLeg = Name.StartsWith(TEXT("thigh")) || Name.StartsWith(TEXT("calf")) || Name.StartsWith(TEXT("foot")) || Name.StartsWith(TEXT("ball"));
				const bool bArm = Name.StartsWith(TEXT("upperarm")) || Name.StartsWith(TEXT("lowerarm")) || Name.StartsWith(TEXT("hand"));
				const float Share = bLeg ? 0.1f : bArm ? 0.3f : 1.0f;
				Joint->SetAngularDriveParams(4000.0f * Fade * Share, 0.0f, 0.0f);
			}
		}
		// -ImpactJournalChute: what holds him up during the fall, every tenth of a second for the first 2.5 s.
		static const bool bFallLog = GroundFlag(TEXT("ImpactJournalChute"));
		static float NextFallLog = 0.0f;
		if (bFallLog && bRagdolling && DownTime < 2.5f && DownTime >= NextFallLog)
		{
			NextFallLog = DownTime + 0.1f;
			USkeletalMeshComponent* M = Character->GetMesh();
			float KneeSpring = -1.0f;
			float KneeDamp = -1.0f;
			float KneeLimit = -1.0f;
			for (FConstraintInstance* Joint : M->Constraints)
			{
				if (Joint != nullptr && Joint->JointName == TEXT("calf_l"))
				{
					Joint->GetAngularDriveParams(KneeSpring, KneeDamp, KneeLimit);
				}
			}
			UE_LOG(LogImpact, Display, TEXT("[chute-journal] %.2f s : bassin %.0f cm, tete %.0f cm, genou ressort %.0f, muscles %.2f, mains %.2f s, cible anim %d, contact %.2f s, vitesse %.0f"),
			       DownTime, M->GetSocketLocation(TEXT("pelvis")).Z - Character->GetActorLocation().Z + Character->GetCapsuleComponent()->GetScaledCapsuleHalfHeight(),
			       M->GetSocketLocation(TEXT("head")).Z - Character->GetActorLocation().Z + Character->GetCapsuleComponent()->GetScaledCapsuleHalfHeight(),
			       KneeSpring, PhysicalAnimation->StrengthMultiplyer, BraceTime, M->bUpdateJointsFromAnimation ? 1 : 0, GroundContactTime,
			       Character->GetRagdollingState().Velocity.Size());
		}
		if (!bRagdolling)
		{
			NextFallLog = 0.0f;
		}

		if (bHeldFallActive && bVoluntaryControl && !bCrawling)
		{
			// Set above.
		}
		else if (!bVoluntaryControl || bCrawling || BraceTime > 0.0f)
		{
			// ALS gives the ragdoll some muscle depending on its speed; a dead or unconscious body has none.
			Character->GetMesh()->SetAllMotorsAngularDriveParams(0.0f, 0.0f, 0.0f);
		}
		else if (GroundMontage == nullptr && GroundContactTime > 0.5f)
		{
			// -ImpactSolSansFlail: once he is lying on the floor, ALS's flight muscles are switched off. ALS drives
			// every joint towards A_Als_Flail with a stiffness of speed/1000 x 25000, and plays the flail at
			// speed/1000: made for a body in the air. On a floor the two feed each other - the flail moves the
			// body, the movement stiffens and speeds up the flail - and the body thrashes across the ground
			// instead of settling: measured, limbs at 340 cm/s for two seconds, the body turning 100 degrees
			// and travelling 100 to 330 cm before the posture could start, in half the runs.
			static const bool bNoFlail = GroundFlag(TEXT("ImpactSolSansFlail"));
			if (bNoFlail)
			{
				Character->GetMesh()->SetAllMotorsAngularDriveParams(0.0f, 0.0f, 0.0f);
			}
		}

		// Here and not in the conscious branch: a dead man has to be shovable too.
		PushedByPawns(DeltaTime);
		// Getting up costs more each time, and after the third collapse in a row he stops trying. A person
		// does not stand up into the same fall indefinitely; he stays down, or drags himself. Without this,
		// a man with a bullet in his thigh fell and rose every four seconds for as long as he was watched.
		const float Patience = 2.5f + RecentFalls * 1.6f;
		// Lying animated, he has at least a second and a half of it first: entering the take and standing up the next
		// instant is the enter-and-leave flicker the first attempt was made of.
		if (bVoluntaryControl && !bCrawling && bMayGetUp && !bHeldDown && RecentFalls < 3
		    && DownTime > Patience && (SettledTime > 0.75f || bGroundPosed) && (!bLying || LyingTime > 1.5f) && !bFallingAnimated)
		{
			Character->GroundMontageOverride = ReleveMixamo() ? MakeMixamoGetUp() : nullptr;
			if (bLying)
			{
				// Out of the lying take the way ALS gets anyone up: a ragdoll for no time at all, so ALS has a body to
				// snapshot, then its own get-up, whose montage ends and clears GettingUp itself.
				LyingMontage = nullptr;
				Character->StartRagdolling();
				StopGroundSlot(0.2f);
				Character->StopRagdolling();
				RefreshPoseNow();
			}
			else if (bGroundPosed)
			{
				// Already out of the ragdoll and holding a ground posture, so there is no ragdoll left to
				// stop: ending the posture is what lets him stand. Without this he took the posture once
				// and stayed in it for good, because the get-up asked a body that was no longer simulated
				// to stop simulating.
				// Forgetting that he was writhing matters as much as stopping the montage: the state is only
				// armed on the way into it, so a man who stood up and went down again stayed a plain
				// ragdoll the second time - measured, 111 cm where the first fall gave 82.
				bGroundPosed = false;
				GroundTake = nullptr;
				bWrithing = false;
				if (UAnimInstance* Anim = Character->GetMesh()->GetAnimInstance())
				{
					Anim->Montage_Stop(0.35f);
				}
			}
			else
			{
				// Let go of the ragdoll slot first. The get-up is a montage too, and one of them has to
				// be out of the way: this is the same mistake as before, only from the other side.
				StopGroundSlot(0.2f);
				Character->StopRagdolling();
				RefreshPoseNow();
			}

			// Getting up at full speed whatever has been done to him was the thing that gave the game
			// away: a man with a bullet in his thigh and a third of his blood gone stood up exactly as
			// briskly as an unhurt one. Reported from play: "il se releve tout le temps normalement, peu
			// importe comment ils sont blesses". ALS plays a montage to stand up; what it costs him is in
			// its speed. Blood lost and a leg that no longer carries both slow it, and every fall he has
			// already taken slows it further.
			const float Cost = FMath::Clamp(Weakness + LimpAmount * 0.5f + RecentFalls * 0.15f, 0.0f, 1.0f);
			// The Mixamo get-ups take 8 to 11 s at their own speed: played half as fast again.
			const float Rate = FMath::Lerp(1.0f, 0.40f, Cost) * (MixamoGetUp != nullptr ? 1.5f : 1.0f);
			if (UAnimInstance* Anim = Character->GetMesh()->GetAnimInstance())
			{
				Anim->Montage_SetPlayRate(nullptr, Rate);
			}
			UE_LOG(LogImpact, Display, TEXT("[reaction] t=%.2f %s se relève a %.0f%% de vitesse (%d chute%s, faiblesse %.2f)"),
			       GetWorld()->GetTimeSeconds(), *GetOwner()->GetName(), Rate * 100.0f, RecentFalls,
			       RecentFalls == 1 ? TEXT("") : TEXT("s"), Weakness);
		}
		else if (bVoluntaryControl && !bCrawling && RecentFalls >= 3)
		{
			if (!bGaveUpStanding)
			{
				bGaveUpStanding = true;
				UE_LOG(LogImpact, Display, TEXT("[reaction] t=%.2f %s renonce a se relever apres %d chutes"),
				       GetWorld()->GetTimeSeconds(), *GetOwner()->GetName(), RecentFalls);
			}
			// Giving up is a rest, not a sentence. After twenty seconds on the ground he has got his breath
			// back and tries once more. Without this, three quick falls left a man lying down for good while
			// his leg could still carry him.
			else if (DownTime > 20.0f)
			{
				RecentFalls = 2;
				bGaveUpStanding = false;
				UE_LOG(LogImpact, Display, TEXT("[reaction] t=%.2f %s reprend son souffle et va reessayer"),
				       GetWorld()->GetTimeSeconds(), *GetOwner()->GetName());
			}
		}
	}
	else
	{
		DownTime = 0.0f;
		SettledTime = 0.0f;
		GroundContactTime = 0.0f;
	}

	if (!bWasRagdolling && bRagdolling)
	{
		// Reported from play: "on peut pas les pousser, t'as jamais rajoute cette fonction, ils reagissent
		// pas au joueur comme dans la video". Right on both counts. The engine's CharacterMesh profile
		// answers ECR_Ignore on the Pawn channel, so a player walks straight through a body on the floor -
		// no contact, so nothing for the movement component's push to act on. It was deliberate and it was
		// only half right: it is needed upright, where the capsule is live, and wrong on the ground, where
		// ALS has already switched the capsule off (AlsCharacter_Actions.cpp:788) and there is no capsule
		// left to shove. So a ragdoll answers blocking, and the push comes free from the movement
		// component's own physics interaction. -ImpactCorpsTraversables puts it back the way it was.
		static const bool bSolid = !FParse::Param(FCommandLine::Get(), TEXT("ImpactCorpsTraversables"));
		if (bSolid)
		{
			Character->GetMesh()->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
			// And not something to stand on. A walker meeting a body on the floor stepped up onto it and stood there, the body
			// became the floor under his feet, and when it slid he was thrown with it: measured on the push bench, the player 31 m
			// away one second later and still flying at 20 m/s. A body is pushed, not climbed.
			Character->GetMesh()->CanCharacterStepUpOn = ECB_No;
			Character->GetMesh()->SetWalkableSlopeOverride(FWalkableSlopeOverride(WalkableSlope_Unwalkable, 0.0f));
		}
	}

	if ((bWasRagdolling || bWasLying) && !bRagdolling && !bLying)
	{
		// Back on the feet: ALS turned physics off, give the bodies their collision and muscles back.
		PrepareMesh();
		ResetBones();
		StopGroundSlot(0.2f);
		PhysicalAnimation->SetStrengthMultiplyer(1.0f);
		bBaseMeshStored = false;
	}
	bWasRagdolling = bRagdolling;
	bWasLying = bLying;
}

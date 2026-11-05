#include "ImpactGangAttitudeComponent.h"

#include "AIController.h"
#include "AISystem.h"
#include "AlsAnimationInstanceProxy.h"
#include "AlsCharacter.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimNodeBase.h"
#include "Animation/AnimSequence.h"
#include "Animation/AttributesRuntime.h"
#include "BonePose.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/Pawn.h"
#include "Impact.h"
#include "ImpactNpcShooterComponent.h"
#include "ImpactPlayerHealthComponent.h"
#include "ImpactReactionComponent.h"
#include "ImpactSoldierBrainComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "TwoBoneIK.h"
#include "UObject/UObjectIterator.h"
#include "Utility/AlsGameplayTags.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(ImpactGangAttitudeComponent)

namespace
{
	// Which parts of the body a take is laid on.
	enum : uint8
	{
		MaskPelvis = 1 << 0,
		MaskLegs = 1 << 1,
		MaskSpine = 1 << 2,
		MaskHead = 1 << 3,
		MaskArmLeft = 1 << 4,
		MaskArmRight = 1 << 5,
		// The fingers of the gun hand: always ALS's grip, whatever a take does with the arm.
		MaskGripRight = 1 << 6,
		MaskArms = MaskArmLeft | MaskArmRight,
		MaskUpper = MaskSpine | MaskHead | MaskArms,
		MaskFull = MaskUpper | MaskPelvis | MaskLegs,
	};

	struct FGangClip
	{
		const UAnimSequence* Sequence = nullptr;
		float Time = 0.0f;
		float Weight = 0.0f;
		uint8 Mask = 0;
		bool bLoop = true;
	};

	// The moves made in code while their Mixamo takes are missing: the arms are laid by IK on targets around the chest.
	enum EProcMove : uint8
	{
		ProcNone,
		ProcSpread,    // arms wide, "what?!", chin up
		ProcThump,     // the free fist beating the chest
		ProcPoint,     // the free arm pointing at the player
		ProcCelebrate, // the gun in the air, the other fist pumping
		ProcBeckon,    // "come on": the free hand drawing him in
		ProcTalk       // talking with the free hand
	};

	// The bones the procedural part works on (UE4 mannequin skeleton of ALS).
	enum EKey : uint8
	{
		KRoot, KPelvis, KSpine1, KSpine2, KSpine3, KNeck, KHead,
		KClavicleL, KUpperArmL, KLowerArmL, KHandL,
		KClavicleR, KUpperArmR, KLowerArmR, KHandR, KTwistR,
		KFootL, KFootR, KBallL,
		KCount
	};

	const TCHAR* const KeyNames[KCount] = {
		TEXT("root"), TEXT("pelvis"), TEXT("spine_01"), TEXT("spine_02"), TEXT("spine_03"), TEXT("neck_01"), TEXT("head"),
		TEXT("clavicle_l"), TEXT("upperarm_l"), TEXT("lowerarm_l"), TEXT("hand_l"),
		TEXT("clavicle_r"), TEXT("upperarm_r"), TEXT("lowerarm_r"), TEXT("hand_r"), TEXT("lowerarm_twist_01_r"),
		TEXT("foot_l"), TEXT("foot_r"), TEXT("ball_l"),
	};
}

struct FImpactGangPayload final : FAlsPoseEditPayload
{
	// Written on the game thread before each update (UImpactGangAttitudeComponent::FillPayload).
	float Master = 0.0f;
	TArray<FGangClip, TInlineAllocator<4>> Clips;
	bool bGun = false;
	FVector BoreInHand = FVector::ZeroVector;   // the rear of the bore, in hand_r space
	FVector MuzzleInHand = FVector::ZeroVector;
	float Cant = 0.0f;         // degrees the gun is turned on its side, its top towards his left
	float Recoil = 0.0f;       // 0..1, the kick of the last round
	float AimWeight = 0.0f;    // how much the barrel is brought back onto AimTarget
	FVector AimTarget = FVector::ZeroVector;
	float KeepLeftHand = 0.0f; // the support hand kept where ALS put it on the weapon
	float LeanBack = 0.0f;     // degrees, chest out
	float HeadTilt = 0.0f;     // degrees, towards the gun shoulder
	float Swagger = 0.0f;      // 0..1
	uint8 FreeArms = MaskArms; // the arms the layer may move (not one going to a wound)

	// Kept by the animation thread from one frame to the next.
	uint16 BonesSerial = 0;
	int32 BonesCount = -1;
	TArray<uint8> Regions;
	FCompactPoseBoneIndex Keys[KCount] = {
		FCompactPoseBoneIndex(INDEX_NONE), FCompactPoseBoneIndex(INDEX_NONE), FCompactPoseBoneIndex(INDEX_NONE),
		FCompactPoseBoneIndex(INDEX_NONE), FCompactPoseBoneIndex(INDEX_NONE), FCompactPoseBoneIndex(INDEX_NONE),
		FCompactPoseBoneIndex(INDEX_NONE), FCompactPoseBoneIndex(INDEX_NONE), FCompactPoseBoneIndex(INDEX_NONE),
		FCompactPoseBoneIndex(INDEX_NONE), FCompactPoseBoneIndex(INDEX_NONE), FCompactPoseBoneIndex(INDEX_NONE),
		FCompactPoseBoneIndex(INDEX_NONE), FCompactPoseBoneIndex(INDEX_NONE), FCompactPoseBoneIndex(INDEX_NONE),
		FCompactPoseBoneIndex(INDEX_NONE), FCompactPoseBoneIndex(INDEX_NONE), FCompactPoseBoneIndex(INDEX_NONE),
		FCompactPoseBoneIndex(INDEX_NONE)};
	float FeetAmplitude = 20.0f;
	float GunUp = 0.0f;        // 0..1, running with a handgun: the pistol up beside the head
	uint8 ProcMove = 0;        // a move made in code when there is no take for it (see EProcMove)
	float ProcTime = 0.0f;
	float ProcWeight = 0.0f;

	// Read back by the game thread at the next update.
	float AimError = -1.0f;
	// Checks of the skeleton's frame, centimetres: the right shoulder on MeshRight, the toes on MeshForward (both must be > 0).
	float RightCheck = 0.0f;
	float ForwardCheck = 0.0f;
	// The animated pose as it left the graph - what the physics is pulled towards - for the game thread to compare with the
	// pose the physics actually gave ([corps-mou]): upper arms (shoulder to elbow), head and chest axes, mesh space.
	FVector AnimArm[2] = {FVector::ZeroVector, FVector::ZeroVector};
	FVector AnimHead = FVector::ZeroVector;
	FVector AnimChest = FVector::ZeroVector;
	bool bAnimValid = false;
	TWeakObjectPtr<UImpactGangAttitudeComponent> Owner;
};

namespace
{
	// Component-space transforms of a local pose, parents before children (the compact pose is ordered that way).
	void BuildComponentSpace(const FCompactPose& Pose, TArray<FTransform>& Out)
	{
		Out.SetNumUninitialized(Pose.GetNumBones(), EAllowShrinking::No);
		for (const FCompactPoseBoneIndex Bone : Pose.ForEachBoneIndex())
		{
			const FCompactPoseBoneIndex Parent = Pose.GetParentBoneIndex(Bone);
			Out[Bone.GetInt()] = Parent.IsValid() ? Pose[Bone] * Out[Parent.GetInt()] : Pose[Bone];
		}
	}

	// Turns a bone about its own origin by a rotation given in component space; its children follow.
	void RotateBone(FCompactPose& Pose, TArray<FTransform>& Space, const FCompactPoseBoneIndex Bone, const FQuat& Delta)
	{
		if (!Bone.IsValid() || Delta.Equals(FQuat::Identity, 1.e-6f))
		{
			return;
		}
		const FQuat Wanted = (Delta * Space[Bone.GetInt()].GetRotation()).GetNormalized();
		const FCompactPoseBoneIndex Parent = Pose.GetParentBoneIndex(Bone);
		const FQuat ParentRotation = Parent.IsValid() ? Space[Parent.GetInt()].GetRotation() : FQuat::Identity;
		Pose[Bone].SetRotation((ParentRotation.Inverse() * Wanted).GetNormalized());
		BuildComponentSpace(Pose, Space);
	}

	// The rotation that takes From (unit) Degrees towards Toward, in the plane of the two.
	FQuat Turn(const FVector& From, const FVector& Toward, const float Degrees)
	{
		const FVector Side = (Toward - From * FVector::DotProduct(Toward, From)).GetSafeNormal();
		if (Side.IsNearlyZero() || FMath::IsNearlyZero(Degrees))
		{
			return FQuat::Identity;
		}
		const float Radians = FMath::DegreesToRadians(Degrees);
		return FQuat::FindBetweenNormals(From, (From * FMath::Cos(Radians) + Side * FMath::Sin(Radians)).GetSafeNormal());
	}

	FQuat Part(const FQuat& Rotation, const float Fraction)
	{
		return FQuat::Slerp(FQuat::Identity, Rotation, FMath::Clamp(Fraction, 0.0f, 1.0f));
	}

	// The skeleton's own frame: the ALS mannequin faces +Y in its mesh space, up is +Z.
	const FVector MeshForward(0.0f, 1.0f, 0.0f);
	const FVector MeshUp(0.0f, 0.0f, 1.0f);
	const FVector MeshRight = FVector::CrossProduct(MeshUp, MeshForward);

	void CacheBones(FImpactGangPayload& Payload, const FCompactPose& Pose)
	{
		const FBoneContainer& Bones = Pose.GetBoneContainer();
		if (Payload.BonesSerial == Bones.GetSerialNumber() && Payload.BonesCount == Pose.GetNumBones())
		{
			return;
		}
		Payload.BonesSerial = Bones.GetSerialNumber();
		Payload.BonesCount = Pose.GetNumBones();
		for (int32 Key = 0; Key < KCount; ++Key)
		{
			const int32 MeshIndex = Bones.GetPoseBoneIndexForBoneName(FName(KeyNames[Key]));
			Payload.Keys[Key] = MeshIndex == INDEX_NONE ? FCompactPoseBoneIndex(INDEX_NONE) : Bones.MakeCompactPoseIndex(FMeshPoseBoneIndex(MeshIndex));
		}
		// Each bone belongs to the part of the body its chain starts from; the IK and virtual bones belong to none, they are ALS's
		// (the gun hangs on "VB hand_r_to_ik_hand_gun": a take that knows nothing of it would throw the weapon two metres away).
		const FReferenceSkeleton& Reference = Bones.GetReferenceSkeleton();
		Payload.Regions.SetNumZeroed(Pose.GetNumBones());
		for (const FCompactPoseBoneIndex Bone : Pose.ForEachBoneIndex())
		{
			const FName Name = Reference.GetBoneName(Bones.MakeMeshPoseIndex(Bone).GetInt());
			const FString Text = Name.ToString();
			const FCompactPoseBoneIndex Parent = Pose.GetParentBoneIndex(Bone);
			uint8 Region = Parent.IsValid() ? Payload.Regions[Parent.GetInt()] : 0;
			if (Text.StartsWith(TEXT("VB ")) || Text.StartsWith(TEXT("ik_")))
			{
				Region = 0;
			}
			else if (Name == TEXT("pelvis"))
			{
				Region = MaskPelvis;
			}
			else if (Name == TEXT("spine_01"))
			{
				Region = MaskSpine;
			}
			else if (Name == TEXT("neck_01"))
			{
				Region = MaskHead;
			}
			else if (Name == TEXT("clavicle_l"))
			{
				Region = MaskArmLeft;
			}
			else if (Name == TEXT("clavicle_r"))
			{
				Region = MaskArmRight;
			}
			else if (Name == TEXT("thigh_l") || Name == TEXT("thigh_r"))
			{
				Region = MaskLegs;
			}
			else if (Parent.IsValid() && Parent == Payload.Keys[KHandR])
			{
				Region = MaskGripRight;
			}
			Payload.Regions[Bone.GetInt()] = Region;
		}
	}

	// Lays the arm (upper, lower, hand) so the hand reaches Target, the elbow staying on the side it was.
	void ReachWithArm(FCompactPose& Pose, TArray<FTransform>& Space, const FCompactPoseBoneIndex Upper, const FCompactPoseBoneIndex Lower,
	                  const FCompactPoseBoneIndex Hand, const FTransform& Target, const float Alpha)
	{
		if (!Upper.IsValid() || !Lower.IsValid() || !Hand.IsValid() || Alpha <= 0.0f)
		{
			return;
		}
		const FVector A = Space[Upper.GetInt()].GetLocation();
		const FVector B = Space[Lower.GetInt()].GetLocation();
		const FVector C = Space[Hand.GetInt()].GetLocation();
		const FVector Goal = FMath::Lerp(C, Target.GetLocation(), Alpha);
		const FVector Pole = B + (B - (A + C) * 0.5f).GetSafeNormal() * 30.0f;
		FVector Joint, End;
		AnimationCore::SolveTwoBoneIK(A, B, C, Pole, Goal, Joint, End, false, 1.0, 1.0);
		RotateBone(Pose, Space, Upper, FQuat::FindBetweenNormals((B - A).GetSafeNormal(), (Joint - A).GetSafeNormal()));
		const FVector B2 = Space[Lower.GetInt()].GetLocation();
		const FVector C2 = Space[Hand.GetInt()].GetLocation();
		RotateBone(Pose, Space, Lower, FQuat::FindBetweenNormals((C2 - B2).GetSafeNormal(), (End - B2).GetSafeNormal()));
		const FQuat Current = Space[Hand.GetInt()].GetRotation();
		RotateBone(Pose, Space, Hand, Part(Target.GetRotation() * Current.Inverse(), Alpha));
	}

	class FGangPoseEditor final : public IAlsPoseEditor
	{
	public:
		virtual void PreUpdate(UAnimInstance& Instance, TSharedPtr<FAlsPoseEditPayload>& Payload) override
		{
			const AActor* Owner = Instance.GetOwningActor();
			UImpactGangAttitudeComponent* Attitude = Owner != nullptr ? Owner->FindComponentByClass<UImpactGangAttitudeComponent>() : nullptr;
			if (Attitude == nullptr)
			{
				Payload.Reset();
				return;
			}
			if (!Payload.IsValid())
			{
				Payload = MakeShared<FImpactGangPayload>();
			}
			Attitude->FillPayload(static_cast<FImpactGangPayload&>(*Payload));
		}

		virtual void Edit(const FAnimInstanceProxy& Proxy, FAlsPoseEditPayload& Base, FPoseContext& Output) override
		{
			FImpactGangPayload& P = static_cast<FImpactGangPayload&>(Base);
			P.bAnimValid = false;
			if (P.Master <= KINDA_SMALL_NUMBER || !Output.Pose.IsValid())
			{
				P.AimError = -1.0f;
				return;
			}
			FMemMark Mark(FMemStack::Get());
			FCompactPose& Pose = Output.Pose;
			CacheBones(P, Pose);
			for (int32 Key = KRoot; Key <= KHandR; ++Key)
			{
				if (Key != KTwistR && !P.Keys[Key].IsValid())
				{
					return;
				}
			}
			const auto Key = [&P](const EKey Which) { return P.Keys[Which]; };
			TArray<FTransform> Space;
			BuildComponentSpace(Pose, Space);
			const auto At = [&Space](const FCompactPoseBoneIndex Bone) -> const FTransform& { return Space[Bone.GetInt()]; };

			// Before anything moves: how the support hand holds the weapon, relative to the gun hand.
			const FTransform LeftInRight = At(Key(KHandL)).GetRelativeTransform(At(Key(KHandR)));
			P.RightCheck = FVector::DotProduct(At(Key(KClavicleR)).GetLocation() - At(Key(KClavicleL)).GetLocation(), MeshRight);
			P.ForwardCheck = Key(KBallL).IsValid() ? FVector::DotProduct(At(Key(KBallL)).GetLocation() - At(Key(KFootL)).GetLocation(), MeshForward) : 0.0f;

			// 1. The attitude takes, laid on the parts of the body they own. The spine is matched in mesh space so the chest faces
			//    where it faced in the take whatever ALS's hips are doing (they twist with each running step).
			if (P.Clips.Num() > 0)
			{
				FCompactPose Take;
				Take.SetBoneContainer(&Pose.GetBoneContainer());
				FBlendedCurve TakeCurve;
				TakeCurve.InitFrom(Pose.GetBoneContainer());
				UE::Anim::FStackAttributeContainer TakeAttributes;
				for (const FGangClip& Clip : P.Clips)
				{
					const float Weight = Clip.Weight * P.Master;
					const uint8 Mask = Clip.Mask & (MaskSpine | MaskHead | MaskPelvis | MaskLegs | P.FreeArms);
					if (Clip.Sequence == nullptr || Weight <= KINDA_SMALL_NUMBER || Mask == 0)
					{
						continue;
					}
					FAnimationPoseData TakeData(Take, TakeCurve, TakeAttributes);
					Clip.Sequence->GetAnimationPose(TakeData, FAnimExtractContext(double(Clip.Time), false, {}, Clip.bLoop));
					const FTransform PelvisInTake = Take[Key(KPelvis)] * Take[Key(KRoot)];
					const FTransform PelvisNow = Pose[Key(KPelvis)] * Pose[Key(KRoot)];
					for (const FCompactPoseBoneIndex Bone : Pose.ForEachBoneIndex())
					{
						if ((P.Regions[Bone.GetInt()] & Mask) == 0)
						{
							continue;
						}
						FTransform Wanted = Take[Bone];
						if (Bone == Key(KSpine1) && (Mask & MaskPelvis) == 0)
						{
							const FQuat ChestInMesh = (Take[Bone] * PelvisInTake).GetRotation();
							Wanted.SetRotation((PelvisNow.GetRotation().Inverse() * ChestInMesh).GetNormalized());
						}
						const FTransform Current = Pose[Bone];
						Pose[Bone].Blend(Current, Wanted, Weight);
					}
				}
				BuildComponentSpace(Pose, Space);
			}

			// 2. The walk: shoulders rolling against the legs, a nod on each footfall, the arms held wide. In step with ALS's own
			//    feet: how far the left foot is ahead of the right, in mesh space, swings like a sine over a stride.
			if (P.Swagger > 0.0f && Key(KFootL).IsValid() && Key(KFootR).IsValid())
			{
				const float Ahead = FVector::DotProduct(At(Key(KFootL)).GetLocation() - At(Key(KFootR)).GetLocation(), MeshForward);
				P.FeetAmplitude = FMath::Max(FMath::Max(P.FeetAmplitude * 0.995f, FMath::Abs(Ahead)), 12.0f);
				const float Step = FMath::Clamp(Ahead / P.FeetAmplitude, -1.0f, 1.0f);
				const float S = P.Swagger * P.Master;
				RotateBone(Pose, Space, Key(KSpine3), Turn(MeshForward, MeshRight, -9.0f * Step * S));
				RotateBone(Pose, Space, Key(KSpine2), Turn(MeshUp, MeshRight, 4.0f * Step * S));
				RotateBone(Pose, Space, Key(KHead), Turn(MeshForward, -MeshUp, 7.0f * Step * Step * S));
				if (P.FreeArms & MaskArmLeft)
				{
					RotateBone(Pose, Space, Key(KUpperArmL), Turn(-MeshUp, -MeshRight, 9.0f * S));
				}
				if (P.FreeArms & MaskArmRight)
				{
					RotateBone(Pose, Space, Key(KUpperArmR), Turn(-MeshUp, MeshRight, 9.0f * S));
				}
			}

			// 3. Chest out.
			if (!FMath::IsNearlyZero(P.LeanBack))
			{
				const float Lean = P.LeanBack * P.Master;
				RotateBone(Pose, Space, Key(KSpine2), Turn(MeshForward, MeshUp, Lean * 0.6f));
				RotateBone(Pose, Space, Key(KSpine3), Turn(MeshForward, MeshUp, Lean * 0.4f));
			}

			const bool bGunArm = P.bGun && (P.FreeArms & MaskArmRight) != 0;
			// The bore: from the rear of the barrel to the muzzle. Not from the weapon's origin, which is the grip and sits under the
			// bore - that line pointed 9 deg above the bore on the rifle and about 17 on the pistol (review of 29 October), and putting
			// it on the player left the real barrel that far off him.
			const auto Rear = [&]() { return At(Key(KHandR)).TransformPosition(P.BoreInHand); };
			const auto Muzzle = [&]() { return At(Key(KHandR)).TransformPosition(P.MuzzleInHand); };

			// 3b. Running with a handgun: the arm bent, the pistol up beside the head and pointing ahead and up - ALS has no running pose
			//     for handguns, and its aiming arm stayed frozen straight out in front (seen on the bench photos of 29 October).
			if (bGunArm && P.GunUp > 0.0f)
			{
				const FVector Shoulder = At(Key(KUpperArmR)).GetLocation();
				const FVector Barrel = (Muzzle() - Rear()).GetSafeNormal();
				const FVector WantedBarrel = (MeshUp * 0.85f + MeshForward * 0.5f).GetSafeNormal();
				const FTransform Goal(FQuat::FindBetweenNormals(Barrel, WantedBarrel) * At(Key(KHandR)).GetRotation(),
				                      Shoulder + MeshForward * 24.0f + MeshUp * 6.0f - MeshRight * 8.0f);
				ReachWithArm(Pose, Space, Key(KUpperArmR), Key(KLowerArmR), Key(KHandR), Goal, P.GunUp * P.Master);
			}

			// The top of the weapon as ALS holds it (upright), kept in the hand's frame so it turns with the hand from here on.
			FVector TopInHand = FVector::UpVector;
			if (bGunArm)
			{
				const FVector Barrel = (Muzzle() - Rear()).GetSafeNormal();
				TopInHand = At(Key(KHandR)).InverseTransformVectorNoScale((MeshUp - Barrel * FVector::DotProduct(MeshUp, Barrel)).GetSafeNormal());
			}

			// 4. The gun on its side: the hand rolls about the bore, the top of the weapon towards his left. Half the roll goes into
			//    the forearm's twist bone, about the forearm's own axis, so the skin turns along the forearm and not at the wrist.
			if (bGunArm && P.Cant > 0.0f)
			{
				const FVector Barrel = (Muzzle() - Rear()).GetSafeNormal();
				const FVector Top = At(Key(KHandR)).TransformVectorNoScale(TopInHand);
				const FVector Left = (-MeshRight - Barrel * FVector::DotProduct(-MeshRight, Barrel)).GetSafeNormal();
				const FQuat Roll = Turn(Top, Left, P.Cant * P.Master);
				const FVector Forearm = (At(Key(KHandR)).GetLocation() - At(Key(KLowerArmR)).GetLocation()).GetSafeNormal();
				RotateBone(Pose, Space, Key(KHandR), Roll);
				if (Key(KTwistR).IsValid() && !Forearm.IsNearlyZero())
				{
					FVector Axis;
					float Angle = 0.0f;
					Roll.ToAxisAndAngle(Axis, Angle);
					const float Sign = FVector::DotProduct(Axis, Forearm) >= 0.0f ? 1.0f : -1.0f;
					RotateBone(Pose, Space, Key(KTwistR), FQuat(Forearm, Sign * Angle * 0.5f));
				}
			}

			// 5. Back on target: whatever the pose did to the weapon, the bore is brought onto the player, a little by the chest and
			//    mostly by the gun arm. Three passes, since turning the chest also moves the muzzle.
			if (bGunArm && P.AimWeight > 0.0f)
			{
				const FVector Target = Proxy.GetComponentTransform().InverseTransformPosition(P.AimTarget);
				const float Weight = P.AimWeight * P.Master;
				for (int32 Pass = 0; Pass < 3; ++Pass)
				{
					const FVector Barrel = (Muzzle() - Rear()).GetSafeNormal();
					const FVector Wanted = (Target - Muzzle()).GetSafeNormal();
					FQuat Delta = FQuat::FindBetweenNormals(Barrel, Wanted);
					const float Angle = FMath::RadiansToDegrees(Delta.GetAngle());
					if (Angle < 0.2f)
					{
						break;
					}
					Delta = Part(Delta, Weight * FMath::Min(1.0f, 45.0f / Angle));
					RotateBone(Pose, Space, Key(KSpine2), Part(Delta, 0.2f));
					RotateBone(Pose, Space, Key(KSpine3), Part(Delta, 0.25f));
					RotateBone(Pose, Space, Key(KUpperArmR), Part(Delta, 0.55f));
				}
				P.AimError = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct((Muzzle() - Rear()).GetSafeNormal(),
				                                                                                  (Target - Muzzle()).GetSafeNormal()), -1.0f, 1.0f)));
			}
			else
			{
				P.AimError = -1.0f;
			}

			// 6. The kick of a round, laid on after the aim (before it, the aim took it straight back out - review of 29 October): the
			//    forearm flips up along the top of the gun (sideways when it is on its side), the shoulder and the chest give a little.
			if (bGunArm && P.Recoil > 0.0f)
			{
				const FVector Barrel = (Muzzle() - Rear()).GetSafeNormal();
				const FVector GunTop = At(Key(KHandR)).TransformVectorNoScale(TopInHand);
				const FVector Lift = (GunTop - Barrel * FVector::DotProduct(GunTop, Barrel)).GetSafeNormal();
				const float Kick = P.Recoil * P.Master;
				RotateBone(Pose, Space, Key(KLowerArmR), Turn(Barrel, Lift, 16.0f * Kick));
				RotateBone(Pose, Space, Key(KUpperArmR), Turn(Barrel, Lift, 5.0f * Kick));
				RotateBone(Pose, Space, Key(KSpine3), Turn(MeshForward, MeshUp, 3.0f * Kick));
			}

			// 6b. A move made in code. Its targets are around the chest in mesh space, so they follow him wherever he faces.
			if (P.ProcMove != ProcNone && P.ProcWeight > 0.0f)
			{
				const float W = P.ProcWeight * P.Master;
				const float T = P.ProcTime;
				const bool bLeft = (P.FreeArms & MaskArmLeft) != 0;
				const bool bRight = (P.FreeArms & MaskArmRight) != 0;
				const FVector Chest = At(Key(KSpine3)).GetLocation();
				const FVector ShoulderL = At(Key(KUpperArmL)).GetLocation();
				const FVector ShoulderR = At(Key(KUpperArmR)).GetLocation();
				const FVector Toward = (Proxy.GetComponentTransform().InverseTransformPosition(P.AimTarget) - ShoulderL).GetSafeNormal();
				const float Beat = 0.5f + 0.5f * FMath::Sin(T * UE_TWO_PI * 2.0f);
				const auto LeftTo = [&](const FVector& Where)
				{
					if (bLeft)
					{
						ReachWithArm(Pose, Space, Key(KUpperArmL), Key(KLowerArmL), Key(KHandL), FTransform(At(Key(KHandL)).GetRotation(), Where), W);
					}
				};
				const auto RightTo = [&](const FVector& Where, const FQuat& Turned)
				{
					if (bRight)
					{
						ReachWithArm(Pose, Space, Key(KUpperArmR), Key(KLowerArmR), Key(KHandR), FTransform(Turned * At(Key(KHandR)).GetRotation(), Where), W);
					}
				};
				switch (P.ProcMove)
				{
				case ProcSpread:
					LeftTo(Chest - MeshRight * 58.0f + MeshForward * 18.0f - MeshUp * (8.0f - 6.0f * Beat));
					RightTo(Chest + MeshRight * 58.0f + MeshForward * 18.0f - MeshUp * (8.0f - 6.0f * Beat), FQuat::Identity);
					RotateBone(Pose, Space, Key(KHead), Turn(MeshForward, MeshUp, 12.0f * W));
					break;
				case ProcThump:
				{
					const float In = 0.5f + 0.5f * FMath::Sin(T * UE_TWO_PI * 3.0f);
					LeftTo(FMath::Lerp(Chest + MeshForward * 38.0f - MeshRight * 14.0f + MeshUp * 4.0f, Chest + MeshForward * 14.0f - MeshRight * 6.0f, In));
					RotateBone(Pose, Space, Key(KHead), Turn(MeshForward, MeshUp, 8.0f * W));
					break;
				}
				case ProcPoint:
					LeftTo(ShoulderL + Toward * 62.0f);
					RotateBone(Pose, Space, Key(KSpine3), Turn(MeshForward, MeshUp, -5.0f * W));
					break;
				case ProcCelebrate:
					if (bGunArm)
					{
						const FVector Barrel = (Muzzle() - Rear()).GetSafeNormal();
						RightTo(ShoulderR + MeshUp * 52.0f + MeshForward * 6.0f, FQuat::FindBetweenNormals(Barrel, MeshUp));
					}
					LeftTo(ShoulderL + MeshUp * (16.0f + 30.0f * Beat) + MeshForward * 12.0f);
					RotateBone(Pose, Space, Key(KHead), Turn(MeshForward, MeshUp, 14.0f * W));
					break;
				case ProcBeckon:
				{
					const float Flick = 0.5f + 0.5f * FMath::Sin(T * UE_TWO_PI * 2.5f);
					LeftTo(Chest + MeshForward * (26.0f + 20.0f * Flick) - MeshRight * 14.0f + MeshUp * 2.0f);
					RotateBone(Pose, Space, Key(KHead), Turn(MeshUp, MeshRight, -8.0f * W));
					break;
				}
				case ProcTalk:
					LeftTo(Chest + MeshForward * 30.0f - MeshRight * 10.0f - MeshUp * 6.0f
					       + (MeshRight * FMath::Cos(T * 5.0f) + MeshUp * FMath::Sin(T * 5.0f)) * 7.0f);
					break;
				default:
					break;
				}
			}

			// 7. The support hand back where it held the weapon (canting and kicking moved the weapon away from it).
			if (P.KeepLeftHand > 0.0f && (P.FreeArms & MaskArmLeft) != 0)
			{
				ReachWithArm(Pose, Space, Key(KUpperArmL), Key(KLowerArmL), Key(KHandL), LeftInRight * At(Key(KHandR)), P.KeepLeftHand * P.Master);
			}

			// 8. The head leans towards the gun.
			if (!FMath::IsNearlyZero(P.HeadTilt))
			{
				RotateBone(Pose, Space, Key(KHead), Turn(MeshUp, MeshRight, P.HeadTilt * P.Master));
			}

			P.AnimArm[0] = (At(Key(KLowerArmL)).GetLocation() - At(Key(KUpperArmL)).GetLocation()).GetSafeNormal();
			P.AnimArm[1] = (At(Key(KLowerArmR)).GetLocation() - At(Key(KUpperArmR)).GetLocation()).GetSafeNormal();
			P.AnimHead = At(Key(KHead)).GetUnitAxis(EAxis::X);
			P.AnimChest = At(Key(KSpine3)).GetUnitAxis(EAxis::X);
			P.bAnimValid = true;
		}
	};

	FGangPoseEditor GangPoseEditor;

	// The attitude takes, Mixamo motions retargeted onto ALS's skeleton (Tools/import_gang.py), found by their name so a missing
	// one only leaves a gap: <prefix>_1, _2... in /Game/Impact/Anims/Gang.
	const TCHAR* const TakeFolder = TEXT("/Game/Impact/Anims/Gang/");

	float Approach(const float Current, const float Target, const float DeltaTime, const float Speed)
	{
		return FMath::FInterpConstantTo(Current, Target, DeltaTime, Speed);
	}
}

bool UImpactGangAttitudeComponent::bBenchMovesOnly = false;

UImpactGangAttitudeComponent::UImpactGangAttitudeComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
}

bool UImpactGangAttitudeComponent::IsEnabled()
{
	static const bool bOn = FParse::Param(FCommandLine::Get(), TEXT("ImpactAttitudeGang"));
	return bOn;
}

void UImpactGangAttitudeComponent::BeginPlay()
{
	Super::BeginPlay();
	FAlsAnimationInstanceProxy::PoseEditor = &GangPoseEditor;
	Random.Initialize(static_cast<int32>(GetUniqueID() % 100000) * 7919 + 13);
	NextGesture = Random.FRandRange(2.0f, 6.0f);
	LoadTakes();
}

void UImpactGangAttitudeComponent::LoadTakes()
{
	const auto Load = [this](const TCHAR* Prefix, const uint8 Mask, TArray<FTake>& Into)
	{
		for (int32 Index = 1; Index <= 8; ++Index)
		{
			const FString Name = FString::Printf(TEXT("%s_%d"), Prefix, Index);
			UAnimSequence* Sequence = LoadObject<UAnimSequence>(nullptr, *FString::Printf(TEXT("%s%s.%s"), TakeFolder, *Name, *Name), nullptr,
			                                                    LOAD_NoWarn | LOAD_Quiet);
			if (Sequence == nullptr)
			{
				continue;
			}
			Loaded.Add(Sequence);
			FTake& Take = Into.AddDefaulted_GetRef();
			Take.Sequence = Sequence;
			Take.Mask = Mask;
			Take.Length = Sequence->GetPlayLength();
		}
	};
	// The hanging-about idle owns the whole upper body: the gun hand hangs and sways with it (the fingers keep ALS's grip).
	Load(TEXT("Gang_Repos"), MaskUpper, Idles);
	Load(TEXT("Gang_Geste"), MaskUpper, Gestures);
	Load(TEXT("Gang_Provoc"), MaskUpper, Taunts);
	Load(TEXT("Gang_Crie"), MaskUpper, Yells);
	Load(TEXT("Gang_Fete"), MaskUpper, Celebrations);
	Load(TEXT("Gang_Recharge"), MaskArms | MaskSpine, Reloads);
	// Running with the gun: the gun arm and the chest, the other arm keeps ALS's swing, in step with the legs.
	Load(TEXT("Gang_Course"), MaskArmRight | MaskSpine, GunRuns);
	static bool bListed = false;
	if (!bListed)
	{
		bListed = true;
		UE_LOG(LogImpact, Display, TEXT("[gang] prises : %d repos, %d gestes, %d provocations, %d cris, %d fetes, %d recharges, %d courses"),
		       Idles.Num(), Gestures.Num(), Taunts.Num(), Yells.Num(), Celebrations.Num(), Reloads.Num(), GunRuns.Num());
	}
	IdleIndex = Idles.Num() > 0 ? Random.RandRange(0, Idles.Num() - 1) : 0;
	IdleTime = Random.FRandRange(0.0f, 4.0f);
}

const UImpactGangAttitudeComponent::FTake* UImpactGangAttitudeComponent::PickTake(const EMove Kind)
{
	const TArray<FTake>* From = Kind == EMove::Gesture ? &Gestures
	                            : Kind == EMove::Taunt ? &Taunts
	                            : Kind == EMove::Yell ? &Yells
	                            : Kind == EMove::Celebrate ? &Celebrations
	                            : Kind == EMove::Reload ? &Reloads
	                            : nullptr;
	if (From == nullptr || From->Num() == 0)
	{
		// No yell of his own: a taunt says it as well.
		return Kind == EMove::Yell && Taunts.Num() > 0 ? &Taunts[Random.RandRange(0, Taunts.Num() - 1)] : nullptr;
	}
	return &(*From)[Random.RandRange(0, From->Num() - 1)];
}

bool UImpactGangAttitudeComponent::MoveUsesRightArm() const
{
	return ProcMove == ProcSpread || ProcMove == ProcCelebrate;
}

void UImpactGangAttitudeComponent::StartMove(const EMove Kind, const TCHAR* Why, const bool bFreeHandOnly)
{
	const FTake* Take = PickTake(Kind);
	// A take owns the whole upper body: not one while his gun arm must stay on the player.
	if (bFreeHandOnly && Take != nullptr && (Take->Mask & MaskArmRight) != 0)
	{
		Take = nullptr;
	}
	uint8 Made = ProcNone;
	if (Take == nullptr || Take->Sequence == nullptr)
	{
		// No take for it (or none he can use now): the same intention made in code.
		const uint8 Taunts3[] = {ProcSpread, ProcThump, ProcBeckon};
		Made = Kind == EMove::Taunt ? (bFreeHandOnly ? (Random.FRand() < 0.5f ? static_cast<uint8>(ProcThump) : static_cast<uint8>(ProcBeckon))
		                                             : Taunts3[Random.RandRange(0, 2)])
		       : Kind == EMove::Yell ? static_cast<uint8>(ProcPoint)
		       : Kind == EMove::Celebrate ? (Random.FRand() < 0.7f ? static_cast<uint8>(ProcCelebrate) : static_cast<uint8>(ProcSpread))
		       : Kind == EMove::Gesture ? (bFreeHandOnly || Random.FRand() < 0.6f ? static_cast<uint8>(ProcTalk) : static_cast<uint8>(ProcSpread))
		       : static_cast<uint8>(ProcNone);
		if (Made == ProcNone)
		{
			return;
		}
	}
	Move = Kind;
	MoveTake = Made == ProcNone ? Take : nullptr;
	ProcMove = Made;
	MoveLength = MoveTake != nullptr ? MoveTake->Length : Random.FRandRange(1.6f, 2.3f);
	MoveStart = 0.0f;
	// The Mixamo conversations and yells run 8 to 21 s: a few seconds out of them, so a gesture is a gesture and not a scene.
	if (MoveTake != nullptr && MoveTake->Length > 4.5f)
	{
		MoveLength = Random.FRandRange(3.0f, 4.0f);
		MoveStart = Random.FRandRange(0.15f, 0.6f) * (MoveTake->Length - MoveLength);
	}
	MoveTime = 0.0f;
	MoveWeight = 0.0f;
	bMoveCut = false;
	bMoveRightArm = MoveTake != nullptr ? (MoveTake->Mask & MaskArmRight) != 0 : MoveUsesRightArm();
	static const TCHAR* const ProcNames[] = {TEXT("-"), TEXT("bras ecartes"), TEXT("se frappe la poitrine"), TEXT("pointe du doigt"),
	                                         TEXT("arme en l'air"), TEXT("viens"), TEXT("parle avec la main")};
	UE_LOG(LogImpact, Display, TEXT("[gang] t=%.2f %s %s : %s (%.1f s)"), GetWorld()->GetTimeSeconds(), *GetOwner()->GetName(), Why,
	       MoveTake != nullptr ? *MoveTake->Sequence->GetName() : ProcNames[FMath::Min<int32>(ProcMove, UE_ARRAY_COUNT(ProcNames) - 1)], MoveLength);
}

void UImpactGangAttitudeComponent::UpdateWeaponFrame(const AAlsCharacter& Me)
{
	// The weapon is the visible skeletal mesh with a muzzle (as UImpactNpcShooterComponent finds it); its origin is the grip.
	bGun = false;
	const FGameplayTag Overlay = Me.GetOverlayMode();
	bRifle = Overlay == AlsOverlayModeTags::Rifle;
	bTwoHanded = bRifle || Overlay == AlsOverlayModeTags::PistolTwoHanded;
	if (!bRifle && Overlay != AlsOverlayModeTags::PistolOneHanded && Overlay != AlsOverlayModeTags::PistolTwoHanded)
	{
		return;
	}
	const USkeletalMeshComponent* Body = Me.GetMesh();
	TInlineComponentArray<USkeletalMeshComponent*> Meshes(&Me);
	for (const USkeletalMeshComponent* Mesh : Meshes)
	{
		if (Mesh != Body && Mesh->IsVisible() && Mesh->DoesSocketExist(TEXT("muzzle")))
		{
			const FTransform Hand = Body->GetSocketTransform(TEXT("hand_r"), RTS_World);
			GripInHand = Hand.InverseTransformPosition(Mesh->GetComponentLocation());
			MuzzleInHand = Hand.InverseTransformPosition(Mesh->GetSocketLocation(TEXT("muzzle")));
			// The rear of the bore: ALS's rifle has a "barrel" bone and its pistol a "slide", both on the line of the muzzle.
			const FName RearBone = Mesh->DoesSocketExist(TEXT("barrel")) ? FName(TEXT("barrel")) : FName(TEXT("slide"));
			BoreInHand = Mesh->DoesSocketExist(RearBone) ? Hand.InverseTransformPosition(Mesh->GetSocketLocation(RearBone)) : GripInHand;
			// A weapon that drifted off the hand (dropped, flying) is not his any more.
			bGun = GripInHand.Size() < 40.0f && (MuzzleInHand - BoreInHand).Size() > 5.0f;
			return;
		}
	}
}

void UImpactGangAttitudeComponent::TickComponent(const float DeltaTime, const ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	AAlsCharacter* Me = Cast<AAlsCharacter>(GetOwner());
	if (Me == nullptr)
	{
		return;
	}
	const UWorld* World = GetWorld();
	const UImpactReactionComponent* Reaction = Me->FindComponentByClass<UImpactReactionComponent>();
	const UImpactNpcShooterComponent* Shooter = Me->FindComponentByClass<UImpactNpcShooterComponent>();
	const UImpactSoldierBrainComponent* Soldier = Me->FindComponentByClass<UImpactSoldierBrainComponent>();
	const APawn* Player = UGameplayStatics::GetPlayerPawn(this, 0);
	UpdateWeaponFrame(*Me);

	// Only a man on his feet and in control carries himself: ALS's own actions (ragdoll, lying, getting up, mantling, rolling),
	// a stagger and a standing hit pose have the whole pose, and the layer is gone before they show.
	const bool bUpright = !Me->GetLocomotionAction().IsValid() && Me->GetLocomotionMode() == AlsLocomotionModeTags::Grounded
	                      && (Reaction == nullptr || (Reaction->HasVoluntaryControl() && !Reaction->IsStaggering()));
	const float HitPose = Reaction != nullptr ? Reaction->GetHitPoseWeight() : 0.0f;
	Master = Approach(Master, bUpright ? 1.0f - HitPose : 0.0f, DeltaTime, bUpright && HitPose <= 0.0f ? 2.5f : 12.0f);
	FreeArms = MaskArms;
	if (Reaction != nullptr)
	{
		FVector Target, Approach2;
		bool bApproached = false;
		if (Reaction->GetHandGoal(0, Target, Approach2, bApproached))
		{
			FreeArms &= ~MaskArmLeft;
		}
		if (Reaction->GetHandGoal(1, Target, Approach2, bApproached))
		{
			FreeArms &= ~MaskArmRight;
		}
	}

	const bool bAlerted = Shooter != nullptr && Shooter->IsAlerted();
	const float Speed = Me->GetVelocity().Size2D();
	const bool bCrouched = Me->GetStance() == AlsStanceTags::Crouching;
	const bool bAiming = Me->GetRotationMode() == AlsRotationModeTags::Aiming;
	const EImpactSoldierState State = Soldier != nullptr ? Soldier->GetState() : EImpactSoldierState::Idle;
	const bool bFiring = Shooter != nullptr && Shooter->IsBursting();

	// Where the gun is to point: the player's chest while he is the focus, otherwise where the man is looking.
	if (const ACharacter* Target = Cast<ACharacter>(Player))
	{
		AimTarget = Target->GetMesh()->GetSocketLocation(TEXT("spine_02"));
		if (const AAIController* AI = Cast<AAIController>(Me->GetController()); AI != nullptr && AI->GetFocusActor() != Player)
		{
			const FVector Focal = AI->GetFocalPoint();
			if (FAISystem::IsValidLocation(Focal))
			{
				AimTarget = Focal;
			}
		}
	}

	// What just happened.
	if (Shooter != nullptr)
	{
		if (Shooter->GetShotsFired() > ShotsSeen)
		{
			Recoil = 1.0f;
			SinceBurst = 0.0f;
		}
		ShotsSeen = Shooter->GetShotsFired();
	}
	Recoil = FMath::Max(0.0f, Recoil - DeltaTime * 7.0f);
	SinceBurst += DeltaTime;
	SinceHomieDown += DeltaTime;
	const bool bBurstEnded = Shooter != nullptr && Shooter->GetBurstsDone() > BurstsSeen;
	const bool bHitHim = Shooter != nullptr && Shooter->GetPlayerHits() > HitsSeen;
	if (Shooter != nullptr)
	{
		BurstsSeen = Shooter->GetBurstsDone();
		HitsSeen = Shooter->GetPlayerHits();
	}
	bHitHimThisBurst |= bHitHim;
	const UImpactPlayerHealthComponent* Health = Player != nullptr ? Player->FindComponentByClass<UImpactPlayerHealthComponent>() : nullptr;
	const bool bPlayerDown = Health != nullptr && Health->IsDead();
	const bool bReloading = Shooter != nullptr && Shooter->IsReloading();

	// A homie going down within twenty metres, looked for twice a second. Each man is remembered as he falls, wherever he falls, so
	// walking up to a body later is not a new fall (review of 29 October), and he is forgotten if he gets up.
	HomieTimer -= DeltaTime;
	bool bHomieWentDown = false;
	if (HomieTimer <= 0.0f)
	{
		HomieTimer = 0.5f;
		HomiesDown.RemoveAll([](const TWeakObjectPtr<const AActor>& Each) { return !Each.IsValid(); });
		for (const UImpactGangAttitudeComponent* Other : TObjectRange<UImpactGangAttitudeComponent>())
		{
			const AAlsCharacter* Him = Other != this && Other->GetWorld() == World ? Cast<AAlsCharacter>(Other->GetOwner()) : nullptr;
			const UImpactReactionComponent* His = Him != nullptr ? Him->FindComponentByClass<UImpactReactionComponent>() : nullptr;
			if (His == nullptr)
			{
				continue;
			}
			const bool bKnown = HomiesDown.Contains(Him);
			if (His->IsDown() && !bKnown)
			{
				HomiesDown.Add(Him);
				bHomieWentDown |= FVector::Dist(Him->GetActorLocation(), Me->GetActorLocation()) < 2000.0f;
			}
			else if (!His->IsDown() && bKnown)
			{
				HomiesDown.Remove(Him);
			}
		}
	}

	// The current move plays out. A man who has to fire lets go of a move that holds his gun arm, fading from where it is - jumping
	// its clock to the end took the weight from 0 to 0.83 in one frame (review of 29 October). A move of the free hand goes on.
	const bool bMustShoot = bFiring || (bAiming && Soldier != nullptr && Soldier->MayFire());
	if (Move != EMove::None)
	{
		MoveTime += DeltaTime;
		if (bMustShoot && bMoveRightArm && Move != EMove::Reload && Move != EMove::Celebrate)
		{
			bMoveCut = true;
		}
		const float Shape = FMath::Min(FMath::Clamp(MoveTime / 0.25f, 0.0f, 1.0f), FMath::Clamp((MoveLength - MoveTime) / 0.3f, 0.0f, 1.0f));
		MoveWeight = bMoveCut ? FMath::Max(0.0f, FMath::Min(Shape, MoveWeight - DeltaTime / 0.25f)) : Shape;
		if (MoveTime >= MoveLength || (bMoveCut && MoveWeight <= 0.0f))
		{
			Move = EMove::None;
			MoveTake = nullptr;
			ProcMove = ProcNone;
			MoveWeight = 0.0f;
			bMoveCut = false;
		}
	}

	// What comes next. Not while a round is being fired, and only standing on his own. Celebrating the player's fall keeps its place
	// against anything queued after it in the same moment (a burst ending, a homie, the alert).
	if (Pending != EMove::None)
	{
		PendingDelay -= DeltaTime;
	}
	const auto Queue = [this](const EMove Kind, const float Delay, const TCHAR* Why)
	{
		if (Pending == EMove::Celebrate && Kind != EMove::Celebrate)
		{
			return;
		}
		Pending = Kind;
		PendingDelay = Delay;
		PendingWhy = Why;
	};
	const bool bFree = Move == EMove::None && Master > 0.6f && !bFiring;
	if (bPlayerDown && !bPlayerDownSeen)
	{
		Queue(EMove::Celebrate, Random.FRandRange(0.3f, 1.4f), TEXT("joueur a terre, fete"));
	}
	bPlayerDownSeen = bPlayerDown;
	if (bBurstEnded)
	{
		if (bHitHimThisBurst && Random.FRand() < 0.55f)
		{
			Queue(EMove::Celebrate, Random.FRandRange(0.15f, 0.4f), TEXT("l'a touche, fete"));
		}
		else if (Random.FRand() < 0.3f)
		{
			Queue(EMove::Taunt, Random.FRandRange(0.2f, 0.6f), TEXT("entre deux rafales, provoque"));
		}
		bHitHimThisBurst = false;
	}
	if (bHomieWentDown && Random.FRand() < 0.6f)
	{
		Queue(EMove::Yell, Random.FRandRange(0.2f, 0.7f), TEXT("un pote tombe, crie"));
	}
	if (bAlerted && !bWasAlerted && Random.FRand() < 0.7f)
	{
		Queue(EMove::Yell, Random.FRandRange(0.1f, 0.5f), TEXT("te repere, crie"));
	}
	bWasAlerted = bAlerted;
	if (bReloading && !bWasReloading && Reloads.Num() > 0)
	{
		Queue(EMove::Reload, 0.0f, TEXT("recharge"));
	}
	bWasReloading = bReloading;
	SinceTaunt += DeltaTime;
	if (State == EImpactSoldierState::InCover && Soldier->GetUnseenTime() > 3.5f && SinceTaunt > 7.0f && Pending == EMove::None && Random.FRand() < DeltaTime * 0.3f)
	{
		Queue(EMove::Taunt, 0.0f, TEXT("a couvert, provoque"));
	}
	NextGesture -= DeltaTime;
	if (!bAlerted && Speed < 60.0f && NextGesture <= 0.0f && Pending == EMove::None)
	{
		Queue(EMove::Gesture, 0.0f, TEXT("traine, geste"));
		NextGesture = Random.FRandRange(5.0f, 10.0f);
	}
	if (bBenchMovesOnly)
	{
		Pending = EMove::None;
	}
	if (Pending != EMove::None && PendingDelay <= 0.0f && bFree)
	{
		const EMove Kind = Pending;
		Pending = EMove::None;
		// While he must shoot, only a move of the free hand: his gun arm stays on the player.
		StartMove(Kind, PendingWhy, bMustShoot && Kind != EMove::Celebrate);
		if (Kind == EMove::Taunt || Kind == EMove::Yell)
		{
			SinceTaunt = 0.0f;
		}
	}
	else if (Pending != EMove::None && PendingDelay < -3.0f)
	{
		// Too late to mean anything.
		Pending = EMove::None;
	}

	// The layers that are always there, eased in and out.
	const bool bStill = Speed < 60.0f;
	IdleWeight = Approach(IdleWeight, !bCrouched && bStill && !bAiming ? (bAlerted ? 0.6f : 1.0f) : 0.0f, DeltaTime, 2.0f);
	RunWeight = Approach(RunWeight, bAlerted && !bAiming && !bCrouched && Speed > 220.0f && GunRuns.Num() > 0 ? 1.0f : 0.0f, DeltaTime, 3.0f);
	SwaggerBlend = Approach(SwaggerBlend, !bCrouched && !bAiming && Speed > 30.0f && Speed < 320.0f ? (bAlerted ? 0.5f : 1.0f) : 0.0f, DeltaTime, 2.0f);
	GunUpBlend = Approach(GunUpBlend, bGun && !bRifle && !bAiming && Speed > 180.0f && GunRuns.Num() == 0 ? 1.0f : 0.0f, DeltaTime, 3.0f);
	AimBlend = Approach(AimBlend, bAiming && bGun ? 1.0f : 0.0f, DeltaTime, 5.0f);
	IdleTime += DeltaTime;
	if (Idles.IsValidIndex(IdleIndex) && IdleTime > Idles[IdleIndex].Length)
	{
		// A new idle now and then, so two men side by side do not breathe in step.
		IdleTime = 0.0f;
		IdleIndex = Random.RandRange(0, Idles.Num() - 1);
	}
	RunTime += DeltaTime * FMath::Clamp(Speed / 380.0f, 0.6f, 1.5f);
	LastSpeed = Speed;

	// [chute-temps]: a body is heavy - from the moment ALS's ragdoll starts, how long the hips take to reach the floor, against
	// the time a free fall from that height would take.
	const bool bRagdoll = Me->GetLocomotionAction() == AlsLocomotionActionTags::Ragdolling;
	if (bRagdoll && !bWasRagdoll)
	{
		FallStart = World->GetTimeSeconds();
		FallFloorZ = Me->GetActorLocation().Z - Me->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		FallPrevZ = Me->GetMesh()->GetSocketLocation(TEXT("pelvis")).Z;
		FallHeight = FallPrevZ - FallFloorZ;
		FallFastest = 0.0f;
		FallStartZ = FallPrevZ;
		FallRise = 0.0f;
		bRiseLogged = false;
		// Already on the floor (a ragdoll started again from lying down): no fall to time.
		if (FallHeight < 60.0f)
		{
			FallStart = -1.0f;
		}
		// What could make a body float: damping, gravity switched off, a light mass.
		if (const FBodyInstance* Hips = Me->GetMesh()->GetBodyInstance(TEXT("pelvis")); Hips != nullptr && !bFallBodyLogged)
		{
			bFallBodyLogged = true;
			const FBodyInstance* Chest = Me->GetMesh()->GetBodyInstance(TEXT("spine_03"));
			UE_LOG(LogImpact, Display, TEXT("[chute-corps] %s : bassin %.1f kg amorti %.2f/%.2f gravite %d ; torse %.1f kg amorti %.2f/%.2f gravite %d ; gravite du monde %.0f"),
			       *Me->GetName(), Hips->GetBodyMass(), Hips->LinearDamping, Hips->AngularDamping, Hips->bEnableGravity ? 1 : 0,
			       Chest != nullptr ? Chest->GetBodyMass() : 0.0f, Chest != nullptr ? Chest->LinearDamping : 0.0f,
			       Chest != nullptr ? Chest->AngularDamping : 0.0f, Chest != nullptr && Chest->bEnableGravity ? 1 : 0, World->GetGravityZ());
		}
	}
	bWasRagdoll = bRagdoll;
	if (FallStart >= 0.0f)
	{
		const float Z = Me->GetMesh()->GetSocketLocation(TEXT("pelvis")).Z;
		FallFastest = FMath::Max(FallFastest, (FallPrevZ - Z) / FMath::Max(DeltaTime, 1.e-3f));
		FallPrevZ = Z;
		const float Since = World->GetTimeSeconds() - FallStart;
		if (Since <= 0.4f)
		{
			FallRise = FMath::Max(FallRise, Z - FallStartZ);
		}
		else if (!bRiseLogged)
		{
			bRiseLogged = true;
			UE_LOG(LogImpact, Display, TEXT("[chute-saut] t=%.2f %s : le bassin remonte de %.1f cm dans les 0,4 s apres le debut du ragdoll"),
			       World->GetTimeSeconds(), *Me->GetName(), FallRise);
		}
		const bool bLanded = Z - FallFloorZ < 35.0f;
		if (bLanded || Since > 4.0f)
		{
			if (!bRiseLogged)
			{
				bRiseLogged = true;
				UE_LOG(LogImpact, Display, TEXT("[chute-saut] t=%.2f %s : le bassin remonte de %.1f cm dans les 0,4 s apres le debut du ragdoll"),
				       World->GetTimeSeconds(), *Me->GetName(), FallRise);
			}
			const float FreeFall = FMath::Sqrt(2.0f * FMath::Max(FallHeight - 35.0f, 1.0f) / 980.0f);
			UE_LOG(LogImpact, Display, TEXT("[chute-temps] t=%.2f %s : bassin %s en %.2f s (de %.0f cm ; chute libre %.2f s), descente la plus rapide %.0f cm/s"),
			       World->GetTimeSeconds(), *Me->GetName(), bLanded ? TEXT("au sol") : TEXT("PAS au sol"), Since, FallHeight, FreeFall, FallFastest);
			FallStart = -1.0f;
		}
	}

	// [arme-lachee]: reported from play, 27 October - "ils lachent leur arme et elle s'evanouit". The copy UImpactBodyComponent::
	// DropWeapon spawns is followed for five seconds: is it there, on the floor, drawn.
	const FGameplayTag Overlay = Me->GetOverlayMode();
	const bool bArmedNow = Overlay == AlsOverlayModeTags::Rifle || Overlay == AlsOverlayModeTags::PistolOneHanded || Overlay == AlsOverlayModeTags::PistolTwoHanded;
	// Not in the first second and a half of a level: the weapons are being handed out then, and a level started again after the
	// player's death changed overlays on everyone at once (false "no copy" lines).
	if (bWasArmed && !bArmedNow && World->GetTimeSeconds() > 1.5f)
	{
		AActor* Nearest = nullptr;
		float NearestDistance = 300.0f;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			const float Distance = It->ActorHasTag(TEXT("ImpactArmeLachee")) ? FVector::Dist(It->GetActorLocation(), Me->GetActorLocation()) : FLT_MAX;
			if (Distance < NearestDistance)
			{
				NearestDistance = Distance;
				Nearest = *It;
			}
		}
		DroppedWeapon = Nearest;
		DroppedTime = World->GetTimeSeconds();
		DroppedChecks = 0;
		UE_LOG(LogImpact, Display, TEXT("[arme-lachee] t=%.2f %s : %s"), World->GetTimeSeconds(), *Me->GetName(),
		       Nearest != nullptr ? TEXT("copie au sol trouvee") : TEXT("AUCUNE copie de l'arme"));
	}
	bWasArmed = bArmedNow;
	// [arme-effacee]: armed, yet his weapon mesh has no asset - the weapon is not drawn. Once per episode, saying what he was doing.
	if (bArmedNow)
	{
		bool bDrawn = false;
		TInlineComponentArray<USkeletalMeshComponent*> Meshes(Me);
		for (const USkeletalMeshComponent* Mesh : Meshes)
		{
			bDrawn |= Mesh != Me->GetMesh() && Mesh->IsVisible() && Mesh->GetSkeletalMeshAsset() != nullptr && Mesh->GetName().Contains(TEXT("Overlay"));
		}
		if (!bDrawn && !bWeaponGoneLogged)
		{
			bWeaponGoneLogged = true;
			UE_LOG(LogImpact, Display, TEXT("[arme-effacee] t=%.2f %s : arme en main mais pas dessinee (action %s)"), World->GetTimeSeconds(), *Me->GetName(),
			       *Me->GetLocomotionAction().ToString());
		}
		bWeaponGoneLogged &= !bDrawn;
	}
	{
		static const float Marks[] = {0.5f, 2.0f, 5.0f};
		const float Since = World->GetTimeSeconds() - DroppedTime;
		if (Since >= Marks[DroppedChecks])
		{
			const AActor* Weapon = DroppedWeapon.Get();
			if (Weapon == nullptr)
			{
				UE_LOG(LogImpact, Display, TEXT("[arme-lachee] t=%.2f %s : +%.1f s, l'arme n'existe plus"), World->GetTimeSeconds(), *Me->GetName(), Since);
			}
			else
			{
				FHitResult Floor;
				FCollisionQueryParams Params(SCENE_QUERY_STAT(ImpactDroppedFloor), false, Weapon);
				const FVector Here = Weapon->GetActorLocation();
				const bool bFloor = World->LineTraceSingleByObjectType(Floor, Here + FVector(0.0f, 0.0f, 50.0f), Here - FVector(0.0f, 0.0f, 3000.0f),
				                                                       FCollisionObjectQueryParams(ECC_WorldStatic), Params);
				const USkeletalMeshComponent* Visual = Weapon->FindComponentByClass<USkeletalMeshComponent>();
				UPrimitiveComponent* Root = Cast<UPrimitiveComponent>(Weapon->GetRootComponent());
				UE_LOG(LogImpact, Display, TEXT("[arme-lachee] t=%.2f %s : +%.1f s, a %s du sol, vitesse %.0f cm/s, visuel %s (%s, %.0f cm de la boite)"),
				       World->GetTimeSeconds(), *Me->GetName(), Since,
				       bFloor ? *FString::Printf(TEXT("%.0f cm"), Here.Z - Floor.ImpactPoint.Z) : TEXT("AUCUN SOL sous elle"),
				       Root != nullptr ? Root->GetPhysicsLinearVelocity().Size() : 0.0f,
				       Visual == nullptr ? TEXT("ABSENT") : Visual->IsVisible() ? TEXT("visible") : TEXT("CACHE"),
				       Visual != nullptr && Visual->WasRecentlyRendered(0.3f) ? TEXT("dessine") : TEXT("pas dessine"),
				       Visual != nullptr ? FVector::Dist(Visual->GetComponentLocation(), Here) : 0.0f);
			}
			if (++DroppedChecks >= static_cast<int32>(UE_ARRAY_COUNT(Marks)))
			{
				DroppedTime = -1.0f;
			}
		}
	}

	LogTimer -= DeltaTime;
	if (LogTimer <= 0.0f && DragCount > 0)
	{
		UE_LOG(LogImpact, Display, TEXT("[corps-mou] t=%.2f %s : en course %d images, ecart animation/physique : bras G %.0f (max %.0f), bras D %.0f (max %.0f), tete %.0f (max %.0f), torse %.0f (max %.0f) deg"),
		       World->GetTimeSeconds(), *Me->GetName(), DragCount, DragSum[0] / DragCount, DragMax[0], DragSum[1] / DragCount, DragMax[1],
		       DragSum[2] / DragCount, DragMax[2], DragSum[3] / DragCount, DragMax[3]);
		for (int32 Part = 0; Part < 4; ++Part)
		{
			DragSum[Part] = 0.0f;
			DragMax[Part] = 0.0f;
		}
		DragCount = 0;
	}
	if (LogTimer <= 0.0f)
	{
		LogTimer = 2.0f;
		UE_LOG(LogImpact, Display, TEXT("[gang] t=%.2f %s : %s, couche %.2f, repos %.2f, course %.2f, roule %.2f, vise %.2f, geste %s %.2f, canon %.1f deg, arme %s"),
		       World->GetTimeSeconds(), *Me->GetName(), *GetAttitudeText(), Master, IdleWeight, RunWeight, SwaggerBlend, AimBlend,
		       MoveTake != nullptr ? *MoveTake->Sequence->GetName() : TEXT("-"), MoveWeight, AimErrorSeen,
		       !bGun ? TEXT("aucune") : bRifle ? TEXT("fusil") : TEXT("pistolet"));
	}
}

void UImpactGangAttitudeComponent::FillPayload(FImpactGangPayload& Out)
{
	// What the animation thread measured on the last pose, before this frame's inputs replace the old ones.
	AimErrorSeen = Out.AimError;
	// How far the physics let the body drift from its animation, while he runs: the bone transforms read here are that same
	// frame's, after the physics blend.
	if (Out.bAnimValid && Master > 0.9f && LastSpeed > 250.0f)
	{
		if (const AAlsCharacter* Me = Cast<AAlsCharacter>(GetOwner()))
		{
			const USkeletalMeshComponent* Mesh = Me->GetMesh();
			const auto Bone = [Mesh](const TCHAR* Name) { return Mesh->GetSocketTransform(FName(Name), RTS_Component); };
			const FVector Final[4] = {
				(Bone(TEXT("lowerarm_l")).GetLocation() - Bone(TEXT("upperarm_l")).GetLocation()).GetSafeNormal(),
				(Bone(TEXT("lowerarm_r")).GetLocation() - Bone(TEXT("upperarm_r")).GetLocation()).GetSafeNormal(),
				Bone(TEXT("head")).GetUnitAxis(EAxis::X),
				Bone(TEXT("spine_03")).GetUnitAxis(EAxis::X)};
			const FVector Animated[4] = {Out.AnimArm[0], Out.AnimArm[1], Out.AnimHead, Out.AnimChest};
			for (int32 Part = 0; Part < 4; ++Part)
			{
				const float Angle = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct(Final[Part], Animated[Part]), -1.0f, 1.0f)));
				DragSum[Part] += Angle;
				DragMax[Part] = FMath::Max(DragMax[Part], Angle);
			}
			++DragCount;
		}
	}
	if (!bFrameLogged && !FMath::IsNearlyZero(Out.RightCheck))
	{
		bFrameLogged = true;
		UE_LOG(LogImpact, Display, TEXT("[gang] cadre du squelette : epaule droite %+.1f cm sur l'axe droit, orteils %+.1f cm devant (positifs attendus)"),
		       Out.RightCheck, Out.ForwardCheck);
	}
	Out.Owner = this;
	// -ImpactGangSansCouche (bench control): everything of the gang mode but the pose layer itself.
	static const bool bLayerOff = FParse::Param(FCommandLine::Get(), TEXT("ImpactGangSansCouche"));
	Out.Master = bLayerOff ? 0.0f : Master;
	Out.Clips.Reset();
	const float Free = 1.0f - MoveWeight;
	if (IdleWeight > 0.01f && Idles.IsValidIndex(IdleIndex))
	{
		const FTake& Take = Idles[IdleIndex];
		Out.Clips.Add({Take.Sequence, FMath::Min(IdleTime, Take.Length), IdleWeight * Free, Take.Mask, true});
	}
	if (RunWeight > 0.01f && GunRuns.IsValidIndex(RunIndex))
	{
		const FTake& Take = GunRuns[RunIndex];
		Out.Clips.Add({Take.Sequence, FMath::Fmod(RunTime, FMath::Max(Take.Length, 0.1f)), RunWeight * Free, Take.Mask, true});
	}
	if (MoveTake != nullptr && MoveWeight > 0.01f)
	{
		Out.Clips.Add({MoveTake->Sequence, FMath::Min(MoveStart + MoveTime, MoveTake->Length), MoveWeight, MoveTake->Mask, false});
	}
	// What the current move takes. The gun arm's own layers (the cant, the aim, the pistol up, the head tilt, the kick) give way only
	// to a move that takes that arm: most of the code moves use the free hand, and meanwhile the gun stays held the gang way.
	const bool bMoveRight = MoveWeight > 0.0f && (MoveTake != nullptr ? (MoveTake->Mask & MaskArmRight) != 0 : MoveUsesRightArm());
	const bool bMoveLeft = MoveWeight > 0.0f && (MoveTake != nullptr ? (MoveTake->Mask & MaskArmLeft) != 0 : ProcMove != ProcNone);
	const float GunFree = bMoveRight ? Free : 1.0f;
	const float LeftFree = bMoveLeft ? Free : 1.0f;
	// An idle take owns both arms, the support hand with them.
	const float IdleOwnsArms = Idles.IsValidIndex(IdleIndex) ? IdleWeight * Free : 0.0f;
	Out.bGun = bGun;
	Out.BoreInHand = BoreInHand;
	Out.MuzzleInHand = MuzzleInHand;
	// The pistol all the way on its side, the rifle half way: the canted hold reads at once, and from where the player stands.
	Out.Cant = AimBlend * (bRifle ? 35.0f : 80.0f) * GunFree;
	Out.Recoil = Recoil * AimBlend * GunFree;
	Out.AimWeight = AimBlend * GunFree;
	Out.AimTarget = AimTarget;
	// The support hand stays on a two-handed weapon whatever the layer does to the gun hand - the cant, the kick, the swagger's wide
	// arms - and not only while aiming (review of 29 October: walking, it came off the rifle).
	Out.KeepLeftHand = bTwoHanded ? LeftFree * (1.0f - IdleOwnsArms) : 0.0f;
	Out.LeanBack = 4.0f + 4.0f * AimBlend - 4.0f * RunWeight;
	Out.HeadTilt = AimBlend * (bRifle ? 6.0f : 12.0f) * GunFree;
	Out.Swagger = SwaggerBlend * Free;
	Out.GunUp = GunUpBlend * GunFree;
	Out.ProcMove = MoveTake == nullptr ? ProcMove : static_cast<uint8>(ProcNone);
	Out.ProcTime = MoveTime;
	Out.ProcWeight = MoveTake == nullptr ? MoveWeight : 0.0f;
	Out.FreeArms = FreeArms;
}

void UImpactGangAttitudeComponent::BenchMove(const int32 Which)
{
	const EMove Kinds[] = {EMove::None, EMove::Taunt, EMove::Taunt, EMove::Yell, EMove::Celebrate, EMove::Taunt, EMove::Gesture};
	if (Which < 1 || Which >= static_cast<int32>(UE_ARRAY_COUNT(Kinds)))
	{
		return;
	}
	const EMove Kind = Kinds[Which];
	const FTake* Take = PickTake(Kind);
	Move = Kind;
	MoveTake = Take != nullptr && Take->Sequence != nullptr ? Take : nullptr;
	ProcMove = MoveTake != nullptr ? static_cast<uint8>(ProcNone) : static_cast<uint8>(Which);
	MoveLength = MoveTake != nullptr ? FMath::Min(MoveTake->Length, 3.5f) : 2.2f;
	MoveStart = MoveTake != nullptr && MoveTake->Length > 4.5f ? 0.35f * (MoveTake->Length - MoveLength) : 0.0f;
	MoveTime = 0.0f;
	MoveWeight = 0.0f;
	bMoveCut = false;
	bMoveRightArm = MoveTake != nullptr ? (MoveTake->Mask & MaskArmRight) != 0 : MoveUsesRightArm();
	UE_LOG(LogImpact, Display, TEXT("[gang] t=%.2f %s banc : geste %d (%s)"), GetWorld()->GetTimeSeconds(), *GetOwner()->GetName(), Which,
	       MoveTake != nullptr ? *MoveTake->Sequence->GetName() : TEXT("calcule"));
}

FString UImpactGangAttitudeComponent::GetAttitudeText() const
{
	if (Master < 0.3f)
	{
		return TEXT("ALS");
	}
	switch (Move)
	{
	case EMove::Gesture: return TEXT("geste");
	case EMove::Taunt: return TEXT("provoque");
	case EMove::Yell: return TEXT("crie");
	case EMove::Celebrate: return TEXT("fete");
	case EMove::Reload: return TEXT("recharge");
	default: break;
	}
	return AimBlend > 0.5f ? TEXT("vise de cote") : RunWeight > 0.5f || GunUpBlend > 0.5f ? TEXT("court l'arme levee") : SwaggerBlend > 0.5f ? TEXT("roule des epaules")
	       : IdleWeight > 0.5f ? TEXT("traine") : TEXT("normal");
}

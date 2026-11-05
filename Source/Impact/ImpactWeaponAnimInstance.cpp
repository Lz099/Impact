#include "ImpactWeaponAnimInstance.h"

#include "Animation/AnimClassInterface.h"
#include "Animation/AnimInstanceProxy.h"
#include "Animation/AnimNode_LinkedInputPose.h"
#include "Animation/AnimNode_Root.h"
#include "Animation/AnimNodeSpaceConversions.h"
#include "BoneControllers/AnimNode_ModifyBone.h"
#include "BoneControllers/AnimNode_TwoBoneIK.h"
#include "UObject/UnrealType.h"

namespace
{
	/** Proxy-owned nodes and values only; no gameplay UObject access on workers. */
	struct FImpactLeftArmNode final : FAnimNode_Base
	{
		FAnimNode_LinkedInputPose* Input = nullptr;
		FAnimNode_ConvertLocalToComponentSpace LocalToComponent;
		FAnimNode_TwoBoneIK ArmIK;
		FAnimNode_ModifyBone HandRotation;
		FAnimNode_ConvertComponentToLocalSpace ComponentToLocal;
		FBoneReference UpperArm;
		FBoneReference LowerArm;
		float Weight = 0.0f;
		bool bValidChain = false;

		FImpactLeftArmNode()
		{
			UpperArm.BoneName = TEXT("upperarm_l");
			LowerArm.BoneName = TEXT("lowerarm_l");
			ArmIK.IKBone.BoneName = TEXT("hand_l");
			ArmIK.EffectorLocationSpace = BCS_ComponentSpace;
			ArmIK.JointTargetLocationSpace = BCS_ComponentSpace;
			ArmIK.bAllowStretching = false;
			ArmIK.bTakeRotationFromEffectorSpace = false;
			ArmIK.bMaintainEffectorRelRot = true;
			ArmIK.AlphaInputType = EAnimAlphaInputType::Float;
			ArmIK.Alpha = 0.0f;
			HandRotation.BoneToModify.BoneName = TEXT("hand_l");
			HandRotation.TranslationMode = BMM_Ignore;
			HandRotation.ScaleMode = BMM_Ignore;
			HandRotation.RotationMode = BMM_Replace;
			HandRotation.RotationSpace = BCS_ComponentSpace;
			HandRotation.AlphaInputType = EAnimAlphaInputType::Float;
			HandRotation.Alpha = 0.0f;

			ArmIK.ComponentPose.SetLinkNode(&LocalToComponent);
			HandRotation.ComponentPose.SetLinkNode(&ArmIK);
			ComponentToLocal.ComponentPose.SetLinkNode(&HandRotation);
		}

		void BindInput(FAnimNode_LinkedInputPose* InInput)
		{
			Input = InInput;
			LocalToComponent.LocalPose.SetLinkNode(Input);
		}

		virtual void Initialize_AnyThread(const FAnimationInitializeContext& Context) override
		{
			bValidChain = false;
			ComponentToLocal.Initialize_AnyThread(Context);
		}

		virtual void CacheBones_AnyThread(const FAnimationCacheBonesContext& Context) override
		{
			ComponentToLocal.CacheBones_AnyThread(Context);
			const FBoneContainer& Bones = Context.AnimInstanceProxy->GetRequiredBones();
			UpperArm.Initialize(Bones);
			LowerArm.Initialize(Bones);
			bValidChain = UpperArm.IsValidToEvaluate(Bones)
				&& LowerArm.IsValidToEvaluate(Bones) && ArmIK.IKBone.IsValidToEvaluate(Bones);
			if (bValidChain)
			{
				// TwoBoneIK follows parents; reject another hierarchy or a stripped LOD.
				bValidChain = Bones.GetParentBoneIndex(ArmIK.IKBone.GetCompactPoseIndex(Bones))
					== LowerArm.GetCompactPoseIndex(Bones)
					&& Bones.GetParentBoneIndex(LowerArm.GetCompactPoseIndex(Bones))
					== UpperArm.GetCompactPoseIndex(Bones);
			}
		}

		virtual void Update_AnyThread(const FAnimationUpdateContext& Context) override
		{
			// Update even at zero weight so all pose links retain valid traversal counters.
			ArmIK.Alpha = HandRotation.Alpha = bValidChain ? Weight : 0.0f;
			ComponentToLocal.Update_AnyThread(Context);
		}

		virtual void Evaluate_AnyThread(FPoseContext& Output) override
		{
			if (!Input)
			{
				// Configuration error, reported on the game thread during Initialize.
				Output.ResetToRefPose();
				return;
			}

			// Exact incoming bones, curves and attributes; never round-trip spaces at zero.
			Input->Evaluate_AnyThread(Output);
			if (!bValidChain || !FAnimWeight::IsRelevant(Weight))
			{
				return;
			}

			FPoseContext ArmPose(Output);
			ComponentToLocal.Evaluate_AnyThread(ArmPose);
			const FBoneContainer& Bones = Output.Pose.GetBoneContainer();
			// Preserve every other local transform exactly, including root and legs.
			// Descendants (fingers, twist bones) keep their ALS local pose and follow the arm.
			for (const FCompactPoseBoneIndex Index : {
				UpperArm.GetCompactPoseIndex(Bones), LowerArm.GetCompactPoseIndex(Bones),
				ArmIK.IKBone.GetCompactPoseIndex(Bones)})
			{
				Output.Pose[Index] = ArmPose.Pose[Index];
			}
		}

		virtual void GatherDebugData(FNodeDebugData& DebugData) override
		{
			DebugData.AddDebugItem(FString::Printf(TEXT("Impact left arm: %.3f%s"), Weight,
				bValidChain ? TEXT("") : TEXT(" (pass-through: invalid chain)")));
			ComponentToLocal.GatherDebugData(DebugData);
		}
	};
}

struct FImpactWeaponAnimInstanceProxy final : FAnimInstanceProxy
{
	explicit FImpactWeaponAnimInstanceProxy(UAnimInstance* Instance)
		: FAnimInstanceProxy(Instance)
	{
	}

	virtual void Initialize(UAnimInstance* Instance) override
	{
		check(IsInGameThread());
		UnbindRoot();
		FAnimInstanceProxy::Initialize(Instance);
		Arm.Weight = 0.0f;
		BoundRoot = nullptr;
		OriginalRootLink = nullptr;
		Arm.BindInput(nullptr);

		// DefaultLinkedInstanceInputNode and RootNode are PRIVATE in UE 5.7.
		// Use compiled public metadata, never a layout/access hack. The default
		// input cache is populated later by InitializeRootNode, so use named lookup now.
		IAnimClassInterface* AnimClass = IAnimClassInterface::GetFromClass(Instance->GetClass());
		FAnimNode_LinkedInputPose* Input = Instance->GetLinkedInputPoseNode(
			FAnimNode_LinkedInputPose::DefaultInputPoseName, FName(TEXT("AnimGraph")));
		if (AnimClass && Input)
		{
			for (const FAnimBlueprintFunction& Function : AnimClass->GetAnimBlueprintFunctions())
			{
				if (Function.Name == FName(TEXT("AnimGraph")))
				{
					FStructProperty* RootProperty = Function.OutputPoseNodeProperty;
					if (RootProperty && RootProperty->Struct == FAnimNode_Root::StaticStruct())
					{
						BoundRoot = RootProperty->ContainerPtrToValuePtr<FAnimNode_Root>(Instance);
					}
					break;
				}
			}
		}

		if (ensureMsgf(BoundRoot && Input,
			TEXT("ImpactWeaponAnimInstance requires an AnimBlueprint child with an InPose Input Pose in AnimGraph. Install its generated class, not the native class.")))
		{
			Arm.BindInput(Input);
			OriginalRootLink = BoundRoot->Result.GetLinkNode();
			BoundRoot->Result.SetLinkNode(&Arm);
			// UAnimInstance calls InitializeRootNode AFTER this method. Its normal
			// traversal initializes our chain, and normal CacheBones handles LOD changes.
			// No premature/double initialization and no separate Evaluate override needed.
		}
	}

	virtual void PreUpdate(UAnimInstance* Instance, float DeltaSeconds) override
	{
		check(IsInGameThread());
		FAnimInstanceProxy::PreUpdate(Instance, DeltaSeconds);
		const UImpactWeaponAnimInstance* WeaponInstance = CastChecked<UImpactWeaponAnimInstance>(Instance);
		// Engine fences proxy work before PreUpdate. The setter only writes the UObject
		// mailbox on the game thread; workers only read these copied values.
		Arm.ArmIK.EffectorLocation = WeaponInstance->LeftHandLocation;
		Arm.ArmIK.JointTargetLocation = WeaponInstance->LeftElbowLocation;
		Arm.HandRotation.Rotation = WeaponInstance->LeftHandRotation;
		Arm.Weight = WeaponInstance->LeftHandWeight;
		if (!ensureMsgf(Arm.Input && Instance->GetLinkedInputPoseNode() == Arm.Input,
			TEXT("Impact post-process input is not registered: expected default InPose in AnimGraph.")))
		{
			Arm.Weight = 0.0f;
		}
	}

	virtual void Uninitialize(UAnimInstance* Instance) override
	{
		UnbindRoot();
		Arm.BindInput(nullptr);
		Arm.Weight = 0.0f;
		FAnimInstanceProxy::Uninitialize(Instance);
	}

private:
	void UnbindRoot()
	{
		// Also handle repeated Initialize calls (e.g. editor reinitialization)
		// without ever retaining our own node as the original root link.
		if (BoundRoot && BoundRoot->Result.GetLinkNode() == &Arm)
		{
			BoundRoot->Result.SetLinkNode(OriginalRootLink);
		}
		BoundRoot = nullptr;
		OriginalRootLink = nullptr;
	}

	FImpactLeftArmNode Arm;
	FAnimNode_Root* BoundRoot = nullptr;
	FAnimNode_Base* OriginalRootLink = nullptr;
};

void UImpactWeaponAnimInstance::SetLeftHandTarget(FVector ComponentSpaceLocation,
	FRotator ComponentSpaceRotation, FVector ComponentSpaceElbow, float Weight)
{
	if (!ensureMsgf(IsInGameThread(), TEXT("SetLeftHandTarget must run on the game thread.")))
	{
		return;
	}

	if (!FMath::IsFinite(Weight) || ComponentSpaceLocation.ContainsNaN()
		|| ComponentSpaceRotation.ContainsNaN() || ComponentSpaceElbow.ContainsNaN())
	{
		LeftHandWeight = 0.0f;
		return;
	}

	LeftHandLocation = ComponentSpaceLocation;
	LeftHandRotation = ComponentSpaceRotation.GetNormalized();
	LeftElbowLocation = ComponentSpaceElbow;
	LeftHandWeight = FMath::Clamp(Weight, 0.0f, 1.0f);
}

FAnimInstanceProxy* UImpactWeaponAnimInstance::CreateAnimInstanceProxy()
{
	return new FImpactWeaponAnimInstanceProxy(this);
}

void UImpactWeaponAnimInstance::DestroyAnimInstanceProxy(FAnimInstanceProxy* InProxy)
{
	delete static_cast<FImpactWeaponAnimInstanceProxy*>(InProxy);
}

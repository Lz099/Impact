// Editor-only bridge authoring. No code or editor dependencies in packaged games.
#if WITH_EDITOR

#include "ImpactWeaponAnimInstance.h"
#include "AnimGraphNode_LinkedInputPose.h"
#include "AnimGraphNode_Root.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimClassInterface.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphSchema.h"
#include "Engine/SkeletalMesh.h"
#include "Factories/AnimBlueprintFactory.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

DEFINE_LOG_CATEGORY_STATIC(LogImpactArmsEditor, Log, All);

namespace ImpactArmsEditor
{
	constexpr const TCHAR* PackagePath = TEXT("/Game/Impact/Weapons/ARKit/ABP_WeaponArms");
	constexpr const TCHAR* ClassPath = TEXT("/Game/Impact/Weapons/ARKit/ABP_WeaponArms.ABP_WeaponArms_C");

	void WriteResult(bool bSuccess, const FString& Message)
	{
		const FString Directory = FPaths::ProjectSavedDir() / TEXT("WeaponKit");
		IFileManager::Get().MakeDirectory(*Directory, true);
		FFileHelper::SaveStringToFile((bSuccess ? TEXT("PASS\n") : TEXT("FAIL\n")) + Message,
			*(Directory / TEXT("ArmsBridgeResult.txt")));
		UE_LOG(LogImpactArmsEditor, Display, TEXT("Arms bridge %s: %s"),
			bSuccess ? TEXT("PASS") : TEXT("FAIL"), *Message);
	}

	void CreateBridge()
	{
		check(IsInGameThread());
		USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, TEXT("/ALS/ALS/Character/SKM_Als.SKM_Als"));
		if (!Mesh || !Mesh->GetSkeleton())
		{
			WriteResult(false, TEXT("Cannot load SKM_Als and its skeleton."));
			return;
		}

		UAnimBlueprint* Blueprint = LoadObject<UAnimBlueprint>(nullptr, PackagePath);
		bool bCreated = false;
		if (!Blueprint)
		{
			if (FPackageName::DoesPackageExist(PackagePath))
			{
				WriteResult(false, TEXT("Destination exists but is not a loadable AnimBlueprint; refusing to replace it."));
				return;
			}
			UPackage* Package = CreatePackage(PackagePath);
			UAnimBlueprintFactory* Factory = NewObject<UAnimBlueprintFactory>();
			Factory->ParentClass = UImpactWeaponAnimInstance::StaticClass();
			Factory->TargetSkeleton = Mesh->GetSkeleton();
			Factory->PreviewSkeletalMesh = Mesh;
			Blueprint = Cast<UAnimBlueprint>(static_cast<UFactory*>(Factory)->FactoryCreateNew(
				UAnimBlueprint::StaticClass(), Package, TEXT("ABP_WeaponArms"), RF_Public | RF_Standalone,
				nullptr, GWarn));
			bCreated = Blueprint != nullptr;
		}
		if (!Blueprint || Blueprint->ParentClass != UImpactWeaponAnimInstance::StaticClass()
			|| Blueprint->TargetSkeleton != Mesh->GetSkeleton())
		{
			WriteResult(false, TEXT("Bridge creation failed or existing asset has a different parent/skeleton."));
			return;
		}

		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		UEdGraph* Graph = nullptr;
		for (UEdGraph* Candidate : Graphs)
		{
			if (Candidate->GetFName() == FName(TEXT("AnimGraph"))) Graph = Candidate;
		}
		if (!Graph)
		{
			WriteResult(false, TEXT("Factory did not provide AnimGraph."));
			return;
		}
		UAnimGraphNode_Root* Root = nullptr;
		UAnimGraphNode_LinkedInputPose* Input = nullptr;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (auto* Candidate = Cast<UAnimGraphNode_Root>(Node))
			{
				if (Root) { WriteResult(false, TEXT("Multiple roots; refusing to modify graph.")); return; }
				Root = Candidate;
			}
			else if (auto* CandidateInput = Cast<UAnimGraphNode_LinkedInputPose>(Node))
			{
				if (Input) { WriteResult(false, TEXT("Multiple inputs; refusing to modify graph.")); return; }
				Input = CandidateInput;
			}
			else { WriteResult(false, TEXT("Unexpected graph logic; refusing to replace it.")); return; }
		}
		if (!Root) { WriteResult(false, TEXT("Missing Output Pose.")); return; }
		if (!Input)
		{
			Input = NewObject<UAnimGraphNode_LinkedInputPose>(Graph, NAME_None, RF_Transactional);
			Graph->AddNode(Input, false, false);
			Input->CreateNewGuid();
			Input->PostPlacedNewNode();
			Input->Node.Name = FAnimNode_LinkedInputPose::DefaultInputPoseName;
			Input->Node.Graph = TEXT("AnimGraph");
			Input->AllocateDefaultPins();
			Input->NodePosX = -300;
		}
		if (Input->Node.Name != FAnimNode_LinkedInputPose::DefaultInputPoseName)
		{
			WriteResult(false, TEXT("Existing input must be named InPose."));
			return;
		}
		UEdGraphPin* InputOutput = nullptr;
		UEdGraphPin* RootInput = nullptr;
		for (UEdGraphPin* Pin : Input->Pins) if (Pin->Direction == EGPD_Output) { InputOutput = Pin; break; }
		for (UEdGraphPin* Pin : Root->Pins) if (Pin->Direction == EGPD_Input) { RootInput = Pin; break; }
		if (!InputOutput || !RootInput || (!InputOutput->LinkedTo.Contains(RootInput)
			&& !Graph->GetSchema()->TryCreateConnection(InputOutput, RootInput)))
		{
			WriteResult(false, TEXT("Could not connect InPose to Output Pose."));
			return;
		}
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		UClass* Generated = Blueprint->GeneratedClass;
		if (Blueprint->Status == BS_Error || !Generated || Generated->HasAnyClassFlags(CLASS_Abstract))
		{
			WriteResult(false, TEXT("Blueprint compilation failed or generated class remains abstract."));
			return;
		}
		bool bRegisteredInput = false;
		if (IAnimClassInterface* Interface = IAnimClassInterface::GetFromClass(Generated))
		{
			for (const FAnimBlueprintFunction& Function : Interface->GetAnimBlueprintFunctions())
			{
				if (Function.Name != FName(TEXT("AnimGraph"))) continue;
				const int32 Index = Function.InputPoseNames.IndexOfByKey(FAnimNode_LinkedInputPose::DefaultInputPoseName);
				bRegisteredInput = Function.OutputPoseNodeProperty && Function.InputPoseNodeProperties.IsValidIndex(Index)
					&& Function.InputPoseNodeProperties[Index];
			}
		}
		if (!bRegisteredInput) { WriteResult(false, TEXT("Compiled InPose metadata is missing.")); return; }
		if (bCreated) FAssetRegistryModule::AssetCreated(Blueprint);
		Blueprint->MarkPackageDirty();
		const FString Filename = FPackageName::LongPackageNameToFilename(PackagePath, FPackageName::GetAssetPackageExtension());
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		if (!UPackage::SavePackage(Blueprint->GetOutermost(), Blueprint, *Filename, Args))
		{
			WriteResult(false, TEXT("Could not save bridge package."));
			return;
		}
		WriteResult(true, ClassPath);
	}

	FAutoConsoleCommand CreateCommand(TEXT("Impact.Arms.CreateBridge"),
		TEXT("Create/validate the InPose bridge at /Game/Impact/Weapons/ARKit/ABP_WeaponArms."),
		FConsoleCommandDelegate::CreateStatic(&CreateBridge));
}

#endif // WITH_EDITOR

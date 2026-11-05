#include "ImpactHelmetVisionComponent.h"

#include "Components/PostProcessComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInterface.h"

DEFINE_LOG_CATEGORY_STATIC(LogImpactHelmetVision, Log, All);

namespace
{
    constexpr int32 HelmetThermalStencil = 253;
}

UImpactHelmetVisionComponent::UImpactHelmetVisionComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.bStartWithTickEnabled = false;
    PrimaryComponentTick.TickInterval = 0.2f;

    NightMaterial = TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(
        TEXT("/Game/Impact/Equipment/M_HelmetNight.M_HelmetNight")));
    ThermalMaterial = TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(
        TEXT("/Game/Impact/Equipment/M_HelmetThermal.M_HelmetThermal")));
}

void UImpactHelmetVisionComponent::BeginPlay()
{
    Super::BeginPlay();
    SetComponentTickEnabled(RequestedMode != 0);
    RefreshVision();
}

void UImpactHelmetVisionComponent::SetVisionMode(int32 Mode)
{
    RequestedMode = (Mode >= 0 && Mode <= 2) ? Mode : 0;
    bMaterialLoadFailed = false;
    bWarnedCustomDepth = false;
    SetComponentTickEnabled(RequestedMode != 0);

    if (RequestedMode == 0)
    {
        DisableVision();
    }
    else if (HasBegunPlay() && IsRegistered())
    {
        RefreshVision();
    }
}

APlayerController* UImpactHelmetVisionComponent::GetLocalController() const
{
    const UWorld* World = GetWorld();
    if (!World || !World->IsGameWorld() || World->GetNetMode() == NM_DedicatedServer)
    {
        return nullptr;
    }

    APlayerController* Controller = Cast<APlayerController>(GetOwner());
    if (const APawn* Pawn = Cast<APawn>(GetOwner()))
    {
        Controller = Cast<APlayerController>(Pawn->GetController());
    }
    // A locally controlled AI Pawn is not a local player.
    return IsValid(Controller) && Controller->IsLocalController() && Controller->GetLocalPlayer()
        ? Controller : nullptr;
}

void UImpactHelmetVisionComponent::TickComponent(float DeltaTime, ELevelTick TickType,
    FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
    RefreshVision();
}

void UImpactHelmetVisionComponent::RefreshVision()
{
    APlayerController* Controller = GetLocalController();
    int32 TargetMode = Controller ? RequestedMode : 0;
    if (TargetMode == 2)
    {
        const IConsoleVariable* CustomDepth = IConsoleManager::Get().FindConsoleVariable(TEXT("r.CustomDepth"));
        if (!CustomDepth || CustomDepth->GetInt() != 3)
        {
            TargetMode = 0;
            if (!bWarnedCustomDepth)
            {
                UE_LOG(LogImpactHelmetVision, Warning,
                    TEXT("Thermal vision requires Custom Depth-Stencil Pass = Enabled with Stencil (r.CustomDepth=3). No settings changed."));
                bWarnedCustomDepth = true;
            }
        }
    }

    if (TargetMode != AppliedMode)
    {
        DisableVision();
        if (TargetMode == 0 || bMaterialLoadFailed)
        {
            return;
        }

        const TSoftObjectPtr<UMaterialInterface>& Asset = TargetMode == 1 ? NightMaterial : ThermalMaterial;
        UMaterialInterface* Material = Asset.LoadSynchronous();
        if (!IsValid(Material) || !Material->IsPostProcessMaterial())
        {
            UE_LOG(LogImpactHelmetVision, Warning,
                TEXT("Missing/invalid post-process material %s. Run Tools/Equipment/MakeVisionMaterials.py in the editor, then call SetVisionMode again."),
                *Asset.ToSoftObjectPath().ToString());
            bMaterialLoadFailed = true;
            return;
        }

        if (!IsValid(PostProcess))
        {
            PostProcess = NewObject<UPostProcessComponent>(GetOwner(), NAME_None, RF_Transient);
            PostProcess->bUnbound = true;
            PostProcess->bEnabled = false;
            PostProcess->BlendWeight = 0.0f;
            PostProcess->Priority = 1000.0f;
            PostProcess->RegisterComponent();
        }

        PostProcess->Settings.WeightedBlendables.Array.Emplace(1.0f, Material);
        AppliedMode = TargetMode;
    }

    if (AppliedMode == 2)
    {
        // Also handles newly spawned Characters, modular meshes and possession changes.
        RefreshThermalMeshes(Controller->GetPawn());
    }
    if (AppliedMode != 0 && IsValid(PostProcess))
    {
        PostProcess->bEnabled = true;
        PostProcess->BlendWeight = 1.0f;
    }
}

void UImpactHelmetVisionComponent::RefreshThermalMeshes(const AActor* ExcludedPawn)
{
    TSet<TWeakObjectPtr<USkeletalMeshComponent>> SeenMeshes;
    for (TActorIterator<ACharacter> It(GetWorld()); It; ++It)
    {
        ACharacter* Character = *It;
        if (!IsValid(Character) || Character == GetOwner() || Character == ExcludedPawn || Character->IsHidden())
        {
            continue;
        }

        TInlineComponentArray<USkeletalMeshComponent*> Meshes;
        Character->GetComponents(Meshes);
        for (USkeletalMeshComponent* Mesh : Meshes)
        {
            if (!IsValid(Mesh) || !Mesh->IsRegistered() || !Mesh->IsVisible() || Mesh->bHiddenInGame)
            {
                continue;
            }

            const TWeakObjectPtr<USkeletalMeshComponent> Key(Mesh);
            if (!TaggedMeshes.Contains(Key))
            {
                // Respect even dormant stencil assignments and non-default write masks.
                // Depth-only usage (stencil zero) is saved and restored as well.
                if (Mesh->CustomDepthStencilValue != 0 ||
                    Mesh->CustomDepthStencilWriteMask != ERendererStencilMask::ERSM_Default)
                {
                    continue;
                }

                FSavedMeshState State;
                State.bRenderCustomDepth = Mesh->bRenderCustomDepth;
                State.StencilValue = Mesh->CustomDepthStencilValue;
                State.WriteMask = Mesh->CustomDepthStencilWriteMask;
                TaggedMeshes.Add(Key, State);
                Mesh->SetCustomDepthStencilValue(HelmetThermalStencil);
                Mesh->SetRenderCustomDepth(true);
            }
            SeenMeshes.Add(Key);
        }
    }

    for (auto It = TaggedMeshes.CreateIterator(); It; ++It)
    {
        if (!SeenMeshes.Contains(It.Key()))
        {
            RestoreMesh(It.Key().Get(), It.Value());
            It.RemoveCurrent();
        }
    }
}

void UImpactHelmetVisionComponent::RestoreMesh(USkeletalMeshComponent* Mesh, const FSavedMeshState& State)
{
    // Another feature may have taken ownership after we tagged the mesh. Only
    // restore while the entire tuple still matches what this component applied.
    if (IsValid(Mesh) && Mesh->bRenderCustomDepth &&
        Mesh->CustomDepthStencilValue == HelmetThermalStencil &&
        Mesh->CustomDepthStencilWriteMask == ERendererStencilMask::ERSM_Default)
    {
        Mesh->SetRenderCustomDepth(State.bRenderCustomDepth);
        Mesh->SetCustomDepthStencilValue(State.StencilValue);
        Mesh->SetCustomDepthStencilWriteMask(State.WriteMask);
    }
}

void UImpactHelmetVisionComponent::DisableVision()
{
    if (IsValid(PostProcess))
    {
        PostProcess->BlendWeight = 0.0f;
        PostProcess->bEnabled = false;
        PostProcess->Settings.WeightedBlendables.Array.Reset();
    }
    for (const auto& Entry : TaggedMeshes)
    {
        RestoreMesh(Entry.Key.Get(), Entry.Value);
    }
    TaggedMeshes.Reset();
    AppliedMode = 0;
}

void UImpactHelmetVisionComponent::DestroyVision()
{
    DisableVision();
    if (IsValid(PostProcess))
    {
        PostProcess->DestroyComponent();
    }
    PostProcess = nullptr;
}

void UImpactHelmetVisionComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    RequestedMode = 0;
    SetComponentTickEnabled(false);
    DestroyVision();
    Super::EndPlay(EndPlayReason);
}

void UImpactHelmetVisionComponent::OnUnregister()
{
    DestroyVision();
    Super::OnUnregister();
}

void UImpactHelmetVisionComponent::OnComponentDestroyed(bool bDestroyingHierarchy)
{
    DestroyVision();
    Super::OnComponentDestroyed(bDestroyingHierarchy);
}

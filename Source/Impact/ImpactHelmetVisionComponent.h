#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "ImpactHelmetVisionComponent.generated.h"

class APlayerController;
class UMaterialInterface;
class UPostProcessComponent;
class USkeletalMeshComponent;
enum class ERendererStencilMask : uint8;

/** Local-player vision; attach once to the player's Pawn or PlayerController. */
UCLASS(ClassGroup=(Impact), meta=(BlueprintSpawnableComponent))
class IMPACT_API UImpactHelmetVisionComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UImpactHelmetVisionComponent();

    /** 0 = off, 1 = night, 2 = thermal. Invalid values turn vision off.
     * Thermal requires r.CustomDepth=3; this component never changes CVars/config.
     */
    UFUNCTION(BlueprintCallable, Category="Impact|Helmet Vision")
    void SetVisionMode(int32 Mode);

    virtual void TickComponent(float DeltaTime, ELevelTick TickType,
        FActorComponentTickFunction* ThisTickFunction) override;

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void OnUnregister() override;
    virtual void OnComponentDestroyed(bool bDestroyingHierarchy) override;

private:
    struct FSavedMeshState
    {
        bool bRenderCustomDepth = false;
        int32 StencilValue = 0;
        ERendererStencilMask WriteMask{};
    };

    APlayerController* GetLocalController() const;
    void RefreshVision();
    void RefreshThermalMeshes(const AActor* ExcludedPawn);
    static void RestoreMesh(USkeletalMeshComponent* Mesh, const FSavedMeshState& State);
    void DisableVision();
    void DestroyVision();

    // Reflected soft references also expose the generated assets to the cooker.
    UPROPERTY(EditDefaultsOnly, Category="Impact|Helmet Vision")
    TSoftObjectPtr<UMaterialInterface> NightMaterial;

    UPROPERTY(EditDefaultsOnly, Category="Impact|Helmet Vision")
    TSoftObjectPtr<UMaterialInterface> ThermalMaterial;

    UPROPERTY(Transient)
    TObjectPtr<UPostProcessComponent> PostProcess;

    TMap<TWeakObjectPtr<USkeletalMeshComponent>, FSavedMeshState> TaggedMeshes;
    int32 RequestedMode = 0;
    int32 AppliedMode = 0;
    bool bMaterialLoadFailed = false;
    bool bWarnedCustomDepth = false;
};

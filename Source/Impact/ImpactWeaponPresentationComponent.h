#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "ImpactTypes.h"
#include "ImpactWeaponTuning.h"
#include "ImpactWeaponPresentationComponent.generated.h"

class UStaticMeshComponent;
class USkeletalMeshComponent;
class UImpactWeaponComponent;
class UImpactAmmoComponent;
class UImpactWeaponAnimInstance;
class ACameraActor;
class SWidget;
class UAnimInstance;
class UUserWidget;
class UMaterialInstanceDynamic;
class UImpactEquipmentComponent;
struct FSlateBrush;

/** Player-only modular weapon, inspection UI and manipulation choreography. */
UCLASS()
class IMPACT_API UImpactWeaponPresentationComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    UImpactWeaponPresentationComponent();
    virtual void TickComponent(float Dt, ELevelTick Type, FActorComponentTickFunction* Tick) override;
    void ToggleInspection();
    void CloseInspection();
    void StartReload();
    void CancelAction();
    bool IsBusy() const { return ActionTime >= 0.f; }
    bool IsInspecting() const { return bInspecting; }
    bool GetMuzzle(FVector& Out) const;
    void EjectCase(EImpactWeapon Weapon, const FTransform& Fallback);
    USkeletalMeshComponent* GetSourceMesh() const { return SourceMesh.Get(); }
    bool SelectPart(int32 Index);
    FImpactWeaponSpec GetRifleSpec() const { return GetTuning().Apply(GetImpactWeaponSpec(EImpactWeapon::Rifle)); }
    void UpdateSights(float Dt);
    void ToggleLaser() { bLaserOn=!bLaserOn; }
    void InspectEquipment() { if(!bInspecting)ToggleInspection();MenuCategory=TEXT("equipment"); }
    /** Benches: the T menu opened on one tab. */
    void ShowMenuTab(const FString& Tab) { if(!bInspecting)ToggleInspection();MenuCategory=Tab; }
protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(EEndPlayReason::Type Reason) override;
private:
    struct FPart
    {
        FString Id, Category, Label, MeshPath;
        FVector Grip = FVector::ZeroVector, Muzzle = FVector::ZeroVector;
        FVector MuzzleOffset = FVector::ZeroVector, Sight = FVector(0,0,13);
        FVector ForegripMount = FVector(34,0,9.3), Mount = FVector::ZeroVector;
        FImpactWeaponTuning Tuning;
        float Zoom=1;
        bool bLaser=false;
        int32 Capacity = 30;
    };
    void LoadCatalog();
    void RefreshWeapon();
    void CreateUI();
    void UpdateCamera(float Dt);
    void UpdateAction(float Dt);
    void UpdateBench(float Dt);
    void BeginAction(const FString& Category, int32 NewIndex, float Duration);
    void ApplyPart(int32 Index);
    FImpactWeaponTuning GetTuning() const;
    FVector PieceOffset(const FString& Category) const;
    void RestoreOffsets();
    void StopAiming();
    void EquipTestTarget();
    /** Test tabs of the T menu: people placed in front of the player (see UImpactGameplaySubsystem::SpawnTestPerson). */
    void SpawnTestPeople(int32 WeaponKind,int32 Vest,bool bHelmet,int32 Count);
    bool IsCharacterMenu() const { return MenuCategory==TEXT("equipment")||MenuCategory.StartsWith(TEXT("test_")); }
    FTransform WeaponFrame() const;
    UImpactWeaponAnimInstance* EnsureArms();

    UPROPERTY(Transient) TObjectPtr<UImpactWeaponComponent> Weapon;
    UPROPERTY(Transient) TObjectPtr<UImpactAmmoComponent> Ammo;
    UPROPERTY(Transient) TObjectPtr<UImpactEquipmentComponent> Equipment;
    UPROPERTY(Transient) TObjectPtr<UStaticMeshComponent> Receiver;
    UPROPERTY(Transient) TObjectPtr<UStaticMeshComponent> PistolMagazine;
    UPROPERTY(Transient) TMap<FString, TObjectPtr<UStaticMeshComponent>> Pieces;
    UPROPERTY(Transient) TObjectPtr<ACameraActor> Camera;
    UPROPERTY(Transient) TObjectPtr<ACameraActor> AimCamera;
    UPROPERTY(Transient) TObjectPtr<UStaticMeshComponent> LaserBeam;
    UPROPERTY(Transient) TObjectPtr<UStaticMeshComponent> LaserDot;
    UPROPERTY(Transient) TObjectPtr<UMaterialInstanceDynamic> ScopeMaterial;
    UPROPERTY(Transient) TSubclassOf<UAnimInstance> PreviousPostProcess;
    TWeakObjectPtr<USkeletalMeshComponent> SourceMesh;
    TWeakObjectPtr<AActor> PreviousView;
    TWeakObjectPtr<AActor> AimPreviousView;
    TWeakObjectPtr<AActor> EquipmentTestTarget;
    TArray<TWeakObjectPtr<UUserWidget>> HiddenHelp;
    TArray<uint8> HelpVisibility;
    TArray<TWeakObjectPtr<AActor>> Cases;
    TArray<FPart> Parts;
    TMap<FString, int32> Selected;
    TSharedPtr<SWidget> UI;
    TSharedPtr<FSlateBrush> ScopeBrush;
    FString BasePath, ActionCategory;
    FString MenuCategory=TEXT("magazine");
    int32 TestWeapon=1, TestVest=0;
    bool bTestHelmet=false;
    FVector Ejection = FVector(13,2,11), DefaultMuzzle = FVector(65,0,11);
    FVector HandStart = FVector::ZeroVector;
    FVector PalmOffset = FVector::ZeroVector;
    FVector ActionGripFrom = FVector::ZeroVector, ActionGripTo = FVector::ZeroVector;
    FQuat HandRotation = FQuat::Identity;
    FTransform Alignment = FTransform::Identity;
    float ActionTime = -1, ActionDuration = 1.8f;
    int32 PendingPart = INDEX_NONE;
    bool bCommitted = false, bInspecting = false, bWasCursor = false, bArmsInstalled = false;
    bool bSightActive=false, bLaserOn=true;
    bool bScopeView=false;
    EImpactWeapon LastWeapon = EImpactWeapon::None;
    float BenchTime = 0;
    int32 BenchStep = 0;
    float DiagnosticTime = 0;
};

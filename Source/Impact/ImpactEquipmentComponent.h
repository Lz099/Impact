#pragma once
#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "ImpactTypes.h"
#include "ImpactArmorRules.h"
#include "ImpactEquipmentComponent.generated.h"

class UStaticMeshComponent;
class USpotLightComponent;
class UImpactHelmetVisionComponent;
struct FImpactBulletResult;

/** Wearable gear independent of ALS animation and the anatomy implementation. */
UCLASS(ClassGroup=Impact,meta=(BlueprintSpawnableComponent))
class IMPACT_API UImpactEquipmentComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    UImpactEquipmentComponent();
    virtual void TickComponent(float Dt,ELevelTick Type,FActorComponentTickFunction* Tick) override;
    void SetVest(int32 Level);
    void SetHelmet(bool Equipped);
    void SetHelmetModule(int32 Module); // 0 none, 1 night, 2 thermal. Lamp is independent.
    void SetLamp(bool Equipped);
    void ToggleVision();
    void ToggleLamp();
    int32 GetVest() const { return Vest; }
    bool HasHelmet() const { return bHelmet; }
    int32 GetHelmetModule() const { return HelmetModule; }
    bool HasLamp() const { return bLamp; }
    bool IsVisionOn() const { return bVisionOn; }
    bool IsLampOn() const { return bLampOn; }
    FString Describe() const;
    FTransform GetVestFrame() const;
    FTransform GetHelmetFrame() const;
    /** Changes only residual speed. Returns true if the projectile was stopped before anatomy. */
    bool Intercept(FImpactBulletHit& Bullet);
    /** Common route used for the first hit and every subsequent body pierced by the weapon. */
    static FImpactBulletResult DeliverBullet(AActor* Target,const FImpactBulletHit& Bullet);
protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(EEndPlayReason::Type Reason) override;
private:
    void RefreshMeshes();
    void UpdateTransforms();
    void UpdateFunctions();
    bool ResolveContact(FImpactBulletHit& Bullet,const FTransform& Frame,const ImpactArmor::FContact& Contact,bool Entry);
    void FilterExit(FImpactBulletHit Bullet,FImpactBulletResult& Result);
    FQuat AlignmentForBone(FName Bone) const;
    UStaticMeshComponent* MakeMesh(const TCHAR* Asset);
    int32 Vest=0,HelmetModule=0;
    bool bHelmet=false,bLamp=false,bLampOn=false,bVisionOn=false;
    float Wear[4][3]={{1,1,1},{1,1,1},{1,1,1},{1,1,1}};
    float HelmetWear=1;
    float OpticDeployment=0;
    FQuat TrunkAlignment=FQuat::Identity,HeadAlignment=FQuat::Identity;
    UPROPERTY(Transient) TObjectPtr<UStaticMeshComponent> VestMesh;
    UPROPERTY(Transient) TObjectPtr<UStaticMeshComponent> HelmetMesh;
    UPROPERTY(Transient) TObjectPtr<UStaticMeshComponent> ModuleMesh;
    UPROPERTY(Transient) TObjectPtr<UStaticMeshComponent> LampMesh;
    UPROPERTY(Transient) TObjectPtr<USpotLightComponent> Lamp;
    UPROPERTY(Transient) TObjectPtr<UImpactHelmetVisionComponent> Vision;
};

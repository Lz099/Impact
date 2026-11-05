#include "ImpactEquipmentComponent.h"
#include "ImpactHelmetVisionComponent.h"
#include "ImpactBodyComponent.h"
#include "ImpactReactionComponent.h"
#include "ImpactActiveBodyComponent.h"
#include "ImpactCivilianController.h"
#include "ImpactAudio.h"
#include "Impact.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SpotLightComponent.h"
#include "Engine/SkeletalMesh.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"
#include "DrawDebugHelpers.h"
#include "Misc/CommandLine.h"

UImpactEquipmentComponent::UImpactEquipmentComponent()
{
    PrimaryComponentTick.bCanEverTick=true;PrimaryComponentTick.TickGroup=TG_PostUpdateWork;
}
void UImpactEquipmentComponent::BeginPlay()
{
    Super::BeginPlay();
    if(auto C=Cast<ACharacter>(GetOwner()))AddTickPrerequisiteComponent(C->GetMesh());
    TrunkAlignment=AlignmentForBone(TEXT("spine_03"));HeadAlignment=AlignmentForBone(TEXT("head"));
    if(auto P=Cast<APawn>(GetOwner());P&&P->IsPlayerControlled())
    {Vision=NewObject<UImpactHelmetVisionComponent>(GetOwner());Vision->RegisterComponent();}
    RefreshMeshes();
}
FQuat UImpactEquipmentComponent::AlignmentForBone(FName Bone) const
{
    const auto C=Cast<ACharacter>(GetOwner());if(!C||!C->GetMesh()->GetSkeletalMeshAsset())return FQuat::Identity;
    const auto Mesh=C->GetMesh();const auto& Ref=Mesh->GetSkeletalMeshAsset()->GetRefSkeleton();
    int32 I=Ref.FindBoneIndex(Bone);FTransform Pose=FTransform::Identity;
    while(I!=INDEX_NONE){Pose=Pose*Ref.GetRefBonePose()[I];I=Ref.GetParentIndex(I);}
    const FQuat BodyForward=Mesh->GetComponentQuat().Inverse()*C->GetActorQuat();
    return Pose.GetRotation().Inverse()*BodyForward;
}
FTransform UImpactEquipmentComponent::GetVestFrame() const
{
    auto C=Cast<ACharacter>(GetOwner());if(!C)return GetOwner()->GetActorTransform();
    const auto Bone=C->GetMesh()->GetSocketTransform(TEXT("spine_03"));
    const FQuat Q=Bone.GetRotation()*TrunkAlignment;
    // ALS mannequin render mesh is broader/deeper than its physics capsules.
    // Same fitted transform is used by the visible carrier and the armor queries.
    return FTransform(Q,Bone.GetLocation()+Q.RotateVector(FVector(1,0,-4)),FVector(1.65,1.08,1));
}
FTransform UImpactEquipmentComponent::GetHelmetFrame() const
{
    auto C=Cast<ACharacter>(GetOwner());if(!C)return GetOwner()->GetActorTransform();
    const auto Bone=C->GetMesh()->GetSocketTransform(TEXT("head"));
    const FQuat Q=Bone.GetRotation()*HeadAlignment;
    return FTransform(Q,Bone.GetLocation()+Q.RotateVector(FVector(3,0,7)),FVector(1.1,1.05,1));
}
UStaticMeshComponent* UImpactEquipmentComponent::MakeMesh(const TCHAR* Asset)
{
    auto M=NewObject<UStaticMeshComponent>(GetOwner());
    M->SetStaticMesh(LoadObject<UStaticMesh>(nullptr,Asset));M->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    M->SetCanEverAffectNavigation(false);M->RegisterComponent();return M;
}
void UImpactEquipmentComponent::RefreshMeshes()
{
    if(!GetOwner()||!GetWorld())return;
    const TCHAR* V[]={TEXT(""),TEXT("/Game/Impact/Equipment/SM_VestLight"),TEXT("/Game/Impact/Equipment/SM_VestMedium"),TEXT("/Game/Impact/Equipment/SM_VestHeavy")};
    if(Vest>0){if(!VestMesh)VestMesh=MakeMesh(V[Vest]);else VestMesh->SetStaticMesh(LoadObject<UStaticMesh>(nullptr,V[Vest]));}
    if(bHelmet&&!HelmetMesh)HelmetMesh=MakeMesh(TEXT("/Game/Impact/Equipment/SM_Helmet"));
    const TCHAR* Modules[]={TEXT(""),TEXT("/Game/Impact/Equipment/SM_HelmetNight"),TEXT("/Game/Impact/Equipment/SM_HelmetThermal")};
    if(bHelmet&&HelmetModule>0){if(!ModuleMesh)ModuleMesh=MakeMesh(Modules[HelmetModule]);else ModuleMesh->SetStaticMesh(LoadObject<UStaticMesh>(nullptr,Modules[HelmetModule]));}
    if(bHelmet&&bLamp&&!LampMesh)LampMesh=MakeMesh(TEXT("/Game/Impact/Equipment/SM_HelmetLamp"));
    if(bHelmet&&bLamp&&!Lamp)
    {Lamp=NewObject<USpotLightComponent>(GetOwner());Lamp->SetIntensity(18000);Lamp->SetAttenuationRadius(1800);Lamp->SetInnerConeAngle(13);Lamp->SetOuterConeAngle(27);Lamp->SetCastShadows(true);Lamp->RegisterComponent();}
    if(VestMesh)VestMesh->SetVisibility(Vest>0);
    if(HelmetMesh)HelmetMesh->SetVisibility(bHelmet);
    if(ModuleMesh)ModuleMesh->SetVisibility(bHelmet&&HelmetModule>0);
    if(LampMesh)LampMesh->SetVisibility(bHelmet&&bLamp);
    UpdateTransforms();UpdateFunctions();
}
void UImpactEquipmentComponent::SetVest(int32 Level){Vest=FMath::Clamp(Level,0,3);RefreshMeshes();}
void UImpactEquipmentComponent::SetHelmet(bool Equipped){bHelmet=Equipped;if(!bHelmet){bVisionOn=false;bLampOn=false;}RefreshMeshes();}
void UImpactEquipmentComponent::SetHelmetModule(int32 Module){HelmetModule=FMath::Clamp(Module,0,2);bVisionOn=false;RefreshMeshes();}
void UImpactEquipmentComponent::SetLamp(bool Equipped){bLamp=Equipped;if(!bLamp)bLampOn=false;RefreshMeshes();}
void UImpactEquipmentComponent::ToggleVision(){if(bHelmet&&HelmetModule>0)bVisionOn=!bVisionOn;UpdateFunctions();}
void UImpactEquipmentComponent::ToggleLamp(){if(bHelmet&&bLamp)bLampOn=!bLampOn;UpdateFunctions();}
void UImpactEquipmentComponent::UpdateFunctions()
{
    if(Vision)Vision->SetVisionMode(bHelmet&&bVisionOn?HelmetModule:0);
    if(Lamp)Lamp->SetVisibility(bHelmet&&bLamp&&bLampOn);
}
void UImpactEquipmentComponent::UpdateTransforms()
{
    const auto Chest=GetVestFrame(),Head=GetHelmetFrame();
    if(VestMesh)VestMesh->SetWorldTransform(Chest);
    if(HelmetMesh)HelmetMesh->SetWorldTransform(Head);
    if(ModuleMesh)
    {
        const FVector Hinge(12,0,5);
        const FQuat Fold(FVector::RightVector,FMath::DegreesToRadians(-95.f*(1.f-OpticDeployment)));
        ModuleMesh->SetWorldTransform(FTransform(Fold,Hinge-Fold.RotateVector(Hinge))*Head);
    }
    if(LampMesh)LampMesh->SetWorldTransform(Head);
    if(Lamp)
    {
        FRotator R=Head.Rotator();
        if(auto P=Cast<APawn>(GetOwner()))R=P->GetBaseAimRotation();
        Lamp->SetWorldLocationAndRotation(Head.TransformPosition(FVector(6.1,13.4,3)),R);
    }
}
void UImpactEquipmentComponent::TickComponent(float Dt,ELevelTick Type,FActorComponentTickFunction* Tick)
{Super::TickComponent(Dt,Type,Tick);OpticDeployment=FMath::FInterpConstantTo(OpticDeployment,bVisionOn?1.f:0.f,Dt,3.f);UpdateTransforms();}
FString UImpactEquipmentComponent::Describe() const
{
    const TCHAR* Names[]={TEXT("Sans gilet"),TEXT("Gilet leger"),TEXT("Gilet moyen"),TEXT("Gilet lourd")};
    FString S=Names[Vest];
    if(Vest)S+=FString::Printf(TEXT(" | avant %.0f%% / dos %.0f%%%s"),Wear[Vest][0]*100,Wear[Vest][1]*100,Vest==3?TEXT(" + flancs"):TEXT(""));
    if(bHelmet)S+=FString::Printf(TEXT("\nCasque %.0f%% | %s"),HelmetWear*100,HelmetModule==1?TEXT("nocturne"):HelmetModule==2?TEXT("thermique"):TEXT("sans optique"));
    return S;
}
bool UImpactEquipmentComponent::Intercept(FImpactBulletHit& Bullet)
{
    using namespace ImpactArmor;
    const FString Bone=Bullet.Hit.BoneName.ToString();
    const bool Torso=Bone.StartsWith(TEXT("spine"))||Bone==TEXT("pelvis");
    const bool Head=Bone==TEXT("head");
    FContact Contact;FTransform Frame;
    const FVector Direction=Bullet.Direction.GetSafeNormal();
    if(Direction.IsNearlyZero())return false;
    if(Torso&&Vest>0)
    {Frame=GetVestFrame();Contact=TraceVest(Vest,Frame.InverseTransformPosition(Bullet.Hit.ImpactPoint-Direction*70),Frame.InverseTransformPosition(Bullet.Hit.ImpactPoint+Direction*2));}
    else if(Head&&bHelmet)
    {Frame=GetHelmetFrame();Contact=TraceHelmet(Frame.InverseTransformPosition(Bullet.Hit.ImpactPoint-Direction*70),Frame.InverseTransformPosition(Bullet.Hit.ImpactPoint+Direction*2));}
    if(Contact.Zone==EZone::None)return false;
    return ResolveContact(Bullet,Frame,Contact,true);
}
bool UImpactEquipmentComponent::ResolveContact(FImpactBulletHit& Bullet,const FTransform& Frame,const ImpactArmor::FContact& Contact,bool Entry)
{
    using namespace ImpactArmor;
    float& Condition=Contact.Zone==EZone::Helmet?HelmetWear:Wear[Vest][Contact.Zone==EZone::Front?0:Contact.Zone==EZone::Back?1:2];
    const float Before=Bullet.GetEnergy();
    const float WearBefore=Condition;
    const auto Out=Resolve(Before,Resistance(Vest,Contact.Zone),Condition);
    Bullet.Speed*=FMath::Sqrt(Out.EnergyOut/FMath::Max(Before,1.f));
    const FVector Point=Frame.TransformPosition(Contact.Point);
    // -ImpactSonsArmure. Asked for: "si les balles touchent le gilet, ca doit faire un bruit de metal, et si le gilet craque aussi".
    // Every round the plate takes rings; the plate giving way (the round goes through, or it falls under half its strength) cracks;
    // the plate spent, protecting no more, ruptures. Without the switch, the bullet-on-wall sound as before.
    static const bool bArmorSounds=FParse::Param(FCommandLine::Get(),TEXT("ImpactSonsArmure"));
    const TCHAR* Heard=TEXT("aucun");
    if(bArmorSounds)
    {
        const bool bHelmetHit=Contact.Zone==EZone::Helmet;
        if(Out.Absorbed>0){ImpactAudio::PlayArmorHit(GetWorld(),Point,bHelmetHit,Out.Absorbed/FMath::Max(Resistance(Vest,Contact.Zone),1.f));Heard=TEXT("metal");}
        if(WearBefore>0&&Condition<=0){ImpactAudio::PlayArmorCrack(GetWorld(),Point,true);Heard=Out.Absorbed>0?TEXT("metal+rupture"):TEXT("rupture");}
        else if(Out.Absorbed>0&&(!Out.bStopped||(WearBefore>.5f&&Condition<=.5f))){ImpactAudio::PlayArmorCrack(GetWorld(),Point,false);Heard=TEXT("metal+craque");}
    }
    else if(Out.Absorbed>0){ImpactAudio::PlaySurfaceImpact(GetWorld(),Point);Heard=TEXT("surface");}
    UE_LOG(LogImpact,Display,TEXT("[protection] %s %s zone=%d %s %.1fJ -> %.1fJ etat=%.1f%% son=%s"),*GetOwner()->GetName(),Entry?TEXT("entree"):TEXT("sortie"),int32(Contact.Zone),Out.bStopped?TEXT("arrete"):TEXT("traverse"),Before,Out.EnergyOut,Condition*100,Heard);
    // A stopped round causes a small physical response, never a fake penetrating wound.
    if(Out.bStopped&&Entry)
    {
        const FVector Direction=Bullet.Direction.GetSafeNormal();
        if(auto Reaction=GetOwner()->FindComponentByClass<UImpactReactionComponent>())Reaction->Shove(Direction,FMath::Clamp(Out.Absorbed*.008f,2.f,18.f),TEXT("impact sur protection"));
        // -ImpactCorpsActif: the round stopped by the plate still hits the body behind it, without a hole.
        if(auto Active=GetOwner()->FindComponentByClass<UImpactActiveBodyComponent>())Active->OnBullet(Bullet,FMath::Clamp(Out.Absorbed/(Bullet.Weapon==EImpactWeapon::Rifle?900.f:450.f),.5f,1.5f));
        if(auto C=Cast<APawn>(GetOwner()))if(auto Brain=Cast<AImpactCivilianController>(C->GetController()))Brain->Hurt(Bullet.Instigator.Get());
    }
    return Out.bStopped;
}
void UImpactEquipmentComponent::FilterExit(FImpactBulletHit Bullet,FImpactBulletResult& Result)
{
    if(!Result.bExited||Result.EnergyOut<=0||Bullet.MassKg<=0)return;
    using namespace ImpactArmor;
    const FString Bone=Bullet.Hit.BoneName.ToString();
    const bool Head=Bone==TEXT("head");
    if(Head?!bHelmet:Vest<=0||!(Bone.StartsWith(TEXT("spine"))||Bone==TEXT("pelvis")))return;
    const auto Frame=Head?GetHelmetFrame():GetVestFrame();
    const FVector D=Bullet.Direction.GetSafeNormal();
    const FVector Start=Frame.InverseTransformPosition(Result.ExitPoint-D),End=Frame.InverseTransformPosition(Result.ExitPoint+D*40);
    const FContact Hit=Head?TraceHelmet(Start,End):TraceVest(Vest,Start,End);
    if(Hit.Zone==EZone::None)return;
    Bullet.Speed=100.f*FMath::Sqrt(2.f*Result.EnergyOut/Bullet.MassKg);
    const bool Stopped=ResolveContact(Bullet,Frame,Hit,false);
    Result.EnergyOut=Bullet.GetEnergy();Result.bExited=!Stopped;
    if(!Stopped)Result.ExitPoint=Frame.TransformPosition(Hit.Point)+D*8;
}
FImpactBulletResult UImpactEquipmentComponent::DeliverBullet(AActor* Target,const FImpactBulletHit& Incoming)
{
    if(!Target)return {};
    FImpactBulletHit Bullet=Incoming;
    if(auto Equipment=Target->FindComponentByClass<UImpactEquipmentComponent>())if(Equipment->Intercept(Bullet))return {};
    if(auto Body=Target->FindComponentByClass<UImpactBodyComponent>())
    {
        auto Out=Body->ReceiveBullet(Bullet);
        if(auto Equipment=Target->FindComponentByClass<UImpactEquipmentComponent>())Equipment->FilterExit(Bullet,Out);
        return Out;
    }
    return {};
}
void UImpactEquipmentComponent::EndPlay(EEndPlayReason::Type Reason)
{
    if(Vision){Vision->SetVisionMode(0);Vision->DestroyComponent();}
    for(auto M:{VestMesh.Get(),HelmetMesh.Get(),ModuleMesh.Get(),LampMesh.Get()})if(M)M->DestroyComponent();
    if(Lamp)Lamp->DestroyComponent();Super::EndPlay(Reason);
}

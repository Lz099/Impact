#include "ImpactEquipmentVerification.h"
#include "ImpactEquipmentComponent.h"
#include "ImpactWeaponPresentationComponent.h"
#include "ImpactBodyComponent.h"
#include "Impact.h"
#include "AlsCharacter.h"
#include "Utility/AlsGameplayTags.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/LightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMeshActor.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "DrawDebugHelpers.h"

namespace
{
void CheckProtection(UWorld* World,AActor* Shooter)
{
    int32 Case=0,Failed=0;
    for(TActorIterator<ACharacter> It(World);It&&Case<5;++It)
    {
        auto Body=It->FindComponentByClass<UImpactBodyComponent>();if(!Body||*It==Shooter)continue;
        auto E=It->FindComponentByClass<UImpactEquipmentComponent>();
        if(!E){E=NewObject<UImpactEquipmentComponent>(*It);It->AddInstanceComponent(E);E->RegisterComponent();}
        // Independent fresh targets: no reset or repair inside a production hit.
        const int32 Level=Case==0?1:Case==1?1:Case==2?3:0;
        E->SetVest(Level);E->SetHelmet(Case>=3);
        const auto Frame=Case<3?E->GetVestFrame():E->GetHelmetFrame();
        const float Height=Case==3?10.f:Case==4?-7.f:0.f;
        const FVector Start=Frame.TransformPosition(FVector(150,0,Height)),End=Frame.TransformPosition(FVector(-150,0,Height));
        FHitResult Hit;FCollisionQueryParams Params(SCENE_QUERY_STAT(ArmorVerification),false);
        const bool Found=It->GetMesh()->LineTraceComponent(Hit,Start,End,Params);
        if(!Found){UE_LOG(LogImpact,Error,TEXT("[equip-test] case=%d mesh ray missed"),Case);++Failed;++Case;continue;}
        FImpactBulletHit Bullet;Bullet.Hit=Hit;Bullet.Direction=(End-Start).GetSafeNormal();Bullet.Instigator=Shooter;
        Bullet.Weapon=Case==1||Case==2?EImpactWeapon::Rifle:EImpactWeapon::Pistol;
        const auto Spec=GetImpactWeaponSpec(Bullet.Weapon);Bullet.Speed=Spec.MuzzleSpeed;Bullet.MassKg=Spec.BulletMassKg;
        const int32 Before=Body->GetPhysiology().GetWounds().Num();
        UImpactEquipmentComponent::DeliverBullet(*It,Bullet);
        const int32 After=Body->GetPhysiology().GetWounds().Num();
        const bool ShouldStop=Case==0||Case==2||Case==3;
        const bool Passed=ShouldStop?After==Before:After>Before;
        Failed+=!Passed;
        UE_LOG(LogImpact,Display,TEXT("[equip-test] case=%d %s bone=%s local=%s expected=%s wounds=%d->%d"),Case,Passed?TEXT("PASS"):TEXT("FAIL"),*Hit.BoneName.ToString(),*Frame.InverseTransformPosition(Hit.ImpactPoint).ToCompactString(),ShouldStop?TEXT("stop"):TEXT("penetrate"),Before,After);
        ++Case;
    }
    UE_LOG(LogImpact,Display,TEXT("[equip-test] end cases=%d failed=%d"),Case,Failed);
}
}

void TickImpactEquipmentVerification(UImpactWeaponPresentationComponent* P,UImpactEquipmentComponent* E,float Time,int32& Step)
{
    if(!P||!E||Time<0)return;
    auto C=Cast<AAlsCharacter>(P->GetOwner());if(!C)return;
    if(Step==0&&Time>3){C->SetOverlayMode(AlsOverlayModeTags::Rifle);E->SetVest(1);E->SetHelmet(true);E->SetHelmetModule(1);E->SetLamp(true);++Step;}
    else if(Step==1&&Time>6){P->InspectEquipment();++Step;}
    else if(Step==2&&Time>10){E->SetVest(2);++Step;}
    else if(Step==3&&Time>14){E->SetVest(3);++Step;}
    else if(Step==4&&Time>18){P->CloseInspection();E->ToggleVision();++Step;}
    else if(Step==5&&Time>22)
    {
        // One witness is occluded: thermal must not reveal the body through the solid screen.
        for(TActorIterator<ACharacter> It(P->GetWorld());It;++It)if(*It!=C&&It->FindComponentByClass<UImpactBodyComponent>())
        {
            const FVector Toward=(C->GetActorLocation()-It->GetActorLocation()).GetSafeNormal2D();
            auto Wall=P->GetWorld()->SpawnActor<AStaticMeshActor>();
            auto Mesh=Wall->GetStaticMeshComponent();Mesh->SetMobility(EComponentMobility::Movable);
            Mesh->SetStaticMesh(LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube")));
            Wall->SetActorLocation(It->GetActorLocation()+Toward*90);Wall->SetActorRotation(Toward.Rotation());Wall->SetActorScale3D(FVector(.2,1.4,2.3));
            UE_LOG(LogImpact,Display,TEXT("[equip-test] thermal occluder in front of %s"),*It->GetName());break;
        }
        E->SetHelmetModule(2);E->ToggleVision();++Step;
    }
    else if(Step==6&&Time>26){E->ToggleVision();E->ToggleLamp();++Step;}
    else if(Step==7&&Time>30)
    {
        // Same showcase, temporary dark lighting only in this dedicated, disposable test process.
        for(TActorIterator<AActor> It(P->GetWorld());It;++It)
        {
            if(*It==C)continue;
            TInlineComponentArray<ULightComponent*> Lights(*It);for(auto L:Lights)L->SetIntensity(.15f);
            TInlineComponentArray<USkyLightComponent*> Sky(*It);for(auto L:Sky)L->SetIntensity(.015f);
        }
        ++Step;
    }
    else if(Step==8&&Time>34){E->ToggleLamp();E->SetHelmetModule(1);E->ToggleVision();++Step;}
    else if(Step==9&&Time>38){E->SetHelmetModule(2);E->ToggleVision();++Step;}
    else if(Step==10&&Time>42){E->ToggleVision();CheckProtection(P->GetWorld(),C);++Step;}
    else if(Step==11&&Time>43){C->SetOverlayMode(AlsOverlayModeTags::Default);P->InspectEquipment();++Step;}
}

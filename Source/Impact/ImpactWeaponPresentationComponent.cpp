#include "ImpactWeaponPresentationComponent.h"
#include "ImpactWeaponComponent.h"
#include "ImpactAmmoComponent.h"
#include "ImpactEquipmentComponent.h"
#include "ImpactEquipmentVerification.h"
#include "ImpactBodyComponent.h"
#include "ImpactGameplaySubsystem.h"
#include "EngineUtils.h"
#include "ImpactWeaponAnimInstance.h"
#include "ImpactWeaponSightCamera.h"
#include "Impact.h"
#include "ImpactAutoCapture.h"
#include "Engine/GameInstance.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintLibrary.h"
#include "AlsCharacter.h"
#include "Utility/AlsGameplayTags.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Engine/GameViewportClient.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Images/SImage.h"

namespace
{
FVector ReadVector(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, FVector Default = FVector::ZeroVector)
{
    const TArray<TSharedPtr<FJsonValue>>* Values;
    return Object->TryGetArrayField(Key, Values) && Values->Num() == 3
        ? FVector((*Values)[0]->AsNumber(), (*Values)[1]->AsNumber(), (*Values)[2]->AsNumber()) : Default;
}
float Smooth(float T) { T = FMath::Clamp(T, 0.f, 1.f); return T*T*(3.f-2.f*T); }
}

UImpactWeaponPresentationComponent::UImpactWeaponPresentationComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.TickGroup = TG_PrePhysics;
}

void UImpactWeaponPresentationComponent::BeginPlay()
{
    Super::BeginPlay();
    Weapon = GetOwner()->FindComponentByClass<UImpactWeaponComponent>();
    Ammo = GetOwner()->FindComponentByClass<UImpactAmmoComponent>();
    Equipment=GetOwner()->FindComponentByClass<UImpactEquipmentComponent>();
    LoadCatalog();
    CreateUI();
}

void UImpactWeaponPresentationComponent::LoadCatalog()
{
    FString Text;
    TSharedPtr<FJsonObject> Json;
    if (!FFileHelper::LoadFileToString(Text, *(FPaths::ProjectContentDir()/TEXT("Impact/Weapons/ARKit/catalog.json"))) ||
        !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Json))
    {
        UE_LOG(LogImpact, Warning, TEXT("[armurerie] catalogue absent: fusil ALS conserve"));
        return;
    }
    BasePath = Json->GetStringField(TEXT("base_mesh"));
    Ejection = ReadVector(Json, TEXT("ejection"), Ejection);
    DefaultMuzzle = ReadVector(Json, TEXT("muzzle"), DefaultMuzzle);
    // Asset preparation can supply the exact grip-frame conversion without changing gameplay code.
    const TSharedPtr<FJsonObject>* Align;
    if (Json->TryGetObjectField(TEXT("als_alignment"), Align))
    {
        FVector R = ReadVector(*Align, TEXT("rotation"));
        Alignment = FTransform(FRotator(R.X,R.Y,R.Z), ReadVector(*Align,TEXT("translation")), ReadVector(*Align,TEXT("scale"),FVector::OneVector));
    }
    const TArray<TSharedPtr<FJsonValue>>* Entries;
    if (Json->TryGetArrayField(TEXT("parts"), Entries))
    {
        for (const auto& Entry : *Entries)
        {
            auto J = Entry->AsObject();
            FPart P;
            P.Id=J->GetStringField(TEXT("id")); P.Category=J->GetStringField(TEXT("category"));
            P.Label=J->GetStringField(TEXT("label")); P.MeshPath=J->GetStringField(TEXT("mesh"));
            P.Grip=ReadVector(J,TEXT("grip")); P.Muzzle=ReadVector(J,TEXT("muzzle"),DefaultMuzzle);
            P.MuzzleOffset=ReadVector(J,TEXT("muzzle_offset"));
            P.ForegripMount=ReadVector(J,TEXT("foregrip_mount"),P.ForegripMount);
            P.Mount=ReadVector(J,TEXT("mount"));
            P.Sight=ReadVector(J,TEXT("sight"),P.Sight);
            double Zoom;
            if(J->TryGetNumberField(TEXT("zoom"),Zoom))P.Zoom=FMath::Clamp(float(Zoom),1.f,8.f);
            J->TryGetBoolField(TEXT("laser"),P.bLaser);
            const TSharedPtr<FJsonObject>* Stats;
            if(J->TryGetObjectField(TEXT("stats"),Stats))
            {
                auto Factor=[&](const TCHAR* Key){double V;return (*Stats)->TryGetNumberField(Key,V)&&FMath::IsFinite(V)?FMath::Clamp(float(V),.5f,2.f):1.f;};
                P.Tuning.Pitch=Factor(TEXT("recoil_pitch")); P.Tuning.Yaw=Factor(TEXT("recoil_yaw"));
                P.Tuning.Spread=Factor(TEXT("spread")); P.Tuning.Velocity=Factor(TEXT("velocity")); P.Tuning.Reload=Factor(TEXT("reload"));
            }
            double Capacity;
            if (J->TryGetNumberField(TEXT("capacity"),Capacity)) P.Capacity=FMath::Clamp(int32(Capacity),1,200);
            Parts.Add(P);
        }
    }
    UE_LOG(LogImpact,Display,TEXT("[armurerie] %d pieces chargees"),Parts.Num());
}

FTransform UImpactWeaponPresentationComponent::WeaponFrame() const
{
    if (IsValid(Receiver) && LastWeapon==EImpactWeapon::Rifle) return Receiver->GetComponentTransform();
    return SourceMesh.IsValid() ? SourceMesh->GetComponentTransform() : GetOwner()->GetActorTransform();
}

void UImpactWeaponPresentationComponent::RefreshWeapon()
{
    const auto Current = Weapon->GetCurrentWeapon();
    if (Current != LastWeapon)
    {
        CancelAction(); CloseInspection(); StopAiming();
        if (SourceMesh.IsValid()) SourceMesh->SetVisibility(true,false);
        SourceMesh.Reset();
        if (Receiver) Receiver->SetVisibility(false,true);
        LastWeapon=Current;
    }
    if (Current==EImpactWeapon::None) return;
    if (!SourceMesh.IsValid())
    {
        auto Character=Cast<ACharacter>(GetOwner());
        TInlineComponentArray<USkeletalMeshComponent*> Meshes(GetOwner());
        for (auto Mesh:Meshes)
            if (Mesh!=Character->GetMesh() && Mesh->IsVisible() && Mesh->DoesSocketExist(TEXT("muzzle"))) { SourceMesh=Mesh; break; }
    }
    if (!SourceMesh.IsValid() || Current!=EImpactWeapon::Rifle || BasePath.IsEmpty()) return;
    if (!Receiver)
    {
        auto Mesh=LoadObject<UStaticMesh>(nullptr,*BasePath);
        if (!Mesh) { BasePath.Empty(); return; }
        Receiver=NewObject<UStaticMeshComponent>(GetOwner(),TEXT("ImpactModularRifle"));
        Receiver->SetStaticMesh(Mesh); Receiver->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Receiver->RegisterComponent();
        for (int32 I=0;I<Parts.Num();++I) if (!Selected.Contains(Parts[I].Category)) ApplyPart(I);
    }
    if (Receiver->GetAttachParent()!=SourceMesh.Get())
    {
        Receiver->AttachToComponent(SourceMesh.Get(),FAttachmentTransformRules::KeepRelativeTransform);
        Receiver->SetRelativeTransform(Alignment);
    }
    Receiver->SetVisibility(true,true);
    SourceMesh->SetVisibility(false,false);
}

void UImpactWeaponPresentationComponent::ApplyPart(int32 Index)
{
    if (!Parts.IsValidIndex(Index) || !Receiver) return;
    const auto& P=Parts[Index];
    auto Mesh=P.MeshPath.IsEmpty()?nullptr:LoadObject<UStaticMesh>(nullptr,*P.MeshPath);
    if (!P.MeshPath.IsEmpty()&&!Mesh) { UE_LOG(LogImpact,Error,TEXT("[armurerie] mesh absent %s"),*P.MeshPath);return; }
    auto& Piece=Pieces.FindOrAdd(P.Category);
    if (!Piece)
    {
        Piece=NewObject<UStaticMeshComponent>(GetOwner());
        Piece->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Piece->RegisterComponent();
        Piece->AttachToComponent(Receiver,FAttachmentTransformRules::SnapToTargetNotIncludingScale);
    }
    Piece->SetStaticMesh(Mesh); Piece->SetRelativeTransform(FTransform::Identity);
    Selected.Add(P.Category,Index);
    if (P.Category==TEXT("magazine") && Ammo) Ammo->SetRifleCapacity(P.Capacity);
    if(Ammo)Ammo->SetRifleReloadDuration(2.f*GetTuning().Reload);
    RestoreOffsets();
    UE_LOG(LogImpact,Display,TEXT("[armurerie] piece=%s capacite=%d"),*P.Id,P.Capacity);
}

FImpactWeaponTuning UImpactWeaponPresentationComponent::GetTuning() const
{
    FImpactWeaponTuning T;
    for(const auto& Pair:Selected)if(Parts.IsValidIndex(Pair.Value))T.Combine(Parts[Pair.Value].Tuning);
    return T;
}

FVector UImpactWeaponPresentationComponent::PieceOffset(const FString& Category) const
{
    const int32* Barrel=Selected.Find(TEXT("barrel"));
    const int32* Fore=Selected.Find(TEXT("foregrip"));
    if(Category==TEXT("foregrip")&&Barrel&&Fore)return Parts[*Barrel].ForegripMount-Parts[*Fore].Mount;
    return Category==TEXT("muzzle")&&Barrel?Parts[*Barrel].MuzzleOffset:FVector::ZeroVector;
}

void UImpactWeaponPresentationComponent::RestoreOffsets()
{
    for(auto& Pair:Pieces)if(Pair.Value){Pair.Value->SetRelativeLocation(PieceOffset(Pair.Key));Pair.Value->SetVisibility(true);}
}

bool UImpactWeaponPresentationComponent::GetMuzzle(FVector& Out) const
{
    if (!Receiver || LastWeapon!=EImpactWeapon::Rifle || !SourceMesh.IsValid()) return false;
    const int32* Index=Selected.Find(TEXT("barrel"));
    Out=WeaponFrame().TransformPosition(Index ? Parts[*Index].Muzzle : DefaultMuzzle);
    if(const int32* Device=Selected.Find(TEXT("muzzle")))Out=WeaponFrame().TransformPosition(Parts[*Device].Muzzle+PieceOffset(TEXT("muzzle")));
    return true;
}

UImpactWeaponAnimInstance* UImpactWeaponPresentationComponent::EnsureArms()
{
    auto Character=Cast<ACharacter>(GetOwner());
    if (!Character) return nullptr;
    auto Mesh=Character->GetMesh();
    if (!bArmsInstalled)
    {
        PreviousPostProcess=Mesh->GetPostProcessInstance() ? Mesh->GetPostProcessInstance()->GetClass() : nullptr;
        // Never replace a different post-process belonging to another feature.
        if (PreviousPostProcess && !PreviousPostProcess->IsChildOf(UImpactWeaponAnimInstance::StaticClass())) return nullptr;
        auto Class=LoadClass<UAnimInstance>(nullptr,TEXT("/Game/Impact/Weapons/ARKit/ABP_WeaponArms.ABP_WeaponArms_C"));
        if (!Class) return nullptr;
        // Installing the post-process must not reset ALS's linked rifle overlay.
        Mesh->SetOverridePostProcessAnimBP(Class, false);
        Mesh->InitializeAnimScriptInstance(false);
        bArmsInstalled=true;
    }
    return Cast<UImpactWeaponAnimInstance>(Mesh->GetPostProcessInstance());
}

void UImpactWeaponPresentationComponent::BeginAction(const FString& Category,int32 NewIndex,float Duration)
{
    ActionCategory=Category; PendingPart=NewIndex; ActionTime=0; ActionDuration=Duration; bCommitted=false;
    auto Character=Cast<ACharacter>(GetOwner());
    const auto Frame=WeaponFrame();
    HandStart=Frame.InverseTransformPosition(Character->GetMesh()->GetSocketLocation(TEXT("hand_l")));
    HandRotation=Frame.GetRotation().Inverse()*Character->GetMesh()->GetSocketQuaternion(TEXT("hand_l"));
    const int32* OldIndex=Selected.Find(Category);
    ActionGripFrom=LastWeapon==EImpactWeapon::Pistol?FVector(0,0,-5):OldIndex?Parts[*OldIndex].Grip:FVector::ZeroVector;
    ActionGripTo=Parts.IsValidIndex(NewIndex)?Parts[NewIndex].Grip:ActionGripFrom;
    if(LastWeapon==EImpactWeapon::Rifle)
    {
        ActionGripFrom+=PieceOffset(Category);ActionGripTo+=PieceOffset(Category);
        if(Category==TEXT("foregrip")&&Parts.IsValidIndex(NewIndex)&&OldIndex)
            ActionGripTo+=Parts[*OldIndex].Mount-Parts[NewIndex].Mount;
    }
    PalmOffset=Character->GetMesh()->GetSocketQuaternion(TEXT("hand_l")).Inverse().RotateVector(
        Character->GetMesh()->GetSocketLocation(TEXT("middle_01_l"))-Character->GetMesh()->GetSocketLocation(TEXT("hand_l")))*.7;
    if(LastWeapon==EImpactWeapon::Pistol&&SourceMesh.IsValid())
    {
        if(!PistolMagazine)
        {
            PistolMagazine=NewObject<UStaticMeshComponent>(GetOwner());
            PistolMagazine->SetStaticMesh(LoadObject<UStaticMesh>(nullptr,TEXT("/Game/Impact/Weapons/ARKit/SM_Pistol_Magazine.SM_Pistol_Magazine")));
            PistolMagazine->SetCollisionEnabled(ECollisionEnabled::NoCollision);
            PistolMagazine->RegisterComponent();
        }
        PistolMagazine->AttachToComponent(SourceMesh.Get(),FAttachmentTransformRules::SnapToTargetNotIncludingScale);
        PistolMagazine->SetVisibility(true);
        SourceMesh->HideBoneByName(TEXT("magazine"),PBO_None);
    }
    EnsureArms();
    Weapon->SetTriggerHeld(false);
}

bool UImpactWeaponPresentationComponent::SelectPart(int32 Index)
{
    if (!bInspecting || IsBusy() || !Parts.IsValidIndex(Index)) return false;
    const auto& P=Parts[Index];
    if (Selected.FindRef(P.Category)==Index) return false;
    BeginAction(P.Category,Index,2.2f);
    return true;
}

void UImpactWeaponPresentationComponent::StartReload()
{
    auto Character=Cast<AAlsCharacter>(GetOwner());
    if (!Character || Character->GetLocomotionAction().IsValid() || IsBusy() || bInspecting || !Ammo) return;
    RefreshWeapon();
    if (Ammo->BeginReload(Weapon->GetCurrentWeapon()))
    {
        StopAiming();
        BeginAction(TEXT("magazine"),INDEX_NONE,Ammo->GetReloadDuration());
        UE_LOG(LogImpact,Display,TEXT("[armurerie] rechargement commence"));
    }
}

void UImpactWeaponPresentationComponent::CancelAction()
{
    if (Ammo) Ammo->CancelReload();
    if(PistolMagazine)PistolMagazine->SetVisibility(false);
    if(SourceMesh.IsValid())SourceMesh->UnHideBoneByName(TEXT("magazine"));
    if (auto Piece=Pieces.Find(ActionCategory)) (*Piece)->SetVisibility(true);
    RestoreOffsets();
    if (bArmsInstalled)
        if (auto C=Cast<ACharacter>(GetOwner()))
            if (auto Arms=Cast<UImpactWeaponAnimInstance>(C->GetMesh()->GetPostProcessInstance()))
                Arms->SetLeftHandTarget(FVector::ZeroVector,FRotator::ZeroRotator,FVector::ZeroVector,0);
    ActionTime=-1; PendingPart=INDEX_NONE;
}

void UImpactWeaponPresentationComponent::UpdateAction(float Dt)
{
    if (!IsBusy()) return;
    ActionTime+=Dt;
    const float T=FMath::Clamp(ActionTime/ActionDuration,0.f,1.f);
    const FVector Grip=FMath::Lerp(ActionGripFrom,ActionGripTo,Smooth((T-.44f)/.14f));
    const FVector Pull=ActionCategory==TEXT("magazine") ? FVector(0,-5,-23) :
        ActionCategory==TEXT("barrel")||ActionCategory==TEXT("muzzle") ? FVector(18,-8,0) :
        ActionCategory==TEXT("foregrip")||ActionCategory==TEXT("pistolgrip") ? FVector(0,-7,-15) :
        ActionCategory==TEXT("laser") ? FVector(0,-12,0) :
        ActionCategory==TEXT("stock") ? FVector(-18,-5,0) : FVector(0,-5,18);
    const float Removed=T<.48f ? Smooth((T-.22f)/.22f) : 1.f-Smooth((T-.58f)/.24f);
    if (T>=.5f && !bCommitted)
    {
        if (PendingPart!=INDEX_NONE) ApplyPart(PendingPart);
        bCommitted=true;
    }
    if (auto Piece=Pieces.Find(ActionCategory); Piece && LastWeapon==EImpactWeapon::Rifle)
    {
        (*Piece)->SetRelativeLocation(PieceOffset(ActionCategory)+Pull*Removed);
        (*Piece)->SetVisibility(!(T>.45f && T<.55f));
        if(ActionCategory==TEXT("barrel"))for(const FString Cat:{TEXT("muzzle"),TEXT("foregrip"),TEXT("laser")})
            if(auto Attached=Pieces.Find(Cat)){(*Attached)->SetRelativeLocation(PieceOffset(Cat)+Pull*Removed);(*Attached)->SetVisibility(!(T>.45f&&T<.55f));}
    }
    if(PistolMagazine&&LastWeapon==EImpactWeapon::Pistol)
    {
        PistolMagazine->SetRelativeLocation(Pull*Removed);
        PistolMagazine->SetVisibility(!(T>.45f&&T<.55f));
    }
    // Reach, grasp, remove, bring back, release. The target follows the gun every frame.
    FVector Target=Grip+Pull*Removed-HandRotation.RotateVector(PalmOffset);
    Target=FMath::Lerp(HandStart,Target,Smooth(T/.22f));
    const float Weight=Smooth(T/.12f)*(1.f-Smooth((T-.83f)/.17f));
    auto Character=Cast<ACharacter>(GetOwner());
    auto Body=Character->GetMesh();
    const auto Frame=WeaponFrame();
    if (auto Arms=EnsureArms())
    {
        FVector World=Frame.TransformPosition(Target);
        FVector Elbow=Body->GetSocketLocation(TEXT("upperarm_l"))+Character->GetActorRightVector()*-35+FVector(0,0,-30);
        Arms->SetLeftHandTarget(Body->GetComponentTransform().InverseTransformPosition(World),
            (Body->GetComponentQuat().Inverse()*Frame.GetRotation()*HandRotation).Rotator(),
            Body->GetComponentTransform().InverseTransformPosition(Elbow),Weight);
        DiagnosticTime+=Dt;
        if (FParse::Param(FCommandLine::Get(),TEXT("ImpactWeaponBench")) && DiagnosticTime>.25f)
        {
            DiagnosticTime=0;
            UE_LOG(LogImpact,Display,TEXT("[arme-main] categorie=%s t=%.2f poids=%.2f ecart=%.1fcm"),
                *ActionCategory,T,Weight,FVector::Dist(World,Body->GetSocketLocation(TEXT("hand_l"))));
        }
    }
    if (T>=1.f)
    {
        // Ammo owns its timer. Do not cancel its last transfer if tick ordering puts it a frame behind.
        if (Ammo && Ammo->IsReloading()) Ammo->AdvanceReload(ActionDuration);
        CancelAction();
        UE_LOG(LogImpact,Display,TEXT("[armurerie] manipulation terminee"));
    }
}

void UImpactWeaponPresentationComponent::ToggleInspection()
{
    if (bInspecting) { CloseInspection(); return; }
    auto C=Cast<AAlsCharacter>(GetOwner());
    if (!C || IsBusy() || C->GetLocomotionAction().IsValid()) return;
    RefreshWeapon();
    if(Weapon->GetCurrentWeapon()!=EImpactWeapon::Rifle)MenuCategory=TEXT("equipment");
    auto PC=Cast<APlayerController>(C->GetController());
    if (!PC) return;
    EquipmentTestTarget.Reset();
    float Best=0.75f;
    for(TActorIterator<ACharacter> It(GetWorld());It;++It)
    {
        const FVector To=It->GetActorLocation()-C->GetActorLocation();
        if(*It==C||To.Size()>2000||!It->FindComponentByClass<UImpactBodyComponent>())continue;
        const float Facing=To.GetSafeNormal()|PC->GetControlRotation().Vector();
        if(Facing>Best){Best=Facing;EquipmentTestTarget=*It;}
    }
    StopAiming();
    Weapon->SetTriggerHeld(false);
    C->GetCharacterMovement()->StopMovementImmediately();
    PreviousView=PC->GetViewTarget(); bWasCursor=PC->bShowMouseCursor;
    PC->SetIgnoreMoveInput(true); PC->SetIgnoreLookInput(true);
    PC->bShowMouseCursor=true;
    FInputModeGameAndUI Mode; Mode.SetHideCursorDuringCapture(false); PC->SetInputMode(Mode);
    bInspecting=true;
    TArray<UUserWidget*> Widgets;
    UWidgetBlueprintLibrary::GetAllWidgetsOfClass(GetWorld(),Widgets,UUserWidget::StaticClass(),true);
    for(auto Widget:Widgets) if(Widget->GetClass()->GetName()==TEXT("W_Als_Hud_C")&&Widget->IsVisible())
    {
        HiddenHelp.Add(Widget);
        HelpVisibility.Add(uint8(Widget->GetVisibility()));
        Widget->SetVisibility(ESlateVisibility::Hidden);
    }
    if (!Camera) Camera=GetWorld()->SpawnActor<ACameraActor>();
    Camera->GetCameraComponent()->SetFieldOfView(48);
    UpdateCamera(0);
    PC->SetViewTargetWithBlend(Camera,.35f);
}

void UImpactWeaponPresentationComponent::CloseInspection()
{
    if (!bInspecting) return;
    CancelAction();
    bInspecting=false;
    for(int32 I=0;I<HiddenHelp.Num();++I)if(HiddenHelp[I].IsValid())HiddenHelp[I]->SetVisibility(ESlateVisibility(HelpVisibility[I]));
    HiddenHelp.Reset();
    HelpVisibility.Reset();
    if (auto C=Cast<APawn>(GetOwner())) if (auto PC=Cast<APlayerController>(C->GetController()))
    {
        PC->SetViewTargetWithBlend(PreviousView.IsValid()?PreviousView.Get():GetOwner(),.35f);
        PC->SetIgnoreMoveInput(false); PC->SetIgnoreLookInput(false); PC->bShowMouseCursor=bWasCursor;
        PC->SetInputMode(FInputModeGameOnly());
    }
}

void UImpactWeaponPresentationComponent::UpdateCamera(float Dt)
{
    if (!bInspecting || !Camera) return;
    const auto Frame=WeaponFrame();
    const bool Gear=IsCharacterMenu();
    const FVector Focus=Gear&&Equipment?Equipment->GetVestFrame().GetLocation()+FVector(0,0,15):Frame.TransformPosition(FVector(18,0,7));
    FVector Location=Focus+GetOwner()->GetActorForwardVector()*110+GetOwner()->GetActorRightVector()*104+FVector(0,0,22);
    if(Gear)Location=Focus+GetOwner()->GetActorForwardVector()*210+GetOwner()->GetActorRightVector()*100+FVector(0,0,30);
    FRotator Rotation=(Focus-Location).Rotation(); Rotation.Roll=Gear?0:-12;
    if (Dt>0) { Location=FMath::VInterpTo(Camera->GetActorLocation(),Location,Dt,15); Rotation=FMath::RInterpTo(Camera->GetActorRotation(),Rotation,Dt,15); }
    Camera->SetActorLocationAndRotation(Location,Rotation);
}

void UImpactWeaponPresentationComponent::EquipTestTarget()
{
    auto Target=EquipmentTestTarget.Get();if(!Target||!Equipment)return;
    auto E=Target->FindComponentByClass<UImpactEquipmentComponent>();
    if(!E){E=NewObject<UImpactEquipmentComponent>(Target);Target->AddInstanceComponent(E);E->RegisterComponent();}
    E->SetVest(Equipment->GetVest());E->SetHelmet(Equipment->HasHelmet());
    E->SetHelmetModule(Equipment->GetHelmetModule());E->SetLamp(Equipment->HasLamp());
    UE_LOG(LogImpact,Display,TEXT("[equipement] configuration copiee sur %s"),*Target->GetName());
}

void UImpactWeaponPresentationComponent::SpawnTestPeople(const int32 WeaponKind,const int32 Vest,const bool bHelmet,const int32 Count)
{
    auto Subsystem=GetWorld()?GetWorld()->GetSubsystem<UImpactGameplaySubsystem>():nullptr;if(!Subsystem)return;
    for(int32 I=0;I<Count;++I)Subsystem->SpawnTestPerson(WeaponKind,Vest,bHelmet);
}

void UImpactWeaponPresentationComponent::StopAiming()
{
    if(!bSightActive)return;
    if(auto Pawn=Cast<APawn>(GetOwner()))if(auto PC=Cast<APlayerController>(Pawn->GetController()))
        if(PC->GetViewTarget()==AimCamera)PC->SetViewTargetWithBlend(AimPreviousView.IsValid()?AimPreviousView.Get():GetOwner(),.15f);
    bSightActive=false;bScopeView=false;AimPreviousView.Reset();
}

void UImpactWeaponPresentationComponent::UpdateSights(float Dt)
{
    auto C=Cast<AAlsCharacter>(GetOwner());
    auto PC=C?Cast<APlayerController>(C->GetController()):nullptr;
    if(!PC)return;
    const bool Ready=LastWeapon==EImpactWeapon::Rifle&&Receiver&&!IsBusy()&&!bInspecting&&!C->GetLocomotionAction().IsValid();
    const int32* Optic=Selected.Find(TEXT("optic"));
    if(Ready&&Weapon->IsAiming()&&Optic)
    {
        const auto& P=Parts[*Optic];const auto Frame=WeaponFrame();
        if(!AimCamera)AimCamera=GetWorld()->SpawnActor<AImpactWeaponSightCamera>();
        bScopeView=P.Zoom>=3.f;
        if(ScopeMaterial&&GetWorld()->GetGameViewport())
        {
            FVector2D Size;GetWorld()->GetGameViewport()->GetViewportSize(Size);
            ScopeMaterial->SetScalarParameterValue(TEXT("Aspect"),Size.X/FMath::Max(1.f,Size.Y));
        }
        AimCamera->GetCameraComponent()->SetFieldOfView(FMath::RadiansToDegrees(2.f*FMath::Atan(FMath::Tan(FMath::DegreesToRadians(60.f)*.5f)/P.Zoom)));
        AimCamera->SetActorLocationAndRotation(Frame.TransformPosition(P.Sight),Frame.GetUnitAxis(EAxis::X).Rotation());
        if(!bSightActive){AimPreviousView=PC->GetViewTarget();bSightActive=true;PC->SetViewTargetWithBlend(AimCamera,.18f);}
    }
    else StopAiming();
    const int32* Laser=Selected.Find(TEXT("laser"));
    const bool Show=Ready&&bLaserOn&&Laser&&Parts[*Laser].bLaser;
    if(!Show){if(LaserBeam)LaserBeam->SetVisibility(false);if(LaserDot)LaserDot->SetVisibility(false);return;}
    if(!LaserBeam)
    {
        auto Material=LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/Impact/Weapons/ARKit/M_Full_LaserGlow"));
        auto Make=[&](const TCHAR* Shape){auto Mesh=NewObject<UStaticMeshComponent>(GetOwner());Mesh->SetStaticMesh(LoadObject<UStaticMesh>(nullptr,Shape));Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);Mesh->SetCastShadow(false);Mesh->RegisterComponent();Mesh->SetMaterial(0,Material);return Mesh;};
        LaserBeam=Make(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
        LaserDot=Make(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
    }
    const auto Frame=WeaponFrame();const FVector Start=Frame.TransformPosition(FVector(30.3,3.1,10));
    const FVector End=Start+Frame.GetUnitAxis(EAxis::X)*10000;
    FHitResult Hit;FCollisionQueryParams Query(SCENE_QUERY_STAT(WeaponLaser),false,GetOwner());
    const bool bHit=GetWorld()->LineTraceSingleByChannel(Hit,Start,End,ECC_ImpactBullet,Query);
    const FVector Stop=bHit?Hit.ImpactPoint:End;const float Distance=FVector::Distance(Start,Stop);
    LaserBeam->SetWorldLocation((Start+Stop)*.5);LaserBeam->SetWorldRotation(FRotationMatrix::MakeFromZ(Stop-Start).Rotator());
    LaserBeam->SetWorldScale3D(FVector(.0008,.0008,Distance/100));LaserBeam->SetVisibility(true);
    LaserDot->SetWorldLocation(Stop+(bHit?Hit.ImpactNormal*.12:FVector::ZeroVector));
    const float Size=FMath::Clamp(.2f+Distance*.00008f,.2f,.8f)/50;
    LaserDot->SetWorldScale3D(FVector(Size));LaserDot->SetVisibility(bHit);
}

void UImpactWeaponPresentationComponent::CreateUI()
{
    if (!GetWorld()->GetGameViewport()) return;
    const TWeakObjectPtr<UImpactWeaponPresentationComponent> Weak(this);
    ScopeMaterial=UMaterialInstanceDynamic::Create(LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/Impact/Weapons/ARKit/M_Full_ScopeMask")),this);
    ScopeBrush=MakeShared<FSlateBrush>();ScopeBrush->SetResourceObject(ScopeMaterial);ScopeBrush->ImageSize=FVector2D(1280,720);
    TSharedRef<SVerticalBox> Menu=SNew(SVerticalBox);
    Menu->AddSlot().AutoHeight().Padding(0,0,0,10)[SNew(STextBlock).Text_Lambda([Weak]{return FText::FromString(!Weak.IsValid()?TEXT(""):Weak->MenuCategory==TEXT("equipment")?TEXT("EQUIPEMENT"):Weak->MenuCategory.StartsWith(TEXT("test_"))?TEXT("PNJ DE TEST"):TEXT("ATELIER  /  AR"));}).Font(FCoreStyle::GetDefaultFontStyle("Bold",22)).ColorAndOpacity(FLinearColor(.5f,1.f,.85f))];
    Menu->AddSlot().AutoHeight().Padding(0,0,0,12)[SNew(STextBlock).AutoWrapText(true).Font(FCoreStyle::GetDefaultFontStyle("Regular",13)).Text_Lambda([Weak]{
        if(!Weak.IsValid())return FText::GetEmpty();
        if(Weak->MenuCategory==TEXT("equipment")&&Weak->Equipment)return FText::FromString(Weak->Equipment->Describe());
        if(Weak->MenuCategory==TEXT("test_civils"))return FText::FromString(TEXT("Des civils sans arme, devant toi. Ferme T et tire."));
        if(Weak->MenuCategory==TEXT("test_armes"))return FText::FromString(TEXT("Choisis l'arme, le gilet et le casque, fais-les apparaitre, ferme T et tire."));
        const auto S=Weak->GetRifleSpec();const auto T=Weak->GetTuning();
        return FText::FromString(FString::Printf(TEXT("Vitesse %.0f m/s  |  Energie %.0f J\nRecul %.2f / %.2f  |  Recharge %.1f s"),S.MuzzleSpeed*.01f,.5f*S.BulletMassKg*FMath::Square(S.MuzzleSpeed*.01f),S.RecoilPitchDeg,S.RecoilYawDeg,2.f*T.Reload));})];
    TSharedRef<SWrapBox> Tabs=SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(4,4));
    const TArray<FString> Categories={TEXT("magazine"),TEXT("barrel"),TEXT("muzzle"),TEXT("optic"),TEXT("stock"),TEXT("pistolgrip"),TEXT("foregrip"),TEXT("laser"),TEXT("equipment"),TEXT("test_civils"),TEXT("test_armes")};
    const TArray<FString> Names={TEXT("Chargeurs"),TEXT("Canons"),TEXT("Embouts"),TEXT("Viseurs"),TEXT("Crosses"),TEXT("Poignees"),TEXT("Avant"),TEXT("Laser"),TEXT("Personnage"),TEXT("Test civils"),TEXT("Test armes")};
    for(int32 C=0;C<Categories.Num();++C)
    {
        const FString Cat=Categories[C];int32 Count=0;for(const auto& P:Parts)if(P.Category==Cat)++Count;
        Tabs->AddSlot()[SNew(SBox).WidthOverride(164)[SNew(SButton).ContentPadding(5)
            .IsEnabled_Lambda([Weak,Cat]{return Weak.IsValid()&&!Weak->IsBusy()&&(Cat==TEXT("equipment")||Cat.StartsWith(TEXT("test_"))||Weak->LastWeapon==EImpactWeapon::Rifle);})
            .OnClicked_Lambda([Weak,Cat]{if(Weak.IsValid())Weak->MenuCategory=Cat;return FReply::Handled();})
            [SNew(STextBlock).Font(FCoreStyle::GetDefaultFontStyle("Regular",13))
                .ColorAndOpacity_Lambda([Weak,Cat]{return FSlateColor(Weak.IsValid()&&Weak->MenuCategory==Cat?FLinearColor(.5,1,.85):FLinearColor::White);})
                .Text(FText::FromString(Cat==TEXT("equipment")||Cat.StartsWith(TEXT("test_"))?Names[C]:FString::Printf(TEXT("%s (%d)"),*Names[C],Count)))]]];
    }
    Menu->AddSlot().AutoHeight()[Tabs];
    TSharedRef<SScrollBox> Options=SNew(SScrollBox);
    for (int32 I=0;I<Parts.Num();++I)
    {
        Options->AddSlot().Padding(0,2)[SNew(SButton).ContentPadding(FMargin(8,7))
            .Visibility_Lambda([Weak,I]{return Weak.IsValid()&&Weak->MenuCategory==Weak->Parts[I].Category?EVisibility::Visible:EVisibility::Collapsed;})
            .IsEnabled_Lambda([Weak]{return Weak.IsValid()&&!Weak->IsBusy();})
            .OnClicked_Lambda([Weak,I]{if(Weak.IsValid())Weak->SelectPart(I);return FReply::Handled();})
            [SNew(STextBlock).Font(FCoreStyle::GetDefaultFontStyle("Regular",14))
                .ColorAndOpacity_Lambda([Weak,I]{return FSlateColor(Weak.IsValid()&&Weak->Selected.FindRef(Weak->Parts[I].Category)==I?FLinearColor(.5,1,.85):FLinearColor::White);})
                .Text_Lambda([Weak,I]{ if(!Weak.IsValid())return FText::GetEmpty();const auto& P=Weak->Parts[I];return FText::FromString((Weak->Selected.FindRef(P.Category)==I?TEXT("[OK]  "):TEXT("        "))+P.Label);})]];
    }
    TSharedRef<SVerticalBox> Gear=SNew(SVerticalBox);
    auto GearButton=[&](const TCHAR* Label,TFunction<void(UImpactEquipmentComponent*)> Click,TFunction<bool(const UImpactEquipmentComponent*)> Selected,TFunction<bool(const UImpactEquipmentComponent*)> Enabled)
    {
        const FString Text(Label);
        Gear->AddSlot().AutoHeight().Padding(0,2)[SNew(SButton).ContentPadding(6)
            .IsEnabled_Lambda([Weak,Enabled]{return Weak.IsValid()&&Weak->Equipment&&!Weak->IsBusy()&&Enabled(Weak->Equipment);})
            .OnClicked_Lambda([Weak,Click]{if(Weak.IsValid()&&Weak->Equipment)Click(Weak->Equipment);return FReply::Handled();})
            [SNew(STextBlock).Font(FCoreStyle::GetDefaultFontStyle("Regular",14)).Text_Lambda([Weak,Selected,Text]{return FText::FromString((Weak.IsValid()&&Weak->Equipment&&Selected(Weak->Equipment)?TEXT("[OK] "):TEXT("     "))+Text);})]];
    };
    auto Always=[](const UImpactEquipmentComponent*){return true;};
    auto Helmet=[](const UImpactEquipmentComponent* E){return E->HasHelmet();};
    const TCHAR* VestNames[]={TEXT("Sans gilet"),TEXT("Gilet leger - couverture centrale"),TEXT("Gilet moyen - plaques avant/dos"),TEXT("Gilet lourd - plaques + flancs")};
    for(int32 I=0;I<4;++I)GearButton(VestNames[I],[I](auto E){E->SetVest(I);},[I](auto E){return E->GetVest()==I;},Always);
    GearButton(TEXT("Casque balistique"),[](auto E){E->SetHelmet(!E->HasHelmet());},[](auto E){return E->HasHelmet();},Always);
    const TCHAR* ModuleNames[]={TEXT("Casque sans optique"),TEXT("Vision nocturne"),TEXT("Vision thermique")};
    for(int32 I=0;I<3;++I)GearButton(ModuleNames[I],[I](auto E){E->SetHelmetModule(I);},[I](auto E){return E->GetHelmetModule()==I;},Helmet);
    GearButton(TEXT("Lampe sur casque"),[](auto E){E->SetLamp(!E->HasLamp());},[](auto E){return E->HasLamp();},Helmet);
    Gear->AddSlot().AutoHeight().Padding(0,8)[SNew(SButton).Text(FText::FromString(TEXT("Equiper le passant devant moi (essai)")))
        .IsEnabled_Lambda([Weak]{return Weak.IsValid()&&Weak->EquipmentTestTarget.IsValid();})
        .OnClicked_Lambda([Weak]{if(Weak.IsValid())Weak->EquipTestTarget();return FReply::Handled();})];
    Options->AddSlot()[SNew(SBox).Visibility_Lambda([Weak]{return Weak.IsValid()&&Weak->MenuCategory==TEXT("equipment")?EVisibility::Visible:EVisibility::Collapsed;})[Gear]];

    // The two test tabs. Asked for: "rajoute une section test pour les npc civils et une section test pour les npc avec arme, avec
    // gilet, avec casque, pour que je teste".
    auto Heading=[](TSharedRef<SVerticalBox> Box,const TCHAR* Text)
    {Box->AddSlot().AutoHeight().Padding(0,8,0,2)[SNew(STextBlock).Font(FCoreStyle::GetDefaultFontStyle("Bold",14)).ColorAndOpacity(FLinearColor(.5f,1.f,.85f)).Text(FText::FromString(Text))];};
    auto Action=[Weak](TSharedRef<SVerticalBox> Box,const TCHAR* Label,TFunction<void(UImpactWeaponPresentationComponent*)> Click)
    {
        Box->AddSlot().AutoHeight().Padding(0,3)[SNew(SButton).ContentPadding(7)
            .OnClicked_Lambda([Weak,Click]{if(Weak.IsValid())Click(Weak.Get());return FReply::Handled();})
            [SNew(STextBlock).Font(FCoreStyle::GetDefaultFontStyle("Regular",14)).Text(FText::FromString(Label))]];
    };
    auto Choice=[Weak](TSharedRef<SVerticalBox> Box,const TCHAR* Label,TFunction<void(UImpactWeaponPresentationComponent*)> Click,TFunction<bool(const UImpactWeaponPresentationComponent*)> Chosen)
    {
        const FString Text(Label);
        Box->AddSlot().AutoHeight().Padding(0,2)[SNew(SButton).ContentPadding(6)
            .OnClicked_Lambda([Weak,Click]{if(Weak.IsValid())Click(Weak.Get());return FReply::Handled();})
            [SNew(STextBlock).Font(FCoreStyle::GetDefaultFontStyle("Regular",14))
                .ColorAndOpacity_Lambda([Weak,Chosen]{return FSlateColor(Weak.IsValid()&&Chosen(Weak.Get())?FLinearColor(.5,1,.85):FLinearColor::White);})
                .Text_Lambda([Weak,Chosen,Text]{return FText::FromString((Weak.IsValid()&&Chosen(Weak.Get())?TEXT("[OK] "):TEXT("     "))+Text);})]];
    };
    auto Census=[Weak](TSharedRef<SVerticalBox> Box)
    {
        Box->AddSlot().AutoHeight().Padding(0,6)[SNew(STextBlock).Font(FCoreStyle::GetDefaultFontStyle("Regular",13)).Text_Lambda([Weak]{
            auto Subsystem=Weak.IsValid()&&Weak->GetWorld()?Weak->GetWorld()->GetSubsystem<UImpactGameplaySubsystem>():nullptr;
            return FText::FromString(FString::Printf(TEXT("PNJ de test en jeu : %d"),Subsystem?Subsystem->CountTestPeople():0));})];
    };
    auto Remove=[](UImpactWeaponPresentationComponent* Self){if(auto Subsystem=Self->GetWorld()?Self->GetWorld()->GetSubsystem<UImpactGameplaySubsystem>():nullptr)Subsystem->RemoveTestPeople();};

    TSharedRef<SVerticalBox> Civilians=SNew(SVerticalBox);
    Heading(Civilians,TEXT("Civils sans arme"));
    Action(Civilians,TEXT("Faire apparaitre 1 civil"),[](auto Self){Self->SpawnTestPeople(0,0,false,1);});
    Action(Civilians,TEXT("Faire apparaitre 3 civils"),[](auto Self){Self->SpawnTestPeople(0,0,false,3);});
    Action(Civilians,TEXT("Faire apparaitre 3 civils avec gilet moyen et casque"),[](auto Self){Self->SpawnTestPeople(0,2,true,3);});
    Action(Civilians,TEXT("Retirer tous les PNJ de test"),Remove);
    Census(Civilians);

    TSharedRef<SVerticalBox> Armed=SNew(SVerticalBox);
    // The buttons first, so they are seen without scrolling; the choice they will use written above them.
    Heading(Armed,TEXT("Faire apparaitre"));
    Armed->AddSlot().AutoHeight().Padding(0,2)[SNew(STextBlock).Font(FCoreStyle::GetDefaultFontStyle("Regular",13)).AutoWrapText(true).Text_Lambda([Weak]{
        if(!Weak.IsValid())return FText::GetEmpty();
        static const TCHAR* W[]={TEXT(""),TEXT("fusil"),TEXT("pistolet a une main"),TEXT("pistolet a deux mains")};
        static const TCHAR* V[]={TEXT("sans gilet"),TEXT("gilet leger"),TEXT("gilet moyen"),TEXT("gilet lourd")};
        return FText::FromString(FString::Printf(TEXT("Avec : %s, %s, %s"),W[FMath::Clamp(Weak->TestWeapon,1,3)],V[FMath::Clamp(Weak->TestVest,0,3)],Weak->bTestHelmet?TEXT("casque"):TEXT("sans casque")));})];
    Action(Armed,TEXT("1 PNJ arme"),[](auto Self){Self->SpawnTestPeople(Self->TestWeapon,Self->TestVest,Self->bTestHelmet,1);});
    Action(Armed,TEXT("3 PNJ armes"),[](auto Self){Self->SpawnTestPeople(Self->TestWeapon,Self->TestVest,Self->bTestHelmet,3);});
    Action(Armed,TEXT("Retirer tous les PNJ de test"),Remove);
    Census(Armed);
    Heading(Armed,TEXT("Arme"));
    const TCHAR* WeaponLabels[]={TEXT("Fusil"),TEXT("Pistolet a une main"),TEXT("Pistolet a deux mains")};
    for(int32 I=0;I<3;++I)Choice(Armed,WeaponLabels[I],[I](auto Self){Self->TestWeapon=I+1;},[I](auto Self){return Self->TestWeapon==I+1;});
    Heading(Armed,TEXT("Gilet"));
    const TCHAR* VestLabels[]={TEXT("Sans gilet"),TEXT("Gilet leger"),TEXT("Gilet moyen"),TEXT("Gilet lourd")};
    for(int32 I=0;I<4;++I)Choice(Armed,VestLabels[I],[I](auto Self){Self->TestVest=I;},[I](auto Self){return Self->TestVest==I;});
    Heading(Armed,TEXT("Casque"));
    Choice(Armed,TEXT("Sans casque"),[](auto Self){Self->bTestHelmet=false;},[](auto Self){return !Self->bTestHelmet;});
    Choice(Armed,TEXT("Avec casque"),[](auto Self){Self->bTestHelmet=true;},[](auto Self){return Self->bTestHelmet;});

    Options->AddSlot()[SNew(SBox).Visibility_Lambda([Weak]{return Weak.IsValid()&&Weak->MenuCategory==TEXT("test_civils")?EVisibility::Visible:EVisibility::Collapsed;})[Civilians]];
    Options->AddSlot()[SNew(SBox).Visibility_Lambda([Weak]{return Weak.IsValid()&&Weak->MenuCategory==TEXT("test_armes")?EVisibility::Visible:EVisibility::Collapsed;})[Armed]];
    Menu->AddSlot().AutoHeight().Padding(0,12,0,0)[SNew(SBox).HeightOverride_Lambda([Weak]{return FOptionalSize(Weak.IsValid()&&Weak->IsCharacterMenu()?390.f:214.f);})[Options]];
    Menu->AddSlot().AutoHeight().Padding(0,8,0,0)[SNew(STextBlock).Text_Lambda([Weak]{return FText::FromString(!Weak.IsValid()?TEXT(""):Weak->MenuCategory==TEXT("equipment")?TEXT("N : vision   H : lampe\nProtection localisee, usure conservee"):Weak->MenuCategory.StartsWith(TEXT("test_"))?TEXT("Ils apparaissent a 6 m devant toi.\nFerme T pour leur tirer dessus."):TEXT("Clic droit : viser   L : laser"));}).AutoWrapText(true).Font(FCoreStyle::GetDefaultFontStyle("Regular",13))];
    Menu->AddSlot().AutoHeight().Padding(0,12,0,0)[SNew(SButton).Text(FText::FromString(TEXT("Retour au jeu  [T]"))).OnClicked_Lambda([Weak]{if(Weak.IsValid())Weak->CloseInspection();return FReply::Handled();})];
    UI=SNew(SOverlay)
        +SOverlay::Slot()[SNew(SImage).Image(ScopeBrush.Get()).Visibility_Lambda([Weak]{return Weak.IsValid()&&Weak->bSightActive&&Weak->bScopeView?EVisibility::HitTestInvisible:EVisibility::Collapsed;})]
        +SOverlay::Slot().HAlign(HAlign_Left).VAlign(VAlign_Center).Padding(30)
        [SNew(SBox).WidthOverride(374).Visibility_Lambda([Weak]{return Weak.IsValid()&&Weak->bInspecting?EVisibility::Visible:EVisibility::Collapsed;})
            [SNew(SBorder).BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush")).Padding(20).BorderBackgroundColor(FLinearColor(.015f,.025f,.03f,.96f))[Menu]]]
        +SOverlay::Slot().HAlign(HAlign_Right).VAlign(VAlign_Bottom).Padding(30)
        [SNew(SBorder).BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush")).BorderBackgroundColor(FLinearColor(.015f,.025f,.03f,.9f)).Padding(12).Visibility_Lambda([Weak]{return Weak.IsValid()&&Weak->Weapon&&Weak->Weapon->GetCurrentWeapon()!=EImpactWeapon::None?EVisibility::HitTestInvisible:EVisibility::Collapsed;})
            [SNew(STextBlock).Text_Lambda([Weak]{
                if(!Weak.IsValid()||!Weak->Ammo||!Weak->Weapon)return FText::GetEmpty();
                const auto W=Weak->Weapon->GetCurrentWeapon();
                return FText::FromString(FString::Printf(TEXT("%02d / %03d   %s\nR  Recharger     T  Personnaliser"),Weak->Ammo->GetMagazine(W),Weak->Ammo->GetReserve(W),Weak->IsBusy()?TEXT("Manipulation..."):TEXT("")));
            }).Font(FCoreStyle::GetDefaultFontStyle("Bold",16))]]
        +SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center)
        [SNew(STextBlock).Text(FText::FromString(TEXT("+"))).Font(FCoreStyle::GetDefaultFontStyle("Regular",22)).ColorAndOpacity(FLinearColor(1,.1,.05))
            .Visibility_Lambda([Weak]{return Weak.IsValid()&&Weak->bSightActive&&!Weak->bScopeView?EVisibility::HitTestInvisible:EVisibility::Collapsed;})];
    GetWorld()->GetGameViewport()->AddViewportWidgetContent(UI.ToSharedRef(),30);
}

void UImpactWeaponPresentationComponent::EjectCase(EImpactWeapon W,const FTransform& Fallback)
{
    const auto Frame=W==EImpactWeapon::Rifle&&Receiver?WeaponFrame():Fallback;
    const FVector Location=Frame.TransformPosition(W==EImpactWeapon::Rifle&&Receiver?Ejection:FVector(1,6,7));
    FActorSpawnParameters P; P.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    auto Case=GetWorld()->SpawnActor<AStaticMeshActor>(Location,Frame.Rotator(),P);
    if (!Case) return;
    auto Mesh=Case->GetStaticMeshComponent();
    Mesh->SetMobility(EComponentMobility::Movable);
    Mesh->SetStaticMesh(LoadObject<UStaticMesh>(nullptr,TEXT("/Game/Impact/Weapons/ARKit/SM_ARKit_Casing_01.SM_ARKit_Casing_01")));
    Mesh->SetWorldScale3D(W==EImpactWeapon::Rifle?FVector::OneVector:FVector(.42,1,1));
    Mesh->SetCollisionEnabled(ECollisionEnabled::PhysicsOnly);
    Mesh->SetCollisionObjectType(ECC_PhysicsBody);
    Mesh->SetCollisionResponseToAllChannels(ECR_Ignore);
    Mesh->SetCollisionResponseToChannel(ECC_WorldStatic,ECR_Block);
    Mesh->SetCollisionResponseToChannel(ECC_WorldDynamic,ECR_Block);
    Mesh->SetSimulatePhysics(true); Mesh->SetMassOverrideInKg(NAME_None,.006f,true);
    const FVector Kick=W==EImpactWeapon::Rifle?FVector(FMath::FRandRange(-25.f,25.f),150,110):FVector(150,FMath::FRandRange(-25.f,25.f),110);
    Mesh->SetPhysicsLinearVelocity(GetOwner()->GetVelocity()+Frame.TransformVectorNoScale(Kick));
    Mesh->SetPhysicsAngularVelocityInDegrees(FVector(500,800,300));
    Case->SetLifeSpan(20); Cases.Add(Case);
    while(Cases.Num()>64) { if(Cases[0].IsValid())Cases[0]->Destroy(); Cases.RemoveAt(0); }
}

void UImpactWeaponPresentationComponent::TickComponent(float Dt,ELevelTick Type,FActorComponentTickFunction* Tick)
{
    Super::TickComponent(Dt,Type,Tick);
    if (!Weapon) return;
    RefreshWeapon();
    auto C=Cast<AAlsCharacter>(GetOwner());
    if (C && C->GetLocomotionAction().IsValid()) { CancelAction(); CloseInspection(); }
    UpdateAction(Dt); UpdateCamera(Dt);
    UpdateBench(Dt);
}

void UImpactWeaponPresentationComponent::UpdateBench(float Dt)
{
    if(FParse::Param(FCommandLine::Get(),TEXT("ImpactEquipmentBench")))
    {
        auto Capture=GetWorld()->GetGameInstance()->GetSubsystem<UImpactAutoCapture>();
        BenchTime=Capture?Capture->GetTestTime():BenchTime+Dt;
        TickImpactEquipmentVerification(this,Equipment,BenchTime,BenchStep);return;
    }
    if (!FParse::Param(FCommandLine::Get(),TEXT("ImpactWeaponBench"))) return;
    auto Capture=GetWorld()->GetGameInstance()->GetSubsystem<UImpactAutoCapture>();
    BenchTime=Capture ? Capture->GetTestTime() : BenchTime+Dt;
    if(BenchTime<0) return;
    auto C=Cast<AAlsCharacter>(GetOwner());
    if (!C) return;
    const auto FindPart=[&](const TCHAR* Id){return Parts.IndexOfByPredicate([&](const FPart& P){return P.Id==Id;});};
    if(FParse::Param(FCommandLine::Get(),TEXT("ImpactAttachmentBench")))
    {
        const bool SightsOnly=FParse::Param(FCommandLine::Get(),TEXT("ImpactSightBench"));
        if(BenchStep==0&&BenchTime>3){C->SetOverlayMode(AlsOverlayModeTags::Rifle);++BenchStep;}
        else if(BenchStep==1&&BenchTime>6){ToggleInspection();++BenchStep;}
        if(SightsOnly&&BenchStep==2&&BenchTime>9){ApplyPart(FindPart(TEXT("laser_red")));BenchStep=Parts.Num()+2;}
        const int32 Slot=SightsOnly?-1:FMath::FloorToInt((BenchTime-9.f)/3.f);
        if(Slot>=0&&Slot<Parts.Num()&&BenchStep==Slot+2)
        {
            MenuCategory=Parts[Slot].Category;SelectPart(Slot);++BenchStep;
            UE_LOG(LogImpact,Display,TEXT("[pack-test] choix %d/%d %s"),Slot+1,Parts.Num(),*Parts[Slot].Id);
        }
        const float After=SightsOnly?9.f:9+Parts.Num()*3.f;
        if(BenchStep==Parts.Num()+2&&BenchTime>After)
        {
            CloseInspection();C->SetDesiredAiming(true);ApplyPart(FindPart(TEXT("optic_scope")));++BenchStep;
        }
        else if(BenchStep==Parts.Num()+3&&BenchTime>After+4){ApplyPart(FindPart(TEXT("optic_compact")));++BenchStep;}
        else if(BenchStep==Parts.Num()+4&&BenchTime>After+8){ApplyPart(FindPart(TEXT("optic_holo")));++BenchStep;}
        else if(BenchStep==Parts.Num()+5&&BenchTime>After+12){ApplyPart(FindPart(TEXT("optic_iron")));++BenchStep;}
        else if(BenchStep==Parts.Num()+6&&BenchTime>After+16)
        {
            C->SetDesiredAiming(false);ApplyPart(FindPart(TEXT("barrel_ribbed")));ApplyPart(FindPart(TEXT("stock_tube")));ApplyPart(FindPart(TEXT("muzzle_tiny")));
            Weapon->SetTriggerHeld(true);const auto S=Weapon->GetCurrentSpec();FVector M;GetMuzzle(M);
            UE_LOG(LogImpact,Display,TEXT("[pack-tir] compact vitesse=%.0f energie=%.0f recul=%.3f/%.3f bouche=%s"),S.MuzzleSpeed*.01f,.5*S.BulletMassKg*FMath::Square(S.MuzzleSpeed*.01f),S.RecoilPitchDeg,S.RecoilYawDeg,*M.ToCompactString());++BenchStep;
        }
        else if(BenchStep==Parts.Num()+7&&BenchTime>After+17){Weapon->SetTriggerHeld(false);++BenchStep;}
        else if(BenchStep==Parts.Num()+8&&BenchTime>After+19)
        {
            ApplyPart(FindPart(TEXT("barrel_rail")));ApplyPart(FindPart(TEXT("stock_telescopic")));ApplyPart(FindPart(TEXT("muzzle_dark")));
            Weapon->SetTriggerHeld(true);const auto S=Weapon->GetCurrentSpec();FVector M;GetMuzzle(M);
            UE_LOG(LogImpact,Display,TEXT("[pack-tir] stable vitesse=%.0f energie=%.0f recul=%.3f/%.3f bouche=%s"),S.MuzzleSpeed*.01f,.5*S.BulletMassKg*FMath::Square(S.MuzzleSpeed*.01f),S.RecoilPitchDeg,S.RecoilYawDeg,*M.ToCompactString());++BenchStep;
        }
        else if(BenchStep==Parts.Num()+9&&BenchTime>After+20){Weapon->SetTriggerHeld(false);StartReload();++BenchStep;}
        return;
    }
    if(BenchStep==0&&BenchTime>3) { C->SetOverlayMode(AlsOverlayModeTags::Rifle); ++BenchStep; }
    else if(BenchStep==1&&BenchTime>6) { ToggleInspection(); ++BenchStep; }
    else if(BenchStep==2&&BenchTime>9) { SelectPart(FindPart(TEXT("barrel_short"))); ++BenchStep; }
    else if(BenchStep==3&&BenchTime>13) { SelectPart(FindPart(TEXT("magazine_60"))); ++BenchStep; }
    else if(BenchStep==4&&BenchTime>17) { CloseInspection(); Weapon->SetTriggerHeld(true); ++BenchStep; }
    else if(BenchStep==5&&BenchTime>18) { Weapon->SetTriggerHeld(false); StartReload(); ++BenchStep; }
    else if(BenchStep==6&&BenchTime>22) { ToggleInspection(); ++BenchStep; }
    else if(BenchStep==7&&BenchTime>26) { SelectPart(FindPart(TEXT("optic_compact"))); ++BenchStep; }
    else if(BenchStep==8&&BenchTime>30) { SelectPart(FindPart(TEXT("stock_telescopic"))); ++BenchStep; }
    else if(BenchStep==9&&BenchTime>34) { CloseInspection(); Weapon->SetTriggerHeld(true); ++BenchStep; }
    else if(BenchStep==10&&BenchTime>35) { Weapon->SetTriggerHeld(false); StartReload(); ++BenchStep; }
    else if(BenchStep==11&&BenchTime>39) { C->SetOverlayMode(AlsOverlayModeTags::PistolTwoHanded); ++BenchStep; }
    else if(BenchStep==12&&BenchTime>41)
    {
        Weapon->SetTriggerHeld(true); Weapon->SetTriggerHeld(false);
        auto PC=Cast<APlayerController>(C->GetController());
        if(Camera&&PC&&SourceMesh.IsValid())
        {
            FVector Focus=SourceMesh->GetComponentLocation()+FVector(0,0,1);
            FVector Location=Focus+C->GetActorForwardVector()*70+C->GetActorRightVector()*60+FVector(0,0,12);
            Camera->SetActorLocationAndRotation(Location,(Focus-Location).Rotation());
            PC->SetViewTargetWithBlend(Camera,.2f);
        }
        ++BenchStep;
    }
    else if(BenchStep==13&&BenchTime>42) { StartReload(); ++BenchStep; }
    else if(BenchStep==14&&BenchTime>45)
    {
        for(auto Case:Cases) if(Case.IsValid())
        {
            auto Mesh=Cast<AStaticMeshActor>(Case.Get())->GetStaticMeshComponent();
            UE_LOG(LogImpact,Display,TEXT("[douille] position=%s vitesse=%.1f"),*Case->GetActorLocation().ToCompactString(),Mesh->GetPhysicsLinearVelocity().Size());
        }
        ++BenchStep;
    }
    else if(BenchStep==15&&BenchTime>46&&Camera&&Cases.Num()&&Cases.Last().IsValid())
    {
        const FVector Focus=Cases.Last()->GetActorLocation();
        const FVector Location=Focus+FVector(13,13,18);
        Camera->SetActorLocationAndRotation(Location,(Focus-Location).Rotation());
        ++BenchStep;
    }
    if(BenchTime>34&&BenchTime<38)C->AddMovementInput(C->GetActorForwardVector(),.45f);
}

void UImpactWeaponPresentationComponent::EndPlay(EEndPlayReason::Type Reason)
{
    StopAiming();
    CancelAction(); CloseInspection();
    if(UI.IsValid()&&GetWorld()->GetGameViewport())GetWorld()->GetGameViewport()->RemoveViewportWidgetContent(UI.ToSharedRef());
    UI.Reset();
    if(SourceMesh.IsValid())SourceMesh->SetVisibility(true,false);
    if(bArmsInstalled) if(auto C=Cast<ACharacter>(GetOwner())) C->GetMesh()->SetOverridePostProcessAnimBP(PreviousPostProcess);
    if(Camera)Camera->Destroy();
    if(AimCamera)AimCamera->Destroy();
    for(auto Case:Cases)if(Case.IsValid())Case->Destroy();
    Super::EndPlay(Reason);
}

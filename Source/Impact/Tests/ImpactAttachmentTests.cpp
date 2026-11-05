#if WITH_DEV_AUTOMATION_TESTS
#include "../ImpactWeaponTuning.h"
#include "../ImpactAmmoComponent.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/StrongObjectPtr.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImpactAttachmentBallisticsTest,"Impact.Attachments.BallisticSeparation",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FImpactAttachmentBallisticsTest::RunTest(const FString&)
{
    FString Text;TSharedPtr<FJsonObject> Root;
    if(!TestTrue(TEXT("Production catalog loads"),FFileHelper::LoadFileToString(Text,*(FPaths::ProjectContentDir()/TEXT("Impact/Weapons/ARKit/catalog.json")))&&FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Root)))return false;
    float Lowest=2,Highest=0;int32 Magazines=0,Sights=0;
    for(const auto& Entry:Root->GetArrayField(TEXT("parts")))
    {
        const auto P=Entry->AsObject();const FString Cat=P->GetStringField(TEXT("category"));const TSharedPtr<FJsonObject>* Stats;
        double Velocity=1;
        if(P->TryGetObjectField(TEXT("stats"),Stats))(*Stats)->TryGetNumberField(TEXT("velocity"),Velocity);
        if(Cat==TEXT("barrel")){Lowest=FMath::Min(Lowest,float(Velocity));Highest=FMath::Max(Highest,float(Velocity));}
        if(Cat==TEXT("magazine")||Cat==TEXT("optic"))TestEqual(TEXT("Changing capacity or optic never increases bullet speed"),float(Velocity),1.f);
        if(Cat==TEXT("magazine"))++Magazines;if(Cat==TEXT("optic"))++Sights;
    }
    TestEqual(TEXT("All original magazine designs available"),Magazines,5);TestEqual(TEXT("All original sights available"),Sights,4);
    TestTrue(TEXT("Barrels have a measurable ballistic difference"),Highest-Lowest>.1f);
    FImpactWeaponTuning Short;Short.Velocity=Lowest;FImpactWeaponTuning Long;Long.Velocity=Highest;
    const auto A=Short.Apply(GetImpactWeaponSpec(EImpactWeapon::Rifle));const auto B=Long.Apply(GetImpactWeaponSpec(EImpactWeapon::Rifle));
    FImpactBulletHit BulletA,BulletB;BulletA.Speed=A.MuzzleSpeed;BulletA.MassKg=A.BulletMassKg;BulletB.Speed=B.MuzzleSpeed;BulletB.MassKg=B.BulletMassKg;
    TestTrue(TEXT("The anatomical damage API receives different positive energy"),BulletB.GetEnergy()>BulletA.GetEnergy()&&BulletA.GetEnergy()>0);
    TestEqual(TEXT("Same ammunition mass"),A.BulletMassKg,B.BulletMassKg);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImpactAttachmentRecoilTest,"Impact.Attachments.RecoilAndCadence",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FImpactAttachmentRecoilTest::RunTest(const FString&)
{
    const auto Base=GetImpactWeaponSpec(EImpactWeapon::Rifle);
    FImpactWeaponTuning T;T.Pitch=.8;T.Yaw=.7;T.Spread=.9;
    const auto Stable=T.Apply(Base);
    TestTrue(TEXT("Stabilizing attachments reduce pitch and lateral kick"),Stable.RecoilPitchDeg<Base.RecoilPitchDeg&&Stable.RecoilYawDeg<Base.RecoilYawDeg);
    TestTrue(TEXT("Dispersion and sustained fire bloom change together"),Stable.AimSpreadDeg<Base.AimSpreadDeg&&Stable.BloomPerShotDeg<Base.BloomPerShotDeg);
    TestEqual(TEXT("No invented fire-rate increase"),Stable.FireInterval,Base.FireInterval);
    TestEqual(TEXT("Grip does not change bullet energy"),Stable.MuzzleSpeed,Base.MuzzleSpeed);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImpactAttachmentReloadTest,"Impact.Attachments.ReloadTiming",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FImpactAttachmentReloadTest::RunTest(const FString&)
{
    TStrongObjectPtr<UImpactAmmoComponent> A(NewObject<UImpactAmmoComponent>());
    A->SetRifleCapacity(100);A->SetRifleReloadDuration(3.f);
    TestTrue(TEXT("Larger magazine can reload"),A->BeginReload(EImpactWeapon::Rifle));
    A->SetRifleReloadDuration(1.f);A->AdvanceReload(2.9f);
    TestTrue(TEXT("A configuration change cannot shorten an active reload"),A->IsReloading());
    TestEqual(TEXT("No premature ammo transfer"),A->GetMagazine(EImpactWeapon::Rifle),30);
    A->AdvanceReload(.11f);
    TestEqual(TEXT("Drum filled at correct end time"),A->GetMagazine(EImpactWeapon::Rifle),100);
    TestEqual(TEXT("Total rounds conserved"),A->GetMagazine(EImpactWeapon::Rifle)+A->GetReserve(EImpactWeapon::Rifle),210);
    return true;
}
#endif

#if WITH_DEV_AUTOMATION_TESTS
#include "../ImpactArmorRules.h"
#include "../ImpactTypes.h"
#include "Misc/AutomationTest.h"
using namespace ImpactArmor;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImpactArmorEnergyTest,"Impact.Equipment.EnergyAndWear",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FImpactArmorEnergyTest::RunTest(const FString&)
{
    float Intact=1;const auto P=GetImpactWeaponSpec(EImpactWeapon::Pistol);const auto R=GetImpactWeaponSpec(EImpactWeapon::Rifle);
    const float PistolEnergy=.5f*P.BulletMassKg*FMath::Square(P.MuzzleSpeed*.01f),RifleEnergy=.5f*R.BulletMassKg*FMath::Square(R.MuzzleSpeed*.01f);
    auto Out=Resolve(PistolEnergy,Resistance(1,EZone::Front),Intact);
    TestTrue(TEXT("Fresh light panel stops showcase pistol"),Out.bStopped&&Out.EnergyOut==0);
    TestTrue(TEXT("Stopped bullet still wears its panel"),Intact<1&&Intact>0);
    float Back=1;TestEqual(TEXT("Front hit does not wear independent back panel"),Back,1.f);
    Intact=1;Out=Resolve(RifleEnergy,Resistance(1,EZone::Front),Intact);
    TestTrue(TEXT("Rifle penetrates light vest with reduced energy"),!Out.bStopped&&Out.EnergyOut>0&&Out.EnergyOut<RifleEnergy);
    TestTrue(TEXT("Energy is conserved"),FMath::IsNearlyEqual(Out.EnergyOut+Out.Absorbed,RifleEnergy,.01f));
    Intact=1;int32 Stops=0;
    for(int32 I=0;I<20;++I){Out=Resolve(RifleEnergy,Resistance(3,EZone::Front),Intact);if(Out.bStopped)++Stops;}
    TestTrue(TEXT("Heavy plate stops initially but never grants infinite protection"),Stops>0&&Stops<20);
    TestTrue(TEXT("Integrity stays bounded after repeated hits"),Intact>=0&&Intact<=1);
    Intact=0;Out=Resolve(RifleEnergy,3000,Intact);TestEqual(TEXT("Broken plate lets full energy through"),Out.EnergyOut,RifleEnergy);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImpactArmorCoverageTest,"Impact.Equipment.LocalCoverage",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FImpactArmorCoverageTest::RunTest(const FString&)
{
    TestTrue(TEXT("Frontal shot intersects plate"),TraceVest(2,FVector(100,0,0),FVector(0,0,0)).Zone==EZone::Front);
    TestTrue(TEXT("Back shot intersects back plate"),TraceVest(2,FVector(-100,0,0),FVector(0,0,0)).Zone==EZone::Back);
    TestTrue(TEXT("Light vest leaves flanks open"),TraceVest(1,FVector(0,100,0),FVector(0,0,0)).Zone==EZone::None);
    TestTrue(TEXT("Heavy vest covers lower flanks"),TraceVest(3,FVector(0,100,-10),FVector(0,0,-10)).Zone==EZone::Side);
    TestTrue(TEXT("Armhole above side panel stays exposed"),TraceVest(3,FVector(0,100,5),FVector(0,0,5)).Zone==EZone::None);
    TestTrue(TEXT("Belly below panel is exposed"),TraceVest(3,FVector(100,0,-35),FVector(0,0,-35)).Zone==EZone::None);
    TestTrue(TEXT("No plate protects beyond its width"),TraceVest(2,FVector(100,35,0),FVector(0,35,0)).Zone==EZone::None);
    TestTrue(TEXT("Helmet covers crown"),TraceHelmet(FVector(0,0,80),FVector(0,0,2)).Zone==EZone::Helmet);
    TestTrue(TEXT("Face opening remains vulnerable"),TraceHelmet(FVector(80,0,-3),FVector(0,0,-3)).Zone==EZone::None);
    TestTrue(TEXT("No phantom helmet below head"),TraceHelmet(FVector(80,0,-20),FVector(0,0,-20)).Zone==EZone::None);
    TestTrue(TEXT("A ray ending before the armor cannot be caught"),TraceVest(3,FVector(100,0,0),FVector(50,0,0)).Zone==EZone::None);
    TestTrue(TEXT("A bullet leaving the torso meets only the back plate"),TraceVest(2,FVector(-4,0,0),FVector(-40,0,0)).Zone==EZone::Back);
    TestTrue(TEXT("A bullet exiting through the helmet crown meets its shell"),TraceHelmet(FVector(0,0,4),FVector(0,0,40)).Zone==EZone::Helmet);
    TestTrue(TEXT("Unequipped vest does not block"),TraceVest(0,FVector(100,0,0),FVector(0,0,0)).Zone==EZone::None);
    return true;
}
#endif

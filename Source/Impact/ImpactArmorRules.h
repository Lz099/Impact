#pragma once
#include "CoreMinimal.h"

/** Tunable game protection, not a certification or a medical damage model. Axes: forward/right/up, cm. */
namespace ImpactArmor
{
enum class EZone : uint8 { None, Front, Back, Side, Helmet };
struct FContact { EZone Zone=EZone::None; FVector Point=FVector::ZeroVector; };
struct FOutcome { bool bStopped=false; float EnergyOut=0; float Absorbed=0; };

inline float Resistance(int32 Vest, EZone Zone)
{
    if(Zone==EZone::Helmet)return 850.f;
    const float Rating[]={0,650,1850,3000};
    return Rating[FMath::Clamp(Vest,0,3)]*(Zone==EZone::Side?.65f:1.f);
}

inline bool SegmentBox(const FVector& Start,const FVector& End,const FBox& Box,float& Enter)
{
    double Min=0,Max=1;const FVector Delta=End-Start;
    for(int32 A=0;A<3;++A)
    {
        if(FMath::Abs(Delta[A])<UE_SMALL_NUMBER){if(Start[A]<Box.Min[A]||Start[A]>Box.Max[A])return false;}
        else{double T0=(Box.Min[A]-Start[A])/Delta[A],T1=(Box.Max[A]-Start[A])/Delta[A];if(T0>T1)Swap(T0,T1);Min=FMath::Max(Min,T0);Max=FMath::Min(Max,T1);if(Min>Max)return false;}
    }
    Enter=float(Min);return true;
}

inline FContact TraceVest(int32 Vest,const FVector& Start,const FVector& End)
{
    FContact Hit;if(Vest<=0)return Hit;
    const float Width=Vest==1?15.5f:Vest==2?17.3f:19.f;
    const float Bottom=Vest==1?-19.7f:Vest==2?-20.4f:-21.f;
    float First=2;
    auto Check=[&](EZone Zone,const FBox& Box){float T;if(SegmentBox(Start,End,Box,T)&&T<First){First=T;Hit.Zone=Zone;Hit.Point=FMath::Lerp(Start,End,T);}};
    Check(EZone::Front,FBox(FVector(9,-Width,Bottom),FVector(13.5,Width,15)));
    Check(EZone::Back,FBox(FVector(-13.5,-Width,Bottom),FVector(-9,Width,15)));
    if(Vest==3){Check(EZone::Side,FBox(FVector(-9,14,-19),FVector(9,21,-3)));Check(EZone::Side,FBox(FVector(-9,-21,-19),FVector(9,-14,-3)));}
    return Hit;
}

inline FContact TraceHelmet(const FVector& Start,const FVector& End)
{
    // Ellipsoid cap open below the brow; face and neck stay exposed.
    const FVector Radius(13,11,14), S=Start/Radius,D=(End-Start)/Radius;
    const double A=D.SizeSquared(),B=2*(S|D),C=S.SizeSquared()-1,Disc=B*B-4*A*C;
    FContact Hit;if(A<UE_SMALL_NUMBER||Disc<0)return Hit;
    double T=(-B-FMath::Sqrt(Disc))/(2*A);
    if(T<0)T=(-B+FMath::Sqrt(Disc))/(2*A);
    if(T<0||T>1)return Hit;
    const FVector P=FMath::Lerp(Start,End,T);
    if(P.Z<0 || (P.X>5&&P.Z<5))return Hit;
    Hit.Zone=EZone::Helmet;Hit.Point=P;return Hit;
}

inline FOutcome Resolve(float Energy,float BaseResistance,float& Integrity)
{
    FOutcome Out;Out.EnergyOut=FMath::Max(0.f,Energy);
    if(!FMath::IsFinite(Energy)||Energy<=0||BaseResistance<=0||Integrity<=0)return Out;
    const float Capacity=BaseResistance*FMath::Clamp(Integrity,0.f,1.f);
    Out.Absorbed=FMath::Min(Energy,Capacity);Out.EnergyOut=FMath::Max(0.f,Energy-Capacity);
    Out.bStopped=Out.EnergyOut<1.f;
    if(Out.bStopped)Out.EnergyOut=0;
    Integrity=FMath::Clamp(Integrity-FMath::Clamp(Energy/BaseResistance*.23f,.025f,.7f),0.f,1.f);
    return Out;
}
}

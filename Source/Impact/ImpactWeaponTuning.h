#pragma once
#include "ImpactTypes.h"

/** Gameplay multipliers for the same ammunition. Optics/magazines never create damage. */
struct FImpactWeaponTuning
{
    float Pitch=1, Yaw=1, Spread=1, Velocity=1, Reload=1;
    void Combine(const FImpactWeaponTuning& Other)
    {
        Pitch*=Other.Pitch; Yaw*=Other.Yaw; Spread*=Other.Spread;
        Velocity*=Other.Velocity; Reload*=Other.Reload;
    }
    FImpactWeaponSpec Apply(FImpactWeaponSpec Spec) const
    {
        Spec.RecoilPitchDeg*=FMath::Clamp(Pitch,.3f,2.f);
        Spec.RecoilYawDeg*=FMath::Clamp(Yaw,.3f,2.f);
        Spec.AimSpreadDeg*=FMath::Clamp(Spread,.4f,2.f);
        Spec.HipSpreadDeg*=FMath::Clamp(Spread,.4f,2.f);
        Spec.BloomPerShotDeg*=FMath::Clamp(Spread,.4f,2.f);
        Spec.MaxBloomDeg*=FMath::Clamp(Spread,.4f,2.f);
        Spec.MuzzleSpeed*=FMath::Clamp(Velocity,.6f,1.2f);
        return Spec;
    }
};

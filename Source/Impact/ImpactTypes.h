#pragma once

#include "CoreMinimal.h"
#include "Engine/HitResult.h"

/** "Bullet" trace channel (DefaultEngine.ini): blocked by meshes and body parts, ignored by character capsules. */
inline constexpr ECollisionChannel ECC_ImpactBullet = ECC_GameTraceChannel1;

/** Weapons of the ALS showcase that can fire, taken from the character's overlay mode. */
enum class EImpactWeapon : uint8
{
	None,
	Rifle,
	Pistol
};

struct FImpactWeaponSpec
{
	const TCHAR* Name;
	float FireInterval;           // s between shots
	bool bAutomatic;
	float AimSpreadDeg;           // cone half angle while aiming
	float HipSpreadDeg;           // cone half angle from the hip
	float BloomPerShotDeg;        // extra spread added by each shot
	float MaxBloomDeg;
	float BloomRecoveryDegPerSec;
	float RecoilPitchDeg;         // camera kick per shot
	float RecoilYawDeg;
	float MuzzleSpeed;            // cm/s
	float BulletMassKg;
	float Range;                  // cm
};

IMPACT_API const FImpactWeaponSpec& GetImpactWeaponSpec(EImpactWeapon Weapon);

/** One bullet arriving on something. */
struct FImpactBulletHit
{
	EImpactWeapon Weapon = EImpactWeapon::None;
	FHitResult Hit;
	FVector Direction = FVector::ForwardVector;
	float Speed = 0.0f;     // cm/s
	float MassKg = 0.0f;
	TWeakObjectPtr<AActor> Instigator;

	/** Kinetic energy in joules. */
	float GetEnergy() const
	{
		const float SpeedMs = Speed * 0.01f;
		return 0.5f * MassKg * SpeedMs * SpeedMs;
	}
};

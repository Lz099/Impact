#pragma once

#include "CoreMinimal.h"
#include "ImpactTypes.h"

/**
 * Sounds synthesized in code (no audio files in the project yet):
 * gunshots per weapon, bullet hitting a hard surface, bullet hitting a body.
 */
namespace ImpactAudio
{
	IMPACT_API void PlayGunshot(UWorld* World, EImpactWeapon Weapon, const FVector& Location, bool bShooterIsListener);
	IMPACT_API void PlaySurfaceImpact(UWorld* World, const FVector& Location);
	IMPACT_API void PlayFleshImpact(UWorld* World, const FVector& Location);
	/** A round on a vest plate or a helmet: a metal clang, louder the more of the plate's strength it took (0..1). */
	IMPACT_API void PlayArmorHit(UWorld* World, const FVector& Location, bool bHelmet, float Strength);
	/** The plate giving way: a crack; a fuller rupture when it is spent and protects no more (bBroken). */
	IMPACT_API void PlayArmorCrack(UWorld* World, const FVector& Location, bool bBroken);
	/** -ImpactSonsArmureWav: the armour sounds written as .wav files, to be listened to outside the game. */
	IMPACT_API void WriteArmorSoundsToDisk();
}

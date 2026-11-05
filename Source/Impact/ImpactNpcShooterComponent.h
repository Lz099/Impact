#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "ImpactNpcShooterComponent.generated.h"

class AAlsCharacter;

/**
 * -ImpactPNJTirent. An armed man shoots at the player: once alerted (a shot heard within forty metres, or a round taken), after
 * a moment to react, while he stands, sees him and points his weapon at him. A rifle in short bursts, a pistol a few rounds at a
 * time, a magazine and a pause to reload; two men at most fire at once. Not a perfect shot: the first round goes wide as a
 * warning, and the spread grows with distance, with each round of a burst, with the player running and with a wounded arm. The round is the same as the player's (UImpactEquipmentComponent::DeliverBullet), so it can also hit someone in
 * between.
 */
UCLASS()
class IMPACT_API UImpactNpcShooterComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UImpactNpcShooterComponent();

	static bool IsEnabled();

	/** Something tells him there is a fight: he turns hostile. */
	void Alert();

	// For the soldier's brain (UImpactSoldierBrainComponent).
	bool IsAlerted() const { return bAlerted; }
	/** Whether he saw the player at the last look (four times a second). */
	bool SeesPlayerNow() const { return bSees; }
	int32 GetBurstsDone() const { return BurstsDone; }
	/** Rounds that actually left the barrel, and how many of them struck the player (-ImpactAttitudeGang: the kick, the celebrating). */
	int32 GetShotsFired() const { return ShotsFired; }
	int32 GetPlayerHits() const { return PlayerHits; }
	bool IsBursting() const { return BurstLeft > 0; }
	bool IsReloading() const { return ReloadLeft > 0.0f; }
	float GetMagazineFraction() const;
	/** A fresh magazine now, 2.5 s without firing (in cover, before it runs out). */
	void ReloadNow();
	/** Whether one of the two firing turns is his or free: a soldier comes up to fire only then. */
	static bool IsTurnFree(const UImpactNpcShooterComponent* Shooter);
	/** Where a round from this man leaves: his weapon's muzzle, or in front of his head. */
	static FVector FindMuzzlePoint(const AAlsCharacter* Character, const FVector& Towards);

protected:
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void EndPlay(EEndPlayReason::Type EndPlayReason) override;

private:
	bool CanShoot(const AAlsCharacter* Me, const APawn* Player) const;
	bool SeesPlayer(const AAlsCharacter* Me, const APawn* Player) const;
	void FireAt(AAlsCharacter* Me, APawn* Player);

	FRandomStream Random;
	bool bAlerted = false;
	bool bSees = false;
	bool bWarned = false;
	float React = -1.0f;      // s left before the first shot once he sees the player
	float Cooldown = 0.0f;    // s to the next round
	float LookTimer = 0.0f;
	int32 BurstLeft = 0;
	int32 BurstIndex = 0;
	int32 Magazine = -1;
	int32 BurstsDone = 0;
	int32 ShotsFired = 0;
	int32 PlayerHits = 0;
	float ReloadLeft = 0.0f;
	float GateOpenTime = 0.0f;
	bool bHeldLogged = false;
};

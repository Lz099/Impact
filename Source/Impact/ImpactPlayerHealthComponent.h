#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "ImpactPlayerHealthComponent.generated.h"

struct FImpactBulletHit;

/**
 * -ImpactPNJTirent. The player's side of being shot: a hundred points, taken by where the round lands (the head almost all, the
 * trunk a fifth, a limb less), a little through armour that stops it. They come back once he has not been hit for a few seconds.
 * At nothing he falls (ALS's ragdoll) and the level starts again a few seconds later. The screen goes red with each hit.
 */
UCLASS()
class IMPACT_API UImpactPlayerHealthComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UImpactPlayerHealthComponent();

	static bool IsEnabled();

	void OnBullet(const FImpactBulletHit& Bullet, bool bStoppedByArmour);

	float GetHealth() const { return Health; }
	bool IsDead() const { return DeadTime >= 0.0f; }
	/** 1 the moment a round lands, fading to 0. */
	float GetHurtFlash() const { return Flash; }

protected:
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
	float Health = 100.0f;
	float SinceHit = 100.0f;
	float Flash = 0.0f;
	float DeadTime = -1.0f;
	bool bRestarting = false;
};

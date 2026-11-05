#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "ImpactAutoCapture.generated.h"

/**
 * Verification helper, inactive unless the game is started with these options:
 *   -ImpactShots=5,10   saves a screenshot at each time (seconds of play, counted once shaders are compiled)
 *   -ImpactQuit=12      closes the game at that time
 * Screenshots go to Saved/Screenshots, log lines start with [autocapture].
 */
UCLASS()
class IMPACT_API UImpactAutoCapture : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** Seconds since the game became ready (shaders compiled), or -1 when not capturing. Scripted tests run on this clock. */
	float GetTestTime() const { return bReady ? Elapsed : -1.0f; }

private:
	bool Tick(float DeltaTime);

	TArray<float> ShotTimes;
	int32 NextShot = 0;
	float QuitTime = -1.0f;
	float Elapsed = 0.0f;
	float WaitLogTimer = 0.0f;
	bool bReady = false;
	FTSTicker::FDelegateHandle TickHandle;
};

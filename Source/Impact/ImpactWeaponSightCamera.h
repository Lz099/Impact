#pragma once
#include "Camera/CameraActor.h"
#include "Camera/CameraTypes.h"
#include "ImpactWeaponSightCamera.generated.h"

/** The small optic must not be cut by the game's 10 cm world near plane. */
UCLASS()
class AImpactWeaponSightCamera : public ACameraActor
{
    GENERATED_BODY()
public:
    virtual void CalcCamera(float DeltaTime,FMinimalViewInfo& OutResult) override
    {
        Super::CalcCamera(DeltaTime,OutResult);
        OutResult.PerspectiveNearClipPlane=1.f;
    }
};

#if WITH_DEV_AUTOMATION_TESTS

#include "AlsCharacterMovementComponent.h"
#include "Components/CapsuleComponent.h"
#include "Curves/CurveVector.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"

#include <limits>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImpactMovementSpeedTest, "Impact.Movement.ExternalGroundSpeed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FImpactMovementSpeedTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("Transient test world"), World))
	{
		return false;
	}
	ON_SCOPE_EXIT { World->DestroyWorld(false); };

	// Two real movement components share one settings asset; no ALS animation assets are required.
	auto* Settings = NewObject<UAlsMovementSettings>(World);
	auto* Curve = NewObject<UCurveVector>(Settings);
	Curve->FloatCurves[0].AddKey(0.0f, 600.0f);
	Curve->FloatCurves[1].AddKey(0.0f, 400.0f);
	Curve->FloatCurves[2].AddKey(0.0f, 2.0f);
	for (auto& RotationMode : Settings->RotationModes)
	{
		for (auto& Stance : RotationMode.Value.Stances)
		{
			auto& Gait = Stance.Value;
			const bool bCrouching = Stance.Key == AlsStanceTags::Crouching;
			Gait.bAllowDirectionDependentMovementSpeed = true;
			Gait.WalkForwardSpeed = bCrouching ? 80.0f : 150.0f;
			Gait.WalkBackwardSpeed = bCrouching ? 60.0f : 100.0f;
			Gait.RunForwardSpeed = bCrouching ? 120.0f : 300.0f;
			Gait.RunBackwardSpeed = bCrouching ? 100.0f : 200.0f;
			Gait.SprintSpeed = bCrouching ? 120.0f : 600.0f;
			Gait.AccelerationAndDecelerationAndGroundFrictionCurve = Curve;
		}
	}

	const auto MakeMovement = [&]() -> UAlsCharacterMovementComponent*
	{
		FActorSpawnParameters SpawnParameters;
		SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		auto* Character = World->SpawnActor<ACharacter>(SpawnParameters);
		if (Character == nullptr)
		{
			return nullptr;
		}
		auto* Movement = NewObject<UAlsCharacterMovementComponent>(Character);
		Movement->SetMovementMode(MOVE_Walking);
		Movement->SetUpdatedComponent(Character->GetCapsuleComponent());
		Movement->SetMovementSettings(Settings);
		Movement->SetMaxAllowedGait(AlsGaitTags::Sprinting);
		return Movement;
	};

	auto* Injured = MakeMovement();
	auto* Healthy = MakeMovement();
	if (!TestNotNull(TEXT("Injured movement"), Injured) || !TestNotNull(TEXT("Healthy movement"), Healthy))
	{
		return false;
	}

	// Exercise ALS's public grounded refresh path, rather than duplicating its speed calculation.
	const auto Refresh = [](UAlsCharacterMovementComponent* Movement)
	{
		Movement->MoveSmooth(FVector::ZeroVector, 1.0f / 60.0f);
	};
	Refresh(Injured);
	Refresh(Healthy);
	TestEqual(TEXT("Default multiplier preserves ALS speed"), Injured->GetMaxSpeed(), 600.0f);

	Injured->SetMaxGroundSpeedMultiplier(0.75f);
	Refresh(Injured);
	TestEqual(TEXT("First injury has a distinct speed"), Injured->GetMaxSpeed(), 450.0f);
	Injured->SetMaxGroundSpeedMultiplier(0.5f);
	for (int32 Frame = 0; Frame < 120; ++Frame)
	{
		Refresh(Injured);
	}
	TestEqual(TEXT("Repeated refresh neither overwrites nor compounds the injury"), Injured->GetMaxSpeed(), 300.0f);
	Refresh(Healthy);
	TestEqual(TEXT("Other character sharing settings stays healthy"), Healthy->GetMaxSpeed(), 600.0f);
	TestEqual(TEXT("Shared asset stays unchanged"),
		Settings->RotationModes.FindChecked(AlsRotationModeTags::ViewDirection).Stances.FindChecked(AlsStanceTags::Standing).SprintSpeed,
		600.0f);

	Injured->Velocity = FVector(225.0f, 0.0f, 0.0f);
	Refresh(Injured);
	TestEqual(TEXT("Gait mapping still uses actual speed and original thresholds"), Injured->GetGaitAmount(), 1.5f);
	TestEqual(TEXT("Acceleration curve is not scaled"), Injured->GetMaxAcceleration(), 600.0f);
	TestEqual(TEXT("Braking curve is not scaled"), Injured->BrakingDecelerationWalking, 400.0f);
	TestEqual(TEXT("Ground friction is not scaled"), Injured->GroundFriction, 2.0f);

	Injured->SetMaxAllowedGait(AlsGaitTags::Walking);
	Injured->Velocity = FVector(-75.0f, 0.0f, 0.0f);
	Refresh(Injured);
	TestEqual(TEXT("Backward walking uses its own ALS limit"), Injured->GetMaxSpeed(), 50.0f);
	Injured->Velocity = FVector(75.0f, 0.0f, 0.0f);
	Injured->SetStance(AlsStanceTags::Crouching);
	Injured->SetRotationMode(AlsRotationModeTags::VelocityDirection);
	Refresh(Injured);
	TestEqual(TEXT("Stance and rotation refresh retain the multiplier"), Injured->MaxWalkSpeed, 40.0f);
	TestEqual(TEXT("Crouched maximum is also limited"), Injured->MaxWalkSpeedCrouched, 40.0f);

	Injured->SetStance(AlsStanceTags::Standing);
	Injured->SetMaxAllowedGait(AlsGaitTags::Sprinting);
	Injured->Velocity = FVector::ZeroVector;
	Injured->bForceMaxAccel = true;
	Refresh(Injured);
	Injured->CalcVelocity(1.0f / 60.0f, Injured->GroundFriction, false, Injured->BrakingDecelerationWalking);
	TestTrue(TEXT("Input accelerates toward the cap instead of assigning velocity"),
		Injured->Velocity.Size2D() > 0.0f && Injured->Velocity.Size2D() < 300.0f);
	for (int32 Frame = 0; Frame < 120; ++Frame)
	{
		Refresh(Injured);
		Injured->CalcVelocity(1.0f / 60.0f, Injured->GroundFriction, false, Injured->BrakingDecelerationWalking);
	}
	TestEqual(TEXT("Sustained input settles at the injured maximum"), Injured->Velocity.Size2D(), 300.0, 0.01);

	Healthy->SetMaxGroundSpeedMultiplier(0.5f);
	Refresh(Healthy);
	Healthy->RequestDirectMove(FVector(1000.0f, 0.0f, 0.0f), true);
	Healthy->CalcVelocity(1.0f / 60.0f, Healthy->GroundFriction, false, Healthy->BrakingDecelerationWalking);
	TestTrue(TEXT("Navigation requests retain physical acceleration"),
		Healthy->Velocity.Size2D() > 0.0f && Healthy->Velocity.Size2D() < 300.0f);
	for (int32 Frame = 0; Frame < 120; ++Frame)
	{
		Refresh(Healthy);
		Healthy->CalcVelocity(1.0f / 60.0f, Healthy->GroundFriction, false, Healthy->BrakingDecelerationWalking);
	}
	TestEqual(TEXT("Navigation requests obey the injured maximum"), Healthy->Velocity.Size2D(), 300.0, 0.01);

	Injured->SetMaxGroundSpeedMultiplier(0.01f);
	Injured->Velocity = FVector::ZeroVector;
	Refresh(Injured);
	Injured->CalcVelocity(1.0f / 60.0f, Injured->GroundFriction, false, Injured->BrakingDecelerationWalking);
	TestEqual(TEXT("Analog minimum cannot bypass a low limit"), Injured->Velocity.Size2D(), 6.0, 0.01);
	Injured->SetMaxGroundSpeedMultiplier(-1.0f);
	Injured->Velocity = FVector::ZeroVector;
	Refresh(Injured);
	Injured->CalcVelocity(1.0f / 60.0f, Injured->GroundFriction, false, Injured->BrakingDecelerationWalking);
	TestEqual(TEXT("Negative input clamps to zero even with movement input"), Injured->Velocity.Size2D(), 0.0);

	for (const float ResetValue : {1.0f, 2.0f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()})
	{
		Injured->SetMaxGroundSpeedMultiplier(0.25f);
		Refresh(Injured);
		Injured->SetMaxGroundSpeedMultiplier(ResetValue);
		Refresh(Injured);
		TestEqual(TEXT("Recovery and invalid high inputs restore normal speed"), Injured->GetMaxSpeed(), 600.0f);
	}
	return true;
}

#endif

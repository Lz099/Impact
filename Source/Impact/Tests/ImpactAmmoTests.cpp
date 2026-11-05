#if WITH_DEV_AUTOMATION_TESTS

#include "../ImpactAmmoComponent.h"
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"

#include <limits>

namespace
{
	int32 TotalAmmo(const UImpactAmmoComponent& Ammo, const EImpactWeapon Weapon)
	{
		return Ammo.GetMagazine(Weapon) + Ammo.GetReserve(Weapon);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImpactAmmoConservationTest, "Impact.Ammo.ConservationAndExhaustion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FImpactAmmoConservationTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UImpactAmmoComponent> Ammo(NewObject<UImpactAmmoComponent>());
	TestNull(TEXT("Ammo tests require no world"), Ammo->GetWorld());
	TestEqual(TEXT("Rifle starts loaded"), Ammo->GetMagazine(EImpactWeapon::Rifle), 30);
	TestEqual(TEXT("Rifle reserve"), Ammo->GetReserve(EImpactWeapon::Rifle), 180);
	TestEqual(TEXT("Pistol starts loaded"), Ammo->GetMagazine(EImpactWeapon::Pistol), 12);
	TestEqual(TEXT("Pistol reserve"), Ammo->GetReserve(EImpactWeapon::Pistol), 72);

	for (const EImpactWeapon Weapon : {EImpactWeapon::Rifle, EImpactWeapon::Pistol})
	{
		const int32 InitialTotal = TotalAmmo(*Ammo, Weapon);
		TestFalse(TEXT("Full magazine rejects reload"), Ammo->BeginReload(Weapon));
		// Every original round must be shootable exactly once, across all reloads.
		for (int32 Shot = 0; Shot < InitialTotal; ++Shot)
		{
			if (!Ammo->CanShoot(Weapon))
			{
				if (!TestTrue(TEXT("Available reserve can reload an empty magazine"), Ammo->BeginReload(Weapon)))
				{
					return false;
				}
				Ammo->AdvanceReload(Ammo->GetReloadDuration());
				TestEqual(TEXT("Reload conserves unspent rounds"), TotalAmmo(*Ammo, Weapon), InitialTotal - Shot);
			}
			TestTrue(TEXT("Each budgeted shot succeeds"), Ammo->ConsumeRound(Weapon));
			TestEqual(TEXT("Only a successful shot removes one round"), TotalAmmo(*Ammo, Weapon), InitialTotal - Shot - 1);
		}
		TestFalse(TEXT("Exhausted weapon cannot shoot"), Ammo->CanShoot(Weapon));
		TestFalse(TEXT("Exhausted weapon cannot consume"), Ammo->ConsumeRound(Weapon));
		TestFalse(TEXT("Empty reserve cannot reload"), Ammo->BeginReload(Weapon));
		TestEqual(TEXT("No underflow after rejected operations"), TotalAmmo(*Ammo, Weapon), 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImpactAmmoCancelTest, "Impact.Ammo.CancelAndRestart",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FImpactAmmoCancelTest::RunTest(const FString& Parameters)
{
	for (const EImpactWeapon Weapon : {EImpactWeapon::Rifle, EImpactWeapon::Pistol})
	{
		TStrongObjectPtr<UImpactAmmoComponent> Ammo(NewObject<UImpactAmmoComponent>());
		for (int32 Shot = 0; Shot < 5; ++Shot)
		{
			TestTrue(TEXT("Prepare a partially spent magazine"), Ammo->ConsumeRound(Weapon));
		}
		const int32 Magazine = Ammo->GetMagazine(Weapon);
		const int32 Reserve = Ammo->GetReserve(Weapon);
		for (const float Fraction : {0.0f, 0.5f, 0.99f})
		{
			TestTrue(TEXT("Start cancellable reload"), Ammo->BeginReload(Weapon));
			const float Duration = Ammo->GetReloadDuration();
			Ammo->AdvanceReload(Duration * Fraction);
			TestTrue(TEXT("No premature completion"), Ammo->IsReloading());
			TestEqual(TEXT("Progress follows elapsed time"), Ammo->GetReloadProgress(), Fraction, 0.0001f);
			TestEqual(TEXT("Pending reload leaves magazine untouched"), Ammo->GetMagazine(Weapon), Magazine);
			TestEqual(TEXT("Pending reload leaves reserve untouched"), Ammo->GetReserve(Weapon), Reserve);
			Ammo->CancelReload();
			Ammo->CancelReload();
			Ammo->AdvanceReload(Duration * 10.0f);
			TestFalse(TEXT("Cancelled reload cannot complete later"), Ammo->IsReloading());
			TestTrue(TEXT("Cancellation releases the weapon"), Ammo->GetReloadWeapon() == EImpactWeapon::None);
			TestEqual(TEXT("Idle progress is zero"), Ammo->GetReloadProgress(), 0.0f);
			TestEqual(TEXT("Idle duration is zero"), Ammo->GetReloadDuration(), 0.0f);
			TestEqual(TEXT("Repeated cancellation preserves magazine"), Ammo->GetMagazine(Weapon), Magazine);
			TestEqual(TEXT("Repeated cancellation preserves reserve"), Ammo->GetReserve(Weapon), Reserve);
			TestTrue(TEXT("Cancellation unlocks shooting"), Ammo->CanShoot(Weapon));
		}
		TestTrue(TEXT("Restart after cancellation"), Ammo->BeginReload(Weapon));
		const float Duration = Ammo->GetReloadDuration();
		Ammo->AdvanceReload(Duration * 0.5f);
		TestTrue(TEXT("Restart gets its full duration"), Ammo->IsReloading());
		Ammo->AdvanceReload(Duration * 0.5f);
		TestFalse(TEXT("Exact duration completes reload"), Ammo->IsReloading());
		TestEqual(TEXT("Top-up restores exactly five rounds"), Ammo->GetMagazine(Weapon), Magazine + 5);
		TestEqual(TEXT("Top-up charges exactly five reserve rounds"), Ammo->GetReserve(Weapon), Reserve - 5);
		Ammo->AdvanceReload(Duration * 100.0f);
		Ammo->CancelReload();
		TestEqual(TEXT("Completed transfer cannot be repeated or refunded"), Ammo->GetReserve(Weapon), Reserve - 5);
		TestEqual(TEXT("Reload conserves total ammunition"), TotalAmmo(*Ammo, Weapon), Magazine + Reserve);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImpactAmmoCrossWeaponTest, "Impact.Ammo.CrossWeaponAndDoubleReload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FImpactAmmoCrossWeaponTest::RunTest(const FString& Parameters)
{
	for (const EImpactWeapon Weapon : {EImpactWeapon::Rifle, EImpactWeapon::Pistol})
	{
		const EImpactWeapon Other = Weapon == EImpactWeapon::Rifle ? EImpactWeapon::Pistol : EImpactWeapon::Rifle;
		TStrongObjectPtr<UImpactAmmoComponent> Ammo(NewObject<UImpactAmmoComponent>());
		TestTrue(TEXT("Spend target ammo"), Ammo->ConsumeRound(Weapon));
		TestTrue(TEXT("Spend other ammo"), Ammo->ConsumeRound(Other));
		const int32 Magazine = Ammo->GetMagazine(Weapon);
		const int32 Reserve = Ammo->GetReserve(Weapon);
		const int32 OtherMagazine = Ammo->GetMagazine(Other);
		const int32 OtherReserve = Ammo->GetReserve(Other);
		TestTrue(TEXT("Start target reload"), Ammo->BeginReload(Weapon));
		const float Duration = Ammo->GetReloadDuration();
		Ammo->AdvanceReload(Duration * 0.5f);
		TestFalse(TEXT("Same weapon cannot restart a reload"), Ammo->BeginReload(Weapon));
		TestFalse(TEXT("Other weapon cannot replace a reload"), Ammo->BeginReload(Other));
		TestTrue(TEXT("Reload retains its target"), Ammo->GetReloadWeapon() == Weapon);
		TestEqual(TEXT("Rejected reload does not reset time"), Ammo->GetReloadProgress(), 0.5f);
		for (const EImpactWeapon Blocked : {Weapon, Other})
		{
			TestFalse(TEXT("All shooting is blocked during reload"), Ammo->CanShoot(Blocked));
			TestFalse(TEXT("Direct consumption cannot bypass the lock"), Ammo->ConsumeRound(Blocked));
		}
		Ammo->AdvanceReload(Duration * 0.5f);
		TestFalse(TEXT("Original deadline completes despite double reload attempts"), Ammo->IsReloading());
		TestEqual(TEXT("Only target magazine is refilled"), Ammo->GetMagazine(Weapon), Magazine + 1);
		TestEqual(TEXT("Only target reserve is charged"), Ammo->GetReserve(Weapon), Reserve - 1);
		TestEqual(TEXT("Other magazine remains independent"), Ammo->GetMagazine(Other), OtherMagazine);
		TestEqual(TEXT("Other reserve remains independent"), Ammo->GetReserve(Other), OtherReserve);
		TestTrue(TEXT("Other weapon is unlocked"), Ammo->CanShoot(Other));
		TestTrue(TEXT("Other weapon can subsequently reload"), Ammo->BeginReload(Other));
		Ammo->AdvanceReload(Ammo->GetReloadDuration());
		TestEqual(TEXT("Second reload uses its own reserve"), Ammo->GetReserve(Other), OtherReserve - 1);
		TestEqual(TEXT("Second reload cannot charge first weapon again"), Ammo->GetReserve(Weapon), Reserve - 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImpactAmmoPartialTest, "Impact.Ammo.ReserveLimitedPartialLoad",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FImpactAmmoPartialTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UImpactAmmoComponent> Ammo(NewObject<UImpactAmmoComponent>());
	for (int32 Shot = 0; Shot < 25; ++Shot)
	{
		TestTrue(TEXT("Leave five live rounds in the rifle"), Ammo->ConsumeRound(EImpactWeapon::Rifle));
	}
	Ammo->SetRifleCapacity(200);
	TestEqual(TEXT("Growing capacity does not grant rounds"), Ammo->GetMagazine(EImpactWeapon::Rifle), 5);
	TestTrue(TEXT("Begin reserve-limited reload"), Ammo->BeginReload(EImpactWeapon::Rifle));
	Ammo->AdvanceReload(Ammo->GetReloadDuration());
	TestEqual(TEXT("Partial load retains old rounds and transfers all available reserve"), Ammo->GetMagazine(EImpactWeapon::Rifle), 185);
	TestEqual(TEXT("Partial load exhausts reserve exactly"), Ammo->GetReserve(EImpactWeapon::Rifle), 0);
	TestEqual(TEXT("Partial load conserves remaining ammunition"), TotalAmmo(*Ammo, EImpactWeapon::Rifle), 185);
	TestFalse(TEXT("Room alone is insufficient for reload"), Ammo->BeginReload(EImpactWeapon::Rifle));
	TestTrue(TEXT("Partially filled weapon can shoot"), Ammo->ConsumeRound(EImpactWeapon::Rifle));
	TestEqual(TEXT("Pistol total unaffected"), TotalAmmo(*Ammo, EImpactWeapon::Pistol), 84);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImpactAmmoCapacityTest, "Impact.Ammo.CapacityChangesConserveRounds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FImpactAmmoCapacityTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UImpactAmmoComponent> Ammo(NewObject<UImpactAmmoComponent>());
	Ammo->SetRifleCapacity(10);
	TestEqual(TEXT("Shrinking limits loaded rounds"), Ammo->GetMagazine(EImpactWeapon::Rifle), 10);
	TestEqual(TEXT("Overflow returns to reserve"), Ammo->GetReserve(EImpactWeapon::Rifle), 200);
	Ammo->SetRifleCapacity(40);
	TestEqual(TEXT("Growing leaves current magazine unchanged"), Ammo->GetMagazine(EImpactWeapon::Rifle), 10);
	TestTrue(TEXT("Begin reload before resize"), Ammo->BeginReload(EImpactWeapon::Rifle));
	const float Duration = Ammo->GetReloadDuration();
	Ammo->AdvanceReload(Duration * 0.5f);
	Ammo->SetRifleCapacity(5);
	TestEqual(TEXT("Mid-reload shrink returns overflow immediately"), Ammo->GetReserve(EImpactWeapon::Rifle), 205);
	TestEqual(TEXT("Mid-reload shrink preserves clock"), Ammo->GetReloadProgress(), 0.5f);
	Ammo->CancelReload();
	TestEqual(TEXT("Cancellation does not undo resize or duplicate its overflow"), Ammo->GetMagazine(EImpactWeapon::Rifle), 5);
	TestEqual(TEXT("Resize then cancel conserves ammunition"), TotalAmmo(*Ammo, EImpactWeapon::Rifle), 210);

	Ammo->SetRifleCapacity(40);
	TestTrue(TEXT("Reload after resized cancellation"), Ammo->BeginReload(EImpactWeapon::Rifle));
	Ammo->AdvanceReload(Duration * 0.5f);
	Ammo->SetRifleCapacity(50);
	Ammo->AdvanceReload(Duration * 0.5f);
	TestEqual(TEXT("Completion uses increased current capacity"), Ammo->GetMagazine(EImpactWeapon::Rifle), 50);
	TestEqual(TEXT("Increased load charges exact reserve"), Ammo->GetReserve(EImpactWeapon::Rifle), 160);

	Ammo->SetRifleCapacity(60);
	TestTrue(TEXT("Start reload before capacity becomes zero"), Ammo->BeginReload(EImpactWeapon::Rifle));
	Ammo->SetRifleCapacity(-1);
	Ammo->AdvanceReload(Duration);
	TestFalse(TEXT("Zero-capacity reload still completes"), Ammo->IsReloading());
	TestEqual(TEXT("Negative capacity clamps to empty magazine"), Ammo->GetMagazine(EImpactWeapon::Rifle), 0);
	TestEqual(TEXT("All remaining rounds are in reserve"), Ammo->GetReserve(EImpactWeapon::Rifle), 210);
	TestFalse(TEXT("Zero capacity cannot shoot"), Ammo->CanShoot(EImpactWeapon::Rifle));
	TestFalse(TEXT("Zero capacity cannot reload"), Ammo->BeginReload(EImpactWeapon::Rifle));

	Ammo->SetRifleCapacity(std::numeric_limits<int32>::max());
	TestTrue(TEXT("Large capacity can reload"), Ammo->BeginReload(EImpactWeapon::Rifle));
	Ammo->AdvanceReload(Duration);
	TestEqual(TEXT("Large capacity cannot invent ammunition"), Ammo->GetMagazine(EImpactWeapon::Rifle), 210);
	Ammo->SetRifleCapacity(std::numeric_limits<int32>::min());
	Ammo->SetRifleCapacity(0);
	TestEqual(TEXT("Repeated extreme resizing conserves ammunition"), Ammo->GetReserve(EImpactWeapon::Rifle), 210);
	TestEqual(TEXT("Rifle capacity never affects pistol magazine"), Ammo->GetMagazine(EImpactWeapon::Pistol), 12);
	TestEqual(TEXT("Rifle capacity never affects pistol reserve"), Ammo->GetReserve(EImpactWeapon::Pistol), 72);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImpactAmmoInvalidInputTest, "Impact.Ammo.InvalidInputsAndTime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FImpactAmmoInvalidInputTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UImpactAmmoComponent> Ammo(NewObject<UImpactAmmoComponent>());
	for (const EImpactWeapon Weapon : {EImpactWeapon::None, static_cast<EImpactWeapon>(255)})
	{
		TestFalse(TEXT("Unknown weapon cannot shoot"), Ammo->CanShoot(Weapon));
		TestFalse(TEXT("Unknown weapon cannot consume"), Ammo->ConsumeRound(Weapon));
		TestFalse(TEXT("Unknown weapon cannot reload"), Ammo->BeginReload(Weapon));
		TestEqual(TEXT("Unknown weapon has no magazine"), Ammo->GetMagazine(Weapon), 0);
		TestEqual(TEXT("Unknown weapon has no reserve"), Ammo->GetReserve(Weapon), 0);
	}
	TestTrue(TEXT("Spend a rifle round"), Ammo->ConsumeRound(EImpactWeapon::Rifle));
	TestTrue(TEXT("Begin timed reload"), Ammo->BeginReload(EImpactWeapon::Rifle));
	Ammo->AdvanceReload(Ammo->GetReloadDuration() * 0.5f);
	for (const float InvalidTime : {0.0f, -1.0f, std::numeric_limits<float>::quiet_NaN(),
		std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()})
	{
		Ammo->AdvanceReload(InvalidTime);
		TestTrue(TEXT("Invalid time does not end reload"), Ammo->IsReloading());
		TestEqual(TEXT("Invalid time cannot reset or poison progress"), Ammo->GetReloadProgress(), 0.5f);
		TestEqual(TEXT("Invalid time leaves magazine untouched"), Ammo->GetMagazine(EImpactWeapon::Rifle), 29);
		TestEqual(TEXT("Invalid time leaves reserve untouched"), Ammo->GetReserve(EImpactWeapon::Rifle), 180);
	}
	Ammo->AdvanceReload(std::numeric_limits<float>::max());
	TestFalse(TEXT("Huge finite hitch completes reload"), Ammo->IsReloading());
	TestEqual(TEXT("Hitch transfers only missing rounds"), Ammo->GetReserve(EImpactWeapon::Rifle), 179);
	TestTrue(TEXT("Shoot after hitch"), Ammo->ConsumeRound(EImpactWeapon::Rifle));
	TestTrue(TEXT("Start fresh reload after hitch"), Ammo->BeginReload(EImpactWeapon::Rifle));
	TestEqual(TEXT("Excess time does not carry into next reload"), Ammo->GetReloadProgress(), 0.0f);
	Ammo->AdvanceReload(Ammo->GetReloadDuration() * 0.5f);
	TestTrue(TEXT("Next reload still needs its own full time"), Ammo->IsReloading());
	TestEqual(TEXT("Pending second reload conserves remaining rounds"), TotalAmmo(*Ammo, EImpactWeapon::Rifle), 208);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

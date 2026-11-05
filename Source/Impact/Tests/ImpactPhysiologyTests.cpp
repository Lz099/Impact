#include "../ImpactPhysiology.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include <limits>

namespace
{
	FImpactWound MakeWound(const float Bleed, const bool bExternal, const bool bPressed = false,
	                       const EImpactTissue Tissue = EImpactTissue::Muscle)
	{
		FImpactWound Wound;
		Wound.Bleed = Bleed;
		Wound.BleedFloor = Bleed;
		Wound.bExternal = bExternal;
		Wound.bPressed = bPressed;
		Wound.Tissue = Tissue;
		return Wound;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImpactInternalBleedingTest, "Impact.Physiology.InternalBleeding",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FImpactInternalBleedingTest::RunTest(const FString& Parameters)
{
	FImpactPhysiology Physiology;
	Physiology.GetWounds().Add(MakeWound(100.0f, false));
	const float Before = Physiology.GetBloodVolumeMl();
	Physiology.Tick(0.1f);

	TestTrue(TEXT("Internal bleeding removes circulating blood"), Physiology.GetBloodVolumeMl() < Before);
	TestEqual(TEXT("Internal bleeding has no visible outflow"), Physiology.GetExternalOutflow(), 0.0f);
	TestEqual(TEXT("Total rate accounts for removed blood"), Physiology.GetOutflow() * 0.1f,
		Before - Physiology.GetBloodVolumeMl(), 0.001f);
	TestEqual(TEXT("Internal wound reports its own actual rate"), Physiology.GetWounds()[0].CurrentFlow,
		Physiology.GetOutflow(), 0.001f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImpactInternalCompressionTest, "Impact.Physiology.InternalCompressionIgnored",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FImpactInternalCompressionTest::RunTest(const FString& Parameters)
{
	// Check both pressure branches: neither an internal artery nor an internal organ is compressible by a hand.
	for (const EImpactTissue Tissue : {EImpactTissue::Aorta, EImpactTissue::Liver})
	{
		FImpactPhysiology Open;
		Open.GetWounds().Add(MakeWound(100.0f, false, false, Tissue));
		FImpactPhysiology Pressed = Open;
		Pressed.GetWounds()[0].bPressed = true;
		Open.Tick(0.1f);
		Pressed.Tick(0.1f);

		TestEqual(TEXT("Hand flag cannot reduce internal blood loss"), Pressed.GetBloodVolumeMl(), Open.GetBloodVolumeMl());
		TestEqual(TEXT("Hand flag cannot reduce internal flow"), Pressed.GetWounds()[0].CurrentFlow,
			Open.GetWounds()[0].CurrentFlow);
		TestEqual(TEXT("Pressed internal wound still has no external flow"), Pressed.GetExternalOutflow(), 0.0f);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImpactExternalCompressionTest, "Impact.Physiology.ExternalCompressionReducesFlow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FImpactExternalCompressionTest::RunTest(const FString& Parameters)
{
	for (const EImpactTissue Tissue : {EImpactTissue::FemoralArtery, EImpactTissue::Muscle})
	{
		FImpactPhysiology Open;
		Open.GetWounds().Add(MakeWound(100.0f, true, false, Tissue));
		FImpactPhysiology Pressed = Open;
		Pressed.GetWounds()[0].bPressed = true;
		Open.Tick(0.1f);
		Pressed.Tick(0.1f);

		TestTrue(TEXT("Compression preserves more blood"), Pressed.GetBloodVolumeMl() > Open.GetBloodVolumeMl());
		TestTrue(TEXT("Compression reduces but does not stop external flow"),
			Pressed.GetExternalOutflow() > 0.0f && Pressed.GetExternalOutflow() < Open.GetExternalOutflow());
		// The two bodies do not lose the same amount, so they do not end the tick at the same pressure:
		// the one bleeding freely drops a little further, and the ratio between them misses the tuning
		// constant by about four hundredths of a percent (measured: 54.975586 against 54.954346). That
		// is the substepped pressure, not a change of tuning, so the tolerance is a fraction of the
		// value rather than four hundredths of a millilitre per second - still far tighter than any
		// change to 0,55 or 0,35 could hide in.
		const float ExpectedFraction = Tissue == EImpactTissue::FemoralArtery ? 0.55f : 0.35f;
		const float Expected = Open.GetExternalOutflow() * ExpectedFraction;
		TestEqual(TEXT("Existing compression tuning is preserved"), Pressed.GetExternalOutflow(),
			Expected, Expected * 0.005f);
		TestEqual(TEXT("Per-wound and aggregate visible rates agree"), Pressed.GetWounds()[0].CurrentFlow,
			Pressed.GetExternalOutflow(), 0.001f);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImpactBloodConservationTest, "Impact.Physiology.ConservationAndEmptyBody",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FImpactBloodConservationTest::RunTest(const FString& Parameters)
{
	FImpactPhysiology Physiology;
	Physiology.GetWounds().Add(MakeWound(1000000.0f, true));
	Physiology.GetWounds().Add(MakeWound(3000000.0f, false));
	FImpactPhysiology Reversed;
	Reversed.GetWounds().Add(Physiology.GetWounds()[1]);
	Reversed.GetWounds().Add(Physiology.GetWounds()[0]);
	const float InitialVolume = Physiology.GetBloodVolumeMl();
	const float Dt = 0.05f;
	Physiology.Tick(Dt);
	Reversed.Tick(Dt);

	TestEqual(TEXT("Fresh body begins at capacity"), InitialVolume, Physiology.GetBloodCapacityMl());
	TestEqual(TEXT("Catastrophic loss stops at zero blood"), Physiology.GetBloodVolumeMl(), 0.0f);
	TestEqual(TEXT("Exhausting tick reports only the remaining blood"), Physiology.GetOutflow() * Dt, InitialVolume, 0.001f);
	TestEqual(TEXT("Wounds share remaining blood proportionally"), Physiology.GetExternalOutflow() * Dt,
		InitialVolume * 0.25f, 0.001f);
	TestEqual(TEXT("Per-wound losses sum to actual loss"),
		(Physiology.GetWounds()[0].CurrentFlow + Physiology.GetWounds()[1].CurrentFlow) * Dt, InitialVolume, 0.001f);
	TestEqual(TEXT("Wound order does not favour internal or external bleeding"), Reversed.GetExternalOutflow(),
		Physiology.GetExternalOutflow(), 0.001f);
	TestEqual(TEXT("Individual external allocation is independent of order"), Reversed.GetWounds()[1].CurrentFlow,
		Physiology.GetWounds()[0].CurrentFlow, 0.001f);

	for (int32 Tick = 0; Tick < 3; ++Tick)
	{
		Physiology.Tick(Dt);
		TestEqual(TEXT("Empty body stays empty"), Physiology.GetBloodVolumeMl(), 0.0f);
		TestEqual(TEXT("Empty body has no total outflow"), Physiology.GetOutflow(), 0.0f);
		TestEqual(TEXT("Empty body has no visible outflow"), Physiology.GetExternalOutflow(), 0.0f);
		for (const FImpactWound& Wound : Physiology.GetWounds())
		{
			TestEqual(TEXT("No wound emits after blood is exhausted"), Wound.CurrentFlow, 0.0f);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImpactFlowTimeScaleTest, "Impact.Physiology.FlowUsesUnscaledDeltaTime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FImpactFlowTimeScaleTest::RunTest(const FString& Parameters)
{
	// This test used to compare against round numbers - 60 mL lost, 300 mL/s, 200 mL/s - and they came
	// out exact because Tick was one single step: Dt = DeltaTime * TimeScale, with the circulation read
	// once before any blood had left. Tick is now cut into substeps of 50 ms, and the pressure that
	// pushes the blood out is read again at each one from the blood still in the body. So a wound bleeds
	// a little less at the end of a frame than at its start, which is right, and the round numbers are
	// no longer reachable: measured 59.486816 instead of 60, and the same factor 0.991447 on all five.
	//
	// Chasing that factor with a loose tolerance would weaken what this test is for - catching TimeScale
	// applied to the wrong term, which is a factor-of-two class of mistake. So the comparison is against
	// a WITNESS instead: the same body, TimeScale 1, ticked over twice the frame. Both see the same 0,4 s
	// of simulated time, so the same eight substeps and the same pressure decay, and what is left between
	// them is exactly what the test is named after - TimeScale multiplies simulated time, and the rates
	// are reported over the CALLER's frame, not the simulated one.
	FImpactPhysiology Scaled;
	Scaled.GetWounds().Add(MakeWound(100.0f, true));
	Scaled.GetWounds().Add(MakeWound(50.0f, false));
	FImpactPhysiology Witness = Scaled;
	Scaled.TimeScale = 2.0f;
	Witness.TimeScale = 1.0f;

	const float Before = Scaled.GetBloodVolumeMl();
	const float WitnessBefore = Witness.GetBloodVolumeMl();
	Scaled.Tick(0.2f);
	Witness.Tick(0.4f);

	const float Lost = Before - Scaled.GetBloodVolumeMl();
	const float WitnessLost = WitnessBefore - Witness.GetBloodVolumeMl();
	TestEqual(TEXT("TimeScale 2 over half a frame loses exactly what TimeScale 1 loses over the whole one"),
		Lost, WitnessLost, 0.0001f);
	TestEqual(TEXT("Total actual rate is reported over the caller's frame, so TimeScale doubles it"),
		Scaled.GetOutflow(), Witness.GetOutflow() * 2.0f, 0.0001f);
	TestEqual(TEXT("External actual rate likewise"),
		Scaled.GetExternalOutflow(), Witness.GetExternalOutflow() * 2.0f, 0.0001f);
	TestEqual(TEXT("External wound uses caller's frame duration"),
		Scaled.GetWounds()[0].CurrentFlow * 0.2f, Witness.GetWounds()[0].CurrentFlow * 0.4f, 0.0001f);
	TestEqual(TEXT("Internal wound uses caller's frame duration"),
		Scaled.GetWounds()[1].CurrentFlow * 0.2f, Witness.GetWounds()[1].CurrentFlow * 0.4f, 0.0001f);

	// And the loss still has to be the right size: two thirds of it external, within the percent the
	// pressure decay costs. Without this the witness alone would pass on a body that bled nothing.
	TestTrue(TEXT("The loss is the nominal 60 mL less the pressure decay, not something else"),
		Lost > 59.0f && Lost <= 60.0f);
	TestEqual(TEXT("Two thirds of what is lost comes out of the external wound"),
		Scaled.GetExternalOutflow() / Scaled.GetOutflow(), 2.0f / 3.0f, 0.001f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImpactInvalidTimeTest, "Impact.Physiology.InvalidTimeClearsFlows",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FImpactInvalidTimeTest::RunTest(const FString& Parameters)
{
	FImpactPhysiology Baseline;
	Baseline.GetWounds().Add(MakeWound(100.0f, true));
	Baseline.Tick(0.1f);
	TestTrue(TEXT("Test begins with a nonzero reported flow"), Baseline.GetOutflow() > 0.0f);
	const float InvalidTimes[] = {0.0f, -1.0f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()};
	for (const float Invalid : InvalidTimes)
	{
		for (const bool bInvalidScale : {false, true})
		{
			FImpactPhysiology Physiology = Baseline;
			if (bInvalidScale)
			{
				Physiology.TimeScale = Invalid;
			}
			Physiology.Tick(bInvalidScale ? 0.1f : Invalid);
			TestEqual(TEXT("Invalid time preserves blood volume"), Physiology.GetBloodVolumeMl(), Baseline.GetBloodVolumeMl());
			TestEqual(TEXT("Invalid time preserves oxygen"), Physiology.GetBrainOxygen(), Baseline.GetBrainOxygen());
			TestEqual(TEXT("Invalid time clears stale aggregate flow"), Physiology.GetOutflow(), 0.0f);
			TestEqual(TEXT("Invalid time clears stale external flow"), Physiology.GetExternalOutflow(), 0.0f);
			TestEqual(TEXT("Invalid time clears stale per-wound flow"), Physiology.GetWounds()[0].CurrentFlow, 0.0f);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImpactBoundedTimeTest, "Impact.Physiology.BoundedTimeAndCoagulation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FImpactBoundedTimeTest::RunTest(const FString& Parameters)
{
	FImpactPhysiology Baseline;
	Baseline.GetWounds().Add(MakeWound(100.0f, true));
	Baseline.GetWounds()[0].ClotTime = 90.0f;
	Baseline.GetWounds()[0].BleedFloor = 5.0f;
	FImpactPhysiology Reference = Baseline;
	for (int32 Step = 0; Step < 20; ++Step)
	{
		Reference.Tick(0.05f);
	}
	TestTrue(TEXT("Small wounds still clot"), Reference.GetWounds()[0].Bleed < Baseline.GetWounds()[0].Bleed);
	TestTrue(TEXT("Coagulation respects its existing floor"), Reference.GetWounds()[0].Bleed >= 5.0f);

	// Both a frame hitch and a test-time multiplier are capped, with no deferred simulation debt.
	for (const bool bAccelerated : {false, true})
	{
		FImpactPhysiology Physiology = Baseline;
		Physiology.TimeScale = bAccelerated ? 100.0f : 1.0f;
		const float FrameDt = bAccelerated ? 0.1f : 5.0f;
		Physiology.Tick(FrameDt);
		TestEqual(TEXT("Capped tick matches one simulated second"), Physiology.GetBloodVolumeMl(), Reference.GetBloodVolumeMl(), 0.01f);
		TestEqual(TEXT("Coagulation matches bounded substeps"), Physiology.GetWounds()[0].Bleed, Reference.GetWounds()[0].Bleed, 0.001f);
		TestEqual(TEXT("Capped tick rates still account for actual removed volume"), Physiology.GetOutflow() * FrameDt,
			Baseline.GetBloodVolumeMl() - Physiology.GetBloodVolumeMl(), 0.01f);
		TestEqual(TEXT("Capped per-wound rate uses the same duration"), Physiology.GetWounds()[0].CurrentFlow,
			Physiology.GetOutflow(), 0.001f);
		Physiology.TimeScale = 1.0f;
		FImpactPhysiology NextReference = Reference;
		Physiology.Tick(0.05f);
		NextReference.Tick(0.05f);
		TestEqual(TEXT("Excess time is not replayed on the next tick"), Physiology.GetBloodVolumeMl(), NextReference.GetBloodVolumeMl(), 0.01f);
	}

	FImpactPhysiology Extreme = Baseline;
	Extreme.TimeScale = std::numeric_limits<float>::max();
	Extreme.Tick(std::numeric_limits<float>::max());
	TestEqual(TEXT("Extreme finite time cannot overflow the simulation"), Extreme.GetBloodVolumeMl(), Reference.GetBloodVolumeMl(), 0.01f);
	TestTrue(TEXT("Extreme finite time still reports a finite rate"), FMath::IsFinite(Extreme.GetOutflow()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImpactHeartStopsTest, "Impact.Physiology.CardiacArrestAndDeathStopPulse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FImpactHeartStopsTest::RunTest(const FString& Parameters)
{
	FImpactPhysiology Physiology;
	Physiology.Tick(0.1f);
	TestTrue(TEXT("Living heart starts with a rate"), Physiology.GetHeartRate() > 0.0f);
	TestTrue(TEXT("Living heart produces a pulse"), Physiology.GetPulse() > 0.0f);
	Physiology.GetWounds().Add(MakeWound(1000000.0f, true));
	Physiology.Tick(0.1f);
	for (int32 Step = 0; Step < 220; ++Step)
	{
		Physiology.Tick(0.05f);
	}
	TestFalse(TEXT("Cardiac arrest precedes delayed death in the game model"), Physiology.IsDead());
	TestEqual(TEXT("Arrested heart has zero rate"), Physiology.GetHeartRate(), 0.0f);
	TestEqual(TEXT("Arrested heart has zero pulse"), Physiology.GetPulse(), 0.0f);

	for (int32 Step = 0; Step < 1600 && !Physiology.IsDead(); ++Step)
	{
		Physiology.Tick(0.05f);
	}
	TestTrue(TEXT("Prolonged anoxia reaches death"), Physiology.IsDead());
	TestTrue(TEXT("Death is also exposed as consciousness state"), Physiology.GetConsciousness() == EImpactConsciousness::Dead);
	TestEqual(TEXT("Death tick reports zero heart rate"), Physiology.GetHeartRate(), 0.0f);
	TestEqual(TEXT("Death tick reports zero pulse"), Physiology.GetPulse(), 0.0f);
	Physiology.Tick(0.1f);
	TestEqual(TEXT("Rate stays zero after death"), Physiology.GetHeartRate(), 0.0f);
	TestEqual(TEXT("Pulse stays zero after death"), Physiology.GetPulse(), 0.0f);
	TestEqual(TEXT("Empty corpse cannot emit visible blood"), Physiology.GetExternalOutflow(), 0.0f);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

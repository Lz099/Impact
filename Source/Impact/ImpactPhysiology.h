#pragma once

#include "CoreMinimal.h"
#include "ImpactAnatomy.h"

/** How awake a person is, from the oxygen reaching the brain. */
enum class EImpactConsciousness : uint8
{
	Conscious,
	Confused,       // dizzy, staggering
	SemiConscious,  // on the ground, moaning, weak movements
	Unconscious,    // limp; breathing still depends on the injuries
	Dead
};

struct FImpactWound
{
	FString Name;               // "artère fémorale gauche"
	EImpactTissue Tissue = EImpactTissue::Muscle;
	FName Bone;                 // body part the wound is on (for the hand and the blood)
	FVector LocalPoint = FVector::ZeroVector; // in that bone's space
	float Bleed = 0.0f;         // potential mL/s before pressure, compression and available volume
	float CurrentFlow = 0.0f;   // actual mL/s averaged over the last Tick's unscaled DeltaTime
	float BleedFloor = 0.0f;    // what is left once clotted
	float ClotTime = 0.0f;      // s, 0 = does not clot (arteries)
	float Pain = 0.0f;          // 0..1
	bool bExternal = true;      // blood comes out (otherwise inside the body)
	bool bPressed = false;      // a hand is on it
	float Spread = 0.0f;       // cm, how far the blood has spread on the skin round this hole
	bool bArterial = false;     // spurts with the heartbeat instead of running out steadily
	float PressedTime = 0.0f;
	bool bClosed = false;
};

/**
 * What the wounds do to the body over time: 5 litres of blood leaving through each wound at its own rate,
 * oxygen reaching the brain (heart, lungs, blood volume), consciousness, death,
 * and what each limb can still do (broken bones, cut nerves, spinal cord).
 * Blood loss, oxygen thresholds and delays are game tuning, not a medically validated model.
 */
class IMPACT_API FImpactPhysiology
{
public:
	/** Wounds caused by one bullet path through the body. Returns a short French summary ("fémur gauche cassé"). */
	FString ApplyBullet(const FImpactBulletPath& Path, const FImpactAnatomy& Anatomy, const USkeletalMeshComponent* Mesh);

	/** Advances at most 1 simulated second, in steps <= 0.05 s. Invalid/near-zero dt or invalid/nonpositive
	 * TimeScale does not advance the model and clears last-tick flows. Excess time is discarded, not queued. */
	void Tick(float DeltaTime);

	float GetBloodFraction() const { return BloodMl / BloodMaxMl; }
	float GetBloodLossFraction() const { return 1.0f - GetBloodFraction(); }
	float GetBloodVolumeMl() const { return BloodMl; }
	float GetBloodCapacityMl() const { return BloodMaxMl; }
	/** Potential total before pressure, compression and remaining blood; use actual flows for FX. */
	float GetBleedRate() const;

	/** Actual internal + external loss / caller's DeltaTime, including TimeScale and the simulation cap.
	 * The tick that empties the body reports its final removed volume; subsequent ticks report zero. */
	float GetOutflow() const { return Outflow; }
	/** External part of GetOutflow(), in the same units; internal bleeding must not feed visible blood FX. */
	float GetExternalOutflow() const { return ExternalOutflow; }
	EImpactConsciousness GetConsciousness() const;
	bool IsDead() const { return bDead; }
	/** Benches only: dead now, from this cause. */
	void KillNow(const FString& Cause) { Kill(Cause); }
	EImpactSpineLevel GetSpinalLesion() const { return SpinalLesion; }
	float GetLimbCapability(EImpactLimb Limb) const { return LimbCapability[static_cast<int32>(Limb)]; }
	float GetPain() const { return Pain; }

	/** Breathing, 0..1. A man with a hole in each lung cannot run, whatever his legs are doing. */
	float GetLungFunction() const { return LungFunction; }

	/** Pumping, 0..1. */
	float GetHeartFunction() const { return HeartFunction; }

	/**
	 * What a wrecked trunk does to a man's ability to stay on his feet and keep moving, 0 to 1. Breathing,
	 * the pump, and the pain of the wound itself - pain weighing most of the three.
	 *
	 * Pain used to count for a fifth, and that made a gut shot almost free: it touches no lung, so the man
	 * kept his balance, ran off, and only went down later when the blood ran out - by which time he was
	 * unconscious and there was nobody awake on the ground to curl up. Measured on the chest, where the
	 * lungs do the work: down at 95 % of blood with a brain at 0,93, four seconds of lying there awake,
	 * then back on his feet. A hole in the belly should do the same, and it does it through pain.
	 * Measured before this existed: twenty-one pistol rounds through the chest left the balance capacity
	 * at its maximum of 140 cm, because that figure was computed from the legs alone. Reported from play:
	 * "je crible le torse d'un gars, il se tourne normal et il court super vite".
	 */
	float GetTrunkTrauma() const
	{
		static float PainWeight = -1.0f;
		if (PainWeight < 0.0f)
		{
			float Parsed = 0.45f;
			PainWeight = FParse::Value(FCommandLine::Get(), TEXT("ImpactDouleurPoids="), Parsed) ? FMath::Clamp(Parsed, 0.0f, 1.0f) : 0.45f;
		}
		return FMath::Clamp((1.0f - LungFunction) * 0.4f + (1.0f - HeartFunction) * 0.25f
		                    + FMath::Clamp(Pain, 0.0f, 1.0f) * PainWeight, 0.0f, 1.0f);
	}
	float GetBrainOxygen() const { return BrainOxygen; }
	const TArray<FImpactWound>& GetWounds() const { return Wounds; }
	TArray<FImpactWound>& GetWounds() { return Wounds; }

	/** The most dangerous wound still bleeding outside, or INDEX_NONE. */
	int32 GetWorstExternalWound() const;

	/** Label text: "Sang 64 % · confus · fémur gauche cassé". */
	FString Describe() const;

	FString GetCauseOfDeath() const { return CauseOfDeath; }

	/**
	 * Test hook only (-ImpactParalyse): take a limb away without a bullet, so the behaviour that follows can be
	 * checked on its own. Aiming a round through a spinal cord is its own problem and should not hide this one.
	 */
	void ForceLimbLoss(const EImpactLimb Limb, const float Capability, const FString& Reason) { ReduceLimb(Limb, Capability, Reason); }

	/** Beats per minute now: climbs as blood is lost, which is what makes an artery spurt faster as it empties. */
	float GetHeartRate() const { return HeartRate; }

	/** Rises and falls with each beat (0 between beats, 1 at the peak). */
	float GetPulse() const { return Pulse; }

	/** Multiplies how fast blood is lost and the brain runs out of oxygen (1 = real time). */
	float TimeScale = 1.0f;

private:
	void AddWound(const FImpactOrgan& Organ, const FImpactPathHit& Hit, const FImpactBulletPath& Path, const USkeletalMeshComponent* Mesh,
	              float EnergyFactor, TArray<FString>& Notes);
	void ReduceLimb(EImpactLimb Limb, float Capability, const FString& Reason);
	void Kill(const FString& Cause);

	float BloodMaxMl = 5000.0f;
	float BloodMl = 5000.0f;
	float BrainOxygen = 1.0f;     // 0..1
	float HeartFunction = 1.0f;   // pumping, 0..1
	float HeartRate = 75.0f;      // beats per minute
	float PulsePhase = 0.0f;
	float Pulse = 0.0f;
	float LungFunction = 1.0f;    // oxygen taken in, 0..1
	float BrainSupplyFactor = 1.0f; // cut carotids
	float AnoxiaTime = 0.0f;      // s without enough oxygen
	float Outflow = 0.0f;
	float ExternalOutflow = 0.0f;
	float Pain = 0.0f;
	bool bDead = false;
	bool bBreathing = true;
	EImpactSpineLevel SpinalLesion = EImpactSpineLevel::None;
	float LimbCapability[static_cast<int32>(EImpactLimb::Count)] = {1.0f, 1.0f, 1.0f, 1.0f};
	TArray<FString> LimbNotes;    // "fémur gauche cassé"
	TArray<FImpactWound> Wounds;
	FString CauseOfDeath;
};

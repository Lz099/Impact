#include "ImpactPhysiology.h"

#include "Impact.h"

#include "Components/SkeletalMeshComponent.h"

namespace
{
	using T = EImpactTissue;

	struct FTissueEffect
	{
		float Bleed;     // mL/s for a pistol-sized wound
		float ClotTime;  // s, 0 = keeps bleeding
		float Pain;
		bool bExternal;
	};

	// Measured before these numbers were raised: a lung took 58 MINUTES to cost 30 % of the blood, a stomach
	// two hours, a flesh wound eleven. It takes about 27 mL/s sustained to put someone on the ground in ninety
	// seconds, which is the range a game needs. Solid organs were between nine and eleven times too low.
	FTissueEffect GetEffect(const EImpactTissue Tissue)
	{
		switch (Tissue)
		{
		case T::Skull: return {1.2f, 120.0f, 0.8f, true};
		case T::Jaw: return {2.0f, 120.0f, 0.9f, true};
		case T::SpinalCord: return {0.3f, 300.0f, 0.6f, false};
		case T::Vertebra: return {1.0f, 200.0f, 0.9f, false};
		case T::Trachea: return {1.5f, 300.0f, 0.7f, false};
		case T::Carotid: return {40.0f, 0.0f, 0.6f, true};
		case T::Jugular: return {14.0f, 0.0f, 0.6f, true};
		case T::Heart: return {60.0f, 0.0f, 0.9f, false};
		case T::Aorta: return {160.0f, 0.0f, 0.8f, false};
		case T::VenaCava: return {70.0f, 0.0f, 0.8f, false};
		case T::Lung: return {13.0f, 400.0f, 0.7f, false};
		case T::Rib: return {1.0f, 200.0f, 0.8f, false};
		case T::Liver: return {34.0f, 0.0f, 0.8f, false};
		case T::Spleen: return {28.0f, 0.0f, 0.8f, false};
		case T::Stomach: return {9.0f, 300.0f, 0.9f, false};
		case T::Intestines: return {8.0f, 300.0f, 0.9f, false};
		case T::Kidney: return {21.0f, 600.0f, 1.0f, false};
		case T::Bladder: return {1.5f, 300.0f, 0.8f, false};
		case T::PelvisBone: return {11.0f, 900.0f, 1.0f, false};
		case T::IliacArtery: return {35.0f, 0.0f, 0.8f, false};
		case T::Femur: return {9.0f, 900.0f, 1.0f, false};
		case T::FemoralArtery: return {26.0f, 0.0f, 0.7f, true};
		case T::Knee: return {2.0f, 300.0f, 1.0f, true};
		case T::Tibia: return {2.5f, 400.0f, 1.0f, true};
		case T::Foot: return {1.0f, 200.0f, 0.8f, true};
		case T::Clavicle: return {1.5f, 300.0f, 0.8f, true};
		case T::SubclavianArtery: return {22.0f, 0.0f, 0.7f, true};
		case T::ShoulderJoint: return {2.0f, 300.0f, 0.9f, true};
		case T::Humerus: return {7.0f, 600.0f, 1.0f, false};
		case T::BrachialArtery: return {11.0f, 0.0f, 0.6f, true};
		case T::Elbow: return {1.5f, 300.0f, 0.9f, true};
		case T::Forearm: return {1.5f, 300.0f, 0.8f, true};
		case T::Hand: return {1.2f, 200.0f, 0.8f, true};
		default: return {0.8f, 90.0f, 0.5f, true};
		}
	}

	bool IsArtery(const EImpactTissue Tissue)
	{
		return Tissue == T::Carotid || Tissue == T::Aorta || Tissue == T::IliacArtery || Tissue == T::FemoralArtery
		       || Tissue == T::SubclavianArtery || Tissue == T::BrachialArtery;
	}

	EImpactLimb LimbOf(const FName Bone)
	{
		const FString Name = Bone.ToString();
		const bool bLeft = Name.EndsWith(TEXT("_l"));
		if (Name.StartsWith(TEXT("thigh")) || Name.StartsWith(TEXT("calf")) || Name.StartsWith(TEXT("foot")) || Name.StartsWith(TEXT("ball")))
		{
			return bLeft ? EImpactLimb::LeftLeg : EImpactLimb::RightLeg;
		}
		if (Name.StartsWith(TEXT("clavicle")) || Name.StartsWith(TEXT("upperarm")) || Name.StartsWith(TEXT("lowerarm")) || Name.StartsWith(TEXT("hand")))
		{
			return bLeft ? EImpactLimb::LeftArm : EImpactLimb::RightArm;
		}
		return EImpactLimb::Count;
	}

	const TCHAR* PartName(const FName Bone)
	{
		const FString Name = Bone.ToString();
		if (Name == TEXT("head")) return TEXT("tête");
		if (Name.StartsWith(TEXT("neck"))) return TEXT("cou");
		if (Name.StartsWith(TEXT("spine")) || Name == TEXT("pelvis")) return TEXT("torse");
		if (Name.StartsWith(TEXT("thigh"))) return TEXT("cuisse");
		if (Name.StartsWith(TEXT("calf"))) return TEXT("jambe");
		if (Name.StartsWith(TEXT("foot")) || Name.StartsWith(TEXT("ball"))) return TEXT("pied");
		if (Name.StartsWith(TEXT("upperarm")) || Name.StartsWith(TEXT("clavicle"))) return TEXT("bras");
		if (Name.StartsWith(TEXT("lowerarm"))) return TEXT("avant-bras");
		if (Name.StartsWith(TEXT("hand"))) return TEXT("main");
		return TEXT("corps");
	}

	FVector ToBoneSpace(const USkeletalMeshComponent* Mesh, const FName Bone, const FVector& World)
	{
		return Mesh->GetSocketTransform(Bone, RTS_World).InverseTransformPosition(World);
	}

	// How the blood that leaves a hole spreads on the skin round it: the stained area grows with the volume that
	// came out (-ImpactTacheK=, cm2 per mL), up to a stain of -ImpactTacheMax= cm. Game tuning, not a measurement:
	// a belly hole losing about 130 mL in its first ten seconds reaches about 14 cm. At 0.6 cm2 per mL the stains
	// were 5 to 9 cm and hidden under the hand holding the wound.
	void GetSkinStainTuning(float& OutAreaPerMl, float& OutMax)
	{
		static float AreaPerMl = -1.0f;
		static float Max = -1.0f;
		if (AreaPerMl < 0.0f)
		{
			float Parsed = 1.5f;
			AreaPerMl = FParse::Value(FCommandLine::Get(), TEXT("ImpactTacheK="), Parsed) ? FMath::Max(0.0f, Parsed) : 1.5f;
			Parsed = 18.0f;
			Max = FParse::Value(FCommandLine::Get(), TEXT("ImpactTacheMax="), Parsed) ? FMath::Max(1.0f, Parsed) : 18.0f;
		}
		OutAreaPerMl = AreaPerMl;
		OutMax = Max;
	}
}

void FImpactPhysiology::ReduceLimb(const EImpactLimb Limb, const float Capability, const FString& Reason)
{
	if (Limb == EImpactLimb::Count)
	{
		return;
	}
	float& Current = LimbCapability[static_cast<int32>(Limb)];
	if (Capability < Current)
	{
		Current = Capability;
		if (!Reason.IsEmpty())
		{
			LimbNotes.AddUnique(Reason);
		}
	}
}

void FImpactPhysiology::Kill(const FString& Cause)
{
	if (!bDead)
	{
		bDead = true;
		CauseOfDeath = Cause;
		HeartFunction = 0.0f;
		HeartRate = 0.0f;
		PulsePhase = 0.0f;
		Pulse = 0.0f;
		BrainOxygen = 0.0f;
	}
}

void FImpactPhysiology::AddWound(const FImpactOrgan& Organ, const FImpactPathHit& Hit, const FImpactBulletPath& Path, const USkeletalMeshComponent* Mesh,
                                 const float EnergyFactor, TArray<FString>& Notes)
{
	const FTissueEffect Effect = GetEffect(Organ.Tissue);
	const EImpactLimb Limb = LimbOf(Organ.Anchor);

	FImpactWound Wound;
	Wound.Name = Organ.Name;
	Wound.Tissue = Organ.Tissue;
	Wound.Bone = Organ.Anchor;
	Wound.LocalPoint = ToBoneSpace(Mesh, Organ.Anchor, Hit.Point);
	Wound.Bleed = Effect.Bleed * EnergyFactor;
	Wound.ClotTime = Effect.ClotTime;
	// An artery (no clot time) keeps its full rate; anything that clots goes to nothing. The old floor of
	// 5 % of the initial rate meant no wound ever closed: a torn liver held 1,7 mL/s for ever and the
	// smallest hole never stopped dripping. Reported from play: "une plaie pisse toujours le sang".
	Wound.BleedFloor = Effect.ClotTime > 0.0f ? 0.0f : Wound.Bleed;
	Wound.Pain = Effect.Pain;
	Wound.bExternal = Effect.bExternal;
	Wound.bArterial = IsArtery(Organ.Tissue);

	// Blood from a cut vessel comes out through the nearest bullet hole: that is where it drips and where a hand presses.
	if (Wound.bExternal)
	{
		const bool bUseExit = Path.bExited && FVector::DistSquared(Hit.Point, Path.Exit) < FVector::DistSquared(Hit.Point, Path.Entry);
		Wound.Bone = bUseExit ? Path.ExitBone : Path.EntryBone;
		Wound.LocalPoint = ToBoneSpace(Mesh, Wound.Bone, bUseExit ? Path.Exit : Path.Entry);
	}

	switch (Organ.Tissue)
	{
	case T::Brain:
	case T::Brainstem:
		Kill(Organ.Name + TEXT(" détruit"));
		Notes.Add(Organ.Name);
		return;
	case T::Skull:
		BrainOxygen = FMath::Min(BrainOxygen, 0.25f); // knocked out
		Notes.Add(TEXT("crâne touché"));
		break;
	case T::SpinalCord:
		SpinalLesion = FMath::Max(SpinalLesion, Organ.SpineLevel);
		Notes.Add(Organ.Name + TEXT(" coupée"));
		break;
	case T::Heart:
		HeartFunction = FMath::Min(HeartFunction, 0.05f);
		Notes.Add(TEXT("cœur"));
		break;
	case T::Lung:
		LungFunction *= 0.55f;
		Notes.Add(Organ.Name);
		break;
	case T::Trachea:
		LungFunction *= 0.5f;
		Notes.Add(Organ.Name);
		break;
	case T::Carotid:
		BrainSupplyFactor *= 0.6f;
		Notes.Add(Organ.Name);
		break;
	case T::PelvisBone:
		ReduceLimb(EImpactLimb::LeftLeg, 0.35f, TEXT("bassin fracturé"));
		ReduceLimb(EImpactLimb::RightLeg, 0.35f, TEXT("bassin fracturé"));
		Notes.Add(Organ.Name + TEXT(" fracturé"));
		break;
	case T::Femur:
		ReduceLimb(Limb, 0.0f, Organ.Name + TEXT(" cassé"));
		Notes.Add(Organ.Name + TEXT(" cassé"));
		break;
	case T::Tibia:
		ReduceLimb(Limb, 0.1f, Organ.Name + TEXT(" cassé"));
		Notes.Add(Organ.Name + TEXT(" cassé"));
		break;
	case T::Knee:
		ReduceLimb(Limb, 0.2f, Organ.Name + TEXT(" touché"));
		Notes.Add(Organ.Name);
		break;
	case T::Foot:
		ReduceLimb(Limb, 0.5f, Organ.Name + TEXT(" touché"));
		Notes.Add(Organ.Name);
		break;
	case T::Humerus:
		ReduceLimb(Limb, 0.0f, Organ.Name + TEXT(" cassé"));
		Notes.Add(Organ.Name + TEXT(" cassé"));
		break;
	case T::ShoulderJoint:
		ReduceLimb(Limb, 0.15f, Organ.Name + TEXT(" touchée"));
		Notes.Add(Organ.Name);
		break;
	case T::Clavicle:
		ReduceLimb(Limb, 0.3f, Organ.Name + TEXT(" cassée"));
		Notes.Add(Organ.Name + TEXT(" cassée"));
		break;
	case T::Elbow:
		ReduceLimb(Limb, 0.25f, Organ.Name + TEXT(" touché"));
		Notes.Add(Organ.Name);
		break;
	case T::Forearm:
		ReduceLimb(Limb, 0.35f, Organ.Name + TEXT(" cassé"));
		Notes.Add(Organ.Name + TEXT(" cassé"));
		break;
	case T::Hand:
		ReduceLimb(Limb, 0.6f, FString());
		Notes.Add(Organ.Name);
		break;
	default:
		Notes.Add(Organ.Name);
		break;
	}

	// Where this wound ends up living matters as much as the damage: it decides where blood comes out and where
	// a hand goes. A shoulder hit that lands on a torso body would send the hand to the belly.
	UE_LOG(LogImpact, Display, TEXT("[plaie] %s : structure %s, trou sur l'os %s, %.1f mL/s, %s"),
	       *Wound.Name, *Organ.Name, *Wound.Bone.ToString(), Wound.Bleed, Wound.bExternal ? TEXT("externe") : TEXT("interne"));

	Wounds.Add(Wound);
}

FString FImpactPhysiology::ApplyBullet(const FImpactBulletPath& Path, const FImpactAnatomy& Anatomy, const USkeletalMeshComponent* Mesh)
{
	TArray<FString> Notes;
	const float EnergyFactor = FMath::Clamp(0.7f + Path.EnergyIn / 2000.0f, 0.7f, 1.6f);
	const TArray<FImpactOrgan>& Organs = Anatomy.GetOrgans();

	// The holes themselves: entry, and a larger exit if the bullet went through.
	const auto Hole = [&](const FName Bone, const FVector& Point, const float Size, const TCHAR* Kind)
	{
		FImpactWound Wound;
		Wound.Name = FString::Printf(TEXT("plaie %s (%s)"), PartName(Bone), Kind);
		Wound.Bone = Bone;
		Wound.LocalPoint = ToBoneSpace(Mesh, Bone, Point);
		// A bullet hole through muscle is not a scratch: a thigh shot that misses the artery costs a
		// person several hundred millilitres before it closes. Measured at the old 0,8 over a 90 s clot:
		// a through-and-through thigh lost 95 mL in all, under 2 % of the blood, so the meter stayed at
		// 99 % while the man held his leg. Reported from play: "le sang reste a 99 super longtemps".
		//
		// Total loss is roughly the rate times the clot time, so the two are chosen together: 5,0 for the
		// entry and 8,75 for the exit over 35 s is about 480 mL, near a tenth of the blood, which is the
		// order a limb wound should be. The short clot time also matters on its own - bleeding is heaviest
		// in the first seconds and slows fast, and a long one left the wound visibly dripping for minutes.
		Wound.Bleed = 5.0f * Size * EnergyFactor;
		Wound.ClotTime = 35.0f;
		// It slows, it does not seal. Left to itself a bullet hole goes on losing about an eighth of what
		// it lost in the first seconds, which is a little under one per cent of the blood a minute: slow
		// enough to walk away from, fast enough that it still matters half an hour later. A wound that
		// shut itself in a hundred seconds was reported from play as "elle se referme toute seule, c'est
		// pas logique". What does close it is a hand held on it, and that is handled in Update.
		Wound.BleedFloor = Wound.Bleed * 0.12f;
		Wound.Pain = 0.5f;
		// The hole shows at once, before any blood has run: the exit, being larger, a little more.
		Wound.Spread = 1.0f + 0.6f * Size;
		UE_LOG(LogImpact, Display, TEXT("[plaie] %s : os %s, hauteur %.0f cm, %.1f mL/s"), *Wound.Name, *Bone.ToString(),
		       Point.Z - Mesh->GetComponentLocation().Z, Wound.Bleed);
		Wounds.Add(Wound);
	};
	const int32 EntryHole = Wounds.Num();
	Hole(Path.EntryBone, Path.Entry, 1.0f, TEXT("entrée"));
	int32 ExitHole = INDEX_NONE;
	if (Path.bExited)
	{
		ExitHole = Wounds.Num();
		Hole(Path.ExitBone, Path.Exit, 1.8f, TEXT("sortie"));
	}
	const float BleedBeforeOrgans = GetBleedRate();

	// Muscle damage weakens the limb even without a broken bone: a thigh wound makes a limp, two arm wounds make it hang.
	const EImpactLimb HitLimb = LimbOf(Path.EntryBone);
	if (HitLimb != EImpactLimb::Count)
	{
		// A bullet through muscle takes a quarter of what the limb could still do. It used to take forty
		// per cent, so two rounds in an arm left 0.36 and it was already useless: reported from play as
		// "ils perdent leur bras super vite". Three now make it hard to use and four make it useless,
		// which is the order a limb that is only torn - not broken - should go in. A bone through the
		// same limb is handled separately and still ruins it outright.
		const bool bArm = HitLimb == EImpactLimb::LeftArm || HitLimb == EImpactLimb::RightArm;
		const float Current = LimbCapability[static_cast<int32>(HitLimb)];
		ReduceLimb(HitLimb, Current * (bArm ? 0.75f : 0.7f), FString());
	}

	for (const FImpactPathHit& Hit : Path.Hits)
	{
		AddWound(Organs[Hit.Organ], Hit, Path, Mesh, EnergyFactor, Notes);
	}

	// Blood from a torn organ does not stay politely inside. Part of it runs out along the track the bullet
	// made, and that is the only blood anyone ever sees from a shot to the body. Without this, every organ in
	// the trunk was marked internal and produced no drop, no stain, no pool and no hand on the wound: three
	// hundred and seventy millilitres a second of bleeding that was completely invisible. Reported from play
	// as "je crible quelqu'un de balles et il n'y a pas de sang".
	const float Internal = FMath::Max(0.0f, GetBleedRate() - BleedBeforeOrgans);
	if (Internal > 0.1f)
	{
		const int32 Track = ExitHole != INDEX_NONE ? ExitHole : EntryHole;
		if (Wounds.IsValidIndex(Track))
		{
			constexpr float RunsOut = 0.3f;
			Wounds[Track].Bleed += Internal * RunsOut;
			Wounds[Track].BleedFloor = Wounds[Track].Bleed * 0.05f;
			UE_LOG(LogImpact, Display, TEXT("[plaie] %s : s'écoule aussi par le trajet, +%.1f mL/s (total %.1f)"),
			       *Wounds[Track].Name, Internal * RunsOut, Wounds[Track].Bleed);
		}
	}

	switch (SpinalLesion)
	{
	case EImpactSpineLevel::CervicalHigh:
		bBreathing = false;
		for (float& Capability : LimbCapability)
		{
			Capability = 0.0f;
		}
		LimbNotes.AddUnique(TEXT("tétraplégique"));
		break;
	case EImpactSpineLevel::CervicalLow:
		LungFunction = FMath::Min(LungFunction, 0.5f);
		ReduceLimb(EImpactLimb::LeftArm, 0.15f, FString());
		ReduceLimb(EImpactLimb::RightArm, 0.15f, FString());
		ReduceLimb(EImpactLimb::LeftLeg, 0.0f, FString());
		ReduceLimb(EImpactLimb::RightLeg, 0.0f, FString());
		LimbNotes.AddUnique(TEXT("tétraplégique"));
		break;
	case EImpactSpineLevel::Thoracic:
	case EImpactSpineLevel::Lumbar:
		ReduceLimb(EImpactLimb::LeftLeg, 0.0f, FString());
		ReduceLimb(EImpactLimb::RightLeg, 0.0f, FString());
		LimbNotes.AddUnique(TEXT("jambes paralysées"));
		break;
	default:
		break;
	}

	for (const FImpactWound& Wound : Wounds)
	{
		Pain = FMath::Max(Pain, Wound.Pain);
	}

	return Notes.IsEmpty() ? FString(TEXT("chair")) : FString::Join(Notes, TEXT(", "));
}

float FImpactPhysiology::GetBleedRate() const
{
	float Total = 0.0f;
	for (const FImpactWound& Wound : Wounds)
	{
		Total += Wound.Bleed;
	}
	return Total;
}

int32 FImpactPhysiology::GetWorstExternalWound() const
{
	int32 Worst = INDEX_NONE;
	for (int32 i = 0; i < Wounds.Num(); ++i)
	{
		if (Wounds[i].bExternal && Wounds[i].Bleed > 0.2f && (Worst == INDEX_NONE || Wounds[i].Bleed > Wounds[Worst].Bleed))
		{
			Worst = i;
		}
	}
	return Worst;
}

void FImpactPhysiology::Tick(const float DeltaTime)
{
	Outflow = 0.0f;
	ExternalOutflow = 0.0f;
	for (FImpactWound& Wound : Wounds)
	{
		Wound.CurrentFlow = 0.0f;
	}
	if (!FMath::IsFinite(DeltaTime) || DeltaTime <= UE_SMALL_NUMBER || !FMath::IsFinite(TimeScale) || TimeScale <= 0.0f)
	{
		return;
	}

	// Bound work and numerical steps even after a hitch or an extreme test speed. Double multiplication
	// avoids overflowing before the cap. Rates below always use the original, unscaled frame duration.
	const float SimulatedDt = static_cast<float>(FMath::Min(static_cast<double>(DeltaTime) * TimeScale, 1.0));
	if (SimulatedDt <= 0.0f)
	{
		return;
	}
	const int32 Steps = FMath::Clamp(FMath::CeilToInt(SimulatedDt / 0.05f), 1, 20);
	const float Dt = SimulatedDt / Steps;
	const float BloodBeforeTick = BloodMl;
	TArray<double> RequestedVolumes;
	RequestedVolumes.SetNumUninitialized(Wounds.Num());
	TArray<double> LostVolumes;
	LostVolumes.Init(0.0, Wounds.Num());

	for (int32 Step = 0; Step < Steps; ++Step)
	{
		// Existing game tuning: pressure falls with volume, then heart function fails after 55% loss.
		const float Loss = GetBloodLossFraction();
		// No free allowance. This used to sit flat at 1.0 until fifteen per cent was gone, so the first seven
		// hundred and fifty millilitres cost nothing at all: someone could lose a litre and a half and feel
		// exactly as well as before.
		const float Volume = FMath::Clamp(1.0f - Loss / 0.55f, 0.0f, 1.0f);
		if (Loss > 0.55f)
		{
			HeartFunction = FMath::Max(0.0f, HeartFunction - Dt * 0.1f);
		}
		const float Circulation = bDead ? 0.0f : HeartFunction * Volume;

		if (!bDead && HeartFunction > 0.0f)
		{
			HeartRate = FMath::Lerp(75.0f, 160.0f, FMath::Clamp(Loss * 2.2f, 0.0f, 1.0f));
			PulsePhase = FMath::Fmod(PulsePhase + Dt * HeartRate / 60.0f, 1.0f);
			const float Beat = FMath::Max(0.0f, FMath::Sin(PulsePhase * 2.0f * PI));
			Pulse = Beat * Beat * HeartFunction;
		}
		else
		{
			HeartRate = 0.0f;
			PulsePhase = 0.0f;
			Pulse = 0.0f;
		}

		double RequestedTotal = 0.0;
		const float PressureFactor = FMath::Clamp(0.1f + 0.9f * Circulation, 0.03f, 1.0f);
		for (int32 Index = 0; Index < Wounds.Num(); ++Index)
		{
			const FImpactWound& Wound = Wounds[Index];
			const float Compression = Wound.bExternal && Wound.bPressed ? (IsArtery(Wound.Tissue) ? 0.55f : 0.35f) : 1.0f;
			const float Bleed = FMath::IsFinite(Wound.Bleed) ? FMath::Max(0.0f, Wound.Bleed) : 0.0f;
			RequestedVolumes[Index] = static_cast<double>(Bleed) * Compression * PressureFactor * Dt;
			RequestedTotal += RequestedVolumes[Index];
		}

		// Share scarce blood proportionally, independent of wound order. Account for the actual change
		// of the stored volume, including its float rounding, rather than reporting hypothetical loss.
		const float BloodBeforeStep = BloodMl;
		BloodMl = static_cast<float>(FMath::Max(0.0, static_cast<double>(BloodMl) - RequestedTotal));
		const double Removed = static_cast<double>(BloodBeforeStep) - BloodMl;
		const double Share = RequestedTotal > 0.0 ? Removed / RequestedTotal : 0.0;
		for (int32 Index = 0; Index < Wounds.Num(); ++Index)
		{
			LostVolumes[Index] += RequestedVolumes[Index] * Share;
			FImpactWound& Wound = Wounds[Index];
			if (Wound.ClotTime > 0.0f)
			{
				// Pressure is what closes a wound. A hand held on it for half a minute lets the clot take,
				// and the floor it would otherwise keep for ever comes down with it. Nothing else does
				// this: a man who never gets his hand to the hole goes on bleeding.
				if (Wound.bExternal && Wound.bPressed && !IsArtery(Wound.Tissue))
				{
					Wound.PressedTime += Dt;
					if (Wound.PressedTime > 30.0f)
					{
						Wound.BleedFloor = FMath::Max(0.0f, Wound.BleedFloor - Dt * 0.25f);
					}
				}
				else
				{
					// Let go too soon and it opens again, though not all the way back.
					Wound.PressedTime = FMath::Max(0.0f, Wound.PressedTime - Dt * 0.6f);
				}

				Wound.Bleed = Wound.BleedFloor + (Wound.Bleed - Wound.BleedFloor) * FMath::Exp(-Dt / Wound.ClotTime);
				if (Wound.Bleed < 0.08f && Wound.BleedFloor <= 0.0f)
				{
					// An exponential towards zero never arrives, and a hundredth of a millilitre a second
					// is still a wound that spits a drop now and then for the rest of the match.
					Wound.Bleed = 0.0f;
					if (!Wound.bClosed)
					{
						Wound.bClosed = true;
						UE_LOG(LogImpact, Display, TEXT("[plaie] %s : refermee a la main apres %.0f s de compression"),
						       *Wound.Name, Wound.PressedTime);
					}
				}
			}
		}

		// Oxygen and anoxia delays remain gameplay parameters, evaluated in bounded simulation steps.
		const float Supply = Circulation * BrainSupplyFactor * (bBreathing ? LungFunction : 0.0f);
		if (Supply < BrainOxygen)
		{
			BrainOxygen = FMath::Max(Supply, BrainOxygen - Dt * (1.0f - Supply) / 16.0f);
		}
		else if (!bDead)
		{
			BrainOxygen = FMath::Min(Supply, BrainOxygen + Dt / 4.0f);
		}
		if (!bDead)
		{
			AnoxiaTime = BrainOxygen < 0.12f ? AnoxiaTime + Dt : FMath::Max(0.0f, AnoxiaTime - Dt);
			if (AnoxiaTime > 60.0f)
			{
				Kill(Loss > 0.4f ? TEXT("hémorragie") : TEXT("manque d'oxygène"));
			}
		}
		Pain = FMath::Max(0.0f, Pain - Dt * 0.01f);
	}

	double ExternalLost = 0.0;
	float StainAreaPerMl = 0.0f;
	float StainMax = 0.0f;
	GetSkinStainTuning(StainAreaPerMl, StainMax);
	for (int32 Index = 0; Index < Wounds.Num(); ++Index)
	{
		FImpactWound& Wound = Wounds[Index];
		Wound.CurrentFlow = static_cast<float>(LostVolumes[Index] / DeltaTime);
		if (Wound.bExternal)
		{
			ExternalLost += LostVolumes[Index];
			// The stain on the skin: its area grows with what came out, so its radius grows ever more slowly.
			if (LostVolumes[Index] > 0.0)
			{
				Wound.Spread = FMath::Min(StainMax, FMath::Sqrt(FMath::Square(Wound.Spread) + StainAreaPerMl * static_cast<float>(LostVolumes[Index])));
			}
		}
	}
	Outflow = (BloodBeforeTick - BloodMl) / DeltaTime;
	ExternalOutflow = FMath::Min(Outflow, static_cast<float>(ExternalLost / DeltaTime));
}

EImpactConsciousness FImpactPhysiology::GetConsciousness() const
{
	if (bDead)
	{
		return EImpactConsciousness::Dead;
	}

	// Losing blood puts someone out even while his brain is still getting oxygen: that is what shock is.
	// This read the oxygen alone, so a man could bleed to nearly half empty and stay perfectly alert until
	// his breathing failed. Reported from play as "ils sont tous a 90 %" — they were, and they were fine.
	const float Loss = GetBloodLossFraction();
	const float FromBlood = 0.2f + 0.8f * FMath::Clamp(1.0f - (Loss - 0.18f) / 0.27f, 0.0f, 1.0f);
	const float Awareness = FMath::Min(BrainOxygen, FromBlood);

	if (Awareness > 0.6f)
	{
		return EImpactConsciousness::Conscious;
	}
	if (Awareness > 0.4f)
	{
		return EImpactConsciousness::Confused;
	}
	if (Awareness > 0.2f)
	{
		return EImpactConsciousness::SemiConscious;
	}
	return EImpactConsciousness::Unconscious;
}

FString FImpactPhysiology::Describe() const
{
	TArray<FString> Parts;
	Parts.Add(FString::Printf(TEXT("Sang %d %%"), FMath::RoundToInt(GetBloodFraction() * 100.0f)));

	switch (GetConsciousness())
	{
	case EImpactConsciousness::Dead:
		Parts.Add(TEXT("mort (") + CauseOfDeath + TEXT(")"));
		break;
	case EImpactConsciousness::Unconscious:
		Parts.Add(TEXT("inconscient"));
		break;
	case EImpactConsciousness::SemiConscious:
		Parts.Add(TEXT("semi-conscient"));
		break;
	case EImpactConsciousness::Confused:
		Parts.Add(TEXT("confus"));
		break;
	default:
		if (GetBloodLossFraction() > 0.15f)
		{
			Parts.Add(TEXT("affaibli"));
		}
		break;
	}

	for (int32 i = 0; i < LimbNotes.Num() && i < 2; ++i)
	{
		Parts.Add(LimbNotes[i]);
	}
	return FString::Join(Parts, TEXT(" · "));
}

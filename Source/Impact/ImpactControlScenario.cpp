#include "ImpactGameplaySubsystem.h"

#include "AlsCharacter.h"
#include "Components/SkeletalMeshComponent.h"
#include "Impact.h"
#include "ImpactBodyComponent.h"
#include "ImpactCivilianController.h"
#include "ImpactReactionComponent.h"
#include "Utility/AlsGameplayTags.h"

// Runs on the real showcase actors. Injuries are deliberately injected to isolate control transitions
// from camera aim, bullet dispersion and penetration. This does not validate shooting accuracy.
void UImpactGameplaySubsystem::RunControlTest(const float Time)
{
	AImpactCivilianController::bFleeingDisabled = true;
	CurrentTestTarget = 0;
	if (Bodies.Num() < 4 || !Bodies[0].IsValid() || !Bodies[1].IsValid() || !Bodies[2].IsValid() || !Bodies[3].IsValid())
	{
		return;
	}
	const auto Check = [this](const bool bPass, const TCHAR* Description)
	{
		if (bPass)
		{
			UE_LOG(LogImpact, Display, TEXT("[validation] PASS %s"), Description);
		}
		else
		{
			bControlTestFailed = true;
			UE_LOG(LogImpact, Error, TEXT("[validation] FAIL %s"), Description);
		}
	};
	const auto ReactionAt = [this](const int32 Index)
	{
		return Bodies[Index]->GetOwner()->FindComponentByClass<UImpactReactionComponent>();
	};
	if (TestStep == 0 && Time >= 1.0f)
	{
		for (int32 Index = 0; Index < 4; ++Index)
		{
			FImpactPhysiology& Physiology = Bodies[Index]->GetPhysiology();
			Physiology.ForceLimbLoss(EImpactLimb::LeftLeg, 0.0f, TEXT("appui retiré (test isolé)"));
			if (Index != 1)
			{
				Physiology.ForceLimbLoss(EImpactLimb::RightLeg, 0.0f, TEXT("appui retiré (test isolé)"));
			}
		}
		TestStep = 1;
	}
	if (TestStep == 1 && Time >= 6.0f)
	{
		Check(ReactionAt(0)->IsDown() && ReactionAt(0)->IsCrawling() && ReactionAt(0)->GetActiveForceCount() > 0,
		      TEXT("deux jambes sans commande : rampement actif sans blessure balistique parasite"));
		Check(ReactionAt(1)->IsDown() && ReactionAt(1)->IsCrawling(), TEXT("une jambe sans appui : pas de relevé ALS normal"));
		Check(!ReactionAt(0)->IsHandPressing(0) && !ReactionAt(0)->IsHandPressing(1), TEXT("ramper exclut la compression"));
		TestStep = 2;
	}
	if (TestStep == 2 && Time >= 8.0f)
	{
		UImpactBodyComponent* Body = Bodies[0].Get();
		const auto& Organs = Body->GetAnatomy().GetOrgans();
		const int32 Brain = Organs.IndexOfByPredicate([](const FImpactOrgan& Organ) { return Organ.Tissue == EImpactTissue::Brain; });
		Check(Brain != INDEX_NONE, TEXT("structure cerveau disponible pour le scénario"));
		if (Brain != INDEX_NONE)
		{
			FImpactBulletPath Path;
			Path.EnergyIn = 500.0f;
			Path.EntryBone = TEXT("head");
			Path.Entry = GetOrganPoint(0, TEXT("cerveau"));
			FImpactPathHit Hit;
			Hit.Organ = Brain;
			Hit.Depth = 10.0f;
			Hit.Point = Path.Entry;
			Path.Hits.Add(Hit);
			Body->GetPhysiology().ApplyBullet(Path, Body->GetAnatomy(), Cast<AAlsCharacter>(Body->GetOwner())->GetMesh());
		}
		// Severe internal blood loss, no external effect expected. Consciousness evolves normally over time.
		FImpactWound Bleed;
		Bleed.Name = TEXT("hémorragie interne de contrôle");
		Bleed.Bone = TEXT("pelvis");
		Bleed.bExternal = false;
		Bleed.Bleed = 6000.0f;
		Bodies[2]->GetPhysiology().GetWounds().Add(Bleed);
		Bodies[3]->GetPhysiology().ForceLimbLoss(EImpactLimb::LeftArm, 0.0f, TEXT("bras retiré (test)"));
		Bodies[3]->GetPhysiology().ForceLimbLoss(EImpactLimb::RightArm, 0.0f, TEXT("bras retiré (test)"));
		TestStep = 3;
	}
	if (TestStep == 3 && Time >= 10.0f)
	{
		Check(Bodies[0]->GetPhysiology().IsDead(), TEXT("lésion cérébrale réellement létale"));
		Check(!ReactionAt(0)->HasVoluntaryControl() && !ReactionAt(0)->IsCrawling() && !ReactionAt(0)->IsBracing()
		      && ReactionAt(0)->GetActiveForceCount() == 0, TEXT("mort pendant rampement : aucune force volontaire"));
		Check(!ReactionAt(3)->IsCrawling() && ReactionAt(3)->GetActiveForceCount() == 0,
		      TEXT("perte des deux bras pendant rampement : arrêt des tractions"));
		TestStep = 4;
	}
	if (Time >= 11.0f)
	{
		CurrentTestTarget = 2;
	}
	if (TestStep == 4 && Time >= 25.0f)
	{
		Check(Bodies[2]->GetPhysiology().GetConsciousness() == EImpactConsciousness::Unconscious,
		      TEXT("hémorragie : inconscience distincte de la mort"));
		Check(!ReactionAt(2)->HasVoluntaryControl() && ReactionAt(2)->GetActiveForceCount() == 0 && !ReactionAt(2)->IsCrawling(),
		      TEXT("inconscience pendant rampement : aucune force volontaire"));
		Check(ReactionAt(1)->IsDown(), TEXT("jambe inutilisable : toujours aucun relevé après 24 secondes"));
		Check(ReactionAt(0)->GetActiveForceCount() == 0 && !ReactionAt(0)->IsHandPressing(0) && !ReactionAt(0)->IsHandPressing(1),
		      TEXT("mort : aucune reprise tardive des gestes"));
		UE_LOG(LogImpact, Display, TEXT("[validation] CONTROL_RESULT=%s"), bControlTestFailed ? TEXT("FAIL") : TEXT("PASS"));
		TestStep = 5;
	}
}

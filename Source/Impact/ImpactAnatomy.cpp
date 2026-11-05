#include "ImpactAnatomy.h"

#include "AnimationRuntime.h"
#include "Components/SkeletalMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/SkinnedAsset.h"
#include "Impact.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsEngine/BodySetup.h"

namespace
{
	/** One organ as written by hand: two ends, each a skeleton landmark plus an offset (forward, left, up) in cm. */
	struct FOrganDef
	{
		const TCHAR* Name;
		bool bFeminine;            // "droite" or "droit"
		bool bPaired;              // written for the left side, mirrored for the right
		EImpactTissue Tissue;
		EImpactSpineLevel SpineLevel;
		const TCHAR* Anchor;
		const TCHAR* BoneA;
		FVector OffsetA;
		const TCHAR* BoneB;
		FVector OffsetB;
		float Radius;
		float Resistance;
	};

	using T = EImpactTissue;
	using S = EImpactSpineLevel;

	// Adult of the mannequin's size (about 1.80 m). Offsets: X forward, Y towards the left side, Z up.
	const FOrganDef OrganDefs[] = {
		// Head
		{TEXT("cerveau"), false, false, T::Brain, S::None, TEXT("head"), TEXT("head"), {1, 0, 8}, TEXT("head"), {4, 0, 11}, 7.0f, 12.0f},
		{TEXT("tronc cérébral"), false, false, T::Brainstem, S::None, TEXT("head"), TEXT("head"), {-1, 0, 1}, TEXT("head"), {0, 0, 6}, 1.7f, 12.0f},
		{TEXT("crâne"), false, false, T::Skull, S::None, TEXT("head"), TEXT("head"), {2, 0, 9}, TEXT("head"), {2, 0, 10}, 10.0f, 200.0f},
		{TEXT("mâchoire"), true, false, T::Jaw, S::None, TEXT("head"), TEXT("head"), {7, 0, -2}, TEXT("head"), {9, 0, 1}, 4.0f, 60.0f},

		// Neck
		{TEXT("vertèbre cervicale haute"), true, false, T::Vertebra, S::CervicalHigh, TEXT("neck_01"), TEXT("neck_01"), {-2, 0, 3}, TEXT("head"), {-2.5f, 0, 0}, 2.0f, 120.0f},
		{TEXT("vertèbre cervicale basse"), true, false, T::Vertebra, S::CervicalLow, TEXT("neck_01"), TEXT("neck_01"), {-2, 0, -3}, TEXT("neck_01"), {-2, 0, 3}, 2.0f, 120.0f},
		{TEXT("moelle cervicale haute"), true, false, T::SpinalCord, S::CervicalHigh, TEXT("neck_01"), TEXT("neck_01"), {-3.8f, 0, 3}, TEXT("head"), {-4.2f, 0, 0}, 0.7f, 10.0f},
		{TEXT("moelle cervicale basse"), true, false, T::SpinalCord, S::CervicalLow, TEXT("neck_01"), TEXT("neck_01"), {-3.8f, 0, -3}, TEXT("neck_01"), {-3.8f, 0, 3}, 0.7f, 10.0f},
		{TEXT("trachée"), true, false, T::Trachea, S::None, TEXT("neck_01"), TEXT("neck_01"), {4, 0, -3}, TEXT("head"), {3, 0, -3}, 1.1f, 15.0f},
		{TEXT("carotide"), true, true, T::Carotid, S::None, TEXT("neck_01"), TEXT("neck_01"), {2.5f, 2.5f, -3}, TEXT("head"), {2, 3, -1}, 0.55f, 10.0f},
		{TEXT("jugulaire"), true, true, T::Jugular, S::None, TEXT("neck_01"), TEXT("neck_01"), {1.5f, 4, -3}, TEXT("head"), {1, 4.5f, -1}, 0.8f, 10.0f},

		// Spine and cord, chest and abdomen
		{TEXT("vertèbre dorsale"), true, false, T::Vertebra, S::Thoracic, TEXT("spine_03"), TEXT("spine_03"), {-4, 0, 0}, TEXT("neck_01"), {-3, 0, -3}, 2.2f, 120.0f},
		{TEXT("vertèbre dorsale basse"), true, false, T::Vertebra, S::Thoracic, TEXT("spine_02"), TEXT("spine_02"), {-4, 0, 0}, TEXT("spine_03"), {-4, 0, 0}, 2.2f, 120.0f},
		{TEXT("vertèbre lombaire"), true, false, T::Vertebra, S::Lumbar, TEXT("spine_01"), TEXT("pelvis"), {-4, 0, 6}, TEXT("spine_02"), {-4, 0, 0}, 2.4f, 120.0f},
		{TEXT("moelle dorsale"), true, false, T::SpinalCord, S::Thoracic, TEXT("spine_03"), TEXT("spine_03"), {-6.2f, 0, 0}, TEXT("neck_01"), {-5.2f, 0, -3}, 0.7f, 10.0f},
		{TEXT("moelle dorsale basse"), true, false, T::SpinalCord, S::Thoracic, TEXT("spine_02"), TEXT("spine_02"), {-6.2f, 0, 0}, TEXT("spine_03"), {-6.2f, 0, 0}, 0.7f, 10.0f},
		{TEXT("moelle lombaire"), true, false, T::SpinalCord, S::Lumbar, TEXT("spine_01"), TEXT("pelvis"), {-6.4f, 0, 6}, TEXT("spine_02"), {-6.2f, 0, 0}, 0.8f, 10.0f},

		{TEXT("cœur"), false, false, T::Heart, S::None, TEXT("spine_03"), TEXT("spine_03"), {7, 2.5f, -7}, TEXT("spine_03"), {7, 3.5f, -4}, 5.0f, 14.0f},
		{TEXT("aorte"), true, false, T::Aorta, S::None, TEXT("spine_03"), TEXT("spine_03"), {6, 1, -2}, TEXT("spine_03"), {-1, 2, -1}, 1.3f, 10.0f},
		{TEXT("aorte"), true, false, T::Aorta, S::None, TEXT("spine_02"), TEXT("spine_03"), {-1, 2, -1}, TEXT("spine_01"), {-1, 1.5f, -2}, 1.3f, 10.0f},
		{TEXT("aorte"), true, false, T::Aorta, S::None, TEXT("spine_01"), TEXT("spine_01"), {-1, 1.5f, -2}, TEXT("pelvis"), {0, 0.5f, 5}, 1.2f, 10.0f},
		{TEXT("veine cave"), true, false, T::VenaCava, S::None, TEXT("spine_02"), TEXT("spine_03"), {3, -2, -6}, TEXT("spine_01"), {0, -2, 0}, 1.4f, 10.0f},
		{TEXT("veine cave"), true, false, T::VenaCava, S::None, TEXT("spine_01"), TEXT("spine_01"), {0, -2, 0}, TEXT("pelvis"), {0, -2, 5}, 1.4f, 10.0f},
		{TEXT("poumon"), false, true, T::Lung, S::None, TEXT("spine_03"), TEXT("spine_03"), {2, 9, -12}, TEXT("spine_03"), {2, 8, 8}, 6.5f, 4.0f},
		{TEXT("côtes"), true, false, T::Rib, S::None, TEXT("spine_03"), TEXT("spine_02"), {2, 0, 2}, TEXT("spine_03"), {2, 0, 10}, 15.0f, 120.0f},
		{TEXT("foie"), false, false, T::Liver, S::None, TEXT("spine_02"), TEXT("spine_02"), {5, -12, 2}, TEXT("spine_02"), {6, -3, 4}, 6.0f, 14.0f},
		{TEXT("rate"), true, false, T::Spleen, S::None, TEXT("spine_02"), TEXT("spine_02"), {-1, 9, 4}, TEXT("spine_02"), {-1, 9, 4}, 4.0f, 14.0f},
		{TEXT("estomac"), false, false, T::Stomach, S::None, TEXT("spine_02"), TEXT("spine_02"), {7, 4, 3}, TEXT("spine_02"), {7, 8, 0}, 4.5f, 12.0f},
		{TEXT("rein"), false, true, T::Kidney, S::None, TEXT("spine_02"), TEXT("spine_02"), {-3, 6, -7}, TEXT("spine_02"), {-3, 6, -1}, 3.2f, 14.0f},
		{TEXT("intestins"), false, false, T::Intestines, S::None, TEXT("spine_01"), TEXT("spine_01"), {7, -7, -2}, TEXT("spine_01"), {7, 7, -2}, 7.0f, 12.0f},
		{TEXT("vessie"), true, false, T::Bladder, S::None, TEXT("pelvis"), TEXT("pelvis"), {8, 0, -6}, TEXT("pelvis"), {8, 0, -6}, 4.0f, 10.0f},
		{TEXT("bassin"), false, true, T::PelvisBone, S::None, TEXT("pelvis"), TEXT("pelvis"), {0, 11, 2}, TEXT("pelvis"), {4, 9, -6}, 3.0f, 100.0f},
		{TEXT("sacrum"), false, false, T::PelvisBone, S::None, TEXT("pelvis"), TEXT("pelvis"), {-6, 0, 2}, TEXT("pelvis"), {-7, 0, -6}, 3.0f, 100.0f},
		{TEXT("artère iliaque"), true, true, T::IliacArtery, S::None, TEXT("pelvis"), TEXT("pelvis"), {0, 1, 5}, TEXT("thigh_l"), {4, -1, 0}, 0.9f, 10.0f},

		// Legs
		{TEXT("fémur"), false, true, T::Femur, S::None, TEXT("thigh_l"), TEXT("thigh_l"), {0, 1, -3}, TEXT("calf_l"), {0, 0, 3}, 2.0f, 150.0f},
		{TEXT("artère fémorale"), true, true, T::FemoralArtery, S::None, TEXT("thigh_l"), TEXT("thigh_l"), {4, -2, -4}, TEXT("calf_l"), {-2, -1, 14}, 0.55f, 10.0f},
		{TEXT("genou"), false, true, T::Knee, S::None, TEXT("calf_l"), TEXT("calf_l"), {1, 0, 0}, TEXT("calf_l"), {1, 0, 0}, 5.0f, 80.0f},
		{TEXT("tibia"), false, true, T::Tibia, S::None, TEXT("calf_l"), TEXT("calf_l"), {2, 0, -6}, TEXT("foot_l"), {1.5f, 0, 6}, 1.8f, 150.0f},
		{TEXT("pied"), false, true, T::Foot, S::None, TEXT("foot_l"), TEXT("foot_l"), {0, 0, 0}, TEXT("ball_l"), {0, 0, 0}, 4.0f, 60.0f},

		// Arms (reference pose: arms out to the sides, palms down)
		{TEXT("clavicule"), true, true, T::Clavicle, S::None, TEXT("clavicle_l"), TEXT("clavicle_l"), {3, 1, 1}, TEXT("upperarm_l"), {2, -2, 2}, 1.2f, 120.0f},
		{TEXT("artère sous-clavière"), true, true, T::SubclavianArtery, S::None, TEXT("clavicle_l"), TEXT("clavicle_l"), {2, 1, -2}, TEXT("upperarm_l"), {0, -1, -3}, 0.6f, 10.0f},
		{TEXT("épaule"), true, true, T::ShoulderJoint, S::None, TEXT("upperarm_l"), TEXT("upperarm_l"), {0, 1, 0}, TEXT("upperarm_l"), {0, 1, 0}, 4.0f, 80.0f},
		{TEXT("humérus"), false, true, T::Humerus, S::None, TEXT("upperarm_l"), TEXT("upperarm_l"), {0, 3, 0}, TEXT("lowerarm_l"), {0, -3, 0}, 1.5f, 150.0f},
		{TEXT("artère humérale"), true, true, T::BrachialArtery, S::None, TEXT("upperarm_l"), TEXT("upperarm_l"), {1, 5, -3}, TEXT("lowerarm_l"), {1, -2, -2}, 0.45f, 10.0f},
		{TEXT("coude"), false, true, T::Elbow, S::None, TEXT("lowerarm_l"), TEXT("lowerarm_l"), {0, 0, 0}, TEXT("lowerarm_l"), {0, 0, 0}, 3.5f, 80.0f},
		{TEXT("avant-bras"), false, true, T::Forearm, S::None, TEXT("lowerarm_l"), TEXT("lowerarm_l"), {0, 3, 0}, TEXT("hand_l"), {0, -2, 0}, 1.6f, 120.0f},
		{TEXT("main"), true, true, T::Hand, S::None, TEXT("hand_l"), TEXT("hand_l"), {0, 0, 0}, TEXT("middle_01_l"), {0, 0, 0}, 3.5f, 50.0f},
	};

	constexpr float SoftTissueResistance = 10.0f; // J/cm in muscle and fat between organs
	constexpr float MaxThroughBody = 90.0f;       // cm searched past the entry
	constexpr float Step = 0.5f;                  // cm sampling along the path

	FName Mirror(const TCHAR* Bone)
	{
		FString Name = Bone;
		if (Name.EndsWith(TEXT("_l")))
		{
			Name.LeftChopInline(2);
			Name += TEXT("_r");
		}
		return FName(*Name);
	}

	float DistanceToSegment(const FVector& Point, const FVector& A, const FVector& B)
	{
		return FMath::Sqrt(FMath::PointDistToSegmentSquared(Point, A, B));
	}

	FColor TissueColor(const EImpactTissue Tissue)
	{
		switch (Tissue)
		{
		case T::Brain:
		case T::Brainstem:
			return FColor(255, 80, 200);
		case T::SpinalCord:
			return FColor::Yellow;
		case T::Carotid:
		case T::Aorta:
		case T::IliacArtery:
		case T::FemoralArtery:
		case T::SubclavianArtery:
		case T::BrachialArtery:
			return FColor::Red;
		case T::Jugular:
		case T::VenaCava:
			return FColor(60, 60, 255);
		case T::Heart:
			return FColor(200, 0, 0);
		case T::Lung:
			return FColor(255, 170, 170);
		case T::Liver:
		case T::Spleen:
		case T::Kidney:
			return FColor(140, 40, 20);
		case T::Stomach:
		case T::Intestines:
		case T::Bladder:
			return FColor(230, 180, 120);
		case T::Trachea:
			return FColor(150, 220, 255);
		default:
			return FColor(235, 235, 220); // bones and joints
		}
	}
}

bool FImpactAnatomy::Build(const USkeletalMeshComponent* Mesh)
{
	Organs.Reset();

	const USkinnedAsset* Asset = Mesh != nullptr ? Mesh->GetSkinnedAsset() : nullptr;
	if (Asset == nullptr)
	{
		return false;
	}

	const FReferenceSkeleton& Skeleton = Asset->GetRefSkeleton();
	const auto RefPose = [&Skeleton](const FName Bone, FTransform& Out)
	{
		const int32 Index = Skeleton.FindBoneIndex(Bone);
		if (Index == INDEX_NONE)
		{
			return false;
		}
		Out = FAnimationRuntime::GetComponentSpaceTransformRefPose(Skeleton, Index);
		return true;
	};

	// Anatomical frame of the reference pose: up the spine, towards the left arm, forward towards the toes.
	FTransform Pelvis, Neck, ArmLeft, ArmRight, Foot, Ball;
	if (!RefPose(TEXT("pelvis"), Pelvis) || !RefPose(TEXT("neck_01"), Neck) || !RefPose(TEXT("upperarm_l"), ArmLeft)
	    || !RefPose(TEXT("upperarm_r"), ArmRight) || !RefPose(TEXT("foot_l"), Foot) || !RefPose(TEXT("ball_l"), Ball))
	{
		UE_LOG(LogImpact, Warning, TEXT("[anatomy] skeleton is not the ALS mannequin, no anatomy"));
		return false;
	}

	const FVector Up = (Neck.GetLocation() - Pelvis.GetLocation()).GetSafeNormal();
	const FVector Left = FVector::VectorPlaneProject(ArmLeft.GetLocation() - ArmRight.GetLocation(), Up).GetSafeNormal();
	FVector Forward = FVector::CrossProduct(Left, Up);
	if (FVector::DotProduct(Forward, Ball.GetLocation() - Foot.GetLocation()) < 0.0f)
	{
		Forward = -Forward;
	}

	const auto Landmark = [&](const FName Bone, const FVector& Offset, FVector& Out)
	{
		FTransform Pose;
		if (!RefPose(Bone, Pose))
		{
			return false;
		}
		Out = Pose.GetLocation() + Forward * Offset.X + Left * Offset.Y + Up * Offset.Z;
		return true;
	};

	for (const FOrganDef& Def : OrganDefs)
	{
		const int32 SideCount = Def.bPaired ? 2 : 1;
		for (int32 SideIndex = 0; SideIndex < SideCount; ++SideIndex)
		{
			const bool bRight = SideIndex == 1;
			const auto Bone = [bRight](const TCHAR* Name) { return bRight ? Mirror(Name) : FName(Name); };
			const auto Offset = [bRight](FVector Value)
			{
				if (bRight)
				{
					Value.Y = -Value.Y;
				}
				return Value;
			};

			FVector A, B;
			FTransform AnchorPose;
			if (!Landmark(Bone(Def.BoneA), Offset(Def.OffsetA), A) || !Landmark(Bone(Def.BoneB), Offset(Def.OffsetB), B)
			    || !RefPose(Bone(Def.Anchor), AnchorPose))
			{
				continue;
			}

			FImpactOrgan& Organ = Organs.AddDefaulted_GetRef();
			Organ.Tissue = Def.Tissue;
			Organ.SpineLevel = Def.SpineLevel;
			Organ.Side = Def.bPaired ? (bRight ? -1 : 1) : 0;
			Organ.Name = Def.Name;
			if (Def.bPaired)
			{
				Organ.Name += bRight ? (Def.bFeminine ? TEXT(" droite") : TEXT(" droit")) : TEXT(" gauche");
			}
			Organ.Anchor = Bone(Def.Anchor);
			Organ.LocalA = AnchorPose.InverseTransformPosition(A);
			Organ.LocalB = AnchorPose.InverseTransformPosition(B);
			Organ.Radius = Def.Radius;
			Organ.Resistance = Def.Resistance;
		}
	}

	UE_LOG(LogImpact, Display, TEXT("[anatomy] %d organs placed"), Organs.Num());
	return true;
}

FVector FImpactAnatomy::GetOrganWorldA(const USkeletalMeshComponent* Mesh, const int32 Index) const
{
	const FImpactOrgan& Organ = Organs[Index];
	return Mesh->GetSocketTransform(Organ.Anchor, RTS_World).TransformPosition(Organ.LocalA);
}

FVector FImpactAnatomy::GetOrganWorldB(const USkeletalMeshComponent* Mesh, const int32 Index) const
{
	const FImpactOrgan& Organ = Organs[Index];
	return Mesh->GetSocketTransform(Organ.Anchor, RTS_World).TransformPosition(Organ.LocalB);
}

FImpactBulletPath FImpactAnatomy::TraceBullet(const USkeletalMeshComponent* Mesh, const FVector& Entry, const FVector& Direction,
                                              const FName EntryBone, const float Energy) const
{
	FImpactBulletPath Path;
	Path.Entry = Entry;
	Path.Direction = Direction;
	Path.EntryBone = EntryBone;
	Path.EnergyIn = Energy;
	Path.EnergyOut = Energy;
	Path.Exit = Entry;
	Path.ExitBone = EntryBone;

	const int32 Samples = FMath::CeilToInt(MaxThroughBody / Step);
	const FVector Far = Entry + Direction * MaxThroughBody;

	// Where the path is inside the body: every body part is traced from both ends, giving the stretches inside flesh.
	TArray<FVector2D> Inside; // distances from the entry, cm
	TArray<FName> InsideBones;
	for (const FBodyInstance* Body : Mesh->Bodies)
	{
		if (Body == nullptr || !Body->IsValidBodyInstance() || !Body->BodySetup.IsValid())
		{
			continue;
		}
		FHitResult In, Out;
		const bool bIn = Body->LineTrace(In, Entry - Direction * 2.0f, Far, false);
		const bool bOut = Body->LineTrace(Out, Far, Entry - Direction * 2.0f, false);
		if (bIn && bOut)
		{
			const float From = FMath::Max(0.0f, FVector::DotProduct(In.ImpactPoint - Entry, Direction));
			const float To = FVector::DotProduct(Out.ImpactPoint - Entry, Direction);
			if (To > From)
			{
				Inside.Add({From, To});
				InsideBones.Add(Body->BodySetup->BoneName);
			}
		}
	}
	if (Inside.IsEmpty())
	{
		Inside.Add({0.0f, 2.0f});
		InsideBones.Add(EntryBone);
	}

	// World shapes of the organs, now.
	struct FShape
	{
		FVector A;
		FVector B;
		float Radius;
		float First = -1.0f;
		float Last = -1.0f;
	};
	TArray<FShape> Shapes;
	Shapes.Reserve(Organs.Num());
	for (int32 i = 0; i < Organs.Num(); ++i)
	{
		Shapes.Add({GetOrganWorldA(Mesh, i), GetOrganWorldB(Mesh, i), Organs[i].Radius});
	}

	// Which organ contains each point of the path.
	for (int32 Sample = 0; Sample <= Samples; ++Sample)
	{
		const float Distance = Sample * Step;
		const FVector Point = Entry + Direction * Distance;
		for (FShape& Shape : Shapes)
		{
			if (DistanceToSegment(Point, Shape.A, Shape.B) <= Shape.Radius)
			{
				if (Shape.First < 0.0f)
				{
					Shape.First = Distance;
				}
				Shape.Last = Distance;
			}
		}
	}

	// A bullet crossing the chest wall meets a rib a bit less than half of the time.
	const uint32 Hash = GetTypeHash(FIntVector(FMath::RoundToInt(Entry.X * 10.0f), FMath::RoundToInt(Entry.Y * 10.0f), FMath::RoundToInt(Entry.Z * 10.0f)));
	const bool bRibOnPath = Hash % 100 < 45;

	// Walk the path and spend energy: bone stops a bullet far more than flesh; skull and ribs are thin shells.
	float Remaining = Energy;
	float Stop = MaxThroughBody;
	float LastInside = 0.0f;
	for (int32 Sample = 0; Sample <= Samples; ++Sample)
	{
		const float Distance = Sample * Step;
		bool bInBody = false;
		for (int32 i = 0; i < Inside.Num(); ++i)
		{
			if (Distance >= Inside[i].X && Distance <= Inside[i].Y)
			{
				bInBody = true;
				Path.ExitBone = InsideBones[i];
				break;
			}
		}
		if (!bInBody)
		{
			continue;
		}
		LastInside = Distance;

		float Resistance = SoftTissueResistance;
		for (int32 i = 0; i < Organs.Num(); ++i)
		{
			const FShape& Shape = Shapes[i];
			if (Shape.First < 0.0f || Distance < Shape.First || Distance > Shape.Last)
			{
				continue;
			}
			const EImpactTissue Tissue = Organs[i].Tissue;
			if (Tissue == T::Skull || Tissue == T::Rib)
			{
				const bool bShell = Distance - Shape.First < 1.0f || Shape.Last - Distance < 1.0f;
				if (!bShell || (Tissue == T::Rib && !bRibOnPath))
				{
					continue;
				}
			}
			Resistance = FMath::Max(Resistance, Organs[i].Resistance);
		}

		Remaining -= Resistance * Step;
		if (Remaining <= 0.0f)
		{
			Stop = Distance;
			Remaining = 0.0f;
			break;
		}
	}

	if (Remaining > 0.0f)
	{
		Stop = LastInside;
		Path.bExited = true;
	}
	Path.EnergyOut = Remaining;
	Path.Exit = Entry + Direction * Stop;

	for (int32 i = 0; i < Organs.Num(); ++i)
	{
		const FShape& Shape = Shapes[i];
		if (Shape.First < 0.0f || Shape.First > Stop)
		{
			continue;
		}
		if (Organs[i].Tissue == T::Rib && !bRibOnPath)
		{
			continue;
		}
		FImpactPathHit& Hit = Path.Hits.AddDefaulted_GetRef();
		Hit.Organ = i;
		Hit.Distance = Shape.First;
		Hit.Depth = FMath::Min(Shape.Last, Stop) - Shape.First + Step;
		Hit.Point = Entry + Direction * Shape.First;
	}
	Path.Hits.Sort([](const FImpactPathHit& A, const FImpactPathHit& B) { return A.Distance < B.Distance; });

	return Path;
}

void FImpactAnatomy::DrawOrgans(UWorld* World, const USkeletalMeshComponent* Mesh, const float Duration) const
{
	for (int32 i = 0; i < Organs.Num(); ++i)
	{
		const FImpactOrgan& Organ = Organs[i];
		if (Organ.Tissue == T::Skull || Organ.Tissue == T::Rib)
		{
			continue; // shells would hide everything inside
		}

		const FVector A = GetOrganWorldA(Mesh, i);
		const FVector B = GetOrganWorldB(Mesh, i);
		const FColor Color = TissueColor(Organ.Tissue);
		const float HalfLength = FVector::Distance(A, B) * 0.5f;
		if (HalfLength < 0.5f)
		{
			DrawDebugSphere(World, A, Organ.Radius, 10, Color, false, Duration, SDPG_Foreground, 0.4f);
		}
		else
		{
			const FQuat Rotation = FRotationMatrix::MakeFromZ(B - A).ToQuat();
			DrawDebugCapsule(World, (A + B) * 0.5f, HalfLength + Organ.Radius, Organ.Radius, Rotation, Color, false, Duration, SDPG_Foreground, 0.4f);
		}
	}
}

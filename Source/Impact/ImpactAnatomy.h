#pragma once

#include "CoreMinimal.h"

class USkeletalMeshComponent;
class UWorld;

/** What a piece of the body is made of; decides what a bullet does to it. */
enum class EImpactTissue : uint8
{
	Muscle, // soft tissue of a body part when nothing else is hit
	Brain,
	Brainstem,
	Skull,
	Jaw,
	SpinalCord,
	Vertebra,
	Trachea,
	Carotid,
	Jugular,
	Heart,
	Aorta,
	VenaCava,
	Lung,
	Rib,
	Liver,
	Spleen,
	Stomach,
	Intestines,
	Kidney,
	Bladder,
	PelvisBone,
	IliacArtery,
	Femur,
	FemoralArtery,
	Knee,
	Tibia,
	Foot,
	Clavicle,
	SubclavianArtery,
	ShoulderJoint,
	Humerus,
	BrachialArtery,
	Elbow,
	Forearm,
	Hand
};

/** Height of a spinal cord section: the higher the cut, the more of the body stops working. */
enum class EImpactSpineLevel : uint8
{
	None,
	Lumbar,       // legs
	Thoracic,     // legs and lower trunk
	CervicalLow,  // legs, trunk, most of the arms
	CervicalHigh  // everything below the neck, breathing included
};

/** Limbs, for what still works. */
enum class EImpactLimb : uint8
{
	LeftArm,
	RightArm,
	LeftLeg,
	RightLeg,
	Count
};

struct FImpactOrgan
{
	FString Name;               // French, with side: "fémur gauche"
	EImpactTissue Tissue = EImpactTissue::Muscle;
	int8 Side = 0;              // +1 left, -1 right, 0 middle
	EImpactSpineLevel SpineLevel = EImpactSpineLevel::None;
	FName Anchor;               // bone it moves with
	FVector LocalA = FVector::ZeroVector; // capsule ends in the anchor bone's space
	FVector LocalB = FVector::ZeroVector;
	float Radius = 1.0f;        // cm
	float Resistance = 10.0f;   // energy a bullet loses per cm inside it, J/cm
};

struct FImpactPathHit
{
	int32 Organ = INDEX_NONE;
	float Depth = 0.0f;         // cm of the bullet path inside the organ
	float Distance = 0.0f;      // cm from the entry
	FVector Point = FVector::ZeroVector;
};

/** Course of one bullet through one body. */
struct FImpactBulletPath
{
	FVector Entry = FVector::ZeroVector;
	FVector Exit = FVector::ZeroVector; // where it left the body, or where it stopped
	FVector Direction = FVector::ForwardVector;
	FName EntryBone;
	FName ExitBone;
	bool bExited = false;
	float EnergyIn = 0.0f;
	float EnergyOut = 0.0f;
	TArray<FImpactPathHit> Hits; // in order along the path, only what the bullet reached
};

/**
 * Bones, organs, vessels and spinal cord placed inside the ALS mannequin, each attached to the bone it moves with.
 * Shapes are written in an anatomical frame (forward, left, up in cm, from a skeleton landmark) and converted
 * to bone space from the reference pose, so they follow the body however it moves.
 */
class IMPACT_API FImpactAnatomy
{
public:
	/** Places every organ for this mesh. Returns false if the skeleton is not the ALS/UE4 mannequin. */
	bool Build(const USkeletalMeshComponent* Mesh);

	bool IsBuilt() const { return !Organs.IsEmpty(); }

	const TArray<FImpactOrgan>& GetOrgans() const { return Organs; }

	FVector GetOrganWorldA(const USkeletalMeshComponent* Mesh, int32 Index) const;
	FVector GetOrganWorldB(const USkeletalMeshComponent* Mesh, int32 Index) const;

	/** Follows a bullet entering at Entry. It loses energy in what it crosses and may stop inside. */
	FImpactBulletPath TraceBullet(const USkeletalMeshComponent* Mesh, const FVector& Entry, const FVector& Direction, FName EntryBone,
	                              float Energy) const;

	/** X-ray view: every organ as a wire shape. */
	void DrawOrgans(UWorld* World, const USkeletalMeshComponent* Mesh, float Duration) const;

private:
	TArray<FImpactOrgan> Organs;
};

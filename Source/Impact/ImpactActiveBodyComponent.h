#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "ImpactActiveBodyComponent.generated.h"

class AAlsCharacter;
class USkeletalMeshComponent;
struct FImpactBulletHit;

/**
 * -ImpactCorpsActif. A body that is always physical, the way the references do it (the Euphoria video, Endorphin Vice, RDR2):
 * every part is simulated and held in the animated pose by the joints' own muscles; while he stands the pelvis is carried by
 * the animation. A round pushes the part it hits and slackens it for a moment. Enough rounds at once, a leg that gives, or the
 * mind going, and the pelvis is let go: he falls by himself from whatever pose he is in, and nothing switches from animation to
 * physics because the body was physical all along. Down and awake, his muscles draw him slowly towards poses worked out here
 * (curled up, holding the wound, stretching out again) - no animation, so nothing ever jumps to a take's first frame; able
 * again, he gets up through ALS's get-up and is an active body once more. Dead, nothing holds.
 *
 * It replaces UImpactReactionComponent on the people it is given to (that one is then not created): reported from play,
 * "c'est bugge de partout, armes qui flottent, personnages qui rentrent dans le sol, qui se TP" - every one of those came
 * from going back and forth between animation and ragdoll.
 */
UCLASS()
class IMPACT_API UImpactActiveBodyComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UImpactActiveBodyComponent();

	static bool IsEnabled();

	/** A round went into this body: push the part it hit, slacken it, and add to the shock. */
	void OnBullet(const FImpactBulletHit& Bullet, float Strength);

	bool IsDown() const { return State != EState::Standing; }
	FString GetStateText() const;

protected:
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
	enum class EState : uint8
	{
		Standing,
		Down,
		GettingUp,
		Dead
	};

	// What he does on the ground while awake.
	enum class EGround : uint8
	{
		Writhe,
		Roll,
		Crawl
	};

	enum class EGroup : uint8
	{
		Spine,
		Head,
		LeftArm,
		RightArm,
		LeftLeg,
		RightLeg
	};

	struct FMuscle
	{
		FName Bone;
		FName Parent;
		float Strength = 0.0f; // full muscle
		float Slack = 0.0f;    // 0 holding, 1 let go
		float Applied = -1.0f;
		float ToneScale = 1.0f; // this part's share on top of Tone (the arms pull harder while crawling)

		// For the poses worked out on the ground: the joint's physics parent (an index into Parts, none for the pelvis), the
		// two bones' reference-pose rotations in component space, the bone's direction there, and the joint's frames.
		int32 ParentPart = INDEX_NONE;
		FName PhysicsParent;
		EGroup Group = EGroup::Spine;
		float Side = 0.0f; // -1 left, 1 right
		FQuat RefParent = FQuat::Identity;
		FQuat RefChild = FQuat::Identity;
		FVector RefDir = FVector::ZeroVector;
		FQuat Frame1 = FQuat::Identity;
		FQuat Frame2 = FQuat::Identity;
		FQuat Held = FQuat::Identity; // child relative to parent when the writhing began
	};

	AAlsCharacter* GetCharacter() const;
	void Setup();
	void SetupPoses(USkeletalMeshComponent* Mesh);
	void SetupStanding();
	void Fall(const TCHAR* Why);
	void UpdateMuscles(float DeltaTime);
	void UpdateGround(float DeltaTime);
	bool CanGetUp() const;
	void StartWrithe();
	void StopWrithe();
	void DriveWrithe();
	void BuildPose(TFunctionRef<void(const FMuscle&, float&, FVector&)> Shape, TArray<FQuat>& Out) const;
	void ApplyTargets(USkeletalMeshComponent* Mesh);
	void HoldCurrent(const USkeletalMeshComponent* Mesh);
	void StartGround(EGround Next);
	void DriveRoll(float DeltaTime);
	void DriveCrawl(float DeltaTime);
	bool WantsToCrawl() const;
	FVector GetChestFront(const USkeletalMeshComponent* Mesh) const;
	FVector GetBodyLength(const USkeletalMeshComponent* Mesh) const;
	FQuat GetCurrentRelative(const USkeletalMeshComponent* Mesh, const FMuscle& Part) const;
	void CheckTargets(USkeletalMeshComponent* Mesh);
	void SetUpperBodyGravity(bool bEnabled);

	TArray<FMuscle> Parts;
	EState State = EState::Standing;
	bool bReady = false;
	bool bPosesReady = false;
	bool bArmsAnimated = false;
	bool bWrithing = false;
	bool bTargetsChecked = false;
	float Shock = 0.0f;
	float Tone = 1.0f;
	float DownTime = 0.0f;
	float SettleTime = 0.0f;
	float WritheTime = 0.0f;
	float WritheBlend = 0.0f;
	EGround Ground = EGround::Writhe;
	float GroundTime = 0.0f;
	TArray<FQuat> PoseCrawlLeft;  // left arm reaching, right arm pulled in
	TArray<FQuat> PoseCrawlRight; // the other way round
	FVector HandPlant = FVector::ZeroVector;
	int32 PullingHand = 0; // -1 left, 1 right
	float RollWay = 0.0f;  // which way he rolls over, chosen when he starts
	FVector CrawlFrom = FVector::ZeroVector;
	// Which side hurts: -1 left leg, 1 right leg, 0 the trunk or nowhere in particular.
	float HurtLeg = 0.0f;
	float LogTimer = 0.0f;
	// The body's axes in the reference pose, component space.
	FVector BodyUp = FVector::UpVector;
	FVector BodyForward = FVector::ForwardVector;
	FVector BodyRight = FVector::RightVector;
	// The two poses he moves between while writhing (child relative to parent, per part), and what the joints aim at now.
	TArray<FQuat> PoseCurled;
	TArray<FQuat> PoseOpen;
	TArray<FQuat> Targets;
	FVector WritheFrom = FVector::ZeroVector;
	// How far the trunk bends after a round while he stands: [corps-actif] reaction.
	float BendWatch = -1.0f;
	float BendPeak = 0.0f;
	FVector BendFrom = FVector::UpVector;
};

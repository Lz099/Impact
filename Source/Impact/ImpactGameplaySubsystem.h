#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "ImpactGameplaySubsystem.generated.h"

class AAlsCharacter;
class ACharacter;
class AImpactFX;
class APlayerController;
class UCanvas;
class UImpactBodyComponent;
class UImpactWeaponComponent;

/**
 * Plugs the Impact systems into any map, including the ALS showcase, without touching ALS assets:
 * gives the player's ALS character the weapon component, places passers-by to shoot at,
 * owns the effects actor and draws the crosshair and the labels above people.
 * Started with -ImpactTest=fire it also plays a scripted shooting sequence for automatic checks.
 */
UCLASS()
class IMPACT_API UImpactGameplaySubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool DoesSupportWorldType(EWorldType::Type WorldType) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	AImpactFX* GetFX() const { return FX; }

	void RegisterBody(UImpactBodyComponent* Body);

	/** X-ray view: organs and bullet paths drawn over every body (F3). */
	void ToggleXRay() { bXRay = !bXRay; }

	/** A shot was fired: people around will react to it. */
	void ReportGunshot(AActor* Shooter, const FVector& Location);

	/**
	 * The T menu's test sections: one person placed in front of the player. WeaponKind 0 unarmed (a civilian), 1 rifle,
	 * 2 pistol in one hand, 3 pistol in two hands; Vest 0 none, 1 light, 2 medium, 3 heavy.
	 */
	ACharacter* SpawnTestPerson(int32 WeaponKind, int32 Vest, bool bHelmet);
	/** Removes every test person, and every dropped weapon lying about; returns how many people went. */
	int32 RemoveTestPeople();
	int32 CountTestPeople() const;

private:
	AAlsCharacter* GetPlayerCharacter() const;
	void SetupPlayer();
	void SpawnPassersBy(const AAlsCharacter* Player);
	int32 TestPeopleMade = 0;
	void ConfigureCharacterCollision();
	void RunTest(float DeltaTime);
	void RunAnatomyTest(float Time, AAlsCharacter* Player, UImpactWeaponComponent* Weapon);
	void RunFleeTest(float Time, AAlsCharacter* Player, UImpactWeaponComponent* Weapon);
	void RunWoundTest(float Time, AAlsCharacter* Player, UImpactWeaponComponent* Weapon);
	void RunLimpTest(float Time, AAlsCharacter* Player, UImpactWeaponComponent* Weapon);
	void RunCrawlTest(float Time, AAlsCharacter* Player, UImpactWeaponComponent* Weapon);
	void RunZonesTest(float Time, AAlsCharacter* Player, UImpactWeaponComponent* Weapon);
	void RunLegsTest(float Time, AAlsCharacter* Player, UImpactWeaponComponent* Weapon);
	void RunPushTest(float Time, AAlsCharacter* Player, UImpactWeaponComponent* Weapon);
	/** -ImpactTest=pnjtest: the T menu's test tabs used as the player would - open, place people, close, shoot, remove. */
	void RunTestPeopleBench(float Time, AAlsCharacter* Player, UImpactWeaponComponent* Weapon);
	/** -ImpactTest=gc: a fall, the garbage collector forced, another fall - twice. What crashed 60 s into games. */
	void RunCollectorTest(float Time);
	/** -ImpactTest=actif: one person in front of the player's camera, two single rounds, then a burst that drops him. */
	void RunActiveBench(float Time, AAlsCharacter* Player, UImpactWeaponComponent* Weapon);
	/** -ImpactTest=soldats: the armed passers-by as soldiers; the player moves every 10 s and fires a burst every 6 s. */
	void RunSoldierBench(float Time, AAlsCharacter* Player, UImpactWeaponComponent* Weapon);
	FVector SoldierBenchOrigin = FVector::ZeroVector;
	int32 SoldierBenchSpot = -1;
	// -ImpactTest=gang: the gang hanging about first (the player away, out of their sight), then the soldiers bench.
	void RunGangBench(float Time, AAlsCharacter* Player, UImpactWeaponComponent* Weapon);
	// -ImpactTest=gangpose: two of them held aiming at the player 6 m away, then each move in turn, for close-up photos.
	void RunGangPoseBench(float Time, AAlsCharacter* Player, UImpactWeaponComponent* Weapon);
	int32 GangPoseIndex[2] = {INDEX_NONE, INDEX_NONE};
	int32 GangPoseMoveDone = 0;
	FVector GangBenchOrigin = FVector::ZeroVector;
	int32 GangBenchStep = 0;
	TWeakObjectPtr<ACharacter> ActiveBenchTarget;
	float BenchRelaunch = 0.0f;
	float BenchNextShot = 0.0f;
	/** -ImpactTest=ventre: one round in the belly of a man standing still, his trunk and pelvis measured before and after. */
	void RunGutTest(float Time, AAlsCharacter* Player, UImpactWeaponComponent* Weapon);
	float GutLogTimer = 0.0f;
	TArray<TWeakObjectPtr<ACharacter>> TestPeople;
	float TestBurstEnd = -1.0f;
	void RunControlTest(float Time);
	void FilmCourseShot(float Time);
	void RunCourseTest(float Time, AAlsCharacter* Player, UImpactWeaponComponent* Weapon);
	void RunPainTest(float Time, AAlsCharacter* Player, UImpactWeaponComponent* Weapon);
	/** -ImpactTest=partie: shots at random passers-by and body parts for a minute, as in play. */
	void RunGameTest(float Time, AAlsCharacter* Player, UImpactWeaponComponent* Weapon);
	/** Looks for the bugs reported from play, in play as on every bench, and writes [bug] lines. */
	void DetectBugs(float DeltaTime);
	void RunBodyShoveTest(float Time, AAlsCharacter* Player, UImpactWeaponComponent* Weapon);
	void RunArmTest(float Time, AAlsCharacter* Player, UImpactWeaponComponent* Weapon);
	void RunRiddleTest(float Time, AAlsCharacter* Player, UImpactWeaponComponent* Weapon);
	int32 RiddleShots = 0;
	// DetectBugs: last pelvis position per body, last T-pose report, whether he was down, and the last [bug] summary.
	TArray<FVector> BugLastHips;
	// [sol-entree-vu]: per body, the drawn head, hands, feet and pelvis last frame, and the watch over the first 0,6 s of LyingDown.
	TArray<TArray<FVector>> EntryLast;
	TArray<float> EntryTime;
	TArray<float> EntryWorst;
	TArray<int32> EntryWorstAt;
	TArray<bool> EntryWasLying;
	TArray<bool> EntrySeen;
	TArray<float> BugLastTPose;
	TArray<bool> BugWasDown;
	TMap<TWeakObjectPtr<AActor>, float> BugDroppedSeen;
	TArray<float> BugLastFarWeapon;
	TArray<FVector> BugLastWeaponRel;
	float BugFarWeaponPeak = 0.0f;
	int32 BugCounts[8] = {0, 0, 0, 0, 0, 0, 0, 0};
	float BugSummaryTimer = 0.0f;
	float GameNextShot = 3.0f;
	int32 GameTarget = 0;
	FName GameBone;
	int32 GameShots = 0;
	float RiddleTimer = 0.0f;
	bool RiddleDown = false;
	bool bControlTestFailed = false;
	void MeasureGait(float Time);
	FVector GetOrganPoint(int32 Index, const TCHAR* OrganName) const;
	FVector GetBonePoint(int32 Index, const TCHAR* Bone) const;
	void AimAt(const FVector& Target) const;
	void DrawOverlay(UCanvas* Canvas, APlayerController* Controller);
	void StepWitness();

	UPROPERTY(Transient)
	TObjectPtr<AImpactFX> FX;

	// -ImpactAnimsPretes: our animations, loaded and compressed at the start and kept.
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UAnimSequence>> PreloadedAnims;

	TArray<TWeakObjectPtr<UImpactBodyComponent>> Bodies;
	float AimLogTimer = 0.0f;
	FDelegateHandle DrawHandle;
	bool bPassersBySpawned = false;
	float NavReportTimer = 2.0f;
	bool bNavBuilt = false;
	int32 NavReports = 0;
	float CollisionTimer = 0.0f;
	int32 GunshotCount = 0;
	bool bXRay = false;

	FString TestName;
	int32 TestStep = 0;
	int32 CurrentTestTarget = 0;

	/** -ImpactTest=limp: one person is shot in a leg and then walked in a straight line, and the gait is measured. */
	int32 PushStep = 0;
	float PushLogTimer = 0.0f;
	FVector PushStart = FVector::ZeroVector;
	FVector ShoveHipsStart = FVector::ZeroVector;
	TArray<FVector> ShoveBoneStart;
	TMap<TWeakObjectPtr<AActor>, FVector> ShoveWeaponStart;
	float ArmLogTimer = 0.0f;
	bool bArmTableLogged = false;
	FVector ArmHandLast[2] = {FVector::ZeroVector, FVector::ZeroVector};
	int32 ArmInsideFrames = 0;
	int32 ArmBehindFrames = 0;
	int32 ArmFrames = 0;
	float ShoveLogTimer = 0.0f;
	float ShoveContactTime = -1.0f;
	int32 LimpWalker = INDEX_NONE;
	FVector LimpDirection = FVector::ForwardVector;
	float GaitLogTimer = 0.0f;
	FVector GroundLast[5] = {};
	float GroundMoveSum = 0.0f;
	float GroundWindow = 0.0f;
	float GroundSlideSum = 0.0f;
	FVector GroundHipsLast = FVector::ZeroVector;
	FVector GroundHipsOrigin = FVector::ZeroVector;
	float GroundPeakLimb = 0.0f;
	float GroundPeakSlide = 0.0f;
	// Joint turning speed (deg/s) smoothed over about a tenth of a second, and its peak per window: a limb
	// snapping into place is a burst there, single-frame noise is not.
	float GroundJointSmooth = 0.0f;
	float GroundJointPeak = 0.0f;
	FString GroundJointPeakName;
	float GroundSlideSmooth = 0.0f;
	float GroundSlideSmoothPeak = 0.0f;
	// Aligned on the moment the ground posture is triggered (or, without it, the moment the body comes to rest,
	// which is the same trigger): samples every 0.1 s and close-ups at fixed delays after it.
	float GroundStart = -1.0f;
	float GroundNextSample = 0.0f;
	FString GroundJointNowName;
	float GroundTime = 0.0f;
	float GroundLogTimer = 0.0f;
	float GaitPelvisHigh = -1.0e9f;
	float GaitPelvisLow = 1.0e9f;
	float GaitLeftStance = 0.0f;
	float GaitRightStance = 0.0f;
	float GaitLeftDipSum = 0.0f;
	float GaitRightDipSum = 0.0f;

	/**
	 * -ImpactTest=course: a running man is shot in the shoulder. Two things are measured at the same instant,
	 * because the complaint was that they were not both true: his legs must keep carrying him, and his trunk
	 * must visibly take the hit. The trunk is judged against its own natural swing while running, not against
	 * standing still — running already moves a spine, so only what exceeds that swing is the reaction.
	 */
	FVector CourseDirection = FVector::ForwardVector;
	FQuat CourseReference = FQuat::Identity;
	bool bCourseReference = false;
	float CourseBeforeSpeedSum = 0.0f;
	float CourseBeforeTime = 0.0f;
	float CourseAfterSpeedSum = 0.0f;
	float CourseAfterTime = 0.0f;
	float CourseBeforePeak = 0.0f;
	float CourseAfterPeak = 0.0f;
	int32 CourseBeforeStrides = 0;
	int32 CourseAfterStrides = 0;
	bool bCourseLeftLower = false;
	FVector CourseStanceLast = FVector::ZeroVector;
	float CourseBeforeSkateSum = 0.0f;
	float CourseAfterSkateSum = 0.0f;
	FQuat CoursePreviousTrunk = FQuat::Identity;
	float CoursePreviousBend = 0.0f;
	float CourseRestSnapSum = 0.0f;
	float CourseRestTime = 0.0f;
	float CourseBeforeSnapSum = 0.0f;
	float CourseAfterSnapSum = 0.0f;
	float CourseBeforeSnap = 0.0f;
	float CourseAfterSnap = 0.0f;
	float CourseShotTime = 0.0f;
	bool bCourseReported = false;

	/**
	 * Close-up views used by automatic tests (-ImpactWitness=times): for a few frames the game camera moves
	 * to the side of the current target, then in front of it, and a screenshot is saved for each (X-ray included).
	 */
	TArray<float> WitnessTimes;
	int32 NextWitness = 0;
	int32 WitnessFrame = -1;
	int32 WitnessIndex = 0;
	int32 WitnessTarget = 0;

	UPROPERTY(Transient)
	TObjectPtr<class ACameraActor> WitnessCamera;
};

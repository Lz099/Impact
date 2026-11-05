#include "ImpactAutoCapture.h"

#include "Impact.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Misc/CommandLine.h"
#include "Misc/Paths.h"
#include "ShaderCompiler.h"
#include "UnrealClient.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(ImpactAutoCapture)

void UImpactAutoCapture::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	FString Shots;
	if (FParse::Value(FCommandLine::Get(), TEXT("ImpactShots="), Shots, false))
	{
		TArray<FString> Parts;
		Shots.ParseIntoArray(Parts, TEXT(","));
		for (const FString& Part : Parts)
		{
			ShotTimes.Add(FCString::Atof(*Part));
		}
		ShotTimes.Sort();
	}
	FParse::Value(FCommandLine::Get(), TEXT("ImpactQuit="), QuitTime);

	if (ShotTimes.Num() > 0 || QuitTime >= 0.0f)
	{
		TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &UImpactAutoCapture::Tick));
		UE_LOG(LogImpact, Display, TEXT("[autocapture] %d shots, quit at %.1f s"), ShotTimes.Num(), QuitTime);
	}
}

void UImpactAutoCapture::Deinitialize()
{
	FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
	Super::Deinitialize();
}

bool UImpactAutoCapture::Tick(float DeltaTime)
{
	const UWorld* World = GetGameInstance()->GetWorld();
	if (World == nullptr || !World->HasBegunPlay())
	{
		return true;
	}

	// The first launch compiles shaders for minutes: start the clock only once they are done,
	// otherwise the screenshots show placeholder materials.
	const int32 PendingShaders = GShaderCompilingManager != nullptr ? GShaderCompilingManager->GetNumRemainingJobs() : 0;
	if (PendingShaders > 0)
	{
		WaitLogTimer -= DeltaTime;
		if (WaitLogTimer <= 0.0f)
		{
			UE_LOG(LogImpact, Display, TEXT("[autocapture] waiting for %d shaders"), PendingShaders);
			WaitLogTimer = 10.0f;
		}
		return true;
	}

	bReady = true;
	Elapsed += DeltaTime;

	if (NextShot < ShotTimes.Num() && Elapsed >= ShotTimes[NextShot])
	{
		const FString File = FPaths::ScreenShotDir() / FString::Printf(TEXT("auto_%02d.png"), NextShot);
		FScreenshotRequest::RequestScreenshot(File, true, false);
		UE_LOG(LogImpact, Display, TEXT("[autocapture] t=%.1f screenshot %s"), Elapsed, *File);
		++NextShot;
	}

	if (QuitTime >= 0.0f && Elapsed >= QuitTime)
	{
		UE_LOG(LogImpact, Display, TEXT("[autocapture] t=%.1f quit"), Elapsed);
		FPlatformMisc::RequestExit(false, TEXT("ImpactAutoCapture"));
		return false;
	}

	return true;
}

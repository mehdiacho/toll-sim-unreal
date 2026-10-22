#include "TollGameMode.h"

#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "HighResScreenshot.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "TimerManager.h"
#include "UnrealClient.h"
#include "TollHUD.h"
#include "TollTrafficActor.h"

ATollGameMode::ATollGameMode()
{
	DefaultPawnClass = nullptr;
	HUDClass = ATollHUD::StaticClass();
	PrimaryActorTick.bCanEverTick = true;
}

void ATollGameMode::StartPlay()
{
	Super::StartPlay();
	UWorld* World = GetWorld();
	ATollTrafficActor* Scene = nullptr;
	for (TActorIterator<ATollTrafficActor> It(World); It; ++It)
	{
		Scene = *It;
		break;
	}
	if (!Scene) Scene = World->SpawnActor<ATollTrafficActor>();
	if (APlayerController* PC = World->GetFirstPlayerController())
	{
		PC->SetViewTarget(Scene);
	}

	if (FParse::Value(FCommandLine::Get(), TEXT("TollShot="), ShotPath))
	{
		float Delay = 8.f;
		FParse::Value(FCommandLine::Get(), TEXT("TollShotDelay="), Delay);
		World->GetTimerManager().SetTimer(ShotTimer, this, &ATollGameMode::TakeShot, Delay, false);
	}

	if (FParse::Value(FCommandLine::Get(), TEXT("TollClip="), ClipDir))
	{
		// every frame advances exactly 1/fps of game time, however long it takes to render,
		// so the replay plays back at real speed in the output and the clip is repeatable
		float Seconds = 30.f, Fps = 30.f, Warmup = 20.f;
		FParse::Value(FCommandLine::Get(), TEXT("TollClipSeconds="), Seconds);
		FParse::Value(FCommandLine::Get(), TEXT("TollClipFPS="), Fps);
		FParse::Value(FCommandLine::Get(), TEXT("TollClipWarmup="), Warmup);
		FApp::SetUseFixedTimeStep(true);
		FApp::SetFixedDeltaTime(1.0 / Fps);
		ClipFrames = FMath::RoundToInt(Seconds * Fps);
		ClipWarmup = FMath::RoundToInt(Warmup * Fps);
		ClipFrame = 0;
	}
}

void ATollGameMode::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (ClipFrame < 0) return;
	const int32 Out = ClipFrame++ - ClipWarmup;
	if (Out >= ClipFrames)
	{
		ClipFrame = -1;
		FPlatformMisc::RequestExit(false, TEXT("TollClip"));
		return;
	}
	if (Out >= 0) FScreenshotRequest::RequestScreenshot(FString::Printf(TEXT("%s/frame_%05d.png"), *ClipDir, Out), false, false);
}

void ATollGameMode::TakeShot()
{
	FScreenshotRequest::RequestScreenshot(ShotPath, false, false);
	FTimerHandle Quit;
	GetWorld()->GetTimerManager().SetTimer(Quit, FTimerDelegate::CreateLambda([]
	{
		FPlatformMisc::RequestExit(false, TEXT("TollShot"));
	}), 2.f, false);
}

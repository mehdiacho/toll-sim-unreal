#include "TollGameMode.h"

#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "HighResScreenshot.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "TimerManager.h"
#include "UnrealClient.h"
#include "TollTrafficActor.h"

ATollGameMode::ATollGameMode()
{
	DefaultPawnClass = nullptr;
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

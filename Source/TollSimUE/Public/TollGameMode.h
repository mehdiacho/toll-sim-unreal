#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "TollGameMode.generated.h"

/** No pawn: the view is the traffic actor's camera. Spawns the scene if the map has none. */
UCLASS()
class TOLLSIMUE_API ATollGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	ATollGameMode();
	virtual void StartPlay() override;

private:
	/** -TollShot=<path> [-TollShotDelay=<s>]: save a still once traffic has settled, then quit. */
	void TakeShot();

	FString ShotPath;
	FTimerHandle ShotTimer;
};

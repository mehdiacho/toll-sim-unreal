#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "TollHUD.generated.h"

/**
 * The credits the CC BY assets need on screen: shown for the first seconds, then on C.
 * Esc quits. Nothing is drawn in -TollShot or -TollClip runs.
 */
UCLASS()
class TOLLSIMUE_API ATollHUD : public AHUD
{
	GENERATED_BODY()

public:
	virtual void BeginPlay() override;
	virtual void DrawHUD() override;

private:
	bool bQuiet = false;   // stills and clips
	bool bShown = false;   // toggled with C
	double ShownUntil = 0; // world seconds the opening credits stay up
};

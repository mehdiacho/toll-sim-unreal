#include "TollHUD.h"

#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

namespace
{
	// keep in step with ATTRIBUTION.md
	const TCHAR* const Credits[] = {
		TEXT("Toll Sim: the A1 corridor, Botswana"),
		TEXT(""),
		TEXT("Namaqualand Environment and Asset Pack by Poly Haven, CC BY 4.0"),
		TEXT("Acacia Tree by Incg5764, CC BY 4.0"),
		TEXT("    creativecommons.org/licenses/by/4.0"),
		TEXT("City Sample Vehicles by Epic Games; Quixel Megascans; Free Shrubs Pack by Greenleaf Vision (Fab)"),
		TEXT("Road network: OpenStreetMap contributors, ODbL"),
		TEXT("Terrain contains modified Copernicus DEM GLO-30 data: DLR e.V. 2010-2014, Airbus Defence and Space GmbH 2014-2018"),
		TEXT(""),
		TEXT("C  credits      Esc  quit"),
	};
	constexpr double OpeningSeconds = 12.0, FadeSeconds = 2.0;
}

void ATollHUD::BeginPlay()
{
	Super::BeginPlay();
	const TCHAR* Cmd = FCommandLine::Get();
	bQuiet = FParse::Param(Cmd, TEXT("TollNoCredits")) || FCString::Strifind(Cmd, TEXT("-TollShot=")) || FCString::Strifind(Cmd, TEXT("-TollClip="));
	if (FParse::Param(Cmd, TEXT("TollCredits"))) bQuiet = false, bShown = true; // a still of the credits
	ShownUntil = GetWorld()->GetTimeSeconds() + OpeningSeconds;
}

void ATollHUD::DrawHUD()
{
	Super::DrawHUD();
	APlayerController* PC = PlayerOwner;
	if (bQuiet || !PC || !Canvas) return;
	if (PC->WasInputKeyJustPressed(EKeys::Escape))
	{
		PC->ConsoleCommand(TEXT("quit"));
		return;
	}
	if (PC->WasInputKeyJustPressed(EKeys::C))
	{
		bShown = !bShown;
		ShownUntil = 0; // C also dismisses the opening credits
	}

	const double Left = ShownUntil - GetWorld()->GetTimeSeconds();
	const float Alpha = bShown ? 1.f : static_cast<float>(FMath::Clamp(Left / FadeSeconds, 0.0, 1.0));
	if (Alpha <= 0.f) return;

	UFont* Font = GEngine->GetMediumFont();
	const float Scale = Canvas->ClipY / 1080.f * 1.2f;
	const float Line = Font->GetMaxCharHeight() * Scale * 1.25f;
	const int32 N = UE_ARRAY_COUNT(Credits);
	float W = 0;
	for (const TCHAR* C : Credits)
	{
		float Tw, Th;
		GetTextSize(C, Tw, Th, Font, Scale);
		W = FMath::Max(W, Tw);
	}
	const float Pad = 14.f * Scale, X = 28.f * Scale;
	float Y = Canvas->ClipY - 28.f * Scale - N * Line - 2 * Pad;
	DrawRect(FLinearColor(0.f, 0.f, 0.f, 0.55f * Alpha), X, Y, W + 2 * Pad, N * Line + 2 * Pad);
	Y += Pad;
	for (int32 i = 0; i < N; ++i, Y += Line)
	{
		const FLinearColor Col = i == 0 || i == N - 1 ? FLinearColor(1.f, 0.85f, 0.55f, Alpha) : FLinearColor(0.92f, 0.92f, 0.9f, Alpha);
		DrawText(Credits[i], Col, X + Pad, Y, Font, Scale);
	}
}

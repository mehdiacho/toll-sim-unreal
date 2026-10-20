// Holds the C++ feed port to the browser. golden-positions.json is written by
// toll-sim's viewer/scripts/export-golden.mjs from the same replay, using the
// browser's own decoder, interpolation and placement code.

#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "TollFeedSubsystem.h"
#include "TollTraffic.h"
#include "TollPlaza.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTollGeometryTest, "TollSim.Traffic.Geometry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FTollGeometryTest::RunTest(const FString&)
{
	using namespace TollTraffic;
	TestEqual(TEXT("bus length"), ClassLen(TEXT("2")), 7.5);
	TestEqual(TEXT("unknown class is a car"), ClassLen(TEXT("x")), 4.6);
	const FVector2D North = FrontToCentre(10, 20, 0, 4);
	TestTrue(TEXT("heading north steps back south"), North.Equals(FVector2D(10, 18), 1e-9));
	const FVector2D East = FrontToCentre(10, 20, 90, 4);
	TestTrue(TEXT("heading east steps back west"), East.Equals(FVector2D(8, 20), 1e-9));
	TestEqual(TEXT("short angle across north"), ShortAngle(350, 10), 370.0);
	TestEqual(TEXT("short angle backwards"), ShortAngle(10, 350), -10.0);
	TestEqual(TEXT("JS rounding of halves"), JsRound(-2.5), -2.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTollGoldenTest, "TollSim.Traffic.GoldenPositions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FTollGoldenTest::RunTest(const FString&)
{
	using namespace TollTraffic;

	double Dt = 0;
	TArray<TollTraffic::FFrame> Frames; // qualified: the engine has its own FFrame
	if (!UTollFeedSubsystem::LoadReplay(Dt, Frames))
	{
		AddError(TEXT("could not load Content/Data/replay.json"));
		return false;
	}

	FString Json;
	TSharedPtr<FJsonObject> Golden;
	const FString GoldenPath = FPaths::ProjectContentDir() / TEXT("Data/golden-positions.json");
	if (!FFileHelper::LoadFileToString(Json, *GoldenPath) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Golden) || !Golden.IsValid())
	{
		AddError(FString::Printf(TEXT("could not load %s"), *GoldenPath));
		return false;
	}
	TestEqual(TEXT("lag matches the browser"), Golden->GetNumberField(TEXT("lag")), Lag);

	TMap<int32, TArray<TSharedPtr<FJsonValue>>> Expected;
	for (const TSharedPtr<FJsonValue>& FV : Golden->GetArrayField(TEXT("frames")))
	{
		const TSharedPtr<FJsonObject> F = FV->AsObject();
		Expected.Add(static_cast<int32>(F->GetNumberField(TEXT("k"))), F->GetArrayField(TEXT("v")));
	}

	// the plaza layouts, for the drawn positions in columns 4 and 5
	TArray<TollPlaza::FLayout> Layouts;
	{
		FString PJson;
		TSharedPtr<FJsonObject> PRoot;
		if (!FFileHelper::LoadFileToString(PJson, *(FPaths::ProjectContentDir() / TEXT("Data/Plazas/plazas.json"))) ||
			!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(PJson), PRoot) || !PRoot.IsValid())
		{
			AddError(TEXT("Content/Data/Plazas/plazas.json missing: run Scripts/sync-data.sh"));
			return false;
		}
		TollPlaza::ParseLayouts(PRoot->GetArrayField(TEXT("plazas")), Layouts);
	}

	TMap<int32, FTrack> Tracks;
	TMap<int32, FString> Classes;
	TArray<FVisible> Visible;
	int32 Compared = 0, Frames_ = 0, Remapped = 0;
	double Worst = 0, WorstHeading = 0, WorstDrawn = 0;
	for (int32 k = 0; k < Frames.Num(); ++k)
	{
		for (const FSpawn& S : Frames[k].Spawn) Classes.Add(S.Id, S.Cls);
		Ingest(Tracks, Frames[k]);
		CollectVisible(Tracks, Classes, Frames[k].T - Lag, Visible);

		const TArray<TSharedPtr<FJsonValue>>* Rows = Expected.Find(k);
		if (!Rows) continue;
		++Frames_;
		TMap<int32, const FVisible*> Ours;
		for (const FVisible& V : Visible) Ours.Add(V.Id, &V);
		TestEqual(FString::Printf(TEXT("frame %d vehicle count"), k), Ours.Num(), Rows->Num());
		for (const TSharedPtr<FJsonValue>& R : *Rows)
		{
			const TArray<TSharedPtr<FJsonValue>>& A = R->AsArray();
			const int32 Id = static_cast<int32>(A[0]->AsNumber());
			const FVisible* const* V = Ours.Find(Id);
			if (!V)
			{
				AddError(FString::Printf(TEXT("frame %d: vehicle %d missing"), k, Id));
				continue;
			}
			const double D = FVector2D::Distance(FVector2D((*V)->X, (*V)->Y), FVector2D(A[1]->AsNumber(), A[2]->AsNumber()));
			Worst = FMath::Max(Worst, D);
			WorstHeading = FMath::Max(WorstHeading, FMath::Abs((*V)->Heading - A[3]->AsNumber()));
			if (D > 0.1)
			{
				AddError(FString::Printf(TEXT("frame %d vehicle %d off by %.3f m"), k, Id, D));
			}
			if (A.Num() > 5)
			{
				const FVector2D Ours2 = TollPlaza::Drawn(Layouts, (*V)->X, (*V)->Y, (*V)->Heading);
				const FVector2D Theirs(A[4]->AsNumber(), A[5]->AsNumber());
				const double DD = FVector2D::Distance(Ours2, Theirs);
				WorstDrawn = FMath::Max(WorstDrawn, DD);
				if (FVector2D::Distance(Theirs, FVector2D(A[1]->AsNumber(), A[2]->AsNumber())) > 0.01) ++Remapped;
				if (DD > 0.1)
				{
					AddError(FString::Printf(TEXT("frame %d vehicle %d drawn off by %.3f m"), k, Id, DD));
				}
			}
			++Compared;
		}
	}
	TestEqual(TEXT("golden frames checked"), Frames_, Expected.Num());
	AddInfo(FString::Printf(TEXT("compared %d positions over %d frames; worst %.4f m, worst heading %.3f deg; %d remapped onto plaza lanes, worst drawn %.4f m"),
		Compared, Frames_, Worst, WorstHeading, Remapped, WorstDrawn));
	TestTrue(TEXT("some vehicles remapped onto plaza lanes"), Remapped > 100);
	TestTrue(TEXT("compared some positions"), Compared > 1000);
	TestTrue(TEXT("headings within 0.01 deg"), WorstHeading < 0.01);
	return true;
}

#endif

// Toll plaza layout and the lane remap, ported from toll-sim's
// viewer/src/plaza-layout.js and the plaza branch of viewer.js updateVehicles.
// The plaza geometry itself is not ported: it comes ready-made in
// Content/Data/Plazas/plazas.json, written by the browser's own builder.
//
// Plaza frame: u along the northbound direction, v to its left, metres.
// t is distance along a lane in its own direction (t = u northbound, -u southbound).

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

namespace TollPlaza
{
	constexpr double BoomT = 5;
	constexpr double FlareIn0 = -260, FlareIn1 = -70;
	constexpr double FlareOut0 = 24, FlareOut1 = 210;
	constexpr double Zone0 = -320, Zone1 = 230;

	struct FLane
	{
		TCHAR Dir = 'N';
		int32 Idx = 0;
		FString Type;
		FString Key;
		double U = 0, V = 0, W = 3.5, SrcV = 0;
	};

	struct FLayout
	{
		FString Id, Name;
		double X = 0, Y = 0, Heading = 0;
		double Ux = 1, Uy = 0, Nx = 0, Ny = 1;
		double HalfWidth = 10;
		bool bRemap = false;
		TArray<FLane> Lanes; // sorted by v, as the browser keeps them

		FVector2D ToUV(double PX, double PY) const;
		FVector2D FromUV(double U, double V) const;
	};

	/** Where a vehicle is in a plaza's zone, if it is in one. */
	struct FContext
	{
		int32 Plaza = INDEX_NONE;
		TCHAR Dir = 'N';
		double T = 0, U = 0, V = 0;
	};

	TOLLSIMUE_API double Flare(double T);
	TOLLSIMUE_API double MapV(const FLayout& P, TCHAR Dir, double V);
	TOLLSIMUE_API const FLane* LaneAt(const FLayout& P, TCHAR Dir, double V, double Slack = 0.4);

	/** Parse the "plazas" array of plazas.json (layout part only). */
	TOLLSIMUE_API void ParseLayouts(const TArray<TSharedPtr<FJsonValue>>& Plazas, TArray<FLayout>& Out);

	/** viewer.js plazaContext. */
	TOLLSIMUE_API bool Context(const TArray<FLayout>& Plazas, double X, double Y, double HeadingDeg, FContext& Out);

	/**
	 * Where to draw a vehicle whose centre the feed puts at (X, Y): eased onto
	 * the drawn plaza lanes through the flare when the exported lanes are packed.
	 */
	TOLLSIMUE_API FVector2D Drawn(const TArray<FLayout>& Plazas, double X, double Y, double HeadingDeg, FContext* OutCtx = nullptr);
}

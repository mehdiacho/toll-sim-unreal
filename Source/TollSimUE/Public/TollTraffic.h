// Feed decoding, per-vehicle interpolation and placement: a line-for-line port
// of toll-sim's viewer/src/traffic.js and vehicle-geometry.js. The automation
// test TollSim.Traffic.GoldenPositions holds it to the browser within 0.1 m.
//
// Units here are the sim's: metres, X east, Y north, heading in degrees
// clockwise from +Y. Conversion to Unreal space lives in TollSpace.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

namespace TollTraffic
{
	/** Seconds the view runs behind the newest frame, to interpolate. */
	constexpr double Lag = 2.4;

	struct FSample
	{
		double T = 0, X = 0, Y = 0, Heading = 0, Speed = 0;
	};

	struct FTrack
	{
		TArray<FSample, TInlineAllocator<4>> S;
		TOptional<double> GoneAt;
	};

	struct FFrameVehicle
	{
		int32 Id = 0;
		double X = 0, Y = 0; // decimetres, as on the wire
		TOptional<double> Heading;
		double Speed = 0; // decimetres per second
	};

	struct FSpawn
	{
		int32 Id = 0;
		FString Plate;
		FString Cls;
		int32 Flags = 0;
	};

	struct FFrame
	{
		double T = 0;
		TArray<FFrameVehicle> Vehicles;
		TArray<FSpawn> Spawn;
		TArray<int32> Gone;
		/** booms, lanes_open, stats, events: kept as JSON for the layers that want them. */
		TSharedPtr<FJsonObject> Extra;
	};

	struct FVisible
	{
		int32 Id = 0;
		FString Cls;
		double X = 0, Y = 0; // vehicle centre, metres
		double Heading = 0, Speed = 0;
	};

	TOLLSIMUE_API double ShortAngle(double A, double B);

	/** Vehicle length in metres for a toll class; matches sim/demand.py. */
	TOLLSIMUE_API double ClassLen(const FString& Cls);

	/** SUMO reports the front bumper; step back half a length to the centre. */
	TOLLSIMUE_API FVector2D FrontToCentre(double X, double Y, double HeadingDeg, double Len);

	/** Decode the compact replay.json into full frames (traffic.js decodeReplay). */
	TOLLSIMUE_API bool DecodeReplay(const FString& Json, double& OutDt, TArray<FFrame>& OutFrames);

	/** Parse one live SSE frame object ({t, vehicles, spawn, gone, ...}). */
	TOLLSIMUE_API bool ParseFrame(const TSharedPtr<FJsonObject>& Obj, FFrame& Out);

	/** Parse spawn rows [[id, plate, cls, flags], ...]. */
	TOLLSIMUE_API void ParseSpawn(const TArray<TSharedPtr<FJsonValue>>& Rows, TArray<FSpawn>& Out);

	/** Add a frame's vehicles to the sample buffers (traffic.js ingestVehicles). */
	TOLLSIMUE_API void Ingest(TMap<int32, FTrack>& Tracks, const FFrame& Frame);

	/** Interpolated sample at time T (traffic.js sampleAt). */
	TOLLSIMUE_API bool SampleAt(const FTrack& Track, double T, FSample& Out);

	/**
	 * Vehicles on screen at RenderT, centred (viewer.js updateVehicles, before the
	 * plaza lane remap). Drops tracks whose vehicle has left by RenderT.
	 */
	TOLLSIMUE_API void CollectVisible(TMap<int32, FTrack>& Tracks, const TMap<int32, FString>& Classes, double RenderT, TArray<FVisible>& Out);

	/** JS Math.round: halves round toward +infinity. */
	inline double JsRound(double V) { return FMath::FloorToDouble(V + 0.5); }
}

// M1 scene: road ribbons from road.json, one instanced box per vehicle at its
// class's size, a sun and sky, and a camera. Detailed vehicles, plazas and
// terrain replace the stand-ins in later milestones; the placement does not change.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "TollPlaza.h"
#include "TollTraffic.h"
#include "TollTrafficActor.generated.h"

class UCameraComponent;
class UInstancedStaticMeshComponent;
class UHierarchicalInstancedStaticMeshComponent;
class UProceduralMeshComponent;
class UDirectionalLightComponent;
class USkyAtmosphereComponent;
class USkyLightComponent;
class UExponentialHeightFogComponent;
class UPostProcessComponent;
class UTollFeedSubsystem;
class ATollPlazaActor;

/** Sim metres (X east, Y north, heading clockwise from north) to Unreal space. */
namespace TollSpace
{
	inline FVector ToWorld(const FVector2D& Origin, double X, double Y, double Z = 0)
	{
		return FVector((X - Origin.X) * 100.0, -(Y - Origin.Y) * 100.0, Z * 100.0);
	}
	inline double ToYaw(double HeadingDeg) { return HeadingDeg - 90.0; }
}

UCLASS()
class TOLLSIMUE_API ATollTrafficActor : public AActor
{
	GENERATED_BODY()

public:
	ATollTrafficActor();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	/** Plaza to frame at start, e.g. A1-G01. Overridden by -TollCam=plaza:<id>. */
	UPROPERTY(EditAnywhere, Category = "Toll")
	FString StartPlaza = TEXT("A1-G01");

	UPROPERTY(VisibleAnywhere, Category = "Toll")
	TObjectPtr<UCameraComponent> Camera;

	UPROPERTY(VisibleAnywhere, Category = "Toll")
	TObjectPtr<UProceduralMeshComponent> Road;

	/** Land around the road from the Copernicus DEM (Content/Data/terrain.bin), relative to road level. */
	UPROPERTY(VisibleAnywhere, Category = "Toll")
	TObjectPtr<UProceduralMeshComponent> Terrain;

	UPROPERTY(VisibleAnywhere, Category = "Toll")
	TArray<TObjectPtr<UInstancedStaticMeshComponent>> ClassMeshes;

	/** Motorbike and rider, one instanced mesh per primitive part (no pack has a bike). */
	UPROPERTY(VisibleAnywhere, Category = "Toll")
	TArray<TObjectPtr<UInstancedStaticMeshComponent>> BikeMeshes;

	UPROPERTY(VisibleAnywhere, Category = "Toll")
	TObjectPtr<UDirectionalLightComponent> Sun;

	UPROPERTY(VisibleAnywhere, Category = "Toll")
	TObjectPtr<USkyAtmosphereComponent> Sky;

	UPROPERTY(VisibleAnywhere, Category = "Toll")
	TObjectPtr<USkyLightComponent> SkyLight;

	UPROPERTY(VisibleAnywhere, Category = "Toll")
	TObjectPtr<UExponentialHeightFogComponent> Fog;

	/** Exposure limits, so night stays night. */
	UPROPERTY(VisibleAnywhere, Category = "Toll")
	TObjectPtr<UPostProcessComponent> Post;

	/** Moonlight, on when the sun is down. */
	UPROPERTY(VisibleAnywhere, Category = "Toll")
	TObjectPtr<UDirectionalLightComponent> Moon;

	/** Head, tail and brake lamps, one instance per lamp. */
	UPROPERTY(VisibleAnywhere, Category = "Toll")
	TObjectPtr<UInstancedStaticMeshComponent> HeadLamps;
	UPROPERTY(VisibleAnywhere, Category = "Toll")
	TObjectPtr<UInstancedStaticMeshComponent> TailLamps;
	UPROPERTY(VisibleAnywhere, Category = "Toll")
	TObjectPtr<UInstancedStaticMeshComponent> BrakeLamps;

	/** Local solar time, hours. -TollTime=<h>. */
	double Hour = 14.0;

	/** Number of vehicles drawn last tick. */
	int32 DrawnCount() const { return Drawn; }

private:
	bool LoadRoad();
	bool LoadTerrain();
	void LoadModels();
	void LoadFoliage();
	/** Terrain height in metres at a sim position, as the terrain mesh draws it. */
	double GroundZ(double X, double Y) const;
	/** Distance in metres from a sim position to the nearest lane centreline, up to Max. */
	double RoadDist(const FVector2D& P, double Max) const;
	/** Whether a sim position is on a plaza's flared apron, or within Margin metres of it. */
	bool OnApron(const FVector2D& P, double Margin) const;
	void LoadPlazas();
	void FrameCamera();
	void FollowCamera(const TArray<TollTraffic::FVisible>& Vis);
	int32 FollowId = INDEX_NONE;

	UPROPERTY()
	TArray<TObjectPtr<ATollPlazaActor>> Plazas; // parallel to Layouts

	TArray<TollPlaza::FLayout> Layouts;

	FVector2D Origin = FVector2D::ZeroVector;
	TMap<FString, FVector2D> PlazaPos;
	TMap<FString, double> PlazaHeading;
	void ApplyTimeOfDay();
	static void SetInstances(UInstancedStaticMeshComponent* Ism, const TArray<FTransform>& Xf, const TArray<float>* Custom = nullptr);

	/** Kinematic body motion per vehicle: no physics, just what the speed trace implies. */
	struct FMotion
	{
		double PrevSpeed = -1;
		double Accel = 0; // m/s^2, smoothed
		double Phase = 0; // suspension bob
		double Dist = 0;  // metres rolled, for wheel spin
		bool bSeen = false;
	};
	TMap<int32, FMotion> Motion;
	double Night = 0; // 0 day .. 1 full night

	/** A detailed vehicle (City Sample Vehicles): rigid parts and wheels, each an instanced mesh. */
	struct FVehicleModel
	{
		struct FWheel
		{
			UInstancedStaticMeshComponent* Ism = nullptr;
			FVector Centre = FVector::ZeroVector; // model space, cm
			double Radius = 35;
		};
		FString Name;
		TArray<UInstancedStaticMeshComponent*> Rigid;
		TArray<FWheel> Wheels;
		double OffsetX = 0; // cm; centres the body on the sim position
		FBox Body;          // body bounds after the offset, cm
		TArray<TArray<FTransform>> RigidXf, WheelXf;
		TArray<float> Packed; // per instance: paint variation and lamp bits, read by the pack's materials
	};
	TArray<FVehicleModel> Models;
	TMap<FString, TArray<int32>> ClassModels; // toll class -> models it is drawn with

	UPROPERTY()
	TArray<TObjectPtr<UInstancedStaticMeshComponent>> ModelMeshes; // keeps the model components alive

	TMap<FIntPoint, TArray<int16>> TerrainChunks; // decimetres, (Cells+1)^2 per chunk
	int32 TerrainCells = 0;
	double TerrainCell = 30;
	TMap<FIntPoint, TArray<FVector2D>> RoadGrid; // lane centreline points in 32 m cells

	UPROPERTY()
	TArray<TObjectPtr<UHierarchicalInstancedStaticMeshComponent>> FoliageMeshes;

	TArray<FTransform> Scratch;
	TArray<FTransform> HeadXf, TailXf, BrakeXf;
	TArray<TArray<FTransform>> BikeXf; // per bike part
	TArray<FVector2D> DrawnPos;
	int32 Drawn = 0;
};

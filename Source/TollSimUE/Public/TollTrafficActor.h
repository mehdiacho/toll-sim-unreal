// M1 scene: road ribbons from road.json, one instanced box per vehicle at its
// class's size, a sun and sky, and a camera. Detailed vehicles, plazas and
// terrain replace the stand-ins in later milestones; the placement does not change.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "TollPlaza.h"
#include "TollTrafficActor.generated.h"

class UCameraComponent;
class UInstancedStaticMeshComponent;
class UProceduralMeshComponent;
class UDirectionalLightComponent;
class USkyAtmosphereComponent;
class USkyLightComponent;
class UExponentialHeightFogComponent;
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

	UPROPERTY(VisibleAnywhere, Category = "Toll")
	TArray<TObjectPtr<UInstancedStaticMeshComponent>> ClassMeshes;

	UPROPERTY(VisibleAnywhere, Category = "Toll")
	TObjectPtr<UDirectionalLightComponent> Sun;

	UPROPERTY(VisibleAnywhere, Category = "Toll")
	TObjectPtr<USkyAtmosphereComponent> Sky;

	UPROPERTY(VisibleAnywhere, Category = "Toll")
	TObjectPtr<USkyLightComponent> SkyLight;

	UPROPERTY(VisibleAnywhere, Category = "Toll")
	TObjectPtr<UExponentialHeightFogComponent> Fog;

	/** Number of vehicles drawn last tick. */
	int32 DrawnCount() const { return Drawn; }

private:
	bool LoadRoad();
	void LoadPlazas();
	void FrameCamera();

	UPROPERTY()
	TArray<TObjectPtr<ATollPlazaActor>> Plazas; // parallel to Layouts

	TArray<TollPlaza::FLayout> Layouts;

	FVector2D Origin = FVector2D::ZeroVector;
	TMap<FString, FVector2D> PlazaPos;
	TMap<FString, double> PlazaHeading;
	TArray<FTransform> Scratch;
	TArray<FVector2D> DrawnPos;
	int32 Drawn = 0;
};

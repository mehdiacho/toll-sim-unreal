// One toll plaza, built from the browser's own geometry (Content/Data/Plazas):
// apron, markings, islands, booths, canopy, signs and fare boards as procedural
// mesh sections, and the booms and booth lights as moving parts.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "TollPlaza.h"
#include "TollPlazaActor.generated.h"

class UProceduralMeshComponent;
class UStaticMeshComponent;
class UMaterialInstanceDynamic;

USTRUCT()
struct FTollBoom
{
	GENERATED_BODY()

	FString Key;
	FString Type;
	UPROPERTY()
	TObjectPtr<UProceduralMeshComponent> Arm;
	UPROPERTY()
	TObjectPtr<UStaticMeshComponent> Lamp;
	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> LampMat;
	double Angle = 0; // radians, 0 = down
	int32 Side = 1;
	bool bOpen = true;
	bool bLocalUp = false;
	double FeedUntil = 0; // render time until which the feed holds it up
};

UCLASS()
class TOLLSIMUE_API ATollPlazaActor : public AActor
{
	GENERATED_BODY()

public:
	ATollPlazaActor();

	/** Build from one entry of plazas.json. TextureDir holds the exported PNGs. */
	void Build(const TSharedPtr<FJsonObject>& Plaza, const FString& TextureDir, const FVector2D& Origin);

	/** A vehicle in this plaza's zone: lifts the boom it is passing (viewer.js localUp). */
	void NoteVehicle(const TollPlaza::FLayout& Layout, const TollPlaza::FContext& Ctx, double Len, double Speed);

	/** A boom event from the feed, [plazaId, dir, idx, up], due at render time At. */
	void NoteFeedBoom(const FString& Key, bool bUp, double At);

	/** Swing the booms toward their targets and light the booth lamps. */
	void Advance(double Dt, double RenderT);

	UPROPERTY(VisibleAnywhere, Category = "Toll")
	TObjectPtr<UProceduralMeshComponent> Static;

	UPROPERTY()
	TArray<FTollBoom> Booms;

	FString PlazaId;

private:
	UMaterialInstanceDynamic* MaterialFor(const FString& MeshRole, const FString& Texture, const FString& TextureDir);
};

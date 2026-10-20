#include "TollTrafficActor.h"

#include "Camera/CameraComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/PostProcessComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Components/SkyLightComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "ProceduralMeshComponent.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "TollFeedSubsystem.h"
#include "TollPlazaActor.h"
#include "UObject/ConstructorHelpers.h"

DEFINE_LOG_CATEGORY_STATIC(LogTollScene, Log, All);

namespace
{
	struct FClassLook
	{
		const TCHAR* Cls;
		double Width, Height;
		FLinearColor Color;
	};

	// lengths come from TollTraffic::ClassLen; colours match the browser's CLASS_INFO
	const FClassLook Looks[] = {
		{TEXT("M"), 0.8, 1.4, FLinearColor::FromSRGBColor(FColor(0xe0, 0x56, 0x4f))},
		{TEXT("1"), 1.8, 1.5, FLinearColor::FromSRGBColor(FColor(0xe8, 0xed, 0xf3))},
		{TEXT("2"), 2.35, 3.1, FLinearColor::FromSRGBColor(FColor(0xf5, 0xb8, 0x3d))},
		{TEXT("3"), 2.5, 3.6, FLinearColor::FromSRGBColor(FColor(0x75, 0xaa, 0xdb))},
		{TEXT("4"), 2.55, 3.9, FLinearColor::FromSRGBColor(FColor(0x9b, 0x8c, 0xf0))},
	};

	struct FLaneLine
	{
		int32 Idx = 0;
		double W = 0;
		TArray<FVector2D> P;
	};

	struct FMeshBuf
	{
		TArray<FVector> V;
		TArray<int32> T;
		TArray<FVector> N;
	};

	/** A strip between two offsets from a polyline (positive = left in SUMO coords), as world.js ribbon(). */
	void Ribbon(const TArray<FVector2D>& P, const FVector2D& Origin, double Left, double Right, double Z, FMeshBuf& Out)
	{
		const int32 Num = P.Num();
		if (Num < 2) return;
		const int32 Base = Out.V.Num();
		for (int32 i = 0; i < Num; ++i)
		{
			const FVector2D Dir = (P[FMath::Min(i + 1, Num - 1)] - P[FMath::Max(i - 1, 0)]).GetSafeNormal();
			const FVector2D Nrm(-Dir.Y, Dir.X);
			Out.V.Add(TollSpace::ToWorld(Origin, P[i].X + Nrm.X * Left, P[i].Y + Nrm.Y * Left, Z));
			Out.V.Add(TollSpace::ToWorld(Origin, P[i].X + Nrm.X * Right, P[i].Y + Nrm.Y * Right, Z));
			Out.N.Add(FVector::UpVector);
			Out.N.Add(FVector::UpVector);
		}
		for (int32 i = 0; i + 1 < Num; ++i)
		{
			const int32 A = Base + i * 2;
			// winding faces up in Unreal's left-handed space
			Out.T.Append({A, A + 1, A + 2, A + 1, A + 3, A + 2});
		}
	}

	/** Split a polyline into dashes, as world.js dashed(). */
	TArray<TArray<FVector2D>> Dashed(const TArray<FVector2D>& P, double Every, double On)
	{
		TArray<TArray<FVector2D>> Out;
		TArray<FVector2D> Cur;
		double Acc = 0;
		for (int32 i = 1; i < P.Num(); ++i)
		{
			const FVector2D A = P[i - 1], B = P[i];
			const double L = FVector2D::Distance(A, B);
			double S = 0;
			while (S < L)
			{
				const double Phase = FMath::Fmod(Acc, Every);
				const bool bIn = Phase < On;
				const double Step = FMath::Min(L - S, bIn ? On - Phase : Every - Phase);
				if (bIn)
				{
					if (Cur.Num() == 0) Cur.Add(A + (B - A) * (S / L));
					Cur.Add(A + (B - A) * ((S + Step) / L));
				}
				else if (Cur.Num())
				{
					Out.Add(MoveTemp(Cur));
					Cur.Reset();
				}
				S += Step;
				Acc += Step;
			}
		}
		if (Cur.Num()) Out.Add(MoveTemp(Cur));
		return Out;
	}

	int32 LookIndex(const FString& Cls)
	{
		for (int32 i = 0; i < UE_ARRAY_COUNT(Looks); ++i)
		{
			if (Cls == Looks[i].Cls) return i;
		}
		return 1;
	}
}

ATollTrafficActor::ATollTrafficActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PostPhysics;

	USceneComponent* SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	RootComponent = SceneRoot;

	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));

	Road = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("Road"));
	Road->SetupAttachment(SceneRoot);
	Road->bUseAsyncCooking = true;
	Road->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	Terrain = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("Terrain"));
	Terrain->SetupAttachment(SceneRoot);
	Terrain->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	for (int32 i = 0; i < UE_ARRAY_COUNT(Looks); ++i)
	{
		UInstancedStaticMeshComponent* Ism = CreateDefaultSubobject<UInstancedStaticMeshComponent>(*FString::Printf(TEXT("Class_%s"), Looks[i].Cls));
		Ism->SetupAttachment(SceneRoot);
		Ism->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Ism->SetMobility(EComponentMobility::Movable);
		if (Cube.Succeeded()) Ism->SetStaticMesh(Cube.Object);
		ClassMeshes.Add(Ism);
	}

	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(SceneRoot);
	Camera->SetFieldOfView(55.f);

	Sun = CreateDefaultSubobject<UDirectionalLightComponent>(TEXT("Sun"));
	Sun->SetupAttachment(SceneRoot);
	Sun->SetMobility(EComponentMobility::Movable);
	Sun->SetAtmosphereSunLight(true);
	Sun->SetIntensity(10.f);
	Sun->SetRelativeRotation(FRotator(-38.f, 125.f, 0.f));

	Moon = CreateDefaultSubobject<UDirectionalLightComponent>(TEXT("Moon"));
	Moon->SetupAttachment(SceneRoot);
	Moon->SetMobility(EComponentMobility::Movable);
	Moon->SetIntensity(0.f);
	Moon->SetLightColor(FLinearColor(0.6f, 0.7f, 1.0f));
	Moon->SetRelativeRotation(FRotator(-55.f, 60.f, 0.f));
	Moon->SetAtmosphereSunLight(false);

	const auto Lamps = [&](const TCHAR* Name)
	{
		UInstancedStaticMeshComponent* L = CreateDefaultSubobject<UInstancedStaticMeshComponent>(Name);
		L->SetupAttachment(SceneRoot);
		L->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		L->SetMobility(EComponentMobility::Movable);
		L->SetCastShadow(false);
		if (Cube.Succeeded()) L->SetStaticMesh(Cube.Object);
		return L;
	};
	HeadLamps = Lamps(TEXT("HeadLamps"));
	TailLamps = Lamps(TEXT("TailLamps"));
	BrakeLamps = Lamps(TEXT("BrakeLamps"));

	Sky = CreateDefaultSubobject<USkyAtmosphereComponent>(TEXT("Sky"));
	Sky->SetupAttachment(SceneRoot);

	SkyLight = CreateDefaultSubobject<USkyLightComponent>(TEXT("SkyLight"));
	SkyLight->SetupAttachment(SceneRoot);
	SkyLight->SetMobility(EComponentMobility::Movable);
	SkyLight->bRealTimeCapture = true;

	Fog = CreateDefaultSubobject<UExponentialHeightFogComponent>(TEXT("Fog"));
	Fog->SetupAttachment(SceneRoot);
	Fog->SetFogDensity(0.004f);

	Post = CreateDefaultSubobject<UPostProcessComponent>(TEXT("Post"));
	Post->SetupAttachment(SceneRoot);
	Post->bUnbound = true;
}

bool ATollTrafficActor::LoadTerrain()
{
	// format: Scripts/make_terrain.py
	TArray<uint8> Bytes;
	const FString Path = FPaths::ProjectContentDir() / TEXT("Data/terrain.bin");
	if (!FFileHelper::LoadFileToArray(Bytes, *Path) || Bytes.Num() < 20 || FMemory::Memcmp(Bytes.GetData(), "TTER", 4) != 0)
	{
		UE_LOG(LogTollScene, Warning, TEXT("terrain unavailable: %s"), *Path);
		return false;
	}
	int32 Version, Cells, Count;
	float Cell;
	FMemory::Memcpy(&Version, &Bytes[4], 4);
	FMemory::Memcpy(&Cells, &Bytes[8], 4);
	FMemory::Memcpy(&Cell, &Bytes[12], 4);
	FMemory::Memcpy(&Count, &Bytes[16], 4);
	const int32 N = Cells + 1;
	const int64 Stride = 8 + int64(N) * N * 2;
	if (Version != 1 || Cells <= 0 || 20 + Stride * Count != Bytes.Num())
	{
		UE_LOG(LogTollScene, Error, TEXT("terrain.bin: unexpected layout (v%d, %d cells, %d chunks, %d bytes)"), Version, Cells, Count, Bytes.Num());
		return false;
	}
	const double Size = Cells * double(Cell);
	TerrainCells = Cells;
	TerrainCell = Cell;

	// dry Kalahari-edge soil: reddish on slopes, grassier in the hollows, broken up by noise
	const FLinearColor Soil = FLinearColor::FromSRGBColor(FColor(0xa8, 0x96, 0x7c));
	const FLinearColor Rock = FLinearColor::FromSRGBColor(FColor(0x8e, 0x6c, 0x55));
	const FLinearColor Grass = FLinearColor::FromSRGBColor(FColor(0x86, 0x84, 0x62));
	const auto Hash = [](int64 X, int64 Y)
	{
		uint32 H = uint32(X * 73856093) ^ uint32(Y * 19349663);
		H ^= H >> 13;
		H *= 0x5bd1e995;
		H ^= H >> 15;
		return double(H & 0xffff) / 65535.0;
	};
	// value noise over Scale grid cells, smoothly interpolated, for patches wider than a cell
	const auto Smooth = [&Hash](int64 X, int64 Y, int64 Scale)
	{
		const int64 X0 = FMath::FloorToInt64(double(X) / Scale), Y0 = FMath::FloorToInt64(double(Y) / Scale);
		double Fx = double(X - X0 * Scale) / Scale, Fy = double(Y - Y0 * Scale) / Scale;
		Fx = Fx * Fx * (3 - 2 * Fx);
		Fy = Fy * Fy * (3 - 2 * Fy);
		const double A = FMath::Lerp(Hash(X0 * 7919, Y0), Hash((X0 + 1) * 7919, Y0), Fx);
		const double B = FMath::Lerp(Hash(X0 * 7919, Y0 + 1), Hash((X0 + 1) * 7919, Y0 + 1), Fx);
		return FMath::Lerp(A, B, Fy);
	};

	FIntPoint Lo(MAX_int32, MAX_int32), Hi(MIN_int32, MIN_int32);
	TSet<FIntPoint> Have;
	TArray<FVector> V, Nrm;
	TArray<FLinearColor> Col;
	TArray<int32> T;
	TArray<int16> H;
	H.SetNumUninitialized(N * N);
	for (int32 k = 0; k < Count; ++k)
	{
		const uint8* P = &Bytes[20 + Stride * k];
		int32 Cx, Cy;
		FMemory::Memcpy(&Cx, P, 4);
		FMemory::Memcpy(&Cy, P + 4, 4);
		FMemory::Memcpy(H.GetData(), P + 8, N * N * 2);
		Have.Add(FIntPoint(Cx, Cy));
		TerrainChunks.Add(FIntPoint(Cx, Cy), H);
		Lo = FIntPoint(FMath::Min(Lo.X, Cx), FMath::Min(Lo.Y, Cy));
		Hi = FIntPoint(FMath::Max(Hi.X, Cx), FMath::Max(Hi.Y, Cy));

		V.Reset();
		Nrm.Reset();
		Col.Reset();
		T.Reset();
		const auto Z = [&](int32 i, int32 j) { return H[FMath::Clamp(j, 0, Cells) * N + FMath::Clamp(i, 0, Cells)] * 0.1; };
		for (int32 j = 0; j < N; ++j)
		{
			for (int32 i = 0; i < N; ++i)
			{
				const double X = Cx * Size + i * Cell, Y = Cy * Size + j * Cell;
				const double Dx = (Z(i + 1, j) - Z(i - 1, j)) / (2 * Cell);
				const double Dy = (Z(i, j + 1) - Z(i, j - 1)) / (2 * Cell);
				V.Add(TollSpace::ToWorld(Origin, X, Y, Z(i, j)));
				Nrm.Add(FVector(-Dx, Dy, 1).GetSafeNormal()); // sim y is Unreal -Y
				const double Slope = FMath::Clamp(FMath::Sqrt(Dx * Dx + Dy * Dy) * 4, 0.0, 1.0);
				const double Hollow = FMath::Clamp((Z(i - 2, j) + Z(i + 2, j) + Z(i, j - 2) + Z(i, j + 2)) / 4 - Z(i, j), 0.0, 1.5) / 1.5;
				const int64 Gx = int64(Cx) * Cells + i, Gy = int64(Cy) * Cells + j;
				const double Noise = Hash(Gx, Gy) * 0.2 + Smooth(Gx, Gy, 4) * 0.35 + Smooth(Gx, Gy, 16) * 0.45;
				FLinearColor C = FMath::Lerp(Soil, Grass, float(FMath::Clamp(Hollow + (Noise - 0.5) * 1.4, 0.0, 1.0)));
				C = FMath::Lerp(C, Rock, float(Slope));
				C *= float(0.92 + Noise * 0.16);
				C.A = float(FMath::Clamp(Slope * 1.5 + (Smooth(Gx, Gy, 8) - 0.45) * 2.0, 0.0, 1.0));
				Col.Add(C);
			}
		}
		for (int32 j = 0; j < Cells; ++j)
		{
			for (int32 i = 0; i < Cells; ++i)
			{
				// same winding as the flat ground: Unreal +Y is sim -y
				const int32 A = (j + 1) * N + i, B = j * N + i, C = (j + 1) * N + i + 1, D = j * N + i + 1;
				T.Append({A, B, C, C, B, D});
			}
		}
		Terrain->CreateMeshSection_LinearColor(k, V, T, Nrm, {}, Col, {}, false);
	}

	// flat ground at the edge height (make_terrain.py fades the relief to -0.25 m) in the
	// chunks the corridor misses, then four slabs out to the horizon around that box
	const double Edge = -0.25;
	const FLinearColor Flat = Soil * 0.97f;
	V.Reset();
	Nrm.Reset();
	Col.Reset();
	T.Reset();
	const auto Quad = [&](double X0, double Y0, double X1, double Y1)
	{
		const int32 B = V.Num();
		V.Add(TollSpace::ToWorld(Origin, X0, Y1, Edge));
		V.Add(TollSpace::ToWorld(Origin, X1, Y1, Edge));
		V.Add(TollSpace::ToWorld(Origin, X0, Y0, Edge));
		V.Add(TollSpace::ToWorld(Origin, X1, Y0, Edge));
		for (int32 i = 0; i < 4; ++i)
		{
			Nrm.Add(FVector::UpVector);
			Col.Add(Flat);
		}
		T.Append({B, B + 2, B + 1, B + 1, B + 2, B + 3});
	};
	for (int32 Cy = Lo.Y; Cy <= Hi.Y; ++Cy)
	{
		for (int32 Cx = Lo.X; Cx <= Hi.X; ++Cx)
		{
			if (!Have.Contains(FIntPoint(Cx, Cy))) Quad(Cx * Size, Cy * Size, (Cx + 1) * Size, (Cy + 1) * Size);
		}
	}
	const double X0 = Lo.X * Size, Y0 = Lo.Y * Size, X1 = (Hi.X + 1) * Size, Y1 = (Hi.Y + 1) * Size;
	const double Far = 200000.0;
	Quad(X0 - Far, Y1, X1 + Far, Y1 + Far);
	Quad(X0 - Far, Y0 - Far, X1 + Far, Y0);
	Quad(X0 - Far, Y0, X0, Y1);
	Quad(X1, Y0, X1 + Far, Y1);
	Terrain->CreateMeshSection_LinearColor(Count, V, T, Nrm, {}, Col, {}, false);

	// Megascans ground (Scripts/import_assets.py, not committed) when present, else plain vertex colour
	if (UMaterialInterface* G = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/ThirdParty/Megascans/MI_Ground.MI_Ground")))
	{
		UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(G, this);
		Mid->SetScalarParameterValue(TEXT("VertexTint"), 2.6);
		FLinearColor Warm(0.85, 0.8, 0.78); // slightly muted; neutral reads a little too saturated
		FString TintArg; // -TollGroundTint=r,g,b for tuning
		if (FParse::Value(FCommandLine::Get(), TEXT("TollGroundTint="), TintArg, false))
		{
			TArray<FString> P;
			if (TintArg.ParseIntoArray(P, TEXT(",")) == 3) Warm = FLinearColor(FCString::Atof(*P[0]), FCString::Atof(*P[1]), FCString::Atof(*P[2]));
		}
		Mid->SetVectorParameterValue(TEXT("Tint"), Warm);
		UE_LOG(LogTollScene, Log, TEXT("terrain: MI_Ground, tint %s"), *Warm.ToString());
		for (int32 k = 0; k <= Count; ++k) Terrain->SetMaterial(k, Mid);
	}
	else if (UMaterialInterface* M = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Toll/Materials/M_VertexColor.M_VertexColor")))
	{
		UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(M, this);
		Mid->SetScalarParameterValue(TEXT("Roughness"), 0.95f);
		for (int32 k = 0; k <= Count; ++k) Terrain->SetMaterial(k, Mid);
	}
	UE_LOG(LogTollScene, Log, TEXT("terrain: %d chunks of %d x %.0f m"), Count, Cells, Cell);
	return true;
}

bool ATollTrafficActor::LoadRoad()
{
	const FString Path = FPaths::ProjectContentDir() / TEXT("Data/road.json");
	FString Json;
	TSharedPtr<FJsonObject> Root;
	if (!FFileHelper::LoadFileToString(Json, *Path) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid())
	{
		UE_LOG(LogTollScene, Error, TEXT("road unavailable: %s"), *Path);
		return false;
	}

	// same origin as the browser (world.js): the centre of the network bounds
	const TArray<TSharedPtr<FJsonValue>>& B = Root->GetArrayField(TEXT("bounds"));
	Origin = FVector2D((B[0]->AsNumber() + B[2]->AsNumber()) / 2, (B[1]->AsNumber() + B[3]->AsNumber()) / 2);

	// asphalt, shoulders and markings, as world.js. Botswana drives on the left: SUMO
	// lane 0 is the outer (left) lane, the opposite carriageway lies to the right.
	FMeshBuf Asphalt, Shoulder, White, Yellow;
	TMap<FString, TArray<FLaneLine>> Edges;
	for (const TSharedPtr<FJsonValue>& LV : Root->GetArrayField(TEXT("lanes")))
	{
		const TSharedPtr<FJsonObject> Lane = LV->AsObject();
		TArray<FVector2D> P;
		for (const TSharedPtr<FJsonValue>& PV : Lane->GetArrayField(TEXT("pts")))
		{
			const TArray<TSharedPtr<FJsonValue>>& XY = PV->AsArray();
			P.Add(FVector2D(XY[0]->AsNumber(), XY[1]->AsNumber()));
		}
		if (P.Num() < 2) continue;
		for (int32 i = 0; i + 1 < P.Num(); ++i)
		{
			const int32 Steps = FMath::Max(1, FMath::CeilToInt((P[i + 1] - P[i]).Size() / 4.0));
			for (int32 s = 0; s < Steps; ++s)
			{
				const FVector2D Q = FMath::Lerp(P[i], P[i + 1], double(s) / Steps);
				RoadGrid.FindOrAdd(FIntPoint(FMath::FloorToInt(Q.X / 32), FMath::FloorToInt(Q.Y / 32))).Add(Q);
			}
		}
		RoadGrid.FindOrAdd(FIntPoint(FMath::FloorToInt(P.Last().X / 32), FMath::FloorToInt(P.Last().Y / 32))).Add(P.Last());
		const double W = Lane->GetNumberField(TEXT("w"));
		Ribbon(P, Origin, W / 2, -W / 2, 0.03, Asphalt);
		FString EdgeId = Lane->GetStringField(TEXT("id"));
		int32 Us;
		if (EdgeId.FindLastChar('_', Us)) EdgeId.LeftInline(Us);
		Edges.FindOrAdd(EdgeId).Add({static_cast<int32>(Lane->GetNumberField(TEXT("idx"))), W, MoveTemp(P)});
	}
	for (auto& E : Edges)
	{
		auto& Lanes = E.Value;
		Lanes.Sort([](const FLaneLine& A, const FLaneLine& B) { return A.Idx < B.Idx; });
		const double Ho = Lanes[0].W / 2;
		const TArray<FVector2D>& Outer = Lanes[0].P;
		Ribbon(Outer, Origin, Ho + 1.6, Ho, 0.02, Shoulder);
		Ribbon(Outer, Origin, Ho - 0.2, Ho - 0.35, 0.05, White);
		const double Hi = Lanes.Last().W / 2;
		Ribbon(Lanes.Last().P, Origin, -Hi + 0.2, -Hi + 0.05, 0.05, Yellow);
		for (int32 i = 1; i < Lanes.Num(); ++i)
		{
			const double H = Lanes[i].W / 2;
			for (const TArray<FVector2D>& D : Dashed(Lanes[i].P, 12, 4)) Ribbon(D, Origin, H + 0.07, H - 0.07, 0.05, White);
		}
	}
	Road->CreateMeshSection(0, Asphalt.V, Asphalt.T, Asphalt.N, {}, {}, {}, false);
	Road->CreateMeshSection(2, Shoulder.V, Shoulder.T, Shoulder.N, {}, {}, {}, false);
	Road->CreateMeshSection(3, White.V, White.T, White.N, {}, {}, {}, false);
	Road->CreateMeshSection(4, Yellow.V, Yellow.T, Yellow.N, {}, {}, {}, false);

	// without terrain data, a flat sand-coloured ground under everything
	if (!LoadTerrain())
	{
		const double Half = 200000.0; // metres
		const FVector C = TollSpace::ToWorld(Origin, Origin.X, Origin.Y, 0);
		TArray<FVector> GV = {C + FVector(-Half, -Half, 0) * 100, C + FVector(Half, -Half, 0) * 100, C + FVector(-Half, Half, 0) * 100, C + FVector(Half, Half, 0) * 100};
		Road->CreateMeshSection(1, GV, {0, 2, 1, 1, 2, 3}, {FVector::UpVector, FVector::UpVector, FVector::UpVector, FVector::UpVector}, {}, {}, {}, false);
	}

	if (UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial")))
	{
		const auto Tint = [&](int32 Section, FColor C)
		{
			UMaterialInstanceDynamic* M = UMaterialInstanceDynamic::Create(Base, this);
			M->SetVectorParameterValue(TEXT("Color"), FLinearColor::FromSRGBColor(C));
			Road->SetMaterial(Section, M);
		};
		Tint(0, FColor(0x3b, 0x3d, 0x40)); // asphalt
		Tint(1, FColor(0xb5, 0x9a, 0x72)); // flat ground, when there is no terrain data
		Tint(2, FColor(0x8a, 0x7a, 0x5c)); // gravel shoulder
		// Megascans asphalt and gravel when imported; the ribbons carry no vertex colour, so no tint
		const auto Surface = [&](int32 Section, const TCHAR* Path, FLinearColor Tint)
		{
			if (UMaterialInterface* S = LoadObject<UMaterialInterface>(nullptr, Path))
			{
				UMaterialInstanceDynamic* M = UMaterialInstanceDynamic::Create(S, this);
				M->SetScalarParameterValue(TEXT("VertexTintAmount"), 0.f);
				M->SetVectorParameterValue(TEXT("Tint"), Tint);
				Road->SetMaterial(Section, M);
			}
		};
		Surface(0, TEXT("/Game/ThirdParty/Megascans/RoadAsphalt/MI_RoadAsphalt.MI_RoadAsphalt"), FLinearColor(0.42f, 0.42f, 0.44f)); // weathered, not new
		Surface(2, TEXT("/Game/ThirdParty/Megascans/GravelGround/MI_GravelGround.MI_GravelGround"), FLinearColor(0.85f, 0.72f, 0.58f));
		Tint(3, FColor(0xe9, 0xe9, 0xe2)); // white lines
		Tint(4, FColor(0xe2, 0xb3, 0x3c)); // yellow centre line
		for (int32 i = 0; i < ClassMeshes.Num(); ++i)
		{
			UMaterialInstanceDynamic* M = UMaterialInstanceDynamic::Create(Base, this);
			M->SetVectorParameterValue(TEXT("Color"), Looks[i].Color);
			ClassMeshes[i]->SetMaterial(0, M);
		}
	}

	for (const TSharedPtr<FJsonValue>& PV : Root->GetArrayField(TEXT("plazas")))
	{
		const TSharedPtr<FJsonObject> P = PV->AsObject();
		const FString Id = P->GetStringField(TEXT("id"));
		PlazaPos.Add(Id, FVector2D(P->GetNumberField(TEXT("x")), P->GetNumberField(TEXT("y"))));
		PlazaHeading.Add(Id, P->GetNumberField(TEXT("heading")));
	}
	UE_LOG(LogTollScene, Log, TEXT("road: %d lanes, %d plazas, origin %.1f %.1f"), Root->GetArrayField(TEXT("lanes")).Num(), PlazaPos.Num(), Origin.X, Origin.Y);
	return true;
}

void ATollTrafficActor::ApplyTimeOfDay()
{
	FParse::Value(FCommandLine::Get(), TEXT("TollTime="), Hour);
	Hour = FMath::Fmod(FMath::Fmod(Hour, 24.0) + 24.0, 24.0);
	// Gaborone, 24.6 S: the sun crosses the northern sky. Rise ~6, set ~18.
	const double Day = (Hour - 6.0) / 12.0;
	const double Elev = 78.0 * FMath::Sin(UE_DOUBLE_PI * Day); // negative at night
	const double Yaw = 180.0 - 180.0 * Day;                    // light from the east at dawn, the north at noon
	Sun->SetRelativeRotation(FRotator(-Elev, Yaw, 0));
	const double Up = FMath::Clamp(Elev / 12.0, 0.0, 1.0);
	Sun->SetIntensity(10.f * static_cast<float>(FMath::Max(Up, 0.02)));
	Sun->SetLightColor(FLinearColor::LerpUsingHSV(FLinearColor(1.0f, 0.55f, 0.3f), FLinearColor(1.0f, 0.97f, 0.92f), static_cast<float>(Up)));
	Night = FMath::Clamp((4.0 - Elev) / 10.0, 0.0, 1.0); // dusk starts with the sun 4 deg up
	Moon->SetIntensity(0.25f * static_cast<float>(Night));

	// auto exposure would turn night into a grey day: cap how far it may open up.
	// -TollEV=<ev100> overrides the night cap for tuning.
	FPostProcessSettings& PP = Post->Settings;
	double NightEV = 0.5;
	FParse::Value(FCommandLine::Get(), TEXT("TollEV="), NightEV);
	if (Night > 0.5)
	{
		PP.bOverride_AutoExposureMethod = true;
		PP.AutoExposureMethod = AEM_Manual;
		PP.bOverride_AutoExposureApplyPhysicalCameraExposure = true;
		PP.AutoExposureApplyPhysicalCameraExposure = false;
		PP.bOverride_AutoExposureBias = true;
		PP.AutoExposureBias = static_cast<float>(NightEV);
	}
	PP.bOverride_BloomIntensity = true;
	PP.BloomIntensity = static_cast<float>(FMath::Lerp(0.675, 1.2, Night));
	for (ATollPlazaActor* P : Plazas) P->SetNight(static_cast<float>(Night));

	const float Head = Night > 0.05 ? 25.f : 0.f;
	const float Tail = Night > 0.05 ? 12.f : 0.6f;
	if (UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Toll/Materials/M_Color.M_Color")))
	{
		const auto Lamp = [&](UInstancedStaticMeshComponent* L, FLinearColor C, float E)
		{
			UMaterialInstanceDynamic* M = UMaterialInstanceDynamic::Create(Base, this);
			M->SetVectorParameterValue(TEXT("Color"), C);
			M->SetScalarParameterValue(TEXT("Emissive"), E);
			L->SetMaterial(0, M);
		};
		Lamp(HeadLamps, FLinearColor(1.0f, 0.92f, 0.75f), Head);
		Lamp(TailLamps, FLinearColor(0.8f, 0.03f, 0.02f), Tail);
		Lamp(BrakeLamps, FLinearColor(1.0f, 0.04f, 0.02f), 40.f);
	}
	UE_LOG(LogTollScene, Log, TEXT("time %.2f h: sun %.1f deg up, night %.2f"), Hour, Elev, Night);
}

void ATollTrafficActor::SetInstances(UInstancedStaticMeshComponent* Ism, const TArray<FTransform>& Xf, const TArray<float>* Custom)
{
	const int32 Have = Ism->GetInstanceCount();
	if (Have > Xf.Num())
	{
		Ism->ClearInstances();
		Ism->AddInstances(Xf, false, true);
	}
	else
	{
		if (Have > 0) Ism->BatchUpdateInstancesTransforms(0, MakeArrayView(Xf.GetData(), Have), true, false, false);
		if (Xf.Num() > Have) Ism->AddInstances(TArray<FTransform>(Xf.GetData() + Have, Xf.Num() - Have), false, true);
	}
	if (Custom && Ism->NumCustomDataFloats == 1)
	{
		for (int32 k = 0; k < Xf.Num() && k < Custom->Num(); ++k) Ism->SetCustomDataValue(k, 0, (*Custom)[k], false);
	}
	Ism->MarkRenderStateDirty();
}

namespace
{
// City Sample's MF_UnpackTrafficVehicleInstanceCustomData reads PerInstanceCustomData[0] as a uint:
// the low 16 bits a half-float 0..1 that picks the paint, bits 16+ the lamps.
enum ELampBits : uint32
{
	RunFront = (1u << 16) | (1u << 17),
	RunRear = (1u << 18) | (1u << 19),
	Brake = (1u << 20) | (1u << 21),
	Head = (1u << 24) | (1u << 25),
};
float PackVehicle(float Random, uint32 Lamps)
{
	const uint32 Bits = uint32(FFloat16(FMath::Clamp(Random, 0.f, 1.f)).Encoded) | Lamps;
	float Out;
	FMemory::Memcpy(&Out, &Bits, sizeof(Out));
	return Out;
}
}

void ATollTrafficActor::LoadModels()
{
	// City Sample Vehicles (Fab, free; Content/CitySampleVehicles, not committed). Each model is
	// a body without wheels, glass, brake pads and four or more wheels, all placed in vehicle
	// space with the origin on the ground, +X forward. Without the pack, the boxes stay.
	struct FDef
	{
		const TCHAR* Cls;
		TArray<const TCHAR*> Folders;
	};
	const FDef Defs[] = {
		{TEXT("1"), {TEXT("vehicle02_Car")}}, {TEXT("1"), {TEXT("vehicle03_Car")}}, {TEXT("1"), {TEXT("vehicle05_Car")}},
		{TEXT("1"), {TEXT("vehicle06_Car")}}, {TEXT("1"), {TEXT("vehicle07_Car")}}, {TEXT("1"), {TEXT("vehicle12_Car")}},
		{TEXT("1"), {TEXT("vehicle01_Van")}},
		{TEXT("2"), {TEXT("vehicle09_Van")}}, {TEXT("2"), {TEXT("vehicle04_Truck")}},
		{TEXT("3"), {TEXT("vehicle10_Bus")}}, {TEXT("3"), {TEXT("vehicle11_Truck")}},
		{TEXT("4"), {TEXT("vehicle08_Truck"), TEXT("vehicle08_Trailer")}},
	};
	IAssetRegistry& Registry = IAssetRegistry::GetChecked();
	for (const FDef& D : Defs)
	{
		FVehicleModel M;
		M.Name = D.Folders[0];
		TArray<TPair<UStaticMesh*, bool>> Parts; // mesh, is a wheel
		for (const TCHAR* F : D.Folders)
		{
			const FString Path = FString::Printf(TEXT("/Game/CitySampleVehicles/%s/Mesh"), F);
			Registry.ScanPathsSynchronous({Path});
			TArray<FAssetData> Assets;
			Registry.GetAssetsByPath(FName(*Path), Assets, true);
			for (const FAssetData& A : Assets)
			{
				const FString N = A.AssetName.ToString();
				if (N.Contains(TEXT("_LOD")) || N.Contains(TEXT("lightBar"))) continue;
				if (UStaticMesh* Sm = Cast<UStaticMesh>(A.GetAsset())) Parts.Add({Sm, N.Contains(TEXT("Wheel")) && !N.Contains(TEXT("No_Wheel"))});
			}
		}
		if (Parts.Num() == 0) continue;
		FBox Body(ForceInit);
		for (const auto& P : Parts)
		{
			if (!P.Value) Body += P.Key->GetBoundingBox();
		}
		M.OffsetX = -Body.GetCenter().X;
		M.Body = Body.ShiftBy(FVector(M.OffsetX, 0, 0));
		for (const auto& P : Parts)
		{
			UInstancedStaticMeshComponent* Ism = NewObject<UInstancedStaticMeshComponent>(this);
			Ism->SetupAttachment(RootComponent);
			Ism->SetMobility(EComponentMobility::Movable);
			Ism->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Ism->SetStaticMesh(P.Key);
			Ism->NumCustomDataFloats = 1;
			Ism->RegisterComponent();
			ModelMeshes.Add(Ism);
			if (P.Value)
			{
				const FBox B = P.Key->GetBoundingBox();
				M.Wheels.Add({Ism, B.GetCenter(), B.GetExtent().Z});
			}
			else
			{
				M.Rigid.Add(Ism);
			}
		}
		M.RigidXf.SetNum(M.Rigid.Num());
		M.WheelXf.SetNum(M.Wheels.Num());
		ClassModels.FindOrAdd(D.Cls).Add(Models.Num());
		Models.Add(MoveTemp(M));
	}
	UE_LOG(LogTollScene, Log, TEXT("vehicles: %d detailed models%s"), Models.Num(), Models.Num() ? TEXT("") : TEXT(" (City Sample Vehicles not installed; drawing boxes)"));
}

double ATollTrafficActor::GroundZ(double X, double Y) const
{
	if (TerrainCells <= 0) return 0;
	const double Size = TerrainCells * TerrainCell;
	const int32 Cx = FMath::FloorToInt(X / Size), Cy = FMath::FloorToInt(Y / Size);
	const TArray<int16>* H = TerrainChunks.Find(FIntPoint(Cx, Cy));
	if (!H) return -0.25; // the flat fill around the corridor
	const int32 N = TerrainCells + 1;
	const double U = (X - Cx * Size) / TerrainCell, V = (Y - Cy * Size) / TerrainCell;
	const int32 i = FMath::Clamp(FMath::FloorToInt(U), 0, TerrainCells - 1), j = FMath::Clamp(FMath::FloorToInt(V), 0, TerrainCells - 1);
	const double Fx = U - i, Fy = V - j;
	const auto Z = [&](int32 a, int32 b) { return (*H)[b * N + a] * 0.1; };
	// the mesh splits each cell along (i,j)-(i+1,j+1)
	if (Fy >= Fx) return Z(i, j) + Fy * (Z(i, j + 1) - Z(i, j)) + Fx * (Z(i + 1, j + 1) - Z(i, j + 1));
	return Z(i, j) + Fx * (Z(i + 1, j) - Z(i, j)) + Fy * (Z(i + 1, j + 1) - Z(i + 1, j));
}

double ATollTrafficActor::RoadDist(const FVector2D& P, double Max) const
{
	double Best = Max * Max;
	const int32 Gx = FMath::FloorToInt(P.X / 32), Gy = FMath::FloorToInt(P.Y / 32);
	const int32 R = FMath::CeilToInt(Max / 32);
	for (int32 dy = -R; dy <= R; ++dy)
	{
		for (int32 dx = -R; dx <= R; ++dx)
		{
			if (const TArray<FVector2D>* Pts = RoadGrid.Find(FIntPoint(Gx + dx, Gy + dy)))
			{
				for (const FVector2D& Q : *Pts) Best = FMath::Min(Best, FVector2D::DistSquared(P, Q));
			}
		}
	}
	return FMath::Sqrt(Best);
}

void ATollTrafficActor::LoadFoliage()
{
	// Kalahari bushveld around the plazas: dry grass, low shrubs, bush and acacia, some rock.
	// Megascans plants and the acacia come from Scripts/import_assets.py, the bushes and rocks
	// from the Namaqualand pack (searsia stands in for mopane); none of it is committed.
	struct FKind
	{
		const TCHAR* Name;
		TArray<FString> Paths; // folders searched for static meshes
		FString Contains;      // name filter, empty for all
		double MinH, MaxH;     // metres
		double Grid;           // candidate spacing, m
		double Chance;         // of a candidate being planted, before the patch noise
		double Clear;          // from a lane centreline, m
		double Radius;         // around each plaza, m
		double Cull;           // m
		bool bShadow;
	};
	const FKind Kinds[] = {
		{TEXT("grass"), {TEXT("/Game/ThirdParty/Megascans/DryGrass"), TEXT("/Game/ThirdParty/Megascans/ThatchingGrass")}, TEXT(""), 0.45, 1.1, 2.2, 0.75, 4.5, 450, 320, false},
		{TEXT("shrub"), {TEXT("/Game/ThirdParty/Megascans/DeadShrubs"), TEXT("/Game/Namaqualand/Meshes/Foliage/NN")}, TEXT("burchellii|rooibos|Dead_Shrubs"), 0.7, 1.8, 7, 0.35, 6, 900, 650, true},
		{TEXT("bush"), {TEXT("/Game/Namaqualand/Meshes/Foliage/NN")}, TEXT("searsia_lucida"), 2.2, 4.5, 22, 0.35, 10, 2200, 2600, true},
		{TEXT("acacia"), {TEXT("/Game/ThirdParty/AcaciaTree")}, TEXT(""), 5.0, 8.5, 40, 0.45, 14, 2600, 4000, true},
		{TEXT("rock"), {TEXT("/Game/Namaqualand/Meshes/General/NN")}, TEXT("boulder_0"), 0.3, 1.6, 18, 0.25, 7, 900, 700, true},
	};
	IAssetRegistry& Registry = IAssetRegistry::GetChecked();
	FRandomStream Rand(20261020);
	int32 Total = 0;
	for (const FKind& K : Kinds)
	{
		TArray<FString> Filters;
		K.Contains.ParseIntoArray(Filters, TEXT("|"));
		TArray<UStaticMesh*> Meshes;
		for (const FString& Path : K.Paths)
		{
			Registry.ScanPathsSynchronous({Path});
			TArray<FAssetData> Assets;
			Registry.GetAssetsByPath(FName(*Path), Assets, true);
			for (const FAssetData& A : Assets)
			{
				const FString N = A.AssetName.ToString();
				if (Filters.Num() && !Filters.ContainsByPredicate([&N](const FString& F) { return N.Contains(F); })) continue;
				UStaticMesh* Sm = Cast<UStaticMesh>(A.GetAsset());
				if (!Sm) continue;
				// some plant variations are fallen litter lying flat; scaled to a plant's height they become long ribbons
				const FVector E = Sm->GetBoundingBox().GetSize();
				if (E.Z < 0.4 * FMath::Max(E.X, E.Y)) continue;
				Meshes.Add(Sm);
			}
		}
		if (Meshes.Num() == 0)
		{
			UE_LOG(LogTollScene, Log, TEXT("foliage: no %s meshes (assets not imported)"), K.Name);
			continue;
		}
		TArray<TArray<FTransform>> Xf;
		Xf.SetNum(Meshes.Num());
		TSet<FIntPoint> Done; // candidates shared between overlapping plaza discs
		for (const auto& Pz : PlazaPos)
		{
			const FVector2D C = Pz.Value;
			const int32 R = FMath::CeilToInt(K.Radius / K.Grid);
			for (int32 b = -R; b <= R; ++b)
			{
				for (int32 a = -R; a <= R; ++a)
				{
					const FIntPoint Cell(FMath::FloorToInt(C.X / K.Grid) + a, FMath::FloorToInt(C.Y / K.Grid) + b);
					if (Done.Contains(Cell)) continue;
					Done.Add(Cell);
					FRandomStream R2(int32(HashCombine(GetTypeHash(Cell), GetTypeHash(FString(K.Name)))));
					const FVector2D P((Cell.X + R2.FRand()) * K.Grid, (Cell.Y + R2.FRand()) * K.Grid);
					const double D = FVector2D::Distance(P, C);
					if (D > K.Radius) continue;
					// thin out towards the disc edge, and in broad patches so it isn't uniform
					const double Patch = 0.5 + 0.5 * FMath::Sin(P.X * 0.013 + FMath::Sin(P.Y * 0.009) * 2.0) * FMath::Cos(P.Y * 0.011 + FMath::Sin(P.X * 0.007) * 2.0);
					const double Edge = FMath::Clamp((K.Radius - D) / (K.Radius * 0.3), 0.0, 1.0);
					if (R2.FRand() > K.Chance * (0.35 + 0.9 * Patch) * Edge) continue;
					if (D < 45) continue; // the plaza apron and buildings
					if (RoadDist(P, K.Clear + 1) < K.Clear) continue;
					const int32 Mi = R2.RandRange(0, Meshes.Num() - 1);
					const FBox Bx = Meshes[Mi]->GetBoundingBox();
					const double MeshH = FMath::Max(Bx.GetSize().Z, 1.0);
					const double S = FMath::Lerp(K.MinH, K.MaxH, R2.FRand()) * 100.0 / MeshH;
					const double Z = GroundZ(P.X, P.Y) - 0.05 - Bx.Min.Z * S / 100.0;
					Xf[Mi].Add(FTransform(FRotator(0, R2.FRand() * 360.0, 0), TollSpace::ToWorld(Origin, P.X, P.Y, Z), FVector(S)));
				}
			}
		}
		for (int32 m = 0; m < Meshes.Num(); ++m)
		{
			if (Xf[m].Num() == 0) continue;
			UHierarchicalInstancedStaticMeshComponent* H = NewObject<UHierarchicalInstancedStaticMeshComponent>(this);
			H->SetupAttachment(RootComponent);
			H->SetMobility(EComponentMobility::Movable); // under the actor's movable root
			H->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			H->SetStaticMesh(Meshes[m]);
			H->SetCastShadow(K.bShadow);
			H->SetCullDistances(int32(K.Cull * 80), int32(K.Cull * 100));
			H->RegisterComponent();
			H->AddInstances(Xf[m], false, true);
			FoliageMeshes.Add(H);
			Total += Xf[m].Num();
		}
		int32 Count = 0;
		for (const TArray<FTransform>& X : Xf) Count += X.Num();
		UE_LOG(LogTollScene, Log, TEXT("foliage: %d %s from %d meshes"), Count, K.Name, Meshes.Num());
	}
	UE_LOG(LogTollScene, Log, TEXT("foliage: %d instances"), Total);
}

void ATollTrafficActor::LoadPlazas()
{
	const FString Dir = FPaths::ProjectContentDir() / TEXT("Data/Plazas");
	FString Json;
	TSharedPtr<FJsonObject> Root;
	if (!FFileHelper::LoadFileToString(Json, *(Dir / TEXT("plazas.json"))) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid())
	{
		UE_LOG(LogTollScene, Warning, TEXT("no plaza geometry in %s; plazas not drawn"), *Dir);
		return;
	}
	const TArray<TSharedPtr<FJsonValue>>& Arr = Root->GetArrayField(TEXT("plazas"));
	TollPlaza::ParseLayouts(Arr, Layouts);
	for (const TSharedPtr<FJsonValue>& PV : Arr)
	{
		ATollPlazaActor* A = GetWorld()->SpawnActor<ATollPlazaActor>();
		A->Build(PV->AsObject(), Dir / TEXT("textures"), Origin);
		Plazas.Add(A);
	}
}

void ATollTrafficActor::FrameCamera()
{
	FString Cam;
	FString Plaza = StartPlaza;
	if (FParse::Value(FCommandLine::Get(), TEXT("TollCam="), Cam) && Cam.StartsWith(TEXT("plaza:")))
	{
		Plaza = Cam.RightChop(6);
	}
	const FVector2D* P = PlazaPos.Find(Plaza);
	if (!P) return;
	// road.json plaza heading is radians from east, counter-clockwise (plaza-layout.js)
	const double H = PlazaHeading.FindRef(Plaza);
	const FVector2D Fwd(FMath::Cos(H), FMath::Sin(H));
	const FVector2D Side(Fwd.Y, -Fwd.X);
	const FVector Target = TollSpace::ToWorld(Origin, P->X, P->Y, 0);
	// -TollCamEye=back,side,height in metres from the plaza centre; the default clears the high masts
	double Back = 110, Right = 22, Up = 30;
	FString EyeArg;
	if (FParse::Value(FCommandLine::Get(), TEXT("TollCamEye="), EyeArg, false))
	{
		TArray<FString> Parts;
		EyeArg.ParseIntoArray(Parts, TEXT(","));
		if (Parts.Num() == 3)
		{
			Back = FCString::Atod(*Parts[0]);
			Right = FCString::Atod(*Parts[1]);
			Up = FCString::Atod(*Parts[2]);
		}
	}
	const FVector2D Eye2 = *P - Fwd * Back + Side * Right;
	const FVector Eye = TollSpace::ToWorld(Origin, Eye2.X, Eye2.Y, Up);
	Camera->SetWorldLocationAndRotation(Eye, (Target - Eye).Rotation());
}

void ATollTrafficActor::BeginPlay()
{
	Super::BeginPlay();
	LoadRoad();
	LoadPlazas();
	LoadModels();
	LoadFoliage();
	ApplyTimeOfDay();
	FrameCamera();
	if (UTollFeedSubsystem* Feed = GetGameInstance() ? GetGameInstance()->GetSubsystem<UTollFeedSubsystem>() : nullptr)
	{
		Feed->Start();
	}
}

void ATollTrafficActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	UTollFeedSubsystem* Feed = GetGameInstance() ? GetGameInstance()->GetSubsystem<UTollFeedSubsystem>() : nullptr;
	if (!Feed) return;

	TArray<UTollFeedSubsystem::FBoomEvent> Due;
	Feed->TakeDueBooms(Due);
	for (const UTollFeedSubsystem::FBoomEvent& E : Due)
	{
		for (ATollPlazaActor* P : Plazas)
		{
			if (P->PlazaId == E.Plaza) P->NoteFeedBoom(E.Key, E.bUp, Feed->GetRenderTime());
		}
		UE_LOG(LogTollScene, Verbose, TEXT("feed boom %s %s %s"), *E.Plaza, *E.Key, E.bUp ? TEXT("up") : TEXT("down"));
		{
		}
	}

	const TArray<TollTraffic::FVisible>& Vis = Feed->GetVisible();
	Drawn = Vis.Num();

	// where each vehicle is drawn: eased onto the plaza lanes, and lifting booms as it passes
	DrawnPos.SetNum(Vis.Num());
	for (int32 i = 0; i < Vis.Num(); ++i)
	{
		const TollTraffic::FVisible& V = Vis[i];
		TollPlaza::FContext Ctx;
		DrawnPos[i] = TollPlaza::Drawn(Layouts, V.X, V.Y, V.Heading, &Ctx);
		if (Ctx.Plaza != INDEX_NONE && Plazas.IsValidIndex(Ctx.Plaza))
		{
			Plazas[Ctx.Plaza]->NoteVehicle(Layouts[Ctx.Plaza], Ctx, TollTraffic::ClassLen(V.Cls), V.Speed);
		}
	}
	for (ATollPlazaActor* P : Plazas)
	{
		P->Advance(FMath::Min(DeltaSeconds, 0.1f), Feed->GetRenderTime());
	}
	// body motion: brake pitch from the speed trace, a little suspension bob
	const double Dt = FMath::Clamp(static_cast<double>(DeltaSeconds), 1e-3, 0.1);
	for (auto& M : Motion) M.Value.bSeen = false;
	TArray<FRotator> Body;
	TArray<double> Bob;
	Body.SetNum(Vis.Num());
	Bob.SetNum(Vis.Num());
	for (int32 i = 0; i < Vis.Num(); ++i)
	{
		const TollTraffic::FVisible& V = Vis[i];
		FMotion& M = Motion.FindOrAdd(V.Id);
		M.bSeen = true;
		const double A = M.PrevSpeed < 0 ? 0 : (V.Speed - M.PrevSpeed) / Dt;
		M.PrevSpeed = V.Speed;
		M.Accel += (FMath::Clamp(A, -8.0, 4.0) - M.Accel) * FMath::Min(1.0, Dt * 4);
		M.Phase += Dt * (7.0 + (V.Id % 7) * 0.6) * FMath::Min(V.Speed / 8.0, 1.0);
		M.Dist += V.Speed * Dt;
		const bool bHeavy = V.Cls == TEXT("3") || V.Cls == TEXT("4");
		// braking dips the nose (negative pitch), pulling away lifts it; heavies move less
		const double Pitch = FMath::Clamp(M.Accel * (bHeavy ? 0.25 : 0.5), -2.5, 1.5);
		Body[i] = FRotator(Pitch, TollSpace::ToYaw(V.Heading), FMath::Sin(M.Phase * 0.5) * 0.3 * FMath::Min(V.Speed / 15.0, 1.0));
		Bob[i] = FMath::Sin(M.Phase) * 0.02 * FMath::Min(V.Speed / 10.0, 1.0);
	}
	for (auto It = Motion.CreateIterator(); It; ++It)
	{
		if (!It.Value().bSeen) It.RemoveCurrent();
	}

	HeadXf.Reset();
	TailXf.Reset();
	BrakeXf.Reset();
	const bool bLampsOn = Night > 0.05;

	// detailed models: a model per vehicle picked from its class's pool by id
	for (FVehicleModel& M : Models)
	{
		for (TArray<FTransform>& X : M.RigidXf) X.Reset();
		for (TArray<FTransform>& X : M.WheelXf) X.Reset();
		M.Packed.Reset();
	}
	TArray<bool> Done;
	Done.SetNumZeroed(Vis.Num());
	for (int32 i = 0; i < Vis.Num(); ++i)
	{
		const TollTraffic::FVisible& V = Vis[i];
		const TArray<int32>* Pool = ClassModels.Find(V.Cls);
		if (!Pool || Pool->Num() == 0) continue;
		FVehicleModel& M = Models[(*Pool)[(uint32(V.Id) * 2654435761u >> 8) % uint32(Pool->Num())]];
		Done[i] = true;
		const FTransform Base(Body[i], TollSpace::ToWorld(Origin, DrawnPos[i].X, DrawnPos[i].Y, 0.03 + Bob[i]));
		const FTransform Shift(FVector(M.OffsetX, 0, 0));
		for (TArray<FTransform>& X : M.RigidXf) X.Add(Shift * Base);
		const double Dist = Motion.FindRef(V.Id).Dist * 100.0;
		for (int32 w = 0; w < M.Wheels.Num(); ++w)
		{
			const FVehicleModel::FWheel& Wh = M.Wheels[w];
			// rolling forward turns the front of the wheel down: negative pitch about the hub
			const double Spin = -FMath::RadiansToDegrees(Dist / Wh.Radius);
			M.WheelXf[w].Add(FTransform(-Wh.Centre) * FTransform(FRotator(FMath::Fmod(Spin, 360.0), 0, 0)) * FTransform(Wh.Centre) * Shift * Base);
		}

		// lamps: the pack's lamp bits, and glowing plates at the lamp positions, which read better at dusk
		const bool bBrake = Motion.FindRef(V.Id).Accel < -0.8;
		uint32 Lamps = 0;
		if (bLampsOn) Lamps |= RunFront | RunRear | Head;
		if (bBrake) Lamps |= Brake;
		const FBox& B = M.Body;
		const double LampZ = FMath::Clamp(B.Max.Z * 0.42, 65.0, 115.0);
		const double Side = B.Max.Y - 32.0;
		const FVector LampScale(0.01, 0.2, 0.08);
		for (const double S : {-Side, Side})
		{
			if (bLampsOn)
			{
				HeadXf.Add(FTransform(Body[i], Base.TransformPosition(FVector(B.Max.X + 1.5, S, LampZ)), LampScale));
				if (!bBrake) TailXf.Add(FTransform(Body[i], Base.TransformPosition(FVector(B.Min.X - 1.5, S, LampZ)), LampScale));
			}
			if (bBrake) BrakeXf.Add(FTransform(Body[i], Base.TransformPosition(FVector(B.Min.X - 1.5, S, LampZ)), LampScale));
		}
		M.Packed.Add(PackVehicle(float((uint32(V.Id) * 2246822519u >> 12) % 1000u) / 999.f, Lamps));
	}
	for (FVehicleModel& M : Models)
	{
		for (int32 k = 0; k < M.Rigid.Num(); ++k) SetInstances(M.Rigid[k], M.RigidXf[k], &M.Packed);
		for (int32 w = 0; w < M.Wheels.Num(); ++w) SetInstances(M.Wheels[w].Ism, M.WheelXf[w], &M.Packed);
	}

	// boxes for everything without a model (motorbikes, or all of them without the pack)
	for (int32 c = 0; c < ClassMeshes.Num(); ++c)
	{
		Scratch.Reset();
		const double Len = TollTraffic::ClassLen(Looks[c].Cls);
		const double W = Looks[c].Width;
		const double H = Looks[c].Height;
		const FVector Scale(Len, W, H); // the engine cube is 1 m
		for (int32 i = 0; i < Vis.Num(); ++i)
		{
			const TollTraffic::FVisible& V = Vis[i];
			if (Done[i] || LookIndex(V.Cls) != c) continue;
			const FVector Pos = TollSpace::ToWorld(Origin, DrawnPos[i].X, DrawnPos[i].Y, H / 2 + Bob[i]);
			const FTransform Xf(Body[i], Pos);
			Scratch.Add(FTransform(Body[i], Pos, Scale));

			// lamps on the body's front and rear faces, placed in body space
			const double LampZ = FMath::Min(0.75, H * 0.45) - H / 2;
			const double Side = c == 0 ? 0.0 : W / 2 - 0.25;
			const bool bBrake = Motion.FindRef(V.Id).Accel < -0.8;
			const FVector LampScale(0.04, c == 0 ? 0.14 : 0.26, 0.12);
			for (const double S : {-Side, Side})
			{
				if (c == 0 && S > 0) continue; // a motorbike has one of each
				HeadXf.Add(FTransform(Body[i], Xf.TransformPosition(FVector(Len / 2 + 0.02, S, LampZ) * 100.0), LampScale));
				(bBrake ? BrakeXf : TailXf).Add(FTransform(Body[i], Xf.TransformPosition(FVector(-Len / 2 - 0.02, S, LampZ) * 100.0), LampScale));
			}
		}
		SetInstances(ClassMeshes[c], Scratch);
	}
	SetInstances(HeadLamps, HeadXf);
	SetInstances(TailLamps, TailXf);
	SetInstances(BrakeLamps, BrakeXf);
}

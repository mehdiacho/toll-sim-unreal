#include "TollTrafficActor.h"

#include "Camera/CameraComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/PostProcessComponent.h"
#include "Components/SkyAtmosphereComponent.h"
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
				Col.Add(C * float(0.92 + Noise * 0.16));
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

	if (UMaterialInterface* M = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Toll/Materials/M_VertexColor.M_VertexColor")))
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

void ATollTrafficActor::SetInstances(UInstancedStaticMeshComponent* Ism, const TArray<FTransform>& Xf)
{
	const int32 Have = Ism->GetInstanceCount();
	if (Have > Xf.Num())
	{
		Ism->ClearInstances();
		Ism->AddInstances(Xf, false, true);
		return;
	}
	if (Have > 0) Ism->BatchUpdateInstancesTransforms(0, MakeArrayView(Xf.GetData(), Have), true, false, false);
	if (Xf.Num() > Have) Ism->AddInstances(TArray<FTransform>(Xf.GetData() + Have, Xf.Num() - Have), false, true);
	Ism->MarkRenderStateDirty();
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
			if (LookIndex(V.Cls) != c) continue;
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

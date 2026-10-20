#include "TollTrafficActor.h"

#include "Camera/CameraComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
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

	Sky = CreateDefaultSubobject<USkyAtmosphereComponent>(TEXT("Sky"));
	Sky->SetupAttachment(SceneRoot);

	SkyLight = CreateDefaultSubobject<USkyLightComponent>(TEXT("SkyLight"));
	SkyLight->SetupAttachment(SceneRoot);
	SkyLight->SetMobility(EComponentMobility::Movable);
	SkyLight->bRealTimeCapture = true;

	Fog = CreateDefaultSubobject<UExponentialHeightFogComponent>(TEXT("Fog"));
	Fog->SetupAttachment(SceneRoot);
	Fog->SetFogDensity(0.004f);
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

	// a sand-coloured ground under everything, until the terrain milestone
	const double Half = 200000.0; // metres
	const FVector C = TollSpace::ToWorld(Origin, Origin.X, Origin.Y, 0);
	TArray<FVector> GV = {C + FVector(-Half, -Half, 0) * 100, C + FVector(Half, -Half, 0) * 100, C + FVector(-Half, Half, 0) * 100, C + FVector(Half, Half, 0) * 100};
	Road->CreateMeshSection(1, GV, {0, 2, 1, 1, 2, 3}, {FVector::UpVector, FVector::UpVector, FVector::UpVector, FVector::UpVector}, {}, {}, {}, false);

	if (UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial")))
	{
		const auto Tint = [&](int32 Section, FColor C)
		{
			UMaterialInstanceDynamic* M = UMaterialInstanceDynamic::Create(Base, this);
			M->SetVectorParameterValue(TEXT("Color"), FLinearColor::FromSRGBColor(C));
			Road->SetMaterial(Section, M);
		};
		Tint(0, FColor(0x3b, 0x3d, 0x40)); // asphalt
		Tint(1, FColor(0xd8, 0xc9, 0xa0)); // sand, until the terrain milestone
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
	FrameCamera();
	if (UTollFeedSubsystem* Feed = GetGameInstance() ? GetGameInstance()->GetSubsystem<UTollFeedSubsystem>() : nullptr)
	{
		Feed->Start();
	}
}

void ATollTrafficActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	const UTollFeedSubsystem* Feed = GetGameInstance() ? GetGameInstance()->GetSubsystem<UTollFeedSubsystem>() : nullptr;
	if (!Feed) return;

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
	for (int32 c = 0; c < ClassMeshes.Num(); ++c)
	{
		Scratch.Reset();
		const double Len = TollTraffic::ClassLen(Looks[c].Cls);
		const FVector Scale(Len, Looks[c].Width, Looks[c].Height); // the engine cube is 1 m
		for (int32 i = 0; i < Vis.Num(); ++i)
		{
			const TollTraffic::FVisible& V = Vis[i];
			if (LookIndex(V.Cls) != c) continue;
			const FVector Pos = TollSpace::ToWorld(Origin, DrawnPos[i].X, DrawnPos[i].Y, Looks[c].Height / 2);
			Scratch.Add(FTransform(FRotator(0, TollSpace::ToYaw(V.Heading), 0), Pos, Scale));
		}
		UInstancedStaticMeshComponent* Ism = ClassMeshes[c];
		const int32 Have = Ism->GetInstanceCount();
		if (Have > Scratch.Num())
		{
			Ism->ClearInstances();
			Ism->AddInstances(Scratch, false, true);
		}
		else
		{
			if (Have > 0) Ism->BatchUpdateInstancesTransforms(0, MakeArrayView(Scratch.GetData(), Have), true, false, false);
			if (Scratch.Num() > Have)
			{
				Ism->AddInstances(TArray<FTransform>(Scratch.GetData() + Have, Scratch.Num() - Have), false, true);
			}
			Ism->MarkRenderStateDirty();
		}
	}
}

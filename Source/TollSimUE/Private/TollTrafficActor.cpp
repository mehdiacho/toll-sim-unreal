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

	TArray<FVector> Verts;
	TArray<int32> Tris;
	TArray<FVector> Normals;
	TArray<FVector2D> UVs;
	for (const TSharedPtr<FJsonValue>& LV : Root->GetArrayField(TEXT("lanes")))
	{
		const TSharedPtr<FJsonObject> Lane = LV->AsObject();
		const double HalfW = Lane->GetNumberField(TEXT("w")) / 2;
		const TArray<TSharedPtr<FJsonValue>>& Pts = Lane->GetArrayField(TEXT("pts"));
		if (Pts.Num() < 2) continue;
		TArray<FVector2D> P;
		for (const TSharedPtr<FJsonValue>& PV : Pts)
		{
			const TArray<TSharedPtr<FJsonValue>>& XY = PV->AsArray();
			P.Add(FVector2D(XY[0]->AsNumber(), XY[1]->AsNumber()));
		}
		const int32 Base = Verts.Num();
		double Along = 0;
		for (int32 i = 0; i < P.Num(); ++i)
		{
			const FVector2D Dir = (P[FMath::Min(i + 1, P.Num() - 1)] - P[FMath::Max(i - 1, 0)]).GetSafeNormal();
			const FVector2D Left(-Dir.Y, Dir.X);
			if (i > 0) Along += FVector2D::Distance(P[i], P[i - 1]);
			Verts.Add(TollSpace::ToWorld(Origin, P[i].X + Left.X * HalfW, P[i].Y + Left.Y * HalfW, 0.02));
			Verts.Add(TollSpace::ToWorld(Origin, P[i].X - Left.X * HalfW, P[i].Y - Left.Y * HalfW, 0.02));
			Normals.Add(FVector::UpVector);
			Normals.Add(FVector::UpVector);
			UVs.Add(FVector2D(0, Along / 4));
			UVs.Add(FVector2D(1, Along / 4));
			if (i > 0)
			{
				const int32 A = Base + (i - 1) * 2;
				// winding faces up in Unreal's left-handed space
				Tris.Append({A, A + 1, A + 2, A + 1, A + 3, A + 2});
			}
		}
	}
	Road->CreateMeshSection(0, Verts, Tris, Normals, UVs, {}, {}, false);

	// a sand-coloured ground under everything, until the terrain milestone
	const double Half = 200000.0; // metres
	const FVector C = TollSpace::ToWorld(Origin, Origin.X, Origin.Y, 0);
	TArray<FVector> GV = {C + FVector(-Half, -Half, 0) * 100, C + FVector(Half, -Half, 0) * 100, C + FVector(-Half, Half, 0) * 100, C + FVector(Half, Half, 0) * 100};
	Road->CreateMeshSection(1, GV, {0, 2, 1, 1, 2, 3}, {FVector::UpVector, FVector::UpVector, FVector::UpVector, FVector::UpVector}, {}, {}, {}, false);

	if (UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial")))
	{
		UMaterialInstanceDynamic* Asphalt = UMaterialInstanceDynamic::Create(Base, this);
		Asphalt->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.035f, 0.035f, 0.04f));
		Road->SetMaterial(0, Asphalt);
		UMaterialInstanceDynamic* Sand = UMaterialInstanceDynamic::Create(Base, this);
		Sand->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.42f, 0.33f, 0.22f));
		Road->SetMaterial(1, Sand);
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
	const FVector2D Eye2 = *P - Fwd * 70.0 + Side * 45.0;
	const FVector Eye = TollSpace::ToWorld(Origin, Eye2.X, Eye2.Y, 32.0);
	Camera->SetWorldLocationAndRotation(Eye, (Target - Eye).Rotation());
}

void ATollTrafficActor::BeginPlay()
{
	Super::BeginPlay();
	LoadRoad();
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
	for (int32 c = 0; c < ClassMeshes.Num(); ++c)
	{
		Scratch.Reset();
		const double Len = TollTraffic::ClassLen(Looks[c].Cls);
		const FVector Scale(Len, Looks[c].Width, Looks[c].Height); // the engine cube is 1 m
		for (const TollTraffic::FVisible& V : Vis)
		{
			if (LookIndex(V.Cls) != c) continue;
			const FVector Pos = TollSpace::ToWorld(Origin, V.X, V.Y, Looks[c].Height / 2);
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

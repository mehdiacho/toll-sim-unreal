#include "TollPlazaActor.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "ImageUtils.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/Base64.h"
#include "Misc/Paths.h"
#include "ProceduralMeshComponent.h"
#include "TollTrafficActor.h"

DEFINE_LOG_CATEGORY_STATIC(LogTollPlaza, Log, All);

namespace
{
	TArray<float> Floats(const TSharedPtr<FJsonObject>& Geo, const TCHAR* Field)
	{
		TArray<float> Out;
		FString B64;
		if (!Geo->TryGetStringField(Field, B64)) return Out;
		TArray<uint8> Bytes;
		FBase64::Decode(B64, Bytes);
		Out.SetNumUninitialized(Bytes.Num() / sizeof(float));
		FMemory::Memcpy(Out.GetData(), Bytes.GetData(), Out.Num() * sizeof(float));
		return Out;
	}

	// three.js plaza frame (x = u, y = up, z = -v, metres) to the actor's local
	// frame (X = u, Y = -v, Z = up, centimetres): swap y and z.
	FVector Local(float X, float Y, float Z) { return FVector(X, Z, Y) * 100.0; }

	/** Append a three.js triangle soup to mesh arrays. The y/z swap mirrors, so the winding is reversed. */
	void AppendGeometry(const TSharedPtr<FJsonObject>& Geo, TArray<FVector>& V, TArray<int32>& T, TArray<FVector>& N, TArray<FVector2D>& UV, TArray<FLinearColor>& C,
		const FMatrix* Xf = nullptr)
	{
		const TArray<float> P = Floats(Geo, TEXT("position"));
		const TArray<float> Nm = Floats(Geo, TEXT("normal"));
		const TArray<float> Cl = Floats(Geo, TEXT("color"));
		const TArray<float> Uv = Floats(Geo, TEXT("uv"));
		const int32 Count = P.Num() / 3;
		const int32 Base = V.Num();
		for (int32 i = 0; i < Count; ++i)
		{
			FVector Pos(P[i * 3], P[i * 3 + 1], P[i * 3 + 2]);
			FVector Nrm = Nm.Num() >= (i + 1) * 3 ? FVector(Nm[i * 3], Nm[i * 3 + 1], Nm[i * 3 + 2]) : FVector(0, 1, 0);
			if (Xf)
			{
				Pos = Xf->TransformPosition(Pos);
				Nrm = Xf->TransformVector(Nrm).GetSafeNormal();
			}
			V.Add(Local(Pos.X, Pos.Y, Pos.Z));
			N.Add(FVector(Nrm.X, Nrm.Z, Nrm.Y));
			UV.Add(Uv.Num() >= (i + 1) * 2 ? FVector2D(Uv[i * 2], 1.0 - Uv[i * 2 + 1]) : FVector2D::ZeroVector);
			C.Add(Cl.Num() >= (i + 1) * 3 ? FLinearColor(Cl[i * 3], Cl[i * 3 + 1], Cl[i * 3 + 2]) : FLinearColor::White);
		}
		for (int32 i = 0; i + 2 < Count; i += 3)
		{
			T.Append({Base + i, Base + i + 2, Base + i + 1});
		}
	}

	FMatrix ThreeMatrix(const TArray<TSharedPtr<FJsonValue>>& E)
	{
		// three.js stores column-major; FMatrix::TransformPosition treats rows as basis vectors
		FMatrix M;
		for (int32 c = 0; c < 4; ++c)
			for (int32 r = 0; r < 4; ++r)
				M.M[c][r] = E[c * 4 + r]->AsNumber();
		return M;
	}
}

ATollPlazaActor::ATollPlazaActor()
{
	PrimaryActorTick.bCanEverTick = false;
	Static = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("Static"));
	RootComponent = Static;
	Static->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Static->bUseAsyncCooking = true;
}

UMaterialInstanceDynamic* ATollPlazaActor::MaterialFor(const FString& MeshRole, const FString& Texture, const FString& TextureDir)
{
	static const TCHAR* VertexPath = TEXT("/Game/Toll/Materials/M_VertexColor.M_VertexColor");
	static const TCHAR* TexturedPath = TEXT("/Game/Toll/Materials/M_Textured.M_Textured");
	const bool bTextured = !Texture.IsEmpty() && MeshRole != TEXT("glow");
	UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, bTextured ? TexturedPath : VertexPath);
	if (!Base) return nullptr;
	UMaterialInstanceDynamic* M = UMaterialInstanceDynamic::Create(Base, this);
	if (bTextured)
	{
		if (UTexture2D* Tex = FImageUtils::ImportFileAsTexture2D(TextureDir / Texture + TEXT(".png")))
		{
			M->SetTextureParameterValue(TEXT("Tex"), Tex);
		}
		M->SetScalarParameterValue(TEXT("Emissive"), MeshRole == TEXT("fare") ? 1.0f : 0.06f);
	}
	else
	{
		const float Rough = MeshRole == TEXT("apron") ? 0.93f : MeshRole == TEXT("marks") ? 0.6f : 0.7f;
		M->SetScalarParameterValue(TEXT("Roughness"), Rough);
		M->SetScalarParameterValue(TEXT("Metallic"), MeshRole == TEXT("body") ? 0.1f : 0.0f);
		M->SetScalarParameterValue(TEXT("Emissive"), MeshRole == TEXT("lights") ? 1.0f : 0.0f);
	}
	return M;
}

void ATollPlazaActor::Build(const TSharedPtr<FJsonObject>& Plaza, const FString& TextureDir, const FVector2D& Origin)
{
	PlazaId = Plaza->GetStringField(TEXT("id"));
	const double Heading = Plaza->GetNumberField(TEXT("heading"));
	SetActorLocationAndRotation(TollSpace::ToWorld(Origin, Plaza->GetNumberField(TEXT("x")), Plaza->GetNumberField(TEXT("y"))),
		FRotator(0, -FMath::RadiansToDegrees(Heading), 0));

	int32 Section = 0;
	for (const TSharedPtr<FJsonValue>& MV : Plaza->GetArrayField(TEXT("meshes")))
	{
		const TSharedPtr<FJsonObject> Mesh = MV->AsObject();
		const FString MeshRole = Mesh->GetStringField(TEXT("role"));
		if (MeshRole == TEXT("glow")) continue; // night ground glow: M3, with the light cycle
		FString Texture;
		Mesh->TryGetStringField(TEXT("texture"), Texture);
		TArray<FVector> V;
		TArray<int32> T;
		TArray<FVector> N;
		TArray<FVector2D> UV;
		TArray<FLinearColor> C;
		AppendGeometry(Mesh->GetObjectField(TEXT("geometry")), V, T, N, UV, C);
		Static->CreateMeshSection_LinearColor(Section, V, T, N, UV, C, {}, false);
		Static->SetMaterial(Section, MaterialFor(MeshRole, Texture, TextureDir));
		++Section;
	}

	// lane signals, all showing the green arrow until lanes_open says otherwise
	const TSharedPtr<FJsonObject>* Signals;
	if (Plaza->TryGetObjectField(TEXT("signals"), Signals))
	{
		const TSharedPtr<FJsonObject>* Arrow;
		if ((*Signals)->TryGetObjectField(TEXT("arrow"), Arrow))
		{
			TArray<FVector> V;
			TArray<int32> T;
			TArray<FVector> N;
			TArray<FVector2D> UV;
			TArray<FLinearColor> C;
			for (const TSharedPtr<FJsonValue>& E : (*Arrow)->GetArrayField(TEXT("matrices")))
			{
				const FMatrix M = ThreeMatrix(E->AsArray());
				AppendGeometry((*Arrow)->GetObjectField(TEXT("geometry")), V, T, N, UV, C, &M);
			}
			Static->CreateMeshSection_LinearColor(Section, V, T, N, UV, C, {}, false);
			UMaterialInstanceDynamic* M = MaterialFor(TEXT("signal"), TEXT("sig-arrow"), TextureDir);
			if (M) M->SetScalarParameterValue(TEXT("Emissive"), 2.0f);
			Static->SetMaterial(Section, M);
			++Section;
		}
	}

	// booms: one small mesh each, pivoting about its local z (three.js) = actor-local -Y swing
	const TSharedPtr<FJsonObject>* ArmGeo;
	const bool bHasArm = Plaza->TryGetObjectField(TEXT("boomGeometry"), ArmGeo);
	TMap<FString, TSharedPtr<FJsonObject>> Lamps;
	for (const TSharedPtr<FJsonValue>& LV : Plaza->GetArrayField(TEXT("lamps"))) Lamps.Add(LV->AsObject()->GetStringField(TEXT("key")), LV->AsObject());
	TMap<FString, FString> Types;
	for (const TSharedPtr<FJsonValue>& LV : Plaza->GetArrayField(TEXT("lanes"))) Types.Add(LV->AsObject()->GetStringField(TEXT("key")), LV->AsObject()->GetStringField(TEXT("type")));
	UStaticMesh* Sphere = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	UMaterialInterface* ColorMat = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Toll/Materials/M_Color.M_Color"));
	UMaterialInstanceDynamic* ArmMat = MaterialFor(TEXT("body"), FString(), TextureDir);

	for (const TSharedPtr<FJsonValue>& BV : Plaza->GetArrayField(TEXT("booms")))
	{
		const TSharedPtr<FJsonObject> B = BV->AsObject();
		FTollBoom Boom;
		Boom.Key = B->GetStringField(TEXT("key"));
		Boom.Type = Types.FindRef(Boom.Key);
		Boom.Side = static_cast<int32>(B->GetNumberField(TEXT("side")));
		const double Len = B->GetNumberField(TEXT("len"));
		if (bHasArm)
		{
			UProceduralMeshComponent* Arm = NewObject<UProceduralMeshComponent>(this);
			Arm->SetupAttachment(Static);
			Arm->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Arm->RegisterComponent();
			// bake the length into the arm; it runs along local +X from the pivot
			const FMatrix Scale = FScaleMatrix(FVector(Len, 1, 1));
			TArray<FVector> V;
			TArray<int32> T;
			TArray<FVector> N;
			TArray<FVector2D> UV;
			TArray<FLinearColor> C;
			AppendGeometry(*ArmGeo, V, T, N, UV, C, &Scale);
			Arm->CreateMeshSection_LinearColor(0, V, T, N, UV, C, {}, false);
			Arm->SetMaterial(0, ArmMat);
			Arm->SetRelativeLocation(Local(B->GetNumberField(TEXT("u")), B->GetNumberField(TEXT("y")), -B->GetNumberField(TEXT("v"))));
			Boom.Arm = Arm;
		}
		if (const TSharedPtr<FJsonObject>* L = Lamps.Find(Boom.Key); L && Sphere && ColorMat)
		{
			UStaticMeshComponent* Lamp = NewObject<UStaticMeshComponent>(this);
			Lamp->SetupAttachment(Static);
			Lamp->SetStaticMesh(Sphere);
			Lamp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Lamp->SetCastShadow(false);
			Lamp->SetRelativeScale3D(FVector(0.2));
			Lamp->SetRelativeLocation(Local((*L)->GetNumberField(TEXT("u")), (*L)->GetNumberField(TEXT("y")), -(*L)->GetNumberField(TEXT("v"))));
			Lamp->RegisterComponent();
			Boom.LampMat = UMaterialInstanceDynamic::Create(ColorMat, this);
			Boom.LampMat->SetScalarParameterValue(TEXT("Emissive"), 6.0f);
			Lamp->SetMaterial(0, Boom.LampMat);
			Boom.Lamp = Lamp;
		}
		Booms.Add(Boom);
	}
	Advance(0, 0);
	UE_LOG(LogTollPlaza, Log, TEXT("plaza %s: %d sections, %d booms"), *PlazaId, Section, Booms.Num());
}

void ATollPlazaActor::NoteVehicle(const TollPlaza::FLayout& Layout, const TollPlaza::FContext& Ctx, double Len, double Speed)
{
	const TollPlaza::FLane* Lane = TollPlaza::LaneAt(Layout, Ctx.Dir, Ctx.V, 0.8);
	if (!Lane) return;
	const double Front = Ctx.T + Len / 2;
	const double Rear = Ctx.T - Len / 2;
	const bool bEtag = Lane->Type == TEXT("etag");
	const double Reach = bEtag ? 9 : 2.2;
	if (Front > TollPlaza::BoomT - Reach && Rear < TollPlaza::BoomT + 0.8 && Speed > (bEtag ? 1.0 : 0.35))
	{
		for (FTollBoom& B : Booms)
		{
			if (B.Key == Lane->Key) B.bLocalUp = true;
		}
	}
}

void ATollPlazaActor::NoteFeedBoom(const FString& Key, bool bUp, double At)
{
	for (FTollBoom& B : Booms)
	{
		if (B.Key == Key) B.FeedUntil = bUp ? At + 6.0 : 0;
	}
}

void ATollPlazaActor::Advance(double Dt, double RenderT)
{
	for (FTollBoom& B : Booms)
	{
		const bool bFeedUp = RenderT < B.FeedUntil;
		const bool bUp = B.bOpen && (B.bLocalUp || bFeedUp);
		const double Target = bUp ? UE_DOUBLE_PI * 0.47 : 0;
		const double Speed = bUp ? 1.9 : 1.2;
		B.Angle += FMath::Sign(Target - B.Angle) * FMath::Min(FMath::Abs(Target - B.Angle), Speed * Dt);
		if (B.Arm)
		{
			// three.js: RotY(+-90deg) then RotZ(angle). In actor space the arm points along
			// -+Y (v = side) and lifts toward +Z.
			const double Yaw = B.Side > 0 ? -90.0 : 90.0;
			B.Arm->SetRelativeRotation(FRotator(FMath::RadiansToDegrees(B.Angle), Yaw, 0));
		}
		if (B.LampMat)
		{
			const FLinearColor C = !B.bOpen ? FLinearColor(0.05f, 0.01f, 0.01f) : (bUp || B.Angle > 0.3) ? FLinearColor(0.19f, 1.0f, 0.42f) : FLinearColor(1.0f, 0.16f, 0.1f);
			B.LampMat->SetVectorParameterValue(TEXT("Color"), C);
		}
		B.bLocalUp = false; // re-set by NoteVehicle every frame
	}
}

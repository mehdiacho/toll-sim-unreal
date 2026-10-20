#include "TollTraffic.h"

#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace TollTraffic
{
	double ShortAngle(double A, double B)
	{
		// JS % keeps the sign of the dividend, as fmod does
		const double D = FMath::Fmod(B - A + 540.0, 360.0) - 180.0;
		return A + D;
	}

	double ClassLen(const FString& Cls)
	{
		if (Cls == TEXT("M")) return 2.2;
		if (Cls == TEXT("2")) return 7.5;
		if (Cls == TEXT("3")) return 12.0;
		if (Cls == TEXT("4")) return 18.5;
		return 4.6;
	}

	FVector2D FrontToCentre(double X, double Y, double HeadingDeg, double Len)
	{
		const double A = FMath::DegreesToRadians(HeadingDeg);
		const double H = Len / 2.0;
		return FVector2D(X - FMath::Sin(A) * H, Y - FMath::Cos(A) * H);
	}

	static FString ValueToString(const TSharedPtr<FJsonValue>& V)
	{
		if (!V.IsValid()) return FString();
		if (V->Type == EJson::String) return V->AsString();
		if (V->Type == EJson::Number) return FString::Printf(TEXT("%d"), static_cast<int32>(V->AsNumber()));
		return FString();
	}

	void ParseSpawn(const TArray<TSharedPtr<FJsonValue>>& Rows, TArray<FSpawn>& Out)
	{
		for (const TSharedPtr<FJsonValue>& R : Rows)
		{
			const TArray<TSharedPtr<FJsonValue>>* A;
			if (!R.IsValid() || !R->TryGetArray(A) || A->Num() < 3) continue;
			FSpawn S;
			S.Id = static_cast<int32>((*A)[0]->AsNumber());
			S.Plate = ValueToString((*A)[1]);
			S.Cls = ValueToString((*A)[2]);
			S.Flags = A->Num() > 3 && (*A)[3]->Type == EJson::Number ? static_cast<int32>((*A)[3]->AsNumber()) : 0;
			Out.Add(MoveTemp(S));
		}
	}

	static void ReadExtra(const TSharedPtr<FJsonObject>& Obj, FFrame& Out, const TCHAR* const* Keys, int32 NumKeys, const TCHAR* const* Names)
	{
		Out.Extra = MakeShared<FJsonObject>();
		for (int32 i = 0; i < NumKeys; ++i)
		{
			if (const TSharedPtr<FJsonValue> V = Obj->TryGetField(Keys[i]))
			{
				Out.Extra->SetField(Names[i], V);
			}
		}
	}

	bool ParseFrame(const TSharedPtr<FJsonObject>& Obj, FFrame& Out)
	{
		if (!Obj.IsValid()) return false;
		Out = FFrame();
		Out.T = Obj->GetNumberField(TEXT("t"));
		const TArray<TSharedPtr<FJsonValue>>* Rows;
		if (Obj->TryGetArrayField(TEXT("vehicles"), Rows))
		{
			Out.Vehicles.Reserve(Rows->Num());
			for (const TSharedPtr<FJsonValue>& R : *Rows)
			{
				const TArray<TSharedPtr<FJsonValue>>& A = R->AsArray();
				if (A.Num() < 3) continue;
				FFrameVehicle V;
				V.Id = static_cast<int32>(A[0]->AsNumber());
				V.X = A[1]->AsNumber();
				V.Y = A[2]->AsNumber();
				if (A.Num() > 3 && A[3]->Type == EJson::Number) V.Heading = A[3]->AsNumber();
				V.Speed = A.Num() > 4 && A[4]->Type == EJson::Number ? A[4]->AsNumber() : 0.0;
				Out.Vehicles.Add(V);
			}
		}
		if (Obj->TryGetArrayField(TEXT("spawn"), Rows)) ParseSpawn(*Rows, Out.Spawn);
		if (Obj->TryGetArrayField(TEXT("gone"), Rows))
		{
			for (const TSharedPtr<FJsonValue>& G : *Rows) Out.Gone.Add(static_cast<int32>(G->AsNumber()));
		}
		static const TCHAR* Keys[] = {TEXT("booms"), TEXT("lanes_open"), TEXT("stats"), TEXT("events"), TEXT("clock")};
		ReadExtra(Obj, Out, Keys, UE_ARRAY_COUNT(Keys), Keys);
		return true;
	}

	bool DecodeReplay(const FString& Json, double& OutDt, TArray<FFrame>& OutFrames)
	{
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid()) return false;

		OutDt = Root->GetNumberField(TEXT("dt"));
		const TArray<TSharedPtr<FJsonValue>>* Frames;
		if (!Root->TryGetArrayField(TEXT("frames"), Frames)) return false;

		TMap<int32, FVector2D> Pos;
		TMap<int32, double> Ang;
		OutFrames.Reset(Frames->Num());
		for (const TSharedPtr<FJsonValue>& FV : *Frames)
		{
			const TSharedPtr<FJsonObject> F = FV->AsObject();
			FFrame Frame;
			Frame.T = F->GetNumberField(TEXT("t"));
			const TArray<TSharedPtr<FJsonValue>>& P = F->GetArrayField(TEXT("p"));
			TMap<int32, FVector2D> Next;
			Next.Reserve(P.Num() / 3);
			Frame.Vehicles.Reserve(P.Num() / 3);
			int32 Id = 0;
			for (int32 i = 0; i + 2 < P.Num(); i += 3)
			{
				Id += static_cast<int32>(P[i]->AsNumber());
				const FVector2D* Old = Pos.Find(Id);
				const double X = Old ? Old->X + P[i + 1]->AsNumber() : P[i + 1]->AsNumber();
				const double Y = Old ? Old->Y + P[i + 2]->AsNumber() : P[i + 2]->AsNumber();
				const double* PrevA = Ang.Find(Id);
				TOptional<double> A;
				if (PrevA) A = *PrevA;
				double Spd = 0;
				if (Old)
				{
					const double Dx = X - Old->X;
					const double Dy = Y - Old->Y;
					const double D = FMath::Sqrt(Dx * Dx + Dy * Dy);
					Spd = JsRound(D / OutDt);
					if (D > 3) A = FMath::Fmod(FMath::RadiansToDegrees(FMath::Atan2(Dx, Dy)) + 360.0, 360.0);
				}
				Next.Add(Id, FVector2D(X, Y));
				if (A.IsSet()) Ang.Add(Id, A.GetValue());
				FFrameVehicle V;
				V.Id = Id;
				V.X = X;
				V.Y = Y;
				if (A.IsSet()) V.Heading = JsRound(A.GetValue());
				V.Speed = Spd;
				Frame.Vehicles.Add(V);
			}
			Pos = MoveTemp(Next);

			const TArray<TSharedPtr<FJsonValue>>* Rows;
			if (F->TryGetArrayField(TEXT("s"), Rows)) ParseSpawn(*Rows, Frame.Spawn);
			if (F->TryGetArrayField(TEXT("g"), Rows))
			{
				for (const TSharedPtr<FJsonValue>& G : *Rows) Frame.Gone.Add(static_cast<int32>(G->AsNumber()));
			}
			static const TCHAR* Keys[] = {TEXT("b"), TEXT("o"), TEXT("st"), TEXT("e")};
			static const TCHAR* Names[] = {TEXT("booms"), TEXT("lanes_open"), TEXT("stats"), TEXT("events")};
			ReadExtra(F, Frame, Keys, UE_ARRAY_COUNT(Keys), Names);
			OutFrames.Add(MoveTemp(Frame));
		}

		// Vehicles first seen in a frame have no heading yet: borrow it from the next frame.
		for (int32 k = 0; k < OutFrames.Num(); ++k)
		{
			TMap<int32, double> Later;
			if (k + 1 < OutFrames.Num())
			{
				for (const FFrameVehicle& V : OutFrames[k + 1].Vehicles)
				{
					if (V.Heading.IsSet()) Later.Add(V.Id, V.Heading.GetValue());
				}
			}
			for (FFrameVehicle& V : OutFrames[k].Vehicles)
			{
				if (!V.Heading.IsSet())
				{
					const double* L = Later.Find(V.Id);
					V.Heading = L ? *L : 0.0;
				}
			}
		}
		return true;
	}

	void Ingest(TMap<int32, FTrack>& Tracks, const FFrame& Frame)
	{
		const double T = Frame.T;
		for (const FFrameVehicle& V : Frame.Vehicles)
		{
			FTrack& Track = Tracks.FindOrAdd(V.Id);
			FSample S{T, V.X / 10.0, V.Y / 10.0, V.Heading.Get(0.0), V.Speed / 10.0};
			if (Track.S.Num() > 0) S.Heading = ShortAngle(Track.S.Last().Heading, S.Heading);
			Track.S.Add(S);
			if (Track.S.Num() > 4) Track.S.RemoveAt(0);
			Track.GoneAt.Reset();
		}
		for (const int32 Id : Frame.Gone)
		{
			if (FTrack* Track = Tracks.Find(Id)) Track->GoneAt = T;
		}
	}

	bool SampleAt(const FTrack& Track, double T, FSample& Out)
	{
		const auto& S = Track.S;
		if (S.Num() == 0) return false;
		if (T <= S[0].T)
		{
			Out = S[0];
			return true;
		}
		for (int32 i = 1; i < S.Num(); ++i)
		{
			if (T <= S[i].T)
			{
				const FSample& A = S[i - 1];
				const FSample& B = S[i];
				const double Den = B.T - A.T;
				const double F = (T - A.T) / (Den != 0 ? Den : 1.0);
				Out = {T, A.X + (B.X - A.X) * F, A.Y + (B.Y - A.Y) * F, A.Heading + (B.Heading - A.Heading) * F, A.Speed + (B.Speed - A.Speed) * F};
				return true;
			}
		}
		// short extrapolation past the newest sample
		const FSample& B = S.Last();
		const double Dt = FMath::Min(2.5, T - B.T);
		const double Th = FMath::DegreesToRadians(B.Heading);
		Out = {T, B.X + FMath::Sin(Th) * B.Speed * Dt, B.Y + FMath::Cos(Th) * B.Speed * Dt, B.Heading, B.Speed};
		return true;
	}

	void CollectVisible(TMap<int32, FTrack>& Tracks, const TMap<int32, FString>& Classes, double RenderT, TArray<FVisible>& Out)
	{
		Out.Reset();
		for (auto It = Tracks.CreateIterator(); It; ++It)
		{
			const FTrack& Track = It.Value();
			if (Track.GoneAt.IsSet() && RenderT >= Track.GoneAt.GetValue())
			{
				It.RemoveCurrent();
				continue;
			}
			if (Track.S.Num() == 0 || Track.S[0].T > RenderT + 0.01) continue; // not yet born at render time
			FSample P;
			if (!SampleAt(Track, RenderT, P)) continue;
			const FString* Cls = Classes.Find(It.Key());
			FVisible V;
			V.Id = It.Key();
			V.Cls = Cls ? *Cls : FString(TEXT("1"));
			const FVector2D C = FrontToCentre(P.X, P.Y, P.Heading, ClassLen(V.Cls));
			V.X = C.X;
			V.Y = C.Y;
			V.Heading = P.Heading;
			V.Speed = P.Speed;
			Out.Add(MoveTemp(V));
		}
	}
}

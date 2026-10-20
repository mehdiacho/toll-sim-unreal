#include "TollPlaza.h"

namespace TollPlaza
{
	FVector2D FLayout::ToUV(double PX, double PY) const
	{
		const double Dx = PX - X;
		const double Dy = PY - Y;
		return FVector2D(Dx * Ux + Dy * Uy, Dx * Nx + Dy * Ny);
	}

	FVector2D FLayout::FromUV(double U, double V) const
	{
		return FVector2D(X + Ux * U + Nx * V, Y + Uy * U + Ny * V);
	}

	static double Smooth(double X, double A, double B)
	{
		const double K = FMath::Clamp((X - A) / (B - A), 0.0, 1.0);
		return K * K * (3 - 2 * K);
	}

	double Flare(double T)
	{
		if (T < FlareIn0 || T > FlareOut1) return 0;
		if (T < FlareIn1) return Smooth(T, FlareIn0, FlareIn1);
		if (T > FlareOut0) return 1 - Smooth(T, FlareOut0, FlareOut1);
		return 1;
	}

	double MapV(const FLayout& P, TCHAR Dir, double V)
	{
		if (!P.bRemap) return V;
		TArray<FVector2D> Pts; // (srcV, v)
		for (const FLane& L : P.Lanes)
		{
			if (L.Dir == Dir) Pts.Add(FVector2D(L.SrcV, L.V));
		}
		if (Pts.Num() == 0) return V;
		Pts.StableSort([](const FVector2D& A, const FVector2D& B) { return A.X < B.X; });
		if (V <= Pts[0].X) return Pts[0].Y + (V - Pts[0].X);
		for (int32 i = 1; i < Pts.Num(); ++i)
		{
			if (V <= Pts[i].X)
			{
				const FVector2D& A = Pts[i - 1];
				const FVector2D& B = Pts[i];
				const double Den = B.X - A.X;
				return A.Y + ((V - A.X) / (Den != 0 ? Den : 1.0)) * (B.Y - A.Y);
			}
		}
		const FVector2D& Last = Pts.Last();
		return Last.Y + (V - Last.X);
	}

	const FLane* LaneAt(const FLayout& P, TCHAR Dir, double V, double Slack)
	{
		const FLane* Best = nullptr;
		double BestD = 0;
		for (const FLane& L : P.Lanes)
		{
			if (L.Dir != Dir) continue;
			const double D = FMath::Abs(L.V - V);
			if (D <= L.W / 2 + Slack && (!Best || D < BestD))
			{
				Best = &L;
				BestD = D;
			}
		}
		return Best;
	}

	void ParseLayouts(const TArray<TSharedPtr<FJsonValue>>& Plazas, TArray<FLayout>& Out)
	{
		Out.Reset();
		for (const TSharedPtr<FJsonValue>& PV : Plazas)
		{
			const TSharedPtr<FJsonObject> O = PV->AsObject();
			FLayout P;
			P.Id = O->GetStringField(TEXT("id"));
			P.Name = O->GetStringField(TEXT("name"));
			P.X = O->GetNumberField(TEXT("x"));
			P.Y = O->GetNumberField(TEXT("y"));
			P.Heading = O->GetNumberField(TEXT("heading"));
			P.Ux = O->GetNumberField(TEXT("ux"));
			P.Uy = O->GetNumberField(TEXT("uy"));
			P.Nx = O->GetNumberField(TEXT("nx"));
			P.Ny = O->GetNumberField(TEXT("ny"));
			P.HalfWidth = O->GetNumberField(TEXT("halfWidth"));
			P.bRemap = O->GetBoolField(TEXT("remap"));
			for (const TSharedPtr<FJsonValue>& LV : O->GetArrayField(TEXT("lanes")))
			{
				const TSharedPtr<FJsonObject> LO = LV->AsObject();
				FLane L;
				L.Dir = LO->GetStringField(TEXT("dir"))[0];
				L.Idx = static_cast<int32>(LO->GetNumberField(TEXT("idx")));
				L.Type = LO->GetStringField(TEXT("type"));
				L.Key = LO->GetStringField(TEXT("key"));
				L.U = LO->GetNumberField(TEXT("u"));
				L.V = LO->GetNumberField(TEXT("v"));
				L.W = LO->GetNumberField(TEXT("w"));
				L.SrcV = LO->GetNumberField(TEXT("srcV"));
				P.Lanes.Add(L);
			}
			Out.Add(MoveTemp(P));
		}
	}

	bool Context(const TArray<FLayout>& Plazas, double X, double Y, double HeadingDeg, FContext& Out)
	{
		for (int32 i = 0; i < Plazas.Num(); ++i)
		{
			const FLayout& P = Plazas[i];
			const double Dx = X - P.X;
			const double Dy = Y - P.Y;
			if (Dx * Dx + Dy * Dy > 420.0 * 420.0) continue;
			const FVector2D Q = P.ToUV(X, Y);
			if (FMath::Abs(Q.Y) > P.HalfWidth + 8) continue;
			const double Th = FMath::DegreesToRadians(HeadingDeg);
			const TCHAR Dir = FMath::Sin(Th) * P.Ux + FMath::Cos(Th) * P.Uy >= 0 ? 'N' : 'S';
			const double T = Dir == 'S' ? -Q.X : Q.X;
			if (T < Zone0 || T > Zone1) continue;
			Out = {i, Dir, T, Q.X, Q.Y};
			return true;
		}
		return false;
	}

	FVector2D Drawn(const TArray<FLayout>& Plazas, double X, double Y, double HeadingDeg, FContext* OutCtx)
	{
		FContext C;
		if (!Context(Plazas, X, Y, HeadingDeg, C)) return FVector2D(X, Y);
		const FLayout& P = Plazas[C.Plaza];
		if (P.bRemap)
		{
			const double F = Flare(C.T);
			if (F > 0)
			{
				C.V = C.V + (MapV(P, C.Dir, C.V) - C.V) * F;
				if (OutCtx) *OutCtx = C;
				return P.FromUV(C.U, C.V);
			}
		}
		if (OutCtx) *OutCtx = C;
		return FVector2D(X, Y);
	}
}

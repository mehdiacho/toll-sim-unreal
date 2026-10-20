#include "TollFeedSubsystem.h"

#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogTollFeed, Log, All);

const TCHAR* UTollFeedSubsystem::DefaultStreamUrl = TEXT("https://mehdiacho.tech/projects/toll-sim/live/live/stream");

namespace
{
	constexpr double InitTimeout = 6.0;
	constexpr double FirstProbeDelay = 45.0;
}

void UTollFeedSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	StreamUrl = DefaultStreamUrl;
	FParse::Value(FCommandLine::Get(), TEXT("TollFeed="), StreamUrl);
}

void UTollFeedSubsystem::Deinitialize()
{
	if (Request.IsValid())
	{
		Request->OnProcessRequestComplete().Unbind();
		Request->CancelRequest();
		Request.Reset();
	}
	bStarted = false;
	Super::Deinitialize();
}

TStatId UTollFeedSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UTollFeedSubsystem, STATGROUP_Tickables);
}

bool UTollFeedSubsystem::LoadReplay(double& OutDt, TArray<TollTraffic::FFrame>& OutFrames)
{
	const FString Path = FPaths::ProjectContentDir() / TEXT("Data/replay.json");
	FString Json;
	if (!FFileHelper::LoadFileToString(Json, *Path))
	{
		UE_LOG(LogTollFeed, Error, TEXT("replay unavailable: %s"), *Path);
		return false;
	}
	return TollTraffic::DecodeReplay(Json, OutDt, OutFrames) && OutFrames.Num() > 0;
}

void UTollFeedSubsystem::Start()
{
	if (bStarted) return;
	bStarted = true;
	if (FParse::Param(FCommandLine::Get(), TEXT("TollReplay")))
	{
		StartReplay();
		return;
	}
	TryLive();
}

void UTollFeedSubsystem::SetMode(ETollFeedMode NewMode)
{
	if (Mode == NewMode) return;
	Mode = NewMode;
	UE_LOG(LogTollFeed, Log, TEXT("feed mode: %s"), *UEnum::GetValueAsString(NewMode));
}

void UTollFeedSubsystem::TryLive()
{
	if (Request.IsValid()) return;
	{
		FScopeLock Lock(&StreamLock);
		StreamBytes.Reset();
		bStreamClosed = false;
	}
	StreamText.Reset();
	bGotInit = false;
	LiveDeadline = FPlatformTime::Seconds() + InitTimeout;

	Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(StreamUrl);
	Request->SetVerb(TEXT("GET"));
	Request->SetHeader(TEXT("Accept"), TEXT("text/event-stream"));
	Request->SetHeader(TEXT("Cache-Control"), TEXT("no-cache"));
	Request->SetTimeout(0); // the stream is open-ended
	FHttpRequestStreamDelegateV2 OnBytes;
	OnBytes.BindWeakLambda(this, [this](void* Ptr, int64& Length)
	{
		FScopeLock Lock(&StreamLock);
		StreamBytes.Append(static_cast<const uint8*>(Ptr), Length);
	});
	Request->SetResponseBodyReceiveStreamDelegateV2(OnBytes);
	Request->OnProcessRequestComplete().BindWeakLambda(this, [this](FHttpRequestPtr, FHttpResponsePtr, bool)
	{
		FScopeLock Lock(&StreamLock);
		bStreamClosed = true;
	});
	Request->ProcessRequest();
}

void UTollFeedSubsystem::FailLive()
{
	if (Request.IsValid())
	{
		Request->OnProcessRequestComplete().Unbind();
		Request->CancelRequest();
		Request.Reset();
	}
	if (Mode != ETollFeedMode::Replay) StartReplay();
	// keep probing for the live feed in the background
	ProbeAt = FPlatformTime::Seconds() + FirstProbeDelay;
}

void UTollFeedSubsystem::StartReplay()
{
	SetMode(ETollFeedMode::Replay);
	if (ReplayFrames.Num() == 0 && !LoadReplay(ReplayDt, ReplayFrames))
	{
		SetMode(ETollFeedMode::Offline);
		return;
	}
	OnInit(false, ReplayFrames[0].Spawn, &ReplayFrames[0]);
	ReplayNext = 1;
	ReplayWait = ReplayDt;
}

void UTollFeedSubsystem::ResetState()
{
	Classes.Reset();
	Tracks.Reset();
	Visible.Reset();
	NewestT = 0;
	RenderT = 0;
}

void UTollFeedSubsystem::OnInit(bool bLive, const TArray<TollTraffic::FSpawn>& Registry, const TollTraffic::FFrame* Frame)
{
	ResetState();
	for (const TollTraffic::FSpawn& S : Registry) Classes.Add(S.Id, S.Cls);
	if (Frame) Ingest(*Frame, true);
}

void UTollFeedSubsystem::Ingest(const TollTraffic::FFrame& Frame, bool bFirst)
{
	for (const TollTraffic::FSpawn& S : Frame.Spawn) Classes.Add(S.Id, S.Cls);
	TollTraffic::Ingest(Tracks, Frame);
	NewestT = Frame.T;
	if (bFirst) RenderT = Frame.T - TollTraffic::Lag;
	LatestExtra = Frame.Extra;

	if (bFirst) PendingBooms.Reset();
	const TArray<TSharedPtr<FJsonValue>>* Booms;
	if (Frame.Extra.IsValid() && Frame.Extra->TryGetArrayField(TEXT("booms"), Booms))
	{
		const double At = bFirst ? Frame.T - TollTraffic::Lag : Frame.T - 0.6;
		for (const TSharedPtr<FJsonValue>& BV : *Booms)
		{
			const TArray<TSharedPtr<FJsonValue>>* B;
			if (!BV->TryGetArray(B) || B->Num() < 4) continue;
			FBoomEvent E;
			E.At = At;
			E.Plaza = (*B)[0]->AsString();
			E.Key = (*B)[1]->AsString() + FString::FromInt(static_cast<int32>((*B)[2]->AsNumber()));
			E.bUp = (*B)[3]->Type == EJson::Boolean ? (*B)[3]->AsBool() : (*B)[3]->AsNumber() != 0;
			PendingBooms.Add(MoveTemp(E));
		}
	}
}

void UTollFeedSubsystem::TakeDueBooms(TArray<FBoomEvent>& Out)
{
	Out.Reset();
	int32 N = 0;
	while (N < PendingBooms.Num() && PendingBooms[N].At <= RenderT) Out.Add(PendingBooms[N++]);
	PendingBooms.RemoveAt(0, N);
}

void UTollFeedSubsystem::PumpStream()
{
	TArray<uint8> Bytes;
	bool bClosed;
	{
		FScopeLock Lock(&StreamLock);
		Bytes = MoveTemp(StreamBytes);
		StreamBytes.Reset();
		bClosed = bStreamClosed;
	}
	if (Bytes.Num() > 0)
	{
		const FUTF8ToTCHAR Conv(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
		StreamText.Append(Conv.Get(), Conv.Length());
		StreamText.ReplaceInline(TEXT("\r\n"), TEXT("\n"));
	}

	// SSE: events are separated by a blank line; fields are "name: value"
	int32 End;
	while ((End = StreamText.Find(TEXT("\n\n"))) != INDEX_NONE)
	{
		const FString Block = StreamText.Left(End);
		StreamText.RightChopInline(End + 2);
		FString Event = TEXT("message");
		FString Data;
		TArray<FString> Lines;
		Block.ParseIntoArrayLines(Lines, false);
		for (const FString& Line : Lines)
		{
			FString Name, Value;
			if (!Line.Split(TEXT(":"), &Name, &Value)) continue;
			Value.RemoveFromStart(TEXT(" "));
			if (Name == TEXT("event")) Event = Value;
			else if (Name == TEXT("data")) Data += Data.IsEmpty() ? Value : TEXT("\n") + Value;
		}
		if (!Data.IsEmpty()) HandleEvent(Event, Data);
		if (!Request.IsValid()) return; // the event failed the stream
	}

	if (bClosed)
	{
		UE_LOG(LogTollFeed, Log, TEXT("stream closed"));
		FailLive();
	}
}

void UTollFeedSubsystem::HandleEvent(const FString& Event, const FString& Data)
{
	TSharedPtr<FJsonObject> Obj;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Data);
	if (!FJsonSerializer::Deserialize(Reader, Obj) || !Obj.IsValid())
	{
		if (Event == TEXT("init")) FailLive();
		return; // ignore a bad frame
	}
	if (Event == TEXT("init"))
	{
		bool bLive = false;
		if (!Obj->TryGetBoolField(TEXT("live"), bLive) || !bLive)
		{
			FailLive();
			return;
		}
		bGotInit = true;
		SetMode(ETollFeedMode::Live);
		TArray<TollTraffic::FSpawn> Registry;
		const TArray<TSharedPtr<FJsonValue>>* Rows;
		if (Obj->TryGetArrayField(TEXT("registry"), Rows)) TollTraffic::ParseSpawn(*Rows, Registry);
		TollTraffic::FFrame Frame;
		const TSharedPtr<FJsonObject>* FrameObj;
		const bool bHasFrame = Obj->TryGetObjectField(TEXT("frame"), FrameObj) && TollTraffic::ParseFrame(*FrameObj, Frame);
		OnInit(true, Registry, bHasFrame ? &Frame : nullptr);
	}
	else if (Event == TEXT("frame") && Mode == ETollFeedMode::Live)
	{
		TollTraffic::FFrame Frame;
		if (TollTraffic::ParseFrame(Obj, Frame)) Ingest(Frame, false);
	}
}

void UTollFeedSubsystem::Tick(float DeltaTime)
{
	const double Now = FPlatformTime::Seconds();

	if (Request.IsValid())
	{
		PumpStream();
		if (Request.IsValid() && !bGotInit && Now > LiveDeadline) FailLive();
	}
	else if (Mode == ETollFeedMode::Replay && ProbeAt > 0 && Now >= ProbeAt)
	{
		// a probe is just another attempt; the replay keeps playing until an init says live
		ProbeAt = 0;
		TryLive();
	}

	if (Mode == ETollFeedMode::Replay && ReplayFrames.Num() > 0)
	{
		ReplayWait -= DeltaTime;
		while (ReplayWait <= 0)
		{
			if (ReplayNext >= ReplayFrames.Num())
			{
				StartReplay(); // loop
				break;
			}
			Ingest(ReplayFrames[ReplayNext++], false);
			ReplayWait += ReplayDt;
		}
	}

	// advance render time, easing toward newest - LAG
	const double Dt = FMath::Min(0.1, static_cast<double>(DeltaTime));
	if (NewestT != 0)
	{
		const double Target = NewestT - TollTraffic::Lag;
		const double Err = Target - RenderT;
		if (FMath::Abs(Err) > 8) RenderT = Target;
		else RenderT += Dt * (1 + FMath::Clamp(Err * 0.4, -0.5, 0.5));
	}

	TollTraffic::CollectVisible(Tracks, Classes, RenderT, Visible);
}

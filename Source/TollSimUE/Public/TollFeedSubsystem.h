// One source of traffic over the two the browser uses (viewer/src/feed.js):
// the live SSE stream from the back office, and the bundled replay recording
// whenever the stream is unavailable. SUMO stays authoritative; this only
// mirrors what it publishes.
//
// Command line: -TollReplay forces the replay, -TollFeed=<url> picks another stream.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"
#include "Interfaces/IHttpRequest.h"
#include "TollTraffic.h"
#include "TollFeedSubsystem.generated.h"

UENUM(BlueprintType)
enum class ETollFeedMode : uint8
{
	None,
	Live,
	Replay,
	Offline
};

UCLASS()
class TOLLSIMUE_API UTollFeedSubsystem : public UGameInstanceSubsystem, public FTickableGameObject
{
	GENERATED_BODY()

public:
	static const TCHAR* DefaultStreamUrl;

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// FTickableGameObject
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickable() const override { return bStarted; }
	virtual bool IsTickableInEditor() const override { return false; }
	virtual ETickableTickType GetTickableTickType() const override { return ETickableTickType::Conditional; }

	/** Begin streaming (or replaying). Safe to call more than once. */
	void Start();

	UFUNCTION(BlueprintPure, Category = "Toll")
	ETollFeedMode GetMode() const { return Mode; }

	UFUNCTION(BlueprintPure, Category = "Toll")
	double GetRenderTime() const { return RenderT; }

	/** Vehicles to draw this frame, in sim metres (centres). */
	const TArray<TollTraffic::FVisible>& GetVisible() const { return Visible; }

	/** Newest frame's extra fields (booms, lanes_open, stats, events). */
	TSharedPtr<FJsonObject> GetLatestExtra() const { return LatestExtra; }

	/** A boom raised or lowered by the sim, [plaza, dir, idx, up] on the wire. */
	struct FBoomEvent
	{
		double At = 0; // render time it takes effect (viewer.js: frame t - 0.6)
		FString Plaza;
		FString Key; // dir + idx, e.g. S0
		bool bUp = false;
	};

	/** Boom events due by RenderT, oldest first; removes them. */
	void TakeDueBooms(TArray<FBoomEvent>& Out);

	/** Load and decode Content/Data/replay.json. Exposed for tests. */
	static bool LoadReplay(double& OutDt, TArray<TollTraffic::FFrame>& OutFrames);

private:
	void TryLive();
	void FailLive();
	void StartReplay();
	void OnInit(bool bLive, const TArray<TollTraffic::FSpawn>& Registry, const TollTraffic::FFrame* Frame);
	void Ingest(const TollTraffic::FFrame& Frame, bool bFirst);
	void ResetState();
	void PumpStream();
	void HandleEvent(const FString& Event, const FString& Data);
	void SetMode(ETollFeedMode NewMode);

	bool bStarted = false;
	ETollFeedMode Mode = ETollFeedMode::None;
	FString StreamUrl;

	// interpolation state (viewer.js resetState / ingest / frame)
	TMap<int32, TollTraffic::FTrack> Tracks;
	TMap<int32, FString> Classes;
	double NewestT = 0;
	double RenderT = 0;
	TArray<TollTraffic::FVisible> Visible;
	TSharedPtr<FJsonObject> LatestExtra;
	TArray<FBoomEvent> PendingBooms;

	// replay
	double ReplayDt = 0.5;
	TArray<TollTraffic::FFrame> ReplayFrames;
	int32 ReplayNext = 0;
	double ReplayWait = 0;

	// live stream; bytes arrive on the HTTP thread
	TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> Request;
	FCriticalSection StreamLock;
	TArray<uint8> StreamBytes;
	bool bStreamClosed = false;
	FString StreamText;
	bool bGotInit = false;
	double LiveDeadline = 0; // wall seconds; no init by then means no live feed
	double ProbeAt = 0;      // wall seconds; next attempt at the live feed while replaying
};

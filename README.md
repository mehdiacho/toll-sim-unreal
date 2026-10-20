# toll-sim-unreal

The Unreal Engine 5 client for [toll-sim](https://github.com/mehdiacho/toll-sim):
the same SUMO simulation of the A1 toll plazas, drawn as a world rather than a map.
SUMO stays authoritative. This client reads the same live SSE feed as the browser
viewer, falls back to the bundled replay when the feed is down, and places every
vehicle exactly where the browser does.

Plan and milestones: `docs/v2-unreal-plan.md` in the toll-sim repo.

## Layout

- `Source/TollSimUE/`
  - `TollTraffic`: a port of the browser's `traffic.js` and `vehicle-geometry.js` (decoding, interpolation, front-bumper to centre).
  - `UTollFeedSubsystem`: the live stream with replay fallback, and the 2.4 s render clock.
  - `ATollTrafficActor`: the scene.
- `Content/Data/`: `replay.json` and `road.json` from toll-sim, plus `golden-positions.json`, the browser's own output for the replay. All three are refreshed by `Scripts/sync-data.sh`.
- `Content/ThirdParty/`: not in git. See `assets-manifest.json` and `ATTRIBUTION.md`.

## Build and test (no editor needed)

```
X:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat TollSimUEEditor Win64 Development -Project=%CD%\TollSimUE.uproject -WaitMutex
X:\Epic\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe %CD%\TollSimUE.uproject -ExecCmds="Automation RunTests TollSim;Quit" -unattended -nullrhi -nosplash -log
```

`TollSim.Traffic.GoldenPositions` replays the recording through the C++ port and
compares every vehicle against the browser's positions; the limit is 0.1 m.

## Run

```
UnrealEditor.exe TollSimUE.uproject /Engine/Maps/Entry -game [-TollReplay] [-TollFeed=<url>] [-TollCam=plaza:A1-G01]
```

`-TollShot=<file.png> -TollShotDelay=<s>` saves a still and quits, which works with `-RenderOffscreen`.

## Backup

`Scripts/backup-nas.sh` writes a git bundle and the third-party content to the NAS.

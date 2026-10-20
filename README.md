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
  - `TollPlaza`: plaza layouts and the lane remap that eases vehicles onto the drawn booth lanes (from `plaza-layout.js` and `viewer.js`).
  - `ATollPlazaActor`: one plaza, built from the browser's exported geometry, with moving booms and booth lamps.
  - `ATollTrafficActor`: the scene: road, markings, vehicles, sky, camera.
- `Content/Data/`: `replay.json` and `road.json` from toll-sim, `golden-positions.json` (the browser's own output for the replay), and `Plazas/` (the browser's plaza meshes and textures, written by toll-sim's `viewer/scripts/export-plazas.mjs`). All refreshed by `Scripts/sync-data.sh`.
- `Content/Toll/Materials/`: three plain materials (vertex colour, textured, flat colour), made by `Scripts/make_materials.py`.
- `Content/ThirdParty/`: not in git. See `assets-manifest.json` and `ATTRIBUTION.md`.

## Build and test (no editor needed)

```
X:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat TollSimUEEditor Win64 Development -Project=%CD%\TollSimUE.uproject -WaitMutex
X:\Epic\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe %CD%\TollSimUE.uproject -ExecCmds="Automation RunTests TollSim;Quit" -unattended -nullrhi -nosplash -log
```

`TollSim.Traffic.GoldenPositions` replays the recording through the C++ port and
compares every vehicle against the browser's positions, before and after the plaza
lane remap; the limit is 0.1 m.

If the Unreal editor is open, add `-NoHotReloadFromIDE` to the build so its Live
Coding guard doesn't block it (fine as long as this project isn't loaded there).

## Run

```
UnrealEditor.exe TollSimUE.uproject /Engine/Maps/Entry -game [-TollReplay] [-TollFeed=<url>] [-TollCam=plaza:A1-G01] [-TollCamEye=back,side,height]
```

`-TollCamEye` places the camera in metres from the plaza centre: back along the
northbound direction, to its right, and up (default `110,22,30`).

`-TollTime=<hour>` sets local solar time (default 14; dusk ~17.9, night ~20.5) and
`-TollEV=<ev>` the night exposure (default 0.5).

`-TollShot=<file.png> -TollShotDelay=<s>` saves a still and quits, which works with `-RenderOffscreen`.

## Backup

`Scripts/backup-nas.sh` writes a git bundle and the third-party content to the NAS.

# Handoff: toll-sim-unreal

Start here if you are picking this project up cold. Keep this file current: update it in the
same commit as any milestone or working step (at least every 30 minutes of work).

Last updated 2026-10-22.

## Goal and the choices already made

[toll-sim](https://github.com/mehdiacho/toll-sim) (private) simulates an open-road toll back
office for Botswana's A1, from Gaborone to Mahalapye. SUMO drives the traffic, which passes
three mainline plazas: Mmamashia, Artesia and Dibete. Its public face is a three.js viewer at
https://mehdiacho.tech/projects/toll-sim/, fed live from the NAS.

This repo is **v2**, an Unreal Engine 5 rendering of the same corridor. Mehdi's choices:

- **Unreal**, not Unity (decided 2026-10-20).
- **SUMO stays authoritative.** Unreal reads the same live SSE feed the browser does and falls
  back to the bundled replay. It never runs its own traffic, physics bodies or collisions.
  Motion is kinematic: wheel spin, suspension bob, brake pitch.
- **The web 3D view stays the public link.** v2 is shown as rendered clips on the project page,
  plus a Windows download hosted on Cloudflare R2.
- **The look:** "like an actual world", meaning real textures, vegetation and realistic
  vehicles, all from free Fab, Megascans or CC content.
- **No Kenney or low-poly cars.** Use City Sample Vehicles from Fab.
- **Third-party content is never committed.** It is credited in `ATTRIBUTION.md` and recorded
  in `assets-manifest.json`.

## Milestones

| | Outcome | Proof | State |
|---|---|---|---|
| M0 | C++ project, repo, LFS, NAS backup | compiles headless | **done** (c1edec0) |
| M1 | Feed parity: SSE plus replay; vehicles at the browser's positions | GoldenPositions test, under 0.1 m | **done** (c1edec0, e046feb) |
| M2 | Road, markings, the three plazas from the browser's geometry, DEM terrain | stills `docs/stills/m2-*` | **done** (7877196, b859529) |
| M3 | Vehicles and light: real models, wheel spin, lamps, day, dusk and night | **30 s MRQ dusk clip** | **done**: vehicles, primitive motorbikes, lamps, wheel spin, time of day; 30 s dusk clip `docs/clips/m3-dusk-30s.mp4` (fixed-step frames + ffmpeg, not MRQ) |
| M4 | Look: grass, acacia and mopane, ground materials, post | hero stills at each plaza; under 6 GB VRAM | **in progress**: foliage scatter (b9976b5) and Kalahari ground (stills `m4-ground-*`) done; post, horizon band and VRAM check not started |
| M5 | Ship: clips on the project page, packaged build on R2, ATTRIBUTION complete | download runs from a clean folder and connects live | not started |

## Where everything lives

- **Repo:** `X:\Projects\toll-sim-unreal`, private `mehdiacho/toll-sim-unreal`, branch `main`.
  There is no CI; commit straight to main. LFS holds `*.uasset`, `*.umap`, `*.png`, `*.bin` and
  similar files.
- **The sim:** `X:\Projects\toll-sim`, private `mehdiacho/toll-sim`. It has CI, which must be
  green before a push. See its `docs/HANDOFF.md` for hosting. Its `docs/v2-unreal-plan.md` is
  the original plan; the relevant parts are copied below.
- **Engine:** Unreal Engine **5.8.3** at `X:\Epic\UE_5.8`. It is a custom path, so nothing finds
  it through the registry: call the binaries by full path.
- **Compiler:** Visual Studio Community 2026 (18.11) with the game C++ workload, plus Build
  Tools 2022; Windows SDK 10.0.26100.
- **PC:** i7-12700H, 16 GB RAM, RTX 3060 Laptop with **6 GB VRAM**, the tight spot.
- **Asset packs (git-ignored).** All came from Mehdi's Fab library. The launcher caches
  downloads in `C:\Users\mehdi\AppData\Local\com.epicgames.eglv2\FabLibrary\<name>-<id>\`.

  | Pack | In the project | How it got there |
  |---|---|---|
  | City Sample Vehicles (Epic) | `Content/CitySampleVehicles/` (7.5 GB) | copied from the FabLibrary cache |
  | Namaqualand Environment (Poly Haven) | `Content/Namaqualand/` (4.7 GB) | copied from the FabLibrary cache |
  | Megascans grass, shrubs, surfaces; Acacia Tree (Incg5764) | `Content/ThirdParty/` (~280 MB) | `Scripts/import_assets.py` from the FabLibrary cache |
  | Megaplants Greasewood | `Content/Megaplant_Library/` | added by Mehdi in the editor; **not used yet** |
  | Free Shrubs Pack (Ultra Realistic Wind) | `Content/GV_FreeShrubsPack/` (1.4 GB) | copied from the FabLibrary cache; used as thorn bush |

  `assets-manifest.json` gives the exact cache folder for each pack.
- **DEM:** Copernicus GLO-30 tiles are in `Incoming/dem/` (git-ignored). Their output,
  `Content/Data/terrain.bin`, is committed.
- **NAS backup:** `Scripts/backup-nas.sh` writes a git bundle and a tar of
  `Content/ThirdParty` to `nassync:/volume4/v4/toll-sim-unreal/` (ssh host `nassync`). City
  Sample and Namaqualand aren't in that tar: re-copy them from the FabLibrary cache, or re-add
  them from fab.com.
- **Worklog:** `X:\ai.tools\worklog\projects\toll-sim.md` holds dated decisions, newest first.

## Commands

Run these from Git Bash. Prefix with `MSYS_NO_PATHCONV=1` so paths aren't rewritten. Use
forward slashes in arguments.

```sh
UE=/x/Epic/UE_5.8/Engine
P="X:/Projects/toll-sim-unreal/TollSimUE.uproject"

# build (about 20 s incremental). This fails if an editor or commandlet has the module loaded.
"$UE/Build/BatchFiles/Build.bat" TollSimUEEditor Win64 Development -Project="$P" -WaitMutex -NoHotReloadFromIDE

# tests: TollSim.Traffic.Geometry and TollSim.Traffic.GoldenPositions
MSYS_NO_PATHCONV=1 "$UE/Binaries/Win64/UnrealEditor-Cmd.exe" "$P" -ExecCmds="Automation RunTests TollSim;Quit" -unattended -nullrhi -nosplash -log

# import the non-Unreal assets: Megascans FBX and textures, the acacia FBX. This rebuilds
# M_Foliage, M_Surface and M_Ground and reuses meshes and textures already imported (~5 min).
MSYS_NO_PATHCONV=1 "$UE/Binaries/Win64/UnrealEditor-Cmd.exe" "$P" -run=pythonscript -script="X:/Projects/toll-sim-unreal/Scripts/import_assets.py" -unattended -nosplash

# a still, rendered offscreen. Run the first one after a material change twice: shaders compile on the first.
MSYS_NO_PATHCONV=1 "$UE/Binaries/Win64/UnrealEditor.exe" "$P" -game -RenderOffscreen -ResX=1600 -ResY=900 \
  -TollReplay -TollCam=plaza:A1-G01 -TollCamEye=150,45,3 -TollTime=15 -TollShot="C:/tmp/x.png" -TollShotDelay=30

# refresh feed data from a toll-sim checkout; terrain from DEM tiles
Scripts/sync-data.sh ../toll-sim
python Scripts/make_terrain.py Incoming/dem
```

Run options:

- `-TollReplay` uses the bundled replay; without it, the client tries the live feed first.
- `-TollFeed=<url>` sets the feed URL.
- `-TollCam=plaza:A1-G01|A1-G02|A1-G03` picks the plaza.
- `-TollCamEye=back,side,height` takes metres from the plaza. Good views:
  - `40,14,6` close
  - `110,22,30` default
  - `150,45,3` low roadside
- `-TollCamFollow=<class>` (M, 1, 2, 3 or 4) chases the vehicle of that class nearest the plaza.
  `-TollCamEye` is then relative to the vehicle (default `9,3,2.2`); `0.5,-4.5,1.3` is side-on.
- `-TollTime=<solar hour>` sets the time: 14 is day, **about 17.9 is dusk** (18.8 is already night), 20.5 is night.
- `-TollGroundTint=r,g,b` overrides the ground tint (default 0.5,0.47,0.44, muted bushveld soil) for tuning.
- `-TollEV=<ev>` sets night exposure.

Stills that prove a step go in `docs/stills/` (LFS).

## Code map

- `Source/TollSimUE/`
  - `TollTraffic`: port of the browser's `traffic.js` and `vehicle-geometry.js`.
  - `UTollFeedSubsystem`: SSE with replay fallback, 2.4 s render lag.
  - `TollPlaza`: lane remap.
  - `ATollPlazaActor`: plaza meshes from `Content/Data/Plazas`, booms, lamps.
  - `ATollTrafficActor`: everything else, which includes:
    - terrain from `terrain.bin`, plus the road and markings;
    - sky and time of day;
    - camera;
    - vehicles: `LoadModels`, ISMs per City Sample part;
    - wheel spin about each hub;
    - per-instance custom data: f16 paint fraction plus lamp bits;
    - emissive lamp plates;
    - foliage: `LoadFoliage`, HISMs scattered in discs around each plaza and kept clear of the roads.
- `Scripts/`: `import_assets.py`, `make_materials.py` (`M_VertexColor`, `M_Textured`, `M_Color`),
  `make_terrain.py`, `sync-data.sh`, `backup-nas.sh`.
- `Content/Toll/Materials/`: committed `M_VertexColor`, `M_Textured` and `M_Color`.
  `M_Foliage`, `M_Surface` and `M_Ground` are generated by `import_assets.py` and git-ignored,
  because their default textures point into the ignored packs.

## In flight and the next 3 steps

1. ~~Free Shrubs Pack seller~~ done 2026-10-22: Greenleaf Vision (Fab listing 7ca465ab, which
   matches the cache folder). The listing page doesn't spell out its licence; free Fab listings
   default to the Fab Standard Licence.
2. ~~Motorbikes~~ done 2026-10-22: a bike and rider built from the engine's cube, cylinder and
   sphere (`BikeParts` in `TollTrafficActor.cpp`, one ISM per part), with a head lamp and a tail
   lamp (stills `m3-motorbike-*`). No pack has a bike. A real CC0 or Fab bike mesh would be
   better, but downloading one needs Mehdi's OK. Next: more clips (day, night, and a close chase
   with `-TollCamFollow`).
3. **M4 finish.**
   - A horizon band: the sky-atmosphere ground shows as a dark line at the horizon.
   - Grass cull and density tuning.
   - Hero stills at each plaza.
   - A VRAM check under 6 GB at High.
   - Optionally, Greasewood and Namaqualand `searsia` as mopane stand-ins; mopane itself isn't
     in any pack.

After that comes M5:

- Packaging (`RunUAT BuildCookRun`, Win64 Shipping). Assets referenced only by path need
  `DirectoriesToAlwaysCook` for `/Game/CitySampleVehicles`, `/Game/Namaqualand` and
  `/Game/ThirdParty`.
- An R2 upload.
- Clips on the project page.
- An in-game credits screen for CC BY.

## Open asks to Mehdi

- **Don't open TollSimUE in the editor while a headless import or render is running.** Ask
  first, or check with whoever is working on it.
- **Merges** of toll-sim PRs and the portfolio need his explicit OK. **Downloads** need his OK,
  given in his own words. **Cloudflare DNS and tunnel changes** are his to make.
- Decide whether the Megaplants Greasewood he added should be used.

## Gotchas

- **The editor must not have TollSimUE open during headless imports or renders.** Both write
  the same assets; the build also fails while the module is loaded. Mehdi often has the
  launcher's UnrealEditor open with no project loaded, which is fine with `-NoHotReloadFromIDE`.
- **Material usage flags.** A material used on an ISM or HISM, or with Nanite, needs that
  usage flag set, or `-game` silently renders the default grey material.
  - `M_Color` needed `used_with_instanced_static_meshes`; that's why the lamps were invisible.
  - The Megascans and acacia materials need ISM and Nanite.
- **VertexColor has no "RGB" output pin** in the Python material API. Link its default output
  (`""`), or the multiply gets 0 and the ground renders black with only sky reflection (it read as grey).
- **Texture parameters need a default texture of their sampler type.** Normal maps need a
  normal default and masks a mask default; otherwise the material fails to compile and renders
  the default material. `import_assets.py` uses the Gravel Ground textures as defaults.
- **Foliage.** Some Megascans grass variations are flat litter (height under 0.4 × footprint)
  and are skipped. Scaling them to a plant's height turns them into ribbons. The Namaqualand
  `rocks_01` and `stones_01` sets are flat ground patches; only `boulder_0*` is used.
- **Plaza apron and marks are drawn double-sided.** Some of their quads face down after the
  y flip, so the flared apron used to vanish and heavy lanes appeared to run over sand.
  `-TollLogOffRoad` logs any vehicle drawn more than 4 m from a road lane, for checking this
  kind of thing (inside a plaza flare that's expected).
- **Clips:** `-TollClip=<dir> [-TollClipSeconds=30 -TollClipFPS=30 -TollClipWarmup=20]` writes
  a fixed-step PNG sequence and quits; then use ffmpeg. It's used instead of MRQ, because the
  scene is built at runtime in `-game`.
- **HISMs under the actor are Movable,** because a Static component can't attach to the
  movable root.
- **City Sample per-instance data.**
  - `PerInstanceCustomData[0]` is a uint: the low 16 bits are the f16 paint fraction; the bits
    above are lamps (front run 0–1, rear 2–3, brake 4–5, turn 6–7, head 8–9).
  - The material lamps don't visibly light, so emissive plates are drawn as well.
  - Meshes whose names contain `No_Wheel` are bodies, not wheels.
- **Fab "Add to project"** only lists projects that are open in an editor, and City Sample only
  offers UE 5.0–5.3. That is why the packs were copied from the FabLibrary cache instead.
  "No Unreal Engine projects found" in the launcher is normal for this custom-path engine.
- **Git Bash** rewrites `/Engine/...`-style arguments, so use `MSYS_NO_PATHCONV=1`.
- **LFS hooks:** the global `hooksPath` stops `git lfs install`, so the LFS hooks were written
  into `.git/hooks` by hand.
- **Coordinates.** `TollSpace::ToWorld(Origin, x, y, z)`: sim metres become Unreal cm, with Y
  flipped. The feed position is the front bumper.
- `Config/DefaultEngine.ini` has editor-written changes that weren't made on purpose; leave
  them uncommitted.

## Conventions

- Commit as Mehdi (`mehdiacho <mehdiacho@gmail.com>`). No `Co-Authored-By`, AI trailers or
  "Generated with" lines, and no model names in commits or code.
- Every third-party asset goes in `ATTRIBUTION.md` and `assets-manifest.json` before it's used.
  Check its licence on its own page.
- Don't drive Mehdi's screen; offscreen rendering is fine. Don't install software without asking.
- Never write secrets into the repo, docs or logs.

## From the original plan (toll-sim `docs/v2-unreal-plan.md`)

- **Feed contract.**
  - `GET https://mehdiacho.tech/projects/toll-sim/live/live/road.json` returns lanes, places,
    gantries and `plazas[]`.
  - `GET .../live/live/stream` is SSE:
    - `event: init` carries the snapshot: registry, `booms`, `lanes_open`.
    - `event: frame` carries vehicles `[id, x·10, y·10, heading°, speed·10]`, `stats.plazas`,
      `booms` and events.
  - Rendering runs 2.4 s behind the newest frame and interpolates.
  - Sim metres (X east, Y north, heading clockwise from +Y) map to UE cm as
    `X·100, −Y·100`, with yaw = heading − 90°.
  - Vehicles are placed at their centre: half the class length behind the bumper. Class
    lengths are 2.2, 4.6, 7.5, 12 and 18.5 m.
- **6 GB VRAM.** Use Lumen software RT, Nanite, Virtual Shadow Maps, High (not Epic)
  scalability and aggressive foliage culls. Clips render offline through MRQ, so they can use
  higher settings.
- **Corridor detail.** Full detail covers about 3 km around the road near the three plazas;
  the rest uses a flat fill. The full 150 km at full detail isn't realistic.
- **Clips.** Render with MRQ against the replay. Deferred render, warm-up frames for Lumen and
  AA samples; a PNG sequence, then ffmpeg to H.264 and VP9.
- **Package.**
  - Build with `RunUAT.bat BuildCookRun -platform=Win64 -clientconfig=Shipping -build -cook
    -stage -pak -archive`.
  - Host on a public R2 bucket, not GitHub releases, because the repo is private.
  - State the minimum spec: a GTX 1660 or RTX 2060-class GPU, Windows 10 or 11.
- **Licences.** Fab Standard allows cooked use in a distributed build but not redistributing
  source assets. CC BY needs on-screen credit.

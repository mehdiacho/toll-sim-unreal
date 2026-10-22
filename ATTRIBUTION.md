# Attribution

Third-party content in this project, and the licence each comes under. Only
licence-safe sources are used: Fab (free and Megascans), Poly Haven,
Quaternius (CC0) and Kenney (CC0). None of it is committed: the packs come from
the Fab library and `Scripts/import_assets.py` brings in the ones that are not
Unreal-format. Every entry here has a matching row in
`assets-manifest.json`.

| Asset | Source | Author | Licence | Used for |
|---|---|---|---|---|
| City Sample Vehicles | Fab (free) | Epic Games | Fab Standard Licence, Unreal Engine only | Cars, vans, trucks, bus, truck and trailer (`Content/CitySampleVehicles/`) |
| Namaqualand Environment and Asset Pack | Fab (free) | Poly Haven | CC BY 4.0 (as listed on Fab) | Searsia bushes (mopane stand-in), shrubs, boulders and rocks (`Content/Namaqualand/`) |
| Acacia Tree | Fab (free) | Incg5764 | CC BY 4.0 | Acacia trees (`Content/ThirdParty/AcaciaTree/`) |
| Free Shrubs Pack (Ultra Realistic Wind) | Fab (free) | Greenleaf Vision | Fab Standard Licence | Thorn bush (`Content/GV_FreeShrubsPack/`) |
| Dry Grass, Thatching Grass, Dead Shrubs | Fab (free Quixel Megascans) | Quixel | Fab Standard Licence | Grass and low shrubs (`Content/ThirdParty/Megascans/`) |
| Dry Cracked Mud, Gravel Ground, Rippled Sand, Road Asphalt | Fab (free Quixel Megascans) | Quixel | Fab Standard Licence | Ground, shoulder and asphalt surfaces (`Content/ThirdParty/Megascans/`) |

The packaged game shows these credits on screen for its first 12 seconds and whenever C is
pressed (`Source/TollSimUE/Private/TollHUD.cpp`; keep that list in step with this file).

Traffic data, the road network and the toll plaza layout come from the
`toll-sim` project (SUMO, OpenStreetMap data © OpenStreetMap contributors, ODbL).

The terrain (`Content/Data/terrain.bin`, made by `Scripts/make_terrain.py`) is
derived from the Copernicus DEM GLO-30 tiles S25E025, S25E026, S24E026 and
S23E026: © DLR e.V. 2010–2014 and © Airbus Defence and Space GmbH 2014–2018,
provided under COPERNICUS by the European Union and ESA; all rights reserved.
Contains modified Copernicus data.

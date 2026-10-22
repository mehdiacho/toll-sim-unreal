"""Build /Game/Toll/DA_CookList, which makes the cook take only the pack meshes the scene uses.

The scene finds its meshes by folder at runtime, so nothing references them and the cook would
otherwise need whole pack folders (DirectoriesToAlwaysCook), most of which is never drawn.
Scripts/cook-list.txt comes from a run with -TollListAssets (see HANDOFF); refresh it whenever
LoadModels or LoadFoliage changes what they pick. Materials and textures follow as dependencies.
It's a UTollCookList with hard references: a PrimaryAssetLabel saved from Python had no bundle
data, so the cook ignored it.

  UnrealEditor-Cmd.exe <uproject> -run=pythonscript -script=<this file> -unattended -nosplash
"""
import os
import unreal

PACKS = ("/Game/CitySampleVehicles/", "/Game/Namaqualand/", "/Game/GV_FreeShrubsPack/")
LIST = os.path.join(os.path.dirname(os.path.abspath(__file__)), "cook-list.txt")

paths = [l.strip() for l in open(LIST, encoding="utf-8") if l.strip().startswith(PACKS)]
assets = []
for p in paths:
    a = unreal.load_asset(p)
    if a is None:
        unreal.log_warning(f"cook list: {p} is missing (pack not installed?)")
    else:
        assets.append(a)

name, folder = "DA_CookList", "/Game/Toll"
cook_list = unreal.load_asset(f"{folder}/{name}")
if cook_list is None:
    factory = unreal.DataAssetFactory()
    factory.set_editor_property("data_asset_class", unreal.TollCookList)
    cook_list = unreal.AssetToolsHelpers.get_asset_tools().create_asset(name, folder, unreal.TollCookList, factory)
cook_list.set_editor_property("assets", assets)
unreal.EditorAssetLibrary.save_loaded_asset(cook_list, False)
unreal.log(f"cook list: {len(assets)} of {len(paths)} pack meshes in {folder}/{name}")

"""Halve the City Sample vehicle textures, the bulk of the package (2.45 of 3.6 GB at full size).

Plain textures over 2K get MaxTextureSize 2048. Most are virtual textures, many of them UDIM grids of
2K tiles (up to 28672x102400); MaxTextureSize doesn't mean "2K" for those, so they get LOD bias 1
instead, which halves each tile. This edits the pack's assets in place (the pack isn't committed),
so the editor and clips see it too. Set CAP = False and re-run to restore the originals.

  UnrealEditor-Cmd.exe <uproject> -run=pythonscript -script=<this file> -unattended -nosplash
"""
import unreal

CAP = True
ROOT = "/Game/CitySampleVehicles"

reg = unreal.AssetRegistryHelpers.get_asset_registry()
changed = 0
for a in reg.get_assets_by_path(ROOT, recursive=True):
    if str(a.asset_class_path.asset_name) != "Texture2D":
        continue
    dims = str(a.get_tag_value("Dimensions") or "0x0").split("x")
    if max(int(dims[0]), int(dims[1])) <= 2048:
        continue
    t = unreal.load_asset(str(a.package_name))
    vt = t.get_editor_property("virtual_texture_streaming")
    want_max, want_bias = (0, 1) if vt else (2048, 0)
    if not CAP:
        want_max, want_bias = 0, 0
    if t.get_editor_property("max_texture_size") == want_max and t.get_editor_property("lod_bias") == want_bias:
        continue
    t.set_editor_property("max_texture_size", want_max)
    t.set_editor_property("lod_bias", want_bias)
    unreal.EditorAssetLibrary.save_loaded_asset(t, False)
    changed += 1
unreal.log(f"vehicle textures: {changed} changed ({'capped' if CAP else 'restored'})")

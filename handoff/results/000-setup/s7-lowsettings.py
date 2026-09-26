#!/usr/bin/env python3
"""s7-lowsettings.py — LAPTOP: 720p / low / 30 fps preset for Rocket League.

Edits only [SystemSettings] of the prefix's TASystemSettings.ini (keeps CRLF,
writes a .bak once). Run while the game is NOT running (it rewrites the file on
exit). Brief: 720p, textures low, effects off, 30 fps cap. Some video prefs are
also stored in the profile save and may be re-applied by the game.
"""
import pathlib
import re
import sys

INI = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else
                   pathlib.Path.home() / "rltvos/assets/prefix/drive_c/users/user/Documents/My Games/Rocket League/TAGame/Config/TASystemSettings.ini")
SET = {
    "ResX": "1280", "ResY": "720", "Fullscreen": "True", "Borderless": "False", "UseVsync": "False",
    "CustomFPS": "30", "UncappedFramerate": "False", "ScreenPercentage": "100.000000",
    "DetailMode": "0", "MaxAnisotropy": "1", "MaxShadowResolution": "256", "MaxMultiSamples": "1",
    "SkeletalMeshLODBias": "1", "ParticleLODBias": "1",
    "bAllowDownsampledTranslucency": "False",
}
OFF = ["StaticDecals", "DynamicDecals", "UnbatchedDecals", "DynamicLights", "DynamicShadows",
       "LightEnvironmentShadows", "SHSecondaryLighting", "DirectionalLightmaps", "MotionBlur",
       "MotionBlurPause", "DepthOfField", "AmbientOcclusion", "Bloom", "bAllowLightShafts", "Distortion",
       "LensFlares", "FogVolumes", "bAllowHighQualityMaterials", "bUseTranslucentArenaShaders",
       "bAllowWholeSceneDominantShadows", "bAllowFracturedDamage", "FullEffectIntensity"]
KEEP_TEX = {"TEXTUREGROUP_UI", "TEXTUREGROUP_RenderTarget", "TEXTUREGROUP_ColorLookupTable", "TEXTUREGROUP_Cinematic"}
TEX_MAX = 256

raw = INI.read_bytes()
bak = INI.with_name(INI.name + ".bak-before-low")
if not bak.exists():
    bak.write_bytes(raw)
lines = raw.decode("utf-8", "replace").split("\r\n")
sec, changed = None, 0
for i, line in enumerate(lines):
    m = re.match(r"^\[(.+)\]$", line)
    if m:
        sec = m.group(1)
        continue
    if sec != "SystemSettings" or "=" not in line:
        continue
    k, v = line.split("=", 1)
    new = None
    if k in SET:
        new = SET[k]
    elif k in OFF:
        new = "False"
    elif k.startswith("TEXTUREGROUP_") and k not in KEEP_TEX:
        new = re.sub(r"MaxLODSize=(\d+)", lambda mm: f"MaxLODSize={min(int(mm.group(1)), TEX_MAX)}", v)
    if new is not None and new != v:
        lines[i] = f"{k}={new}"
        changed += 1
INI.write_bytes("\r\n".join(lines).encode("utf-8"))
print(f"{INI}: {changed} settings changed (backup {bak.name})")

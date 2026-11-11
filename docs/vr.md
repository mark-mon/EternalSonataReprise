# VR (OpenXR)

This branch (`vr` of [mark-mon/EternalSonataReprise](https://github.com/mark-mon/EternalSonataReprise)) adds VR
to Eternal Sonata: Reprise on Windows through OpenXR (SteamVR, Meta, WMR, ...). VR is off unless asked for, and
the rest of the game is unchanged.

## Playing

Releases ship `EternalSonataVR.exe`, which goes next to the normal `eternalsonata.exe` of the matching
Reprise version and shares its `rexruntime.dll`, game data, saves and settings. See
[packaging/vr/README.txt](../packaging/vr/README.txt) for the player's readme and
[packaging/vr/vr_config.txt](../packaging/vr/vr_config.txt) for the default settings.

- `vr_config.txt` next to the exe, or `ES_VR=stereo|mono|theater`, turns VR on. Without either the exe runs flat.
- Hold L3 + R3 (or F4 / Back) for the settings, which open on a VR tab in the headset.
- F8 cycles stereo / mono / theater, F9 recentres.
- VR forces the D3D12 renderer. `openxr_loader.dll` is loaded at run time; without it VR just stays off.

## Building

Same as upstream (needs the Xbox 360 PAL `default.xex`), from a recursive clone of this branch:

    git clone --recursive -b vr https://github.com/mark-mon/EternalSonataReprise

`ext/KryptonRecomp` points at [mark-mon/KryptonRecomp](https://github.com/mark-mon/KryptonRecomp) branch `vr`
(the shader translator changes below). The build copies no OpenXR binaries: put `openxr_loader.dll` (Khronos
OpenXR SDK, Apache-2.0) next to the exe. This build's in-game update check is off (`update_repo` cleared in
`src/core/eternalsonata_app.h`), because an upstream update would replace the exe and remove VR.

## How it works

- `src/vr/esvr_xr.*`: OpenXR on the renderer's D3D12 device and queue (`XR_KHR_D3D12_enable`). The headset
  loop runs on its own thread. Layers: a quad screen (title, menus, loading, theater), projection views
  (mono / stereo), a HUD quad and a settings-menu quad. Settings come from `vr_config.txt`; the VR tab edits them live.
- `src/vr/esvr_camera.cpp`: hooks the game's per-frame camera update (`sub_821078B0`, active camera at
  `0x82440618`). Once per game frame it takes one head pose and builds a clip-space matrix per eye,
  `M = P_eye * View_eye * Level * P_game^-1` (keeping the game projection's depth rows). It also decides
  whether a frame is 3D (field / battle with world-camera draws) or flat.
- Shaders (`ext/KryptonRecomp/xenos_hlsl.py`): every projecting vertex shader applies `xe_clip_matrix[xe_eye]`
  after the game's clip position. The renderer only fills it for world-camera draws, so UI, shadow maps and
  similar draws are untouched. Outputs that are copies of the clip position (screen-space lookups such as the
  water's refraction) get the same matrix.
- Stereo: the frame's D3D12 command list runs twice, once per eye. A forwarding command list records each
  resource's state at frame start, so the second run starts from the same states (`src/vr/esvr_plume.cpp`).
- HUD: at the world / UI boundary the world image is kept per eye and the UI target is cleared to transparent,
  so the UI goes to its own panel (full-screen dimming fills are left out of it).
- Settings menu: while VR runs, the SDK overlay draws into its own transparent target, which goes to the
  headset (only the part with content) and over the window.

## Dev switches

- `esvr_dump.now` next to the exe: dumps every layer to `esvr_dump_<n>.ppm` and logs one traced frame
  (`trace ...` lines in `esvr.log`: draws, resolves, water fetches).
- `esvr_fake_pose.txt` next to the exe, `yaw pitch x y z` (degrees, metres), read twice a second: a fake head
  movement for testing on a headset that doesn't move (e.g. the SteamVR null driver). `ES_VR_FAKE_YAW` sets
  a yaw at start.
- `ES_VR_CAMPROBE`, `ES_VR_CULL`: camera probe log, frustum-cull widening.

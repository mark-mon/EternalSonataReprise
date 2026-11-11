Eternal Sonata VR  -  v1.3.9-vr1
=================================
VR for Eternal Sonata: Reprise v1.3.9 (Windows, OpenXR: SteamVR, Oculus/Meta, WMR, ...).
Source: https://github.com/mark-mon/EternalSonataReprise (branch vr), based on
https://github.com/birabittoh/EternalSonataReprise

INSTALL
  1. Install Eternal Sonata: Reprise v1.3.9 as usual and check it runs.
  2. Unzip this into the Reprise folder, next to eternalsonata.exe and rexruntime.dll. Nothing of the
     normal game is replaced: the VR game is EternalSonataVR.exe, and it uses the same rexruntime.dll,
     game data, saves and settings.
  3. Start your OpenXR runtime (e.g. SteamVR), then EternalSonataVR.exe.
  eternalsonata.exe still starts the normal game.

UPDATES
  The VR exe never updates itself. It needs the rexruntime.dll of Reprise v1.3.9: if the normal game
  updates to a newer Reprise version, the VR exe may stop starting until a matching VR release is out.

CONTROLS
  Play as usual (controller, or keyboard + mouse).
  Hold L3 + R3 (both sticks pressed, about a third of a second): settings menu in the headset, opened on
    the VR tab. D-pad / left stick to move, A to change, B to close; L3 + R3 again returns to the game.
    F4 or the Back button open it too.
  F8: cycle stereo / mono / theater.   F9: recentre.

WHAT YOU GET
  - Field and battle in stereo 3D with head tracking. The camera is the game's; your head turns and
    moves around it. The horizon is kept level (keep_pitch = 1 keeps the game's tilt).
  - Title, main menu and loading screens on a screen in front of you.
  - HUD, dialogue and battle HUD on their own transparent panel in front of you (hud = 0 puts them back
    into the eyes; hud_follow = 1 makes the panel follow your head).
  - Water shows the scene behind it correctly from every head position ("Water refraction" in the VR
    tab turns that effect off).
  - VR runs on the D3D12 renderer (picked automatically).

VR TAB / vr_config.txt (next to the exe; "Save VR settings" in the VR tab writes it)
  mode              stereo | mono | theater | off
  resolution        percent of the headset's recommended per-eye size
  world_scale       game units per metre (bigger = the world feels smaller)
  keep_pitch        1 = keep the game camera's up / down tilt
  screen_width / screen_distance          theater and menu screen, metres
  hud, hud_distance, hud_width, hud_height, hud_follow     HUD panel, metres
  menu_distance, menu_size                settings menu panel (1 = 1 mm per pixel at 1 m)
  water_refraction  1 / 0
  A log is written to esvr.log next to the exe.

KNOWN
  - Frame rate is the game's own (30 by default; Reprise's frame_rate setting applies).
  - Windows only.

LICENCES
  Eternal Sonata: Reprise and this VR build: MIT (LICENSE-EternalSonataReprise.txt).
  OpenXR loader: Apache-2.0 (LICENSE-OpenXR-loader.txt).
  Eternal Sonata itself is not included: you need your own copy of the game, as for Reprise.

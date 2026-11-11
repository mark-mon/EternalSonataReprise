// Eternal Sonata VR: the clip-space transform the renderer applies to world camera draws (xe_clip_matrix).
//
// clip' = M * clip after the guest's own projection: M = P_view * R * T * P_game^-1 re-aims the game camera's
// image to the headset view (head rotation, eye offset, the headset's field of view) without touching any guest
// matrix, so every draw through the world camera follows, whatever constants its shader composes.
#pragma once

namespace esvr {

// VR mono / stereo is re-aiming the world camera (the guest frustum then stays at its own 16:9: no ultrawide clip
// scale).
bool ClipActive();
// VR 3D frame with the HUD on its own panel: the renderer keeps the world image apart and starts the UI layer
// transparent (native_renderer_frame.cpp CompositeWorldIntoLayer).
bool HudSeparate();
// VR with the water refraction turned off (vr_config water_refraction = 0): the renderer clears ps_058's b134.
bool WaterRefractionOff();
// Present thread, once per frame: field / battle (3D, re-aimed) or not (flat screen).
void UpdateSceneKind();
// The matrices for the draw being recorded, one per eye (rows; mono: both the same). Leaves `m` (identity) alone
// when there are none.
void ClipMatrix(float m[2][16]);
// The guest cull test's clip scale (FrameWorldClipScale result), widened while VR re-aims the camera.
void CullClipScale(float* x, float* y);

// Dev: true while recording the one frame after an XR dump request (esvr_dump.now): the renderer logs what
// touches the UI layer then. esvr.log line (printf style); esvr_xr.cpp on Windows.
bool TraceFrame();
void RequestTrace();
void Log(const char* format, ...);

}  // namespace esvr

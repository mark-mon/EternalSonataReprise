// Eternal Sonata VR: glue between the native renderer's Plume backend and the OpenXR module (esvr_xr.h).
//
// Stereo: the frame is recorded once (both eyes' clip matrices in every draw state, esvr_clip.h) into the frame's
// D3D12 command list, through a forwarding list that remembers each resource's state before the frame. At the
// present the list runs twice on the queue: XeEye = 0, the left eye's copy to OpenXR, barriers back to the
// recorded states, XeEye = 1, then the renderer's own submit (right eye) and the right eye's copy.
#pragma once

#include <cstdint>

namespace plume {
struct RenderDevice;
struct RenderCommandQueue;
struct RenderCommandList;
struct RenderBufferReference;
}  // namespace plume

namespace esvr {

// VR asked for at start (vr_config.txt / ES_VR). Windows only; false elsewhere.
bool VrRequested();
// D3D12 backend: start OpenXR on the game's device and direct queue (no-op when VR is off).
void InitFromPlume(plume::RenderDevice* device, plume::RenderCommandQueue* queue);
// Present thread, before the window's size is published as the render extent: while VR re-aims the camera the
// world is rendered at the headset's eye size instead (percent from vr_config.txt resolution). False otherwise.
bool RenderExtent(uint32_t* width, uint32_t* height);
// The frame's command list was just begun: the list the renderer records into (stereo: a forwarding wrapper).
plume::RenderCommandList* WrapCommands(plume::RenderCommandList* list);
// XeEye (b5) for the draws of this frame: the stereo eye buffer. False: the renderer binds zeros.
bool EyeConstants(plume::RenderBufferReference* ref);
// Present thread, the frame's list closed, just before the renderer submits it: stereo runs it once for the left
// eye first (see above).
void BeforeFrameSubmit(plume::RenderCommandList* list, bool have_guest_image);
// Present thread, after the frame's command list was submitted: hand this frame's guest image
// (eternalsonata::FramePresentTexture) to OpenXR (stereo: as the right eye).
void SubmitPresentImage();

// VR menu. While VR runs the SDK overlay (F4 settings, gamepad UI mode) is drawn into a target of its own and
// shown in the headset as a panel in front of you (native_renderer_plume.cpp / FrameVrMenuBegin).
bool MenuToHeadset();
// L3 + R3 held for 300 ms on any pad since the last call (the renderer then toggles the gamepad UI mode).
bool TakeMenuToggle();
// The VR tab of the settings dialog (ImGui, overlay thread). Nothing when VR is off.
void DrawVrSettings();

}  // namespace esvr

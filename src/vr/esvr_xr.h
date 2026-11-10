// Eternal Sonata VR: OpenXR output (opt-in: ES_VR=theater|mono|stereo or vr_config.txt next to the exe).
//
// D3D12 backend only. The finished guest frame is drawn into OpenXR swapchain images by a small command list of
// our own, submitted on the game's direct queue right after the frame's own work. The OpenXR frame loop
// (xrWaitFrame / Begin / End) runs on its own thread at the headset rate and re-submits the most recently
// released images, so the headset never waits for the game.
//
// Phases: theater (the frame on a screen in front of you), mono (head-tracked camera, one image for both eyes as
// a projection layer), stereo (one image per eye).
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12Resource;
struct ID3D12CommandList;
struct D3D12_RESOURCE_BARRIER;

namespace esvr {

enum class Mode : uint32_t { Off = 0, Theater = 1, Mono = 2, Stereo = 3 };
// Current mode (start: vr_config.txt / ES_VR; F8 cycles stereo / mono / theater while VR runs).
Mode RequestedMode();
// VR asked for at start (any mode but off). The renderer then picks D3D12.
bool Requested();
void CycleMode();
// Settings menu: switch between stereo / mono / theater (not off) and remember it for SaveConfig.
void SetMode(Mode mode);

struct Settings {
  std::string mode = "off";
  int resolution = 100;                                          // percent of the recommended eye size
  std::atomic<float> world_scale{1.0f};                          // game units per metre
  std::atomic<bool> keep_pitch{false};
  std::atomic<float> screen_width{3.2f}, screen_distance{2.5f};  // theater screen / menus, metres
  std::atomic<bool> hud{true};                                   // HUD of 3D frames on its own panel
  std::atomic<float> hud_distance{1.4f}, hud_width{1.4f}, hud_height{-0.1f};  // metres
  std::atomic<bool> hud_follow{false};                           // head-locked (else fixed in the room)
  std::atomic<float> menu_distance{1.0f}, menu_size{1.0f};      // settings menu panel: metres; 1 = 1 mm per pixel at 1 m
  bool from_file = false;
};
Settings& Cfg();
void SaveConfig();

struct Pose {
  float qx = 0, qy = 0, qz = 0, qw = 1;  // orientation
  float px = 0, py = 0, pz = 0;          // position, metres
};
struct Fov {
  float left = -0.8f, right = 0.8f, up = 0.8f, down = -0.8f;  // tangent angles, radians
};
struct EyeViews {
  bool valid = false;
  Pose head;    // view space in the (recentred) local space
  Pose eye[2];  // eye poses in the same space
  Fov fov[2];   // per-eye field of view
  uint64_t serial = 0;
};

class Xr {
 public:
  static Xr& Get();

  // Render thread, once the D3D12 device and direct queue exist. Starts OpenXR when VR is requested.
  bool Initialize(ID3D12Device* device, ID3D12CommandQueue* queue);
  void Shutdown();
  bool Active() const;

  // Present thread, after the frame's own command lists that write `source` were submitted to the queue.
  // `source` is in `source_state` (a D3D12_RESOURCE_STATES value) and is left in it. `width` x `height` is the
  // part of it that holds the image, at (`x0`, `y0`). `eye`: -1 = the whole frame (theater / mono), 0 / 1 = that
  // eye, 2 = the HUD, 3 = the settings menu (both premultiplied alpha kept).
  void SubmitFrame(ID3D12Resource* source, uint32_t source_state, uint32_t width, uint32_t height, int eye = -1,
                   uint32_t x0 = 0, uint32_t y0 = 0);

  // Present thread, stereo: run the frame's closed command list once for the left eye before the renderer submits
  // it again for the right one: XeEye = 0, the list, the left eye's copy (`image`, left in PIXEL_SHADER_RESOURCE by
  // the frame), the `restore` barriers (frame-end states back to frame-start ones), XeEye = 1.
  void RunFirstEye(ID3D12CommandList* frame_list, const D3D12_RESOURCE_BARRIER* restore, uint32_t restore_count,
                   ID3D12Resource* eye_buffer, ID3D12Resource* image, uint32_t width, uint32_t height);

  // Any thread: the newest head / eye poses and FOVs.
  EyeViews LatestViews() const;
  // The views the next submitted image was rendered with (mono / stereo frame matching).
  void SetRenderedViews(const EyeViews& views);
  void RecommendedEyeSize(uint32_t& width, uint32_t& height) const;
  void RequestRecenter();
  // L3 + R3 held (300 ms) since the last call.
  bool TakeMenuToggle();

 private:
  Xr() = default;
};

// esvr.log next to the exe (ES_VR_LOG overrides).
void Log(const char* format, ...);

}  // namespace esvr

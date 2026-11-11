// Eternal Sonata VR: game camera hooks (head tracking / per-eye cameras) and the camera probe (dev).
#include "generated/eternalsonata_init.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>

#include "../engine/room_presence.h"
#include "../gpu/native_renderer_frame.h"
#include "esvr_clip.h"
#include "esvr_xr.h"

#include <cmath>
#include <cstring>
#include <atomic>
#include <mutex>

namespace esvr {
namespace {

// The active render camera (battle_intro_overview.cpp).
constexpr uint32_t kRenderCamera = 0x82440618u;

bool ProbeEnabled() {
  static const bool on = [] {
    const char* e = std::getenv("ES_VR_CAMPROBE");
    return e && *e && *e != '0';
  }();
  return on;
}

float F(uint8_t* base, uint32_t address) { return std::bit_cast<float>(REX_LOAD_U32(address)); }

// Dev: every 2 s, the active render camera's first 0x200 bytes and its target block as floats.
void Probe(uint8_t* base, uint32_t camera) {
  static auto last = std::chrono::steady_clock::now() - std::chrono::seconds(10);
  auto now = std::chrono::steady_clock::now();
  if (now - last < std::chrono::seconds(2))
    return;
  last = now;
  Log("camprobe camera 0x%08X (active 0x%08X)", camera, REX_LOAD_U32(kRenderCamera));
  auto rows = [&](uint32_t from, uint32_t to) {
    for (uint32_t o = from; o < to; o += 16)
      Log("  +%03X  %12.5f %12.5f %12.5f %12.5f", o, F(base, camera + o), F(base, camera + o + 4),
          F(base, camera + o + 8), F(base, camera + o + 12));
  };
  rows(0x000, 0x200);
  rows(0x3D0, 0x410);
  rows(0x500, 0x530);
}

// ---------------------------------------------------------------------------------------------- matrices
// Column-vector 4x4 (m[row][col], v' = m v).
struct M4 {
  float m[4][4] = {};
  static M4 Identity() {
    M4 r;
    for (int i = 0; i < 4; ++i) r.m[i][i] = 1.0f;
    return r;
  }
};
M4 Mul(const M4& a, const M4& b) {
  M4 r;
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      float v = 0;
      for (int k = 0; k < 4; ++k) v += a.m[i][k] * b.m[k][j];
      r.m[i][j] = v;
    }
  return r;
}
bool Invert(const M4& in, M4& out) {
  double a[4][8];
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 8; ++j) a[i][j] = j < 4 ? in.m[i][j] : (j - 4 == i ? 1.0 : 0.0);
  for (int c = 0; c < 4; ++c) {
    int p = c;
    for (int r = c + 1; r < 4; ++r)
      if (std::fabs(a[r][c]) > std::fabs(a[p][c])) p = r;
    if (std::fabs(a[p][c]) < 1e-12) return false;
    if (p != c)
      for (int j = 0; j < 8; ++j) std::swap(a[p][j], a[c][j]);
    double d = a[c][c];
    for (int j = 0; j < 8; ++j) a[c][j] /= d;
    for (int r = 0; r < 4; ++r) {
      if (r == c) continue;
      double f = a[r][c];
      if (f != 0)
        for (int j = 0; j < 8; ++j) a[r][j] -= f * a[c][j];
    }
  }
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) out.m[i][j] = float(a[i][j + 4]);
  return true;
}
// Rotation (column form) of a unit quaternion.
M4 FromQuat(float x, float y, float z, float w) {
  M4 r = M4::Identity();
  r.m[0][0] = 1 - 2 * (y * y + z * z); r.m[0][1] = 2 * (x * y - z * w);     r.m[0][2] = 2 * (x * z + y * w);
  r.m[1][0] = 2 * (x * y + z * w);     r.m[1][1] = 1 - 2 * (x * x + z * z); r.m[1][2] = 2 * (y * z - x * w);
  r.m[2][0] = 2 * (x * z - y * w);     r.m[2][1] = 2 * (y * z + x * w);     r.m[2][2] = 1 - 2 * (x * x + y * y);
  return r;
}
// World -> view for a pose (rotation + position in metres, scaled to game units).
M4 ViewOf(const Pose& p, float scale) {
  M4 r = FromQuat(p.qx, p.qy, p.qz, p.qw);
  M4 v = M4::Identity();
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) v.m[i][j] = r.m[j][i];  // R^T
  const float t[3] = {p.px * scale, p.py * scale, p.pz * scale};
  for (int i = 0; i < 3; ++i) v.m[i][3] = -(v.m[i][0] * t[0] + v.m[i][1] * t[1] + v.m[i][2] * t[2]);
  return v;
}
// Projection for tangent extents, keeping the game projection's depth rows (rows 2 and 3) so depth stays
// what the game's shaders expect. The game is right handed, looking down -z (w = -z), like OpenXR.
M4 ProjectionFor(const Fov& f, const M4& game) {
  const float l = std::tan(f.left), r = std::tan(f.right), u = std::tan(f.up), d = std::tan(f.down);
  M4 p;
  p.m[0][0] = 2.0f / (r - l);
  p.m[0][2] = (r + l) / (r - l);
  p.m[1][1] = 2.0f / (u - d);
  p.m[1][2] = (u + d) / (u - d);
  for (int j = 0; j < 4; ++j) {
    p.m[2][j] = game.m[2][j];
    p.m[3][j] = game.m[3][j];
  }
  return p;
}

// ---------------------------------------------------------------------------------------------- clip matrix
struct ClipState {
  std::mutex mutex;
  bool valid = false;
  float rows[2][16] = {};
} g_clip;

float EnvFloat(const char* name, float def) {
  const char* e = std::getenv(name);
  return e && *e ? float(std::atof(e)) : def;
}

// The headset's union FOV (both eyes), for mono.
Fov UnionFov(const EyeViews& v) {
  Fov f;
  f.left = std::min(v.fov[0].left, v.fov[1].left);
  f.right = std::max(v.fov[0].right, v.fov[1].right);
  f.up = std::max(v.fov[0].up, v.fov[1].up);
  f.down = std::min(v.fov[0].down, v.fov[1].down);
  return f;
}

// Game thread, active render camera updated: build this frame's clip matrix from the game camera and the newest
// head pose. M = P_head * View_head * Level * P_game^-1 (all in the game camera's view space).
void UpdateClip(uint8_t* base, uint32_t camera) {
  const Mode mode = RequestedMode();
  if (!ClipActive()) {
    {
      std::lock_guard<std::mutex> lock(g_clip.mutex);
      g_clip.valid = false;
    }
    Xr::Get().SetRenderedViews(EyeViews{});  // flat: the frame goes on the screen in front of you
    return;
  }
  // One head pose per frame: every camera update of a frame (the camera can update more than once per frame)
  // uses the pose latched at the first one, so all draws of the frame and the pose the image is submitted with
  // agree (jitter.txt fix 3).
  static uint64_t latched_frame = ~0ull;
  static EyeViews latched;
  static uint64_t latches = 0, updates = 0;
  static auto last_report = std::chrono::steady_clock::now();
  ++updates;
  const uint64_t frame = eternalsonata::FrameIndex();
  if (frame != latched_frame || !latched.valid) {
    latched = Xr::Get().LatestViews();
    latched_frame = frame;
    ++latches;
  }
  const auto now = std::chrono::steady_clock::now();
  if (now - last_report >= std::chrono::seconds(10)) {
    const double secs = std::chrono::duration<double>(now - last_report).count();
    Log("camera: %.1f pose latches/s, %.1f camera updates/s", latches / secs, updates / secs);
    latches = updates = 0;
    last_report = now;
  }
  const EyeViews& views = latched;
  if (!views.valid)
    return;
  // Game projection (+0x130, row-vector D3D layout: clip_row = v_row P) -> column form P^T.
  M4 pg;
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) pg.m[j][i] = F(base, camera + 0x130 + 16 * i + 4 * j);
  M4 pg_inv;
  if (!Invert(pg, pg_inv) || pg.m[3][2] > -0.5f)  // not a perspective camera looking down -z
    return;
  // Game camera axes in world (+0x40 right, +0x50 up, +0x60 back; rows of its world matrix).
  float ax[3][3];
  for (int a = 0; a < 3; ++a)
    for (int c = 0; c < 3; ++c) ax[a][c] = F(base, camera + 0x40 + 16 * a + 4 * c);
  // Level: same yaw, no pitch / roll. L maps game view space to the level view space (both at the camera).
  M4 level = M4::Identity();
  if (!Cfg().keep_pitch) {
    float back[3] = {ax[2][0], 0.0f, ax[2][2]};
    float n = std::sqrt(back[0] * back[0] + back[2] * back[2]);
    if (n > 1e-4f) {
      back[0] /= n;
      back[2] /= n;
      const float up[3] = {0, 1, 0};
      const float right[3] = {up[1] * back[2] - up[2] * back[1], up[2] * back[0] - up[0] * back[2],
                              up[0] * back[1] - up[1] * back[0]};
      const float* lv[3] = {right, up, back};
      // v_level[i] = dot(level axis i, world dir of v) = sum_a dot(lv_i, ax_a) v[a]
      for (int i = 0; i < 3; ++i)
        for (int a = 0; a < 3; ++a)
          level.m[i][a] = lv[i][0] * ax[a][0] + lv[i][1] * ax[a][1] + lv[i][2] * ax[a][2];
    }
  }
  const float scale = Cfg().world_scale;
  const M4 base_view = Mul(level, pg_inv);
  M4 m[2];
  EyeViews rendered = views;
  if (mode == Mode::Stereo) {
    for (int e = 0; e < 2; ++e)
      m[e] = Mul(ProjectionFor(views.fov[e], pg), Mul(ViewOf(views.eye[e], scale), base_view));
  } else {
    const Fov fov = UnionFov(views);
    m[0] = m[1] = Mul(ProjectionFor(fov, pg), Mul(ViewOf(views.head, scale), base_view));
    rendered.fov[0] = rendered.fov[1] = fov;
  }
  {
    std::lock_guard<std::mutex> lock(g_clip.mutex);
    std::memcpy(g_clip.rows[0], m[0].m, sizeof(g_clip.rows[0]));
    std::memcpy(g_clip.rows[1], m[1].m, sizeof(g_clip.rows[1]));
    g_clip.valid = true;
  }
  Xr::Get().SetRenderedViews(rendered);
}

}  // namespace

namespace {
// Field or battle: the 3D game the camera is re-aimed for. Elsewhere (title, menus, loading) the frame is shown
// flat on a screen. Decided once per frame at the present (UpdateSceneKind).
std::atomic<bool> g_scene3d{false};
}  // namespace

void UpdateSceneKind() {
  auto& presence = eternalsonata::GetRoomPresence();
  // Full screen menus over the field draw no world camera draws at all: flat after a few such frames, back to 3D
  // at the first frame with world draws again.
  static uint32_t frames_without_world = 0;
  frames_without_world = eternalsonata::FrameWorldCameraDraws() == 0 ? frames_without_world + 1 : 0;
  const bool scene3d = (presence.IsFieldActive() || presence.IsBattleActive()) && frames_without_world < 3;
  if (scene3d != g_scene3d.exchange(scene3d))
    Log("scene: %s", scene3d ? "3D (field / battle)" : "flat (title / menus / 2D frames)");
}

bool ClipActive() {
  const Mode mode = RequestedMode();
  return (mode == Mode::Mono || mode == Mode::Stereo) && g_scene3d.load(std::memory_order_relaxed) &&
         Xr::Get().Active();
}

bool HudSeparate() { return ClipActive() && Cfg().hud; }

bool WaterRefractionOff() { return ClipActive() && !Cfg().water_refraction; }

void ClipMatrix(float m[2][16]) {
  std::lock_guard<std::mutex> lock(g_clip.mutex);
  if (g_clip.valid)
    std::memcpy(m, g_clip.rows, sizeof(g_clip.rows));
}

// Wider guest culling while VR re-aims the camera (the head turns away from the game camera's frustum).
void CullClipScale(float* x, float* y) {
  if (!ClipActive())
    return;
  static const float factor = EnvFloat("ES_VR_CULL", 8.0f);
  *x /= factor;
  *y /= factor;
}

// Game thread: sub_821078B0 finished updating `camera` (its view and projection for this frame).
void OnCameraUpdated(uint32_t camera, uint8_t* base) {
  if (camera != REX_LOAD_U32(kRenderCamera))
    return;
  if (ProbeEnabled())
    Probe(base, camera);
  if (Xr::Get().Active())
    UpdateClip(base, camera);
}

}  // namespace esvr

REX_EXTERN(__imp__sub_821078B0);
REX_HOOK_RAW(sub_821078B0) {
  const uint32_t camera = ctx.r3.u32;
  __imp__sub_821078B0(ctx, base);
  esvr::OnCameraUpdated(camera, base);
}

// Eternal Sonata VR: OpenXR output for the D3D12 backend. See esvr_xr.h.
// Ported from the Infinite Undiscovery VR module (same OpenXR / D3D12 plumbing).
#include "esvr_xr.h"
#include "esvr_clip.h"
#include "../gpu/native_renderer_frame.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <xinput.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D12
#include "openxr/openxr.h"
#include "openxr/openxr_platform.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace esvr {

namespace {
// Folder of the exe, with a trailing separator.
std::string ExeDir() {
  char path[MAX_PATH] = {};
  GetModuleFileNameA(nullptr, path, MAX_PATH);
  std::string dir(path);
  return dir.substr(0, dir.find_last_of("\\/") + 1);
}
std::mutex g_log_mutex;
FILE* g_log = nullptr;
std::chrono::steady_clock::time_point g_log_t0 = std::chrono::steady_clock::now();
}  // namespace

// ------------------------------------------------------------------------------------------------ log
void Log(const char* format, ...) {
  std::lock_guard<std::mutex> lock(g_log_mutex);
  if (!g_log) {
    const char* path = std::getenv("ES_VR_LOG");
    g_log = std::fopen(path && *path ? path : (ExeDir() + "esvr.log").c_str(), "w");
    if (!g_log) return;
  }
  double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_log_t0).count();
  std::fprintf(g_log, "[%9.3f] ", t);
  va_list ap;
  va_start(ap, format);
  std::vfprintf(g_log, format, ap);
  va_end(ap);
  std::fputc('\n', g_log);
  std::fflush(g_log);
}

// ------------------------------------------------------------------------------------------------ settings
// Sources, later wins: defaults, vr_config.txt next to the exe, ES_VR_* environment (dev).
namespace {
void LoadSettings(Settings& t) {
    auto apply = [&t](const std::string& k, const std::string& v) {
      float f = float(std::atof(v.c_str()));
      std::string lv = v;
      for (auto& c : lv) c = char(tolower(c));
      bool b = lv == "1" || lv == "true" || lv == "on" || lv == "yes";
      if (k == "mode") t.mode = lv;
      else if (k == "resolution") t.resolution = int(f);
      else if (k == "world_scale") t.world_scale = f;
      else if (k == "keep_pitch") t.keep_pitch = b;
      else if (k == "screen_width") t.screen_width = f;
      else if (k == "hud") t.hud = b;
      else if (k == "hud_distance") t.hud_distance = f;
      else if (k == "hud_width") t.hud_width = f;
      else if (k == "hud_height") t.hud_height = f;
      else if (k == "hud_follow") t.hud_follow = b;
      else if (k == "menu_distance") t.menu_distance = f;
      else if (k == "water_refraction") t.water_refraction = b;
      else if (k == "menu_size") t.menu_size = f;
      else if (k == "screen_distance") t.screen_distance = f;
    };
    if (FILE* f = std::fopen((ExeDir() + "vr_config.txt").c_str(), "r")) {
      char line[256];
      while (std::fgets(line, sizeof(line), f)) {
        std::string l(line);
        size_t hash = l.find('#');
        if (hash != std::string::npos) l.resize(hash);
        size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        auto trim = [](std::string x) {
          size_t a = x.find_first_not_of(" \t\r\n"), b = x.find_last_not_of(" \t\r\n");
          return a == std::string::npos ? std::string() : x.substr(a, b - a + 1);
        };
        apply(trim(l.substr(0, eq)), trim(l.substr(eq + 1)));
      }
      std::fclose(f);
      t.from_file = true;
    }
    struct { const char* env; const char* key; } envs[] = {
        {"ES_VR", "mode"}, {"ES_VR_RESOLUTION", "resolution"}, {"ES_VR_SCALE", "world_scale"},
        {"ES_VR_KEEP_PITCH", "keep_pitch"}, {"ES_VR_SCREEN_WIDTH", "screen_width"},
        {"ES_VR_SCREEN_DISTANCE", "screen_distance"}, {"ES_VR_HUD", "hud"}, {"ES_VR_HUD_FOLLOW", "hud_follow"}};
    for (auto& e : envs)
      if (const char* v = std::getenv(e.env))
        if (*v) apply(e.key, v);
}
}  // namespace

Settings& Cfg() {
  static Settings st;
  static std::once_flag once;
  std::call_once(once, [] { LoadSettings(st); });
  return st;
}

void SaveConfig() {
  const Settings& t = Cfg();
  FILE* f = std::fopen((ExeDir() + "vr_config.txt").c_str(), "w");
  if (!f) return;
  std::fprintf(f,
               "# Eternal Sonata VR settings.\n"
               "# mode: stereo, mono, theater or off (F8 cycles stereo / mono / theater while playing, F9 recentres)\n"
               "mode = %s\n"
               "# render resolution, percent of the headset's recommended eye size\n"
               "resolution = %d\n"
               "# game units per metre\n"
               "world_scale = %g\n"
               "# keep the game camera's up / down tilt (0 = level horizon)\n"
               "keep_pitch = %d\n"
               "# theater screen (and menus) width and distance in metres\n"
               "screen_width = %g\nscreen_distance = %g\n"
               "# HUD and in-game menus on a panel in front of you (0 = drawn into the eyes): distance, width and\n"
               "# height (relative to eye level) in metres; hud_follow = 1 keeps it in front of your head, 0 fixes it\n"
               "# in the room in front of where you faced at the last recentre (F9)\n"
               "hud = %d\nhud_distance = %g\nhud_width = %g\nhud_height = %g\nhud_follow = %d\n"
               "# settings menu (L3 + R3 held, or F4): distance in metres and size (1 = 1 mm per pixel at 1 m)\n"
               "menu_distance = %g\nmenu_size = %g\n"
               "# 0 turns off the scene seen through water\nwater_refraction = %d\n",
               t.mode.c_str(), t.resolution, t.world_scale.load(), t.keep_pitch ? 1 : 0, t.screen_width.load(),
               t.screen_distance.load(), t.hud ? 1 : 0, t.hud_distance.load(), t.hud_width.load(), t.hud_height.load(),
               t.hud_follow ? 1 : 0, t.menu_distance.load(), t.menu_size.load(),
               t.water_refraction ? 1 : 0);
  std::fclose(f);
  Log("settings saved");
}

namespace {
Mode ParseMode(const std::string& m) {
  if (m == "theater" || m == "theatre") return Mode::Theater;
  if (m == "mono") return Mode::Mono;
  if (m == "stereo") return Mode::Stereo;
  return Mode::Off;
}
std::atomic<int> g_mode{-1};
}  // namespace

Mode RequestedMode() {
  int m = g_mode.load(std::memory_order_relaxed);
  if (m < 0) {
    m = int(ParseMode(Cfg().mode));
    g_mode.store(m);
  }
  return Mode(m);
}

bool Requested() { return ParseMode(Cfg().mode) != Mode::Off; }

void CycleMode() {
  Mode m = RequestedMode();
  if (m == Mode::Off) return;
  Mode n = m == Mode::Stereo ? Mode::Mono : m == Mode::Mono ? Mode::Theater : Mode::Stereo;
  g_mode.store(int(n));
  Log("mode %d (F8)", int(n));
}

void SetMode(Mode mode) {
  if (RequestedMode() == Mode::Off || mode == Mode::Off) return;
  g_mode.store(int(mode));
  Cfg().mode = mode == Mode::Stereo ? "stereo" : mode == Mode::Mono ? "mono" : "theater";
  Log("mode %d (menu)", int(mode));
}

namespace {

float EnvFloat(const char* name, float def) {
  const char* e = std::getenv(name);
  return e && *e ? float(std::atof(e)) : def;
}

// ------------------------------------------------------------------------------------------------ OpenXR entry points
#define ESVR_XR_FUNCS(X)                                                                         \
  X(xrDestroyInstance) X(xrGetSystem) X(xrGetSystemProperties) X(xrEnumerateViewConfigurationViews) \
  X(xrCreateSession) X(xrDestroySession) X(xrBeginSession) X(xrEndSession) X(xrRequestExitSession) \
  X(xrPollEvent) X(xrCreateReferenceSpace) X(xrDestroySpace) X(xrLocateSpace) X(xrLocateViews)     \
  X(xrEnumerateSwapchainFormats) X(xrCreateSwapchain) X(xrDestroySwapchain)                         \
  X(xrEnumerateSwapchainImages) X(xrAcquireSwapchainImage) X(xrWaitSwapchainImage)                  \
  X(xrReleaseSwapchainImage) X(xrWaitFrame) X(xrBeginFrame) X(xrEndFrame) X(xrResultToString)      \
  X(xrGetD3D12GraphicsRequirementsKHR)

struct XrFns {
  PFN_xrGetInstanceProcAddr xrGetInstanceProcAddr = nullptr;
  PFN_xrCreateInstance xrCreateInstance = nullptr;
  PFN_xrEnumerateInstanceExtensionProperties xrEnumerateInstanceExtensionProperties = nullptr;
#define ESVR_DECL(name) PFN_##name name = nullptr;
  ESVR_XR_FUNCS(ESVR_DECL)
#undef ESVR_DECL
} x;

// ------------------------------------------------------------------------------------------------ math
struct Quat { float x, y, z, w; };
struct Vec3 { float x, y, z; };
Quat QMul(const Quat& a, const Quat& b) {
  return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
          a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
Quat QConj(const Quat& q) { return {-q.x, -q.y, -q.z, q.w}; }
Vec3 QRot(const Quat& q, const Vec3& v) {
  Quat p{v.x, v.y, v.z, 0};
  Quat r = QMul(QMul(q, p), QConj(q));
  return {r.x, r.y, r.z};
}
Pose ToPose(const XrPosef& p) {
  return {p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w,
          p.position.x,    p.position.y,    p.position.z};
}

// ------------------------------------------------------------------------------------------------ state
struct Swapchain {
  XrSwapchain handle = XR_NULL_HANDLE;
  uint32_t width = 0, height = 0;
  std::vector<ID3D12Resource*> images;
  ID3D12DescriptorHeap* rtv_heap = nullptr;
  uint32_t rtv_size = 0;
  bool acquired = false;  // acquired, but xrWaitSwapchainImage timed out: wait again next time (no new acquire)
  uint32_t acquired_index = 0;
};

constexpr uint32_t kSlots = 8;  // a stereo frame uses up to 4 (eye flips, two copies)
struct CopySlot {
  ID3D12CommandAllocator* allocator = nullptr;
  ID3D12GraphicsCommandList* list = nullptr;
  uint64_t fence_value = 0;
};

struct State {
  HMODULE loader = nullptr;
  XrInstance instance = XR_NULL_HANDLE;
  XrSystemId system = XR_NULL_SYSTEM_ID;
  XrSession session = XR_NULL_HANDLE;
  XrSpace local = XR_NULL_HANDLE;
  XrSpace view = XR_NULL_HANDLE;
  XrSessionState session_state = XR_SESSION_STATE_UNKNOWN;
  std::atomic<bool> running{false};
  std::atomic<bool> quit{false};
  std::thread frame_thread;
  uint32_t eye_w = 0, eye_h = 0;
  DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;

  ID3D12Device* device = nullptr;
  ID3D12CommandQueue* queue = nullptr;
  ID3D12Fence* fence = nullptr;
  uint64_t fence_next = 1;
  CopySlot slots[kSlots];
  uint32_t slot_index = 0;
  ID3D12RootSignature* root = nullptr;
  ID3D12PipelineState* pso = nullptr;
  ID3D12DescriptorHeap* srv_heap = nullptr;
  uint32_t srv_size = 0;
  ID3D12Resource* eye_values = nullptr;  // upload heap: uint 0 at 0, uint 1 at 256 (copied into XeEye per eye pass)

  // Swapchains: [0] the whole frame (theater screen / mono both eyes), [1], [2] stereo eyes, [3] the HUD,
  // [4] the settings menu.
  // chain_mutex: swapchain lifetime + which images are published. The frame loop holds it across
  // xrBeginFrame..xrEndFrame, so nothing the game thread calls may take it.
  std::mutex chain_mutex;
  Swapchain chains[5];
  bool published[5] = {};
  std::chrono::steady_clock::time_point hud_time;  // last HUD submitted (chain_mutex)
  std::chrono::steady_clock::time_point menu_time;  // last menu submitted (chain_mutex)
  uint32_t menu_w = 0, menu_h = 0;                 // the part of the menu image that holds it (chain_mutex)
  bool menu_shown = false;                         // frame thread: the menu layer was in the last frame
  XrPosef menu_pose{{0, 0, 0, 1}, {0, 0, -1}};     // frame thread: where it was put when it opened
  std::atomic<bool> menu_toggle{false};            // L3 + R3 held (frame thread -> present thread)
  EyeViews published_views;   // the views the published images were rendered with
  EyeViews rendered_views;    // set by the camera path for the next submit (rendered_mutex)
  std::mutex rendered_mutex;  // never held with anything slow
  bool left_pending = false;  // stereo: left eye copied, released with the right one (chain_mutex)

  mutable std::mutex views_mutex;
  EyeViews latest;
  std::atomic<bool> recenter{true};
  XrPosef recenter_pose{{0, 0, 0, 1}, {0, 0, 0}};

  std::atomic<uint64_t> frames_submitted{0}, xr_frames{0}, copies{0};
} s;

bool Check(XrResult r, const char* what) {
  if (XR_SUCCEEDED(r)) return true;
  char text[XR_MAX_RESULT_STRING_SIZE] = {};
  if (x.xrResultToString && s.instance) x.xrResultToString(s.instance, r, text);
  Log("%s failed: %d %s", what, int(r), text);
  return false;
}

bool LoadLoader() {
  wchar_t path[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring dir(path);
  dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
  s.loader = LoadLibraryW((dir + L"openxr_loader.dll").c_str());
  if (!s.loader) s.loader = LoadLibraryW(L"openxr_loader.dll");
  if (!s.loader) {
    Log("openxr_loader.dll not found next to the exe");
    return false;
  }
  x.xrGetInstanceProcAddr =
      reinterpret_cast<PFN_xrGetInstanceProcAddr>(GetProcAddress(s.loader, "xrGetInstanceProcAddr"));
  if (!x.xrGetInstanceProcAddr) return false;
  x.xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrCreateInstance",
                          reinterpret_cast<PFN_xrVoidFunction*>(&x.xrCreateInstance));
  x.xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties",
                          reinterpret_cast<PFN_xrVoidFunction*>(&x.xrEnumerateInstanceExtensionProperties));
  return x.xrCreateInstance && x.xrEnumerateInstanceExtensionProperties;
}

bool LoadInstanceFunctions() {
  bool ok = true;
#define ESVR_LOAD(name)                                                                    \
  if (XR_FAILED(x.xrGetInstanceProcAddr(s.instance, #name,                                 \
                                        reinterpret_cast<PFN_xrVoidFunction*>(&x.name)))) { \
    Log("missing %s", #name);                                                              \
    ok = false;                                                                            \
  }
  ESVR_XR_FUNCS(ESVR_LOAD)
#undef ESVR_LOAD
  return ok;
}

// ------------------------------------------------------------------------------------------------ copy pipeline
const char* kShader = R"(
Texture2D<float4> src : register(t0);
SamplerState smp : register(s0);
cbuffer C : register(b0) { float4 uvRect; float4 opts; };   // uv scale.xy, offset.zw; opts.x = decode gamma
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID) {
  V o; float2 p = float2((id << 1) & 2, id & 2);
  o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
  o.uv = p * uvRect.xy + uvRect.zw;
  return o;
}
float3 ToLinear(float3 c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
float4 ps(V i) : SV_Target {
  float4 t = src.SampleLevel(smp, i.uv, 0);
  if (opts.y > 0.5) {
    // HUD: the UI drawn over transparent black, i.e. display-encoded colour premultiplied by alpha. Decoded
    // straight (so blending in the compositor's linear space matches) and premultiplied again.
    float a = saturate(t.a);
    float3 c = a > 0.0001 ? ToLinear(saturate(t.rgb / a)) * a : ToLinear(saturate(t.rgb));
    return float4(c, a);
  }
  float3 c = t.rgb;
  if (opts.x > 0.5) c = ToLinear(saturate(c));
  return float4(c, 1);
}
)";

bool CreateCopyPipeline() {
  HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
  if (!dc) { Log("d3dcompiler_47.dll missing"); return false; }
  auto compile = reinterpret_cast<pD3DCompile>(GetProcAddress(dc, "D3DCompile"));
  if (!compile) return false;
  ID3DBlob *vs = nullptr, *ps = nullptr, *err = nullptr;
  if (FAILED(compile(kShader, strlen(kShader), "esvr", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vs, &err)) ||
      FAILED(compile(kShader, strlen(kShader), "esvr", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &ps, &err))) {
    Log("shader compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
    return false;
  }
  D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
  D3D12_ROOT_PARAMETER params[2] = {};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable = {1, &range};
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[1].Constants = {0, 0, 8};
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_STATIC_SAMPLER_DESC sampler = {};
  sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.MaxLOD = D3D12_FLOAT32_MAX;
  sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_ROOT_SIGNATURE_DESC rs = {2, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  ID3DBlob* blob = nullptr;
  using PFNSerialize = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION,
                                        ID3DBlob**, ID3DBlob**);
  auto serialize = reinterpret_cast<PFNSerialize>(
      GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12SerializeRootSignature"));
  if (!serialize || FAILED(serialize(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) ||
      FAILED(s.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                           IID_PPV_ARGS(&s.root)))) {
    Log("root signature failed");
    return false;
  }
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
  pd.pRootSignature = s.root;
  pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
  pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
  pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pd.SampleMask = UINT_MAX;
  pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pd.RasterizerState.DepthClipEnable = TRUE;
  pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pd.NumRenderTargets = 1;
  pd.RTVFormats[0] = s.format;
  pd.SampleDesc.Count = 1;
  if (FAILED(s.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&s.pso)))) {
    Log("pipeline state failed");
    return false;
  }
  D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSlots * 4,
                                   D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  if (FAILED(s.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&s.srv_heap)))) return false;
  s.srv_size = s.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  if (FAILED(s.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s.fence)))) return false;
  for (auto& slot : s.slots) {
    if (FAILED(s.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                IID_PPV_ARGS(&slot.allocator))) ||
        FAILED(s.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator, nullptr,
                                           IID_PPV_ARGS(&slot.list))))
      return false;
    slot.list->Close();
  }
  D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_UPLOAD};
  D3D12_RESOURCE_DESC bd = {};
  bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  bd.Width = 512;
  bd.Height = 1;
  bd.DepthOrArraySize = 1;
  bd.MipLevels = 1;
  bd.SampleDesc.Count = 1;
  bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  if (FAILED(s.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                               nullptr, IID_PPV_ARGS(&s.eye_values))))
    return false;
  uint8_t* mapped = nullptr;
  D3D12_RANGE none = {0, 0};
  if (FAILED(s.eye_values->Map(0, &none, reinterpret_cast<void**>(&mapped)))) return false;
  std::memset(mapped, 0, 512);
  mapped[256 + 0] = 1;  // little endian uint 1
  s.eye_values->Unmap(0, nullptr);
  return true;
}

// ------------------------------------------------------------------------------------------------ dev dump
// esvr_dump.now next to the exe: the next image submitted to each swapchain is written as esvr_dump_<chain>.ppm
// (8-bit, as stored: sRGB-encoded colour; the HUD premultiplied). The trigger file is deleted once all are done.
uint32_t g_dump_pending = 0;  // bit per chain still to write
std::chrono::steady_clock::time_point g_dump_poll;
bool DumpWanted(int ci) {
  auto now = std::chrono::steady_clock::now();
  if (g_dump_pending != 0 && now - g_dump_poll > std::chrono::seconds(3)) g_dump_pending = 0;  // chains not in use
  if (g_dump_pending == 0 && now - g_dump_poll > std::chrono::milliseconds(500)) {
    g_dump_poll = now;
    std::string trigger = ExeDir() + "esvr_dump.now";
    if (GetFileAttributesA(trigger.c_str()) != INVALID_FILE_ATTRIBUTES) {
      DeleteFileA(trigger.c_str());
      g_dump_pending = 0x1F;
      RequestTrace();
      Log("dump requested (frame %llu traced)", (unsigned long long)eternalsonata::FrameIndex() + 1);
    }
  }
  return (g_dump_pending >> ci) & 1;
}
void WriteDump(ID3D12Resource* buffer, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp, uint32_t w, uint32_t h, int ci) {
  if (s.fence->GetCompletedValue() < s.fence_next - 1) {
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    s.fence->SetEventOnCompletion(s.fence_next - 1, ev);
    WaitForSingleObject(ev, 2000);
    CloseHandle(ev);
  }
  uint8_t* data = nullptr;
  if (SUCCEEDED(buffer->Map(0, nullptr, reinterpret_cast<void**>(&data)))) {
    std::string path = ExeDir() + "esvr_dump_" + std::to_string(ci) + ".ppm";
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
      std::fprintf(f, "P6\n%u %u\n255\n", w, h);
      std::vector<uint8_t> row(size_t(w) * 3);
      const bool bgr = s.format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
      for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* p = data + fp.Offset + size_t(y) * fp.Footprint.RowPitch;
        for (uint32_t xx = 0; xx < w; ++xx) {
          row[xx * 3 + 0] = p[xx * 4 + (bgr ? 2 : 0)];
          row[xx * 3 + 1] = p[xx * 4 + 1];
          row[xx * 3 + 2] = p[xx * 4 + (bgr ? 0 : 2)];
        }
        std::fwrite(row.data(), 1, row.size(), f);
      }
      std::fclose(f);
      if (ci >= 3) {  // the HUD's / menu's alpha as a grey image
        std::string apath = ExeDir() + "esvr_dump_" + std::to_string(ci) + "a.ppm";
        if (FILE* fa = std::fopen(apath.c_str(), "wb")) {
          std::fprintf(fa, "P5\n%u %u\n255\n", w, h);
          std::vector<uint8_t> arow(w);
          for (uint32_t y = 0; y < h; ++y) {
            const uint8_t* p = data + fp.Offset + size_t(y) * fp.Footprint.RowPitch;
            for (uint32_t xx = 0; xx < w; ++xx) arow[xx] = p[xx * 4 + 3];
            std::fwrite(arow.data(), 1, arow.size(), fa);
          }
          std::fclose(fa);
        }
      }
      Log("dumped chain %d to %s", ci, path.c_str());
    }
    buffer->Unmap(0, nullptr);
  }
  buffer->Release();
  g_dump_pending &= ~(1u << ci);
  if (g_dump_pending != 0 && ci == 2) g_dump_pending &= ~1u;  // stereo frames never write chain 0
}

// The next of our command lists, waited for and reset (open, pipeline state set).
CopySlot& OpenSlot() {
  CopySlot& slot = s.slots[s.slot_index];
  if (s.fence->GetCompletedValue() < slot.fence_value) {
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    s.fence->SetEventOnCompletion(slot.fence_value, ev);
    WaitForSingleObject(ev, 1000);
    CloseHandle(ev);
  }
  slot.allocator->Reset();
  slot.list->Reset(slot.allocator, s.pso);
  return slot;
}
// Closes and submits it on the game's queue.
void SubmitSlot(CopySlot& slot) {
  slot.list->Close();
  ID3D12CommandList* lists[] = {slot.list};
  s.queue->ExecuteCommandLists(1, lists);
  slot.fence_value = s.fence_next++;
  s.queue->Signal(s.fence, slot.fence_value);
  s.slot_index = (s.slot_index + 1) % kSlots;
}

void DestroySwapchain(Swapchain& c) {
  if (c.handle) x.xrDestroySwapchain(c.handle);
  if (c.rtv_heap) c.rtv_heap->Release();
  c = Swapchain{};
}

// Caller holds chain_mutex. GPU idle for this chain is ensured by waiting on our fence.
bool EnsureSwapchain(Swapchain& c, uint32_t width, uint32_t height) {
  if (c.handle && c.width == width && c.height == height) return true;
  if (c.handle) {
    if (s.fence->GetCompletedValue() < s.fence_next - 1) {
      HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      s.fence->SetEventOnCompletion(s.fence_next - 1, ev);
      WaitForSingleObject(ev, 1000);
      CloseHandle(ev);
    }
    DestroySwapchain(c);
  }
  XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
  ci.format = s.format;
  ci.sampleCount = 1;
  ci.width = width;
  ci.height = height;
  ci.faceCount = 1;
  ci.arraySize = 1;
  ci.mipCount = 1;
  if (!Check(x.xrCreateSwapchain(s.session, &ci, &c.handle), "xrCreateSwapchain")) return false;
  uint32_t n = 0;
  x.xrEnumerateSwapchainImages(c.handle, 0, &n, nullptr);
  std::vector<XrSwapchainImageD3D12KHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
  x.xrEnumerateSwapchainImages(c.handle, n, &n,
                               reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data()));
  D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, n, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  s.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&c.rtv_heap));
  c.rtv_size = s.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  for (uint32_t i = 0; i < n; ++i) {
    c.images.push_back(imgs[i].texture);
    D3D12_RENDER_TARGET_VIEW_DESC rd = {};
    rd.Format = s.format;
    rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE h = c.rtv_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += size_t(i) * c.rtv_size;
    s.device->CreateRenderTargetView(imgs[i].texture, &rd, h);
  }
  c.width = width;
  c.height = height;
  Log("swapchain %ux%u, %u images", width, height, n);
  return true;
}

void HandleEvents() {
  XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
  while (x.xrPollEvent(s.instance, &ev) == XR_SUCCESS) {
    if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
      auto* e = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
      s.session_state = e->state;
      Log("session state %d", int(e->state));
      if (e->state == XR_SESSION_STATE_READY) {
        XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
        bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        if (Check(x.xrBeginSession(s.session, &bi), "xrBeginSession")) s.running = true;
      } else if (e->state == XR_SESSION_STATE_STOPPING) {
        s.running = false;
        x.xrEndSession(s.session);
      } else if (e->state == XR_SESSION_STATE_EXITING || e->state == XR_SESSION_STATE_LOSS_PENDING) {
        s.running = false;
        s.quit = true;
      }
    } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
      s.running = false;
      s.quit = true;
    }
    ev = {XR_TYPE_EVENT_DATA_BUFFER};
  }
}

// Recentred local space: yaw + position of recenter_pose removed.
XrPosef Recentre(const XrPosef& p) {
  const XrPosef& r = s.recenter_pose;
  Quat ry{r.orientation.x, r.orientation.y, r.orientation.z, r.orientation.w};
  Quat inv = QConj(ry);
  Vec3 d{p.position.x - r.position.x, p.position.y - r.position.y, p.position.z - r.position.z};
  Vec3 pos = QRot(inv, d);
  Quat q = QMul(inv, Quat{p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w});
  return {{q.x, q.y, q.z, q.w}, {pos.x, pos.y, pos.z}};
}
// The layer pose in the raw local space for a pose given in the recentred space.
XrPosef Uncentre(const XrPosef& p) {
  const XrPosef& r = s.recenter_pose;
  Quat ry{r.orientation.x, r.orientation.y, r.orientation.z, r.orientation.w};
  Vec3 pos = QRot(ry, Vec3{p.position.x, p.position.y, p.position.z});
  Quat q = QMul(ry, Quat{p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w});
  return {{q.x, q.y, q.z, q.w}, {pos.x + r.position.x, pos.y + r.position.y, pos.z + r.position.z}};
}

void LocateViews(XrTime time) {
  XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
  li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
  li.displayTime = time;
  li.space = s.local;
  XrViewState vs{XR_TYPE_VIEW_STATE};
  XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
  uint32_t n = 0;
  if (XR_FAILED(x.xrLocateViews(s.session, &li, &vs, 2, &n, views)) || n != 2) return;
  XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
  if (XR_FAILED(x.xrLocateSpace(s.view, s.local, time, &head))) return;
  if (!(head.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) return;
  if (s.recenter.exchange(false)) {
    // keep yaw only
    const XrQuaternionf& q = head.pose.orientation;
    float yaw = std::atan2(2.0f * (q.w * q.y + q.x * q.z), 1.0f - 2.0f * (q.y * q.y + q.x * q.x));
    s.recenter_pose.orientation = {0, std::sin(yaw * 0.5f), 0, std::cos(yaw * 0.5f)};
    s.recenter_pose.position = head.pose.position;
    Log("recentred (yaw %.1f deg)", yaw * 57.29578f);
  }
  EyeViews v;
  v.valid = true;
  // Dev (null headset tests): a fake head movement on top of the tracked one. ES_VR_FAKE_YAW=<deg> at start, or
  // esvr_fake_pose.txt next to the exe, read about twice a second while running: "yaw pitch x y z" (degrees,
  // metres; yaw left positive, x right, y up, z back), so one session can compare head poses.
  static float fake_yaw = EnvFloat("ES_VR_FAKE_YAW", 0.0f), fake_pitch = 0.0f;
  static Vec3 fake_offset{0, 0, 0};
  static auto next_read = std::chrono::steady_clock::now();
  if (std::chrono::steady_clock::now() >= next_read) {
    next_read = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    if (FILE* f = std::fopen((ExeDir() + "esvr_fake_pose.txt").c_str(), "r")) {
      float y = 0, pt = 0, fx = 0, fy = 0, fz = 0;
      if (std::fscanf(f, "%f %f %f %f %f", &y, &pt, &fx, &fy, &fz) >= 1 &&
          (y != fake_yaw || pt != fake_pitch || fx != fake_offset.x || fy != fake_offset.y || fz != fake_offset.z)) {
        fake_yaw = y;
        fake_pitch = pt;
        fake_offset = {fx, fy, fz};
        Log("fake pose yaw %.1f pitch %.1f offset %.3f %.3f %.3f", y, pt, fx, fy, fz);
      }
      std::fclose(f);
    }
  }
  // About the tracked head's position, so a fake turn is a head turn rather than an orbit around the origin.
  const XrPosef centred_head = Recentre(head.pose);
  const Vec3 pivot{centred_head.position.x, centred_head.position.y, centred_head.position.z};
  auto fake = [&](XrPosef p) {
    if (fake_yaw == 0.0f && fake_pitch == 0.0f && fake_offset.x == 0.0f && fake_offset.y == 0.0f &&
        fake_offset.z == 0.0f)
      return p;
    const float ya = fake_yaw * 0.01745329f, pa = fake_pitch * 0.01745329f;
    Quat r = QMul(Quat{0, std::sin(ya * 0.5f), 0, std::cos(ya * 0.5f)},
                  Quat{std::sin(pa * 0.5f), 0, 0, std::cos(pa * 0.5f)});
    Quat q = QMul(r, Quat{p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w});
    Vec3 t = QRot(r, Vec3{p.position.x - pivot.x, p.position.y - pivot.y, p.position.z - pivot.z});
    return XrPosef{{q.x, q.y, q.z, q.w},
                   {t.x + pivot.x + fake_offset.x, t.y + pivot.y + fake_offset.y, t.z + pivot.z + fake_offset.z}};
  };
  v.head = ToPose(fake(Recentre(head.pose)));
  for (int i = 0; i < 2; ++i) {
    v.eye[i] = ToPose(fake(Recentre(views[i].pose)));
    v.fov[i] = {views[i].fov.angleLeft, views[i].fov.angleRight, views[i].fov.angleUp, views[i].fov.angleDown};
  }
  std::lock_guard<std::mutex> lock(s.views_mutex);
  v.serial = s.latest.serial + 1;
  s.latest = v;
}

// L3 + R3 held together for 300 ms on any pad opens / closes the settings (vrkit's gesture). XInput is loaded
// here rather than linked; pads that are not connected are only asked again every 2 s (that call is slow).
void PollMenuCombo() {
  using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
  static GetStateFn get_state = [] {
    HMODULE m = LoadLibraryW(L"xinput1_4.dll");
    if (!m) m = LoadLibraryW(L"xinput9_1_0.dll");
    return m ? reinterpret_cast<GetStateFn>(GetProcAddress(m, "XInputGetState")) : nullptr;
  }();
  if (!get_state) return;
  static std::chrono::steady_clock::time_point retry[XUSER_MAX_COUNT];
  static std::chrono::steady_clock::time_point held_since;
  static bool held = false, fired = false;
  const auto now = std::chrono::steady_clock::now();
  bool both = false;
  for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
    if (now < retry[i]) continue;
    XINPUT_STATE st = {};
    if (get_state(i, &st) != ERROR_SUCCESS) {
      retry[i] = now + std::chrono::seconds(2);
      continue;
    }
    constexpr WORD kBoth = XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB;
    if ((st.Gamepad.wButtons & kBoth) == kBoth) both = true;
  }
  if (!both) {
    held = fired = false;
    return;
  }
  if (!held) {
    held = true;
    held_since = now;
  }
  if (!fired && now - held_since >= std::chrono::milliseconds(300)) {
    fired = true;
    s.menu_toggle = true;
    Log("menu toggle (L3 + R3)");
  }
}

// F8 (mode) / F9 (recenter) while the game window has the focus.
void PollKeys() {
  static bool down8 = false, down9 = false;
  HWND fg = GetForegroundWindow();
  DWORD pid = 0;
  if (fg) GetWindowThreadProcessId(fg, &pid);
  bool ours = pid == GetCurrentProcessId();
  bool k8 = ours && (GetAsyncKeyState(VK_F8) & 0x8000);
  bool k9 = ours && (GetAsyncKeyState(VK_F9) & 0x8000);
  if (k8 && !down8) CycleMode();
  if (k9 && !down9) {
    s.recenter = true;
    Log("recenter (F9)");
  }
  down8 = k8;
  down9 = k9;
  PollMenuCombo();
}

void FrameLoop() {
  Log("frame thread started");
  auto last_stats = std::chrono::steady_clock::now();
  double wait_ms = 0;
  uint64_t wait_count = 0;
  while (!s.quit) {
    HandleEvents();
    if (!s.running) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    const auto wait_start = std::chrono::steady_clock::now();
    const bool waited = Check(x.xrWaitFrame(s.session, nullptr, &fs), "xrWaitFrame");
    wait_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wait_start).count();
    ++wait_count;
    if (!waited) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }
    LocateViews(fs.predictedDisplayTime);
    std::lock_guard<std::mutex> lock(s.chain_mutex);
    XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
    if (!Check(x.xrBeginFrame(s.session, &bi), "xrBeginFrame")) continue;
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView pv[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                              {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    XrCompositionLayerQuad hud{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerQuad menu{XR_TYPE_COMPOSITION_LAYER_QUAD};
    const XrCompositionLayerBaseHeader* layers[4];
    uint32_t layer_count = 0;
    Mode mode = RequestedMode();
    const EyeViews& rv = s.published_views;
    bool flat = mode == Mode::Theater || !rv.valid;
    if (fs.shouldRender && s.published[0] && (flat || mode == Mode::Mono)) {
      Swapchain& c = s.chains[0];
      if (flat) {
        float width = Cfg().screen_width, dist = Cfg().screen_distance;
        quad.space = s.local;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.subImage.swapchain = c.handle;
        quad.subImage.imageRect = {{0, 0}, {int32_t(c.width), int32_t(c.height)}};
        quad.pose = Uncentre({{0, 0, 0, 1}, {0, 0, -dist}});
        quad.size = {width, width * float(c.height) / float(c.width)};
        layers[layer_count++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&quad);
      } else {
        // mono: the one image in both eyes, at the pose and FOV it was rendered with
        for (int i = 0; i < 2; ++i) {
          pv[i].subImage.swapchain = c.handle;
          pv[i].subImage.imageRect = {{0, 0}, {int32_t(c.width), int32_t(c.height)}};
          const Pose& p = rv.head;
          pv[i].pose = Uncentre({{p.qx, p.qy, p.qz, p.qw}, {p.px, p.py, p.pz}});
          pv[i].fov = {rv.fov[0].left, rv.fov[0].right, rv.fov[0].up, rv.fov[0].down};
        }
        proj.space = s.local;
        proj.viewCount = 2;
        proj.views = pv;
        layers[layer_count++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&proj);
      }
    } else if (fs.shouldRender && mode == Mode::Stereo && s.published[1] && s.published[2]) {
      for (int i = 0; i < 2; ++i) {
        Swapchain& c = s.chains[1 + i];
        pv[i].subImage.swapchain = c.handle;
        pv[i].subImage.imageRect = {{0, 0}, {int32_t(c.width), int32_t(c.height)}};
        const Pose& p = rv.eye[i];
        pv[i].pose = Uncentre({{p.qx, p.qy, p.qz, p.qw}, {p.px, p.py, p.pz}});
        pv[i].fov = {rv.fov[i].left, rv.fov[i].right, rv.fov[i].up, rv.fov[i].down};
      }
      proj.space = s.local;
      proj.viewCount = 2;
      proj.views = pv;
      layers[layer_count++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&proj);
    }
    // The HUD (UI layer of 3D frames) on a panel in front of you, over the eyes.
    const bool projection = layer_count > 0 && layers[0] == reinterpret_cast<XrCompositionLayerBaseHeader*>(&proj);
    if (projection && Cfg().hud && s.published[3] &&
        std::chrono::steady_clock::now() - s.hud_time < std::chrono::milliseconds(250)) {
      Swapchain& c = s.chains[3];
      const float width = Cfg().hud_width, dist = Cfg().hud_distance, height = Cfg().hud_height;
      hud.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
      hud.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
      hud.subImage.swapchain = c.handle;
      hud.subImage.imageRect = {{0, 0}, {int32_t(c.width), int32_t(c.height)}};
      hud.size = {width, width * float(c.height) / float(c.width)};
      if (Cfg().hud_follow) {
        hud.space = s.view;  // head-locked
        hud.pose = {{0, 0, 0, 1}, {0, height, -dist}};
      } else {
        hud.space = s.local;  // fixed in the room, in front of where you faced at the last recentre
        hud.pose = Uncentre({{0, 0, 0, 1}, {0, height, -dist}});
      }
      layers[layer_count++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&hud);
    }
    // The settings menu, over everything, in any mode. Put down in front of you (level, at eye height) when it
    // opens and left there, so it can be read by turning the head rather than chasing it.
    const bool menu_on = fs.shouldRender && s.published[4] && s.menu_w && s.menu_h &&
                         std::chrono::steady_clock::now() - s.menu_time < std::chrono::milliseconds(250);
    if (menu_on) {
      const float dist = std::clamp(Cfg().menu_distance.load(), 0.3f, 5.0f);
      if (!s.menu_shown) {
        EyeViews v;
        {
          std::lock_guard<std::mutex> vlock(s.views_mutex);
          v = s.latest;
        }
        const Pose& h = v.head;
        float yaw = std::atan2(2.0f * (h.qw * h.qy + h.qx * h.qz), 1.0f - 2.0f * (h.qy * h.qy + h.qx * h.qx));
        Quat q{0, std::sin(yaw * 0.5f), 0, std::cos(yaw * 0.5f)};
        Vec3 ahead = QRot(q, Vec3{0, 0, -dist});
        s.menu_pose = {{q.x, q.y, q.z, q.w}, {h.px + ahead.x, h.py + ahead.y, h.pz + ahead.z}};
        Log("menu opened (yaw %.1f deg)", yaw * 57.29578f);
      }
      Swapchain& c = s.chains[4];
      const float metres_per_pixel = 0.001f * std::clamp(Cfg().menu_size.load(), 0.2f, 4.0f) * dist;
      menu.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
      menu.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
      menu.subImage.swapchain = c.handle;
      menu.subImage.imageRect = {{0, 0}, {int32_t(s.menu_w), int32_t(s.menu_h)}};
      menu.size = {float(s.menu_w) * metres_per_pixel, float(s.menu_h) * metres_per_pixel};
      menu.space = s.local;
      menu.pose = Uncentre(s.menu_pose);
      layers[layer_count++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&menu);
    }
    s.menu_shown = menu_on;
    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = fs.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = layer_count;
    ei.layers = layers;
    Check(x.xrEndFrame(s.session, &ei), "xrEndFrame");
    ++s.xr_frames;
    PollKeys();
    auto now = std::chrono::steady_clock::now();
    if (now - last_stats > std::chrono::seconds(10)) {
      last_stats = now;
      Log("stats: xr frames %llu, submitted images %llu, copies %llu, period %.2f ms, xrWaitFrame %.2f ms avg",
          (unsigned long long)s.xr_frames.load(), (unsigned long long)s.frames_submitted.load(),
          (unsigned long long)s.copies.load(), fs.predictedDisplayPeriod / 1e6,
          wait_count ? wait_ms / double(wait_count) : 0.0);
      wait_ms = 0;
      wait_count = 0;
    }
  }
  Log("frame thread ended");
}

}  // namespace

// ------------------------------------------------------------------------------------------------ public
Xr& Xr::Get() {
  static Xr xr;
  return xr;
}

bool Xr::Active() const { return s.session != XR_NULL_HANDLE; }

namespace {
bool EnsureInstance() {
  static bool tried = false;
  if (s.instance) return true;
  if (tried) return false;
  tried = true;
  Log("ES VR: mode %d", int(RequestedMode()));
  if (!LoadLoader()) return false;
  uint32_t n = 0;
  x.xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr);
  std::vector<XrExtensionProperties> exts(n, {XR_TYPE_EXTENSION_PROPERTIES});
  x.xrEnumerateInstanceExtensionProperties(nullptr, n, &n, exts.data());
  bool has_d3d12 = false;
  for (auto& e : exts) has_d3d12 |= !std::strcmp(e.extensionName, XR_KHR_D3D12_ENABLE_EXTENSION_NAME);
  if (!has_d3d12) { Log("runtime has no %s", XR_KHR_D3D12_ENABLE_EXTENSION_NAME); return false; }
  const char* enabled[] = {XR_KHR_D3D12_ENABLE_EXTENSION_NAME};
  XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
  std::snprintf(ci.applicationInfo.applicationName, sizeof(ci.applicationInfo.applicationName), "%s", "Eternal Sonata VR");
  std::snprintf(ci.applicationInfo.engineName, sizeof(ci.applicationInfo.engineName), "%s", "Reprise");
  ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
  ci.enabledExtensionCount = 1;
  ci.enabledExtensionNames = enabled;
  if (!Check(x.xrCreateInstance(&ci, &s.instance), "xrCreateInstance")) return false;
  if (!LoadInstanceFunctions()) return false;
  XrSystemGetInfo gi{XR_TYPE_SYSTEM_GET_INFO};
  gi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
  if (!Check(x.xrGetSystem(s.instance, &gi, &s.system), "xrGetSystem")) return false;
  XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
  x.xrGetSystemProperties(s.instance, s.system, &sp);
  Log("system: %s", sp.systemName);
  XrViewConfigurationView vcv[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
  x.xrEnumerateViewConfigurationViews(s.instance, s.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2,
                                      &n, vcv);
  s.eye_w = vcv[0].recommendedImageRectWidth;
  s.eye_h = vcv[0].recommendedImageRectHeight;
  Log("recommended eye size %ux%u", s.eye_w, s.eye_h);
  return true;
}

}  // namespace

bool Xr::Initialize(ID3D12Device* device, ID3D12CommandQueue* queue) {
  if (RequestedMode() == Mode::Off || s.session) return false;
  if (!EnsureInstance()) return false;
  uint32_t n = 0;
  XrGraphicsRequirementsD3D12KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
  if (!Check(x.xrGetD3D12GraphicsRequirementsKHR(s.instance, s.system, &req), "xrGetD3D12GraphicsRequirementsKHR"))
    return false;
  LUID luid = device->GetAdapterLuid();
  if (std::memcmp(&luid, &req.adapterLuid, sizeof(LUID)) != 0)
    Log("WARNING: the game's adapter is not the headset's adapter");
  s.device = device;
  s.queue = queue;
  XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
  binding.device = device;
  binding.queue = queue;
  XrSessionCreateInfo si{XR_TYPE_SESSION_CREATE_INFO};
  si.next = &binding;
  si.systemId = s.system;
  if (!Check(x.xrCreateSession(s.instance, &si, &s.session), "xrCreateSession")) return false;
  XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  rs.poseInReferenceSpace = {{0, 0, 0, 1}, {0, 0, 0}};
  rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
  x.xrCreateReferenceSpace(s.session, &rs, &s.local);
  rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
  x.xrCreateReferenceSpace(s.session, &rs, &s.view);
  // swapchain format: 8-bit sRGB preferred (the guest output is display-encoded; the shader decodes it)
  x.xrEnumerateSwapchainFormats(s.session, 0, &n, nullptr);
  std::vector<int64_t> formats(n);
  x.xrEnumerateSwapchainFormats(s.session, n, &n, formats.data());
  std::string list;
  for (auto f : formats) list += std::to_string(f) + " ";
  Log("swapchain formats: %s", list.c_str());
  bool found = false;
  for (DXGI_FORMAT want : {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB}) {
    if (std::find(formats.begin(), formats.end(), int64_t(want)) != formats.end()) {
      s.format = want;
      found = true;
      break;
    }
  }
  if (!found) { Log("no sRGB swapchain format"); return false; }
  if (!CreateCopyPipeline()) return false;
  s.frame_thread = std::thread(FrameLoop);
  Log("OpenXR session created");
  return true;
}

void Xr::Shutdown() {
  if (!s.instance) return;
  s.quit = true;
  if (s.frame_thread.joinable()) s.frame_thread.join();
  {
    std::lock_guard<std::mutex> lock(s.chain_mutex);
    for (auto& c : s.chains) DestroySwapchain(c);
  }
  if (s.session) x.xrDestroySession(s.session);
  if (s.instance) x.xrDestroyInstance(s.instance);
  s.session = XR_NULL_HANDLE;
  s.instance = XR_NULL_HANDLE;
  Log("shut down");
}

void Xr::SubmitFrame(ID3D12Resource* source, uint32_t source_state_bits, uint32_t width, uint32_t height, int eye,
                     uint32_t x0, uint32_t y0) {
  const D3D12_RESOURCE_STATES source_state = D3D12_RESOURCE_STATES(source_state_bits);
  if (!s.session || !s.pso || !width || !height) return;
  ++s.frames_submitted;
  int ci = eye < 0 ? 0 : eye == 2 ? 3 : eye >= 3 ? 4 : 1 + eye;
  uint32_t out_w = width, out_h = height;
  if (ci == 4) {  // the menu's box changes as it opens and moves: the image keeps the overlay's whole size
    const D3D12_RESOURCE_DESC full = source->GetDesc();
    out_w = uint32_t(full.Width);
    out_h = full.Height;
  }
  if (eye >= 0 || RequestedMode() == Mode::Mono) {
    // projection layers: the image keeps its own size (the FOV says where it goes)
  }
  std::lock_guard<std::mutex> lock(s.chain_mutex);
  Swapchain& c = s.chains[ci];
  if (ci == 1 && s.left_pending) {  // a left eye whose right eye never came (mode change): let it go
    Check(x.xrReleaseSwapchainImage(c.handle, nullptr), "xrReleaseSwapchainImage");
    s.left_pending = false;
  }
  if (!EnsureSwapchain(c, out_w, out_h)) return;
  uint32_t index = 0;
  if (c.acquired) {
    index = c.acquired_index;
  } else {
    if (!Check(x.xrAcquireSwapchainImage(c.handle, nullptr, &index), "xrAcquireSwapchainImage")) return;
    c.acquired = true;
    c.acquired_index = index;
  }
  XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
  wi.timeout = 100000000;  // 100 ms
  XrResult wr = x.xrWaitSwapchainImage(c.handle, &wi);
  if (wr != XR_SUCCESS) {
    static int timeouts = 0;
    if (++timeouts <= 20 || timeouts % 100 == 0) Log("xrWaitSwapchainImage: result %d (%d times)", int(wr), timeouts);
    if (wr != XR_TIMEOUT_EXPIRED) c.acquired = false;  // lost (session state): acquire anew
    return;
  }
  c.acquired = false;
  CopySlot& slot = OpenSlot();
  // SRV of the source in this slot's part of the heap
  D3D12_RESOURCE_DESC sd = source->GetDesc();
  D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
  srv.Format = sd.Format;
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture2D.MipLevels = 1;
  D3D12_CPU_DESCRIPTOR_HANDLE cpu = s.srv_heap->GetCPUDescriptorHandleForHeapStart();
  D3D12_GPU_DESCRIPTOR_HANDLE gpu = s.srv_heap->GetGPUDescriptorHandleForHeapStart();
  cpu.ptr += size_t(s.slot_index) * s.srv_size;
  gpu.ptr += uint64_t(s.slot_index) * s.srv_size;
  s.device->CreateShaderResourceView(source, &srv, cpu);
  ID3D12GraphicsCommandList* l = slot.list;
  D3D12_RESOURCE_BARRIER b[2] = {};
  int nb = 0;
  constexpr D3D12_RESOURCE_STATES kSrvState =
      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  bool src_transition = (source_state & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) == 0;
  if (src_transition) {
    b[nb].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[nb].Transition = {source, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, source_state, kSrvState};
    ++nb;
  }
  if (nb) l->ResourceBarrier(nb, b);
  ID3D12DescriptorHeap* heaps[] = {s.srv_heap};
  l->SetDescriptorHeaps(1, heaps);
  l->SetGraphicsRootSignature(s.root);
  l->SetGraphicsRootDescriptorTable(0, gpu);
  float consts[8] = {float(width) / float(sd.Width), float(height) / float(sd.Height), float(x0) / float(sd.Width),
                     float(y0) / float(sd.Height), 1, ci >= 3 ? 1.0f : 0.0f, 0, 0};
  l->SetGraphicsRoot32BitConstants(1, 8, consts, 0);
  D3D12_CPU_DESCRIPTOR_HANDLE rtv = c.rtv_heap->GetCPUDescriptorHandleForHeapStart();
  rtv.ptr += size_t(index) * c.rtv_size;
  l->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  const uint32_t vw = ci == 4 ? std::min(width, c.width) : c.width, vh = ci == 4 ? std::min(height, c.height) : c.height;
  D3D12_VIEWPORT vp = {0, 0, float(vw), float(vh), 0, 1};
  D3D12_RECT sc = {0, 0, LONG(vw), LONG(vh)};
  l->RSSetViewports(1, &vp);
  l->RSSetScissorRects(1, &sc);
  l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  l->DrawInstanced(3, 1, 0, 0);
  if (src_transition) {
    b[0].Transition = {source, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, kSrvState, source_state};
    l->ResourceBarrier(1, b);
  }
  // Dev: esvr_dump.now next to the exe -> the next image of every swapchain written as esvr_dump_<n>.ppm.
  ID3D12Resource* dump = nullptr;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
  if (DumpWanted(ci)) {
    ID3D12Resource* image = c.images[index];
    D3D12_RESOURCE_DESC id = image->GetDesc();
    UINT64 total = 0;
    s.device->GetCopyableFootprints(&id, 0, 1, 0, &footprint, nullptr, nullptr, &total);
    D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (SUCCEEDED(s.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                    nullptr, IID_PPV_ARGS(&dump)))) {
      D3D12_RESOURCE_BARRIER t = {};
      t.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      t.Transition = {image, 0, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
      l->ResourceBarrier(1, &t);
      D3D12_TEXTURE_COPY_LOCATION dst = {dump, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
      dst.PlacedFootprint = footprint;
      D3D12_TEXTURE_COPY_LOCATION srcl = {image, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      srcl.SubresourceIndex = 0;
      l->CopyTextureRegion(&dst, 0, 0, 0, &srcl, nullptr);
      std::swap(t.Transition.StateBefore, t.Transition.StateAfter);
      l->ResourceBarrier(1, &t);
    }
  }
  SubmitSlot(slot);
  if (dump) WriteDump(dump, footprint, c.width, c.height, ci);
  // Stereo: the left image is released together with the right one, so the compositor never pairs eyes of
  // different frames (it always uses the most recently released image of each swapchain).
  if (ci == 1) {
    s.left_pending = true;
    ++s.copies;
    return;
  }
  if (ci == 2 && s.left_pending) {
    Check(x.xrReleaseSwapchainImage(s.chains[1].handle, nullptr), "xrReleaseSwapchainImage");
    s.left_pending = false;
    s.published[1] = true;
  }
  Check(x.xrReleaseSwapchainImage(c.handle, nullptr), "xrReleaseSwapchainImage");
  s.published[ci] = true;
  if (ci == 3) {
    s.hud_time = std::chrono::steady_clock::now();
    ++s.copies;
    return;
  }
  if (ci == 4) {
    s.menu_time = std::chrono::steady_clock::now();
    s.menu_w = vw;
    s.menu_h = vh;
    ++s.copies;
    return;
  }
  if (ci == 0 || ci == 2) {
    // the views this frame was rendered with (not the newest ones: the game may already be a frame ahead)
    std::lock_guard<std::mutex> rlock(s.rendered_mutex);
    s.published_views = s.rendered_views;
  }   // stereo: set once per pair (after the right eye)
  ++s.copies;
}

void Xr::RunFirstEye(ID3D12CommandList* frame_list, const D3D12_RESOURCE_BARRIER* restore, uint32_t restore_count,
                     ID3D12Resource* eye_buffer, ID3D12Resource* image, uint32_t width, uint32_t height) {
  if (!s.session || !s.eye_values) return;
  {
    CopySlot& slot = OpenSlot();
    slot.list->CopyBufferRegion(eye_buffer, 0, s.eye_values, 0, 256);  // XeEye = 0
    SubmitSlot(slot);
  }
  ID3D12CommandList* lists[] = {frame_list};
  s.queue->ExecuteCommandLists(1, lists);
  SubmitFrame(image, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, width, height, 0);
  {
    CopySlot& slot = OpenSlot();
    if (restore_count) slot.list->ResourceBarrier(restore_count, restore);
    slot.list->CopyBufferRegion(eye_buffer, 0, s.eye_values, 256, 256);  // XeEye = 1
    SubmitSlot(slot);
  }
}

EyeViews Xr::LatestViews() const {
  std::lock_guard<std::mutex> lock(s.views_mutex);
  return s.latest;
}

void Xr::SetRenderedViews(const EyeViews& views) {
  std::lock_guard<std::mutex> lock(s.rendered_mutex);
  s.rendered_views = views;
}

void Xr::RecommendedEyeSize(uint32_t& width, uint32_t& height) const {
  width = s.eye_w;
  height = s.eye_h;
}

void Xr::RequestRecenter() { s.recenter = true; }

bool Xr::TakeMenuToggle() { return s.menu_toggle.exchange(false); }

}  // namespace esvr

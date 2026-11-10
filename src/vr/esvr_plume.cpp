// Eternal Sonata VR: see esvr_plume.h.
#include "esvr_plume.h"

#ifdef _WIN32
#include <plume_d3d12.h>

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <unordered_map>
#include <vector>

#include "../gpu/native_renderer_frame.h"
#include "../gpu/native_renderer_plume_internal.h"
#include "esvr_clip.h"
#include "esvr_xr.h"

namespace esvr {

using namespace plume;

namespace {

// Forwards everything to the frame's real command list and remembers the D3D12 state each texture and buffer had
// before the frame's first barrier on it, so the list can be run a second time (stereo) after restoring them.
class StereoCommandList final : public RenderCommandList {
 public:
  void Open(RenderCommandList* target) {
    t_ = target;
    textures_.clear();
    buffers_.clear();
  }
  RenderCommandList* Target() const { return t_; }

  // Transitions from where the frame left each resource back to where it found it.
  void RestoreBarriers(std::vector<D3D12_RESOURCE_BARRIER>& out) const {
    out.clear();
    for (const auto& [texture, before] : textures_) {
      if (texture->resourceStates == before) continue;
      D3D12_RESOURCE_BARRIER b = {};
      b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      b.Transition = {texture->d3d, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, texture->resourceStates, before};
      out.push_back(b);
    }
    for (const auto& [buffer, before] : buffers_) {
      if (buffer->resourceStates == before) continue;
      D3D12_RESOURCE_BARRIER b = {};
      b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      b.Transition = {buffer->d3d, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, buffer->resourceStates, before};
      out.push_back(b);
    }
  }

  void barriers(RenderBarrierStages stages, const RenderBufferBarrier* bufferBarriers, uint32_t bufferBarriersCount,
                const RenderTextureBarrier* textureBarriers, uint32_t textureBarriersCount) override {
    for (uint32_t i = 0; i < textureBarriersCount; ++i) {
      auto* t = static_cast<D3D12Texture*>(const_cast<RenderTexture*>(textureBarriers[i].texture));
      if (t != nullptr) textures_.try_emplace(t, t->resourceStates);
    }
    for (uint32_t i = 0; i < bufferBarriersCount; ++i) {
      auto* b = static_cast<D3D12Buffer*>(const_cast<RenderBuffer*>(bufferBarriers[i].buffer));
      if (b != nullptr) buffers_.try_emplace(b, b->resourceStates);
    }
    t_->barriers(stages, bufferBarriers, bufferBarriersCount, textureBarriers, textureBarriersCount);
  }
  void begin() override { t_->begin(); }
  void end() override { t_->end(); }
  void dispatch(uint32_t threadGroupCountX, uint32_t threadGroupCountY, uint32_t threadGroupCountZ) override { t_->dispatch(threadGroupCountX, threadGroupCountY, threadGroupCountZ); }
  void traceRays(uint32_t width, uint32_t height, uint32_t depth, RenderBufferReference shaderBindingTable, const RenderShaderBindingGroupsInfo &shaderBindingGroupsInfo) override { t_->traceRays(width, height, depth, shaderBindingTable, shaderBindingGroupsInfo); }
  void drawInstanced(uint32_t vertexCountPerInstance, uint32_t instanceCount, uint32_t startVertexLocation, uint32_t startInstanceLocation) override { t_->drawInstanced(vertexCountPerInstance, instanceCount, startVertexLocation, startInstanceLocation); }
  void drawIndexedInstanced(uint32_t indexCountPerInstance, uint32_t instanceCount, uint32_t startIndexLocation, int32_t baseVertexLocation, uint32_t startInstanceLocation) override { t_->drawIndexedInstanced(indexCountPerInstance, instanceCount, startIndexLocation, baseVertexLocation, startInstanceLocation); }
  void setPipeline(const RenderPipeline *pipeline) override { t_->setPipeline(pipeline); }
  void setComputePipelineLayout(const RenderPipelineLayout *pipelineLayout) override { t_->setComputePipelineLayout(pipelineLayout); }
  void setComputePushConstants(uint32_t rangeIndex, const void *data, uint32_t offset, uint32_t size) override { t_->setComputePushConstants(rangeIndex, data, offset, size); }
  void setComputeDescriptorSet(RenderDescriptorSet *descriptorSet, uint32_t setIndex) override { t_->setComputeDescriptorSet(descriptorSet, setIndex); }
  void setGraphicsPipelineLayout(const RenderPipelineLayout *pipelineLayout) override { t_->setGraphicsPipelineLayout(pipelineLayout); }
  void setGraphicsPushConstants(uint32_t rangeIndex, const void *data, uint32_t offset, uint32_t size) override { t_->setGraphicsPushConstants(rangeIndex, data, offset, size); }
  void setGraphicsDescriptorSet(RenderDescriptorSet *descriptorSet, uint32_t setIndex) override { t_->setGraphicsDescriptorSet(descriptorSet, setIndex); }
  void setGraphicsRootDescriptor(RenderBufferReference bufferReference, uint32_t rootDescriptorIndex) override { t_->setGraphicsRootDescriptor(bufferReference, rootDescriptorIndex); }
  void setRaytracingPipelineLayout(const RenderPipelineLayout *pipelineLayout) override { t_->setRaytracingPipelineLayout(pipelineLayout); }
  void setRaytracingPushConstants(uint32_t rangeIndex, const void *data, uint32_t offset, uint32_t size) override { t_->setRaytracingPushConstants(rangeIndex, data, offset, size); }
  void setRaytracingDescriptorSet(RenderDescriptorSet *descriptorSet, uint32_t setIndex) override { t_->setRaytracingDescriptorSet(descriptorSet, setIndex); }
  void setIndexBuffer(const RenderIndexBufferView *view) override { t_->setIndexBuffer(view); }
  void setVertexBuffers(uint32_t startSlot, const RenderVertexBufferView *views, uint32_t viewCount, const RenderInputSlot *inputSlots) override { t_->setVertexBuffers(startSlot, views, viewCount, inputSlots); }
  void setViewports(const RenderViewport *viewports, uint32_t count) override { t_->setViewports(viewports, count); }
  void setScissors(const RenderRect *scissorRects, uint32_t count) override { t_->setScissors(scissorRects, count); }
  void setFramebuffer(const RenderFramebuffer *framebuffer) override { t_->setFramebuffer(framebuffer); }
  void setDepthBias(float depthBias, float depthBiasClamp, float slopeScaledDepthBias) override { t_->setDepthBias(depthBias, depthBiasClamp, slopeScaledDepthBias); }
  void setStencilReference(uint32_t stencilReference) override { t_->setStencilReference(stencilReference); }
  void clearColor(uint32_t attachmentIndex, RenderColor colorValue, const RenderRect *clearRects, uint32_t clearRectsCount) override { t_->clearColor(attachmentIndex, colorValue, clearRects, clearRectsCount); }
  void clearDepthStencil(bool clearDepth, bool clearStencil, float depthValue, uint32_t stencilValue, const RenderRect *clearRects, uint32_t clearRectsCount) override { t_->clearDepthStencil(clearDepth, clearStencil, depthValue, stencilValue, clearRects, clearRectsCount); }
  void copyBufferRegion(RenderBufferReference dstBuffer, RenderBufferReference srcBuffer, uint64_t size) override { t_->copyBufferRegion(dstBuffer, srcBuffer, size); }
  void copyTextureRegion(const RenderTextureCopyLocation &dstLocation, const RenderTextureCopyLocation &srcLocation, uint32_t dstX, uint32_t dstY, uint32_t dstZ, const RenderBox *srcBox) override { t_->copyTextureRegion(dstLocation, srcLocation, dstX, dstY, dstZ, srcBox); }
  void copyBuffer(const RenderBuffer *dstBuffer, const RenderBuffer *srcBuffer) override { t_->copyBuffer(dstBuffer, srcBuffer); }
  void copyTexture(const RenderTexture *dstTexture, const RenderTexture *srcTexture) override { t_->copyTexture(dstTexture, srcTexture); }
  bool blitTexture(const RenderTexture *dstTexture, const RenderTexture *srcTexture, bool linear) override { return t_->blitTexture(dstTexture, srcTexture, linear); }
  void resolveTexture(const RenderTexture *dstTexture, const RenderTexture *srcTexture) override { t_->resolveTexture(dstTexture, srcTexture); }
  void resolveTextureRegion(const RenderTexture *dstTexture, uint32_t dstX, uint32_t dstY, const RenderTexture *srcTexture, const RenderRect *srcRect, RenderResolveMode resolveMode) override { t_->resolveTextureRegion(dstTexture, dstX, dstY, srcTexture, srcRect, resolveMode); }
  void buildBottomLevelAS(const RenderAccelerationStructure *dstAccelerationStructure, RenderBufferReference scratchBuffer, const RenderBottomLevelASBuildInfo &buildInfo) override { t_->buildBottomLevelAS(dstAccelerationStructure, scratchBuffer, buildInfo); }
  void buildTopLevelAS(const RenderAccelerationStructure *dstAccelerationStructure, RenderBufferReference scratchBuffer, RenderBufferReference instancesBuffer, const RenderTopLevelASBuildInfo &buildInfo) override { t_->buildTopLevelAS(dstAccelerationStructure, scratchBuffer, instancesBuffer, buildInfo); }
  void discardTexture(const RenderTexture* texture) override { t_->discardTexture(texture); }
  void resetQueryPool(const RenderQueryPool *queryPool, uint32_t queryFirstIndex, uint32_t queryCount) override { t_->resetQueryPool(queryPool, queryFirstIndex, queryCount); }
  void writeTimestamp(const RenderQueryPool *queryPool, uint32_t queryIndex) override { t_->writeTimestamp(queryPool, queryIndex); }
  void beginQuery(const RenderQueryPool *queryPool, uint32_t queryIndex) override { t_->beginQuery(queryPool, queryIndex); }
  void endQuery(const RenderQueryPool *queryPool, uint32_t queryIndex) override { t_->endQuery(queryPool, queryIndex); }

 private:
  RenderCommandList* t_ = nullptr;
  std::unordered_map<D3D12Texture*, D3D12_RESOURCE_STATES> textures_;
  std::unordered_map<D3D12Buffer*, D3D12_RESOURCE_STATES> buffers_;
};

struct GlueState {
  RenderDevice* device = nullptr;
  std::unique_ptr<RenderBuffer> eye_buffer;  // XeEye, default heap: flipped by a GPU copy between the eye passes
  StereoCommandList proxy;
  bool stereo_frame = false;   // this frame's list is recorded through the proxy
  int lists_this_frame = 0;    // >1: a mid-frame flush ran part of the frame already (no stereo replay then)
  std::vector<D3D12_RESOURCE_BARRIER> restore;
  uint64_t stereo_frames = 0, skipped = 0;
} g;

// The image the eyes get: the world alone when the HUD is separate, else the whole frame.
void* EyeImage(uint32_t* width, uint32_t* height) {
  if (void* world = eternalsonata::FrameVrWorldTexture(width, height))
    return world;
  return eternalsonata::FramePresentTexture(width, height);
}

bool StereoActive() { return RequestedMode() == Mode::Stereo && ClipActive(); }

}  // namespace

namespace {
std::atomic<uint64_t> g_trace_frame{~0ull};
}  // namespace

void RequestTrace() { g_trace_frame = eternalsonata::FrameIndex() + 1; }
bool TraceFrame() { return eternalsonata::FrameIndex() == g_trace_frame.load(std::memory_order_relaxed); }

bool VrRequested() { return Requested(); }

void InitFromPlume(RenderDevice* device, RenderCommandQueue* queue) {
  if (!Requested())
    return;
  g.device = device;
  auto* d = static_cast<D3D12Device*>(device);
  auto* q = static_cast<D3D12CommandQueue*>(queue);
  if (!Xr::Get().Initialize(d->d3d, q->d3d)) {
    Log("VR did not start (see above)");
    return;
  }
  g.eye_buffer = device->createBuffer(RenderBufferDesc::DefaultBuffer(256, RenderBufferFlag::CONSTANT));
  if (!g.eye_buffer)
    Log("eye buffer could not be created: no stereo");
}

bool RenderExtent(uint32_t* width, uint32_t* height) {
  if (Xr::Get().Active())
    UpdateSceneKind();
  if (!ClipActive())
    return false;
  uint32_t w = 0, h = 0;
  Xr::Get().RecommendedEyeSize(w, h);
  if (w == 0 || h == 0)
    return false;
  const double pct = std::clamp(Cfg().resolution, 20, 300) / 100.0;
  *width = uint32_t(w * pct + 0.5);
  *height = uint32_t(h * pct + 0.5);
  return true;
}

RenderCommandList* WrapCommands(RenderCommandList* list) {
  ++g.lists_this_frame;
  g.stereo_frame = StereoActive() && g.eye_buffer != nullptr;
  if (!g.stereo_frame)
    return list;
  g.proxy.Open(list);
  return &g.proxy;
}

bool EyeConstants(RenderBufferReference* ref) {
  if (!g.stereo_frame)
    return false;
  *ref = RenderBufferReference(g.eye_buffer.get());
  return true;
}

void BeforeFrameSubmit(RenderCommandList* list, bool have_guest_image) {
  if (!g.stereo_frame)
    return;
  const bool whole_frame = g.lists_this_frame == 1;
  uint32_t width = 0, height = 0;
  void* texture = have_guest_image ? EyeImage(&width, &height) : nullptr;
  if (!whole_frame || texture == nullptr || g.proxy.Target() != list) {
    ++g.skipped;
    return;
  }
  g.proxy.RestoreBarriers(g.restore);
  auto* image = static_cast<D3D12Texture*>(static_cast<RenderTexture*>(texture));
  auto* frame_list = static_cast<D3D12CommandList*>(list);
  auto* eye_buffer = static_cast<D3D12Buffer*>(g.eye_buffer.get());
  Xr::Get().RunFirstEye(frame_list->d3d, g.restore.data(), uint32_t(g.restore.size()), eye_buffer->d3d, image->d3d,
                        width, height);
  if (++g.stereo_frames % 600 == 1)
    Log("stereo frames %llu (skipped %llu), restore barriers %zu", (unsigned long long)g.stereo_frames,
        (unsigned long long)g.skipped, g.restore.size());
}

void SubmitPresentImage() {
  const bool stereo = g.stereo_frame && g.lists_this_frame == 1;
  g.lists_this_frame = 0;
  g.stereo_frame = false;
  if (!Xr::Get().Active())
    return;
  uint32_t width = 0, height = 0;
  void* texture = EyeImage(&width, &height);
  if (texture == nullptr)
    return;
  auto* t = static_cast<D3D12Texture*>(static_cast<RenderTexture*>(texture));
  Xr::Get().SubmitFrame(t->d3d, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, width, height, stereo ? 1 : -1);
  // The HUD: the UI target the world was kept out of (transparent where there is no UI); the UI sits in its
  // largest centred 16:9 rectangle.
  uint32_t ww = 0, wh = 0;
  if (eternalsonata::FrameVrWorldTexture(&ww, &wh) != nullptr) {
    uint32_t cw = 0, ch = 0;
    void* composite = eternalsonata::FrameVrHudTexture(&cw, &ch);
    if (composite != nullptr && composite != texture && cw != 0 && ch != 0) {
      uint32_t w = cw, h = cw * 9 / 16;
      if (h > ch) {
        h = ch;
        w = ch * 16 / 9;
      }
      auto* c = static_cast<D3D12Texture*>(static_cast<RenderTexture*>(composite));
      Xr::Get().SubmitFrame(c->d3d, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, w, h, 2, (cw - w) / 2, (ch - h) / 2);
    }
  }
  // The menu: only the part the overlay drew in.
  uint32_t mx = 0, my = 0, mw = 0, mh = 0;
  if (void* menu = eternalsonata::FrameVrMenuTexture(&mx, &my, &mw, &mh)) {
    auto* m = static_cast<D3D12Texture*>(static_cast<RenderTexture*>(menu));
    Xr::Get().SubmitFrame(m->d3d, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, mw, mh, 3, mx, my);
  }
}

bool MenuToHeadset() { return Xr::Get().Active(); }

bool TakeMenuToggle() { return Xr::Get().TakeMenuToggle(); }

void DrawVrSettings() {
  if (!Xr::Get().Active()) {
    ImGui::TextWrapped("VR is off. Set mode = stereo (or mono / theater) in vr_config.txt next to the game and "
                       "restart to play in a headset.");
    return;
  }
  Settings& t = Cfg();
  static const char* kModes[] = {"Stereo", "Mono", "Theater"};
  static const Mode kModeValues[] = {Mode::Stereo, Mode::Mono, Mode::Theater};
  int mode = RequestedMode() == Mode::Mono ? 1 : RequestedMode() == Mode::Theater ? 2 : 0;
  if (ImGui::Combo("Mode##vr", &mode, kModes, 3))
    SetMode(kModeValues[mode]);
  if (ImGui::Button("Recenter view##vr"))
    Xr::Get().RequestRecenter();
  ImGui::SameLine();
  ImGui::TextDisabled("(F9)");

  auto slider = [](const char* label, std::atomic<float>& value, float lo, float hi, const char* format) {
    float v = value.load();
    if (ImGui::SliderFloat(label, &v, lo, hi, format))
      value = v;
  };
  auto check = [](const char* label, std::atomic<bool>& value) {
    bool v = value.load();
    if (ImGui::Checkbox(label, &v))
      value = v;
  };

  ImGui::SeparatorText("World");
  ImGui::SliderInt("Resolution %##vr", &t.resolution, 50, 200, "%d%%");
  slider("World scale##vr", t.world_scale, 0.25f, 4.0f, "%.2f");
  check("Keep camera pitch##vr", t.keep_pitch);

  ImGui::SeparatorText("HUD panel");
  check("HUD on its own panel##vr", t.hud);
  check("HUD follows head##vr", t.hud_follow);
  slider("HUD distance (m)##vr", t.hud_distance, 0.5f, 4.0f, "%.2f");
  slider("HUD width (m)##vr", t.hud_width, 0.4f, 4.0f, "%.2f");
  slider("HUD height (m)##vr", t.hud_height, -1.0f, 1.0f, "%.2f");

  ImGui::SeparatorText("Screen (menus, cutscenes, theater)");
  slider("Screen distance (m)##vr", t.screen_distance, 1.0f, 10.0f, "%.2f");
  slider("Screen width (m)##vr", t.screen_width, 1.0f, 10.0f, "%.2f");

  ImGui::SeparatorText("This menu");
  slider("Menu distance (m)##vr", t.menu_distance, 0.5f, 3.0f, "%.2f");
  slider("Menu size##vr", t.menu_size, 0.5f, 2.5f, "%.2f");

  ImGui::Separator();
  static double saved_at = -10.0;
  if (ImGui::Button("Save VR settings##vr")) {
    SaveConfig();
    saved_at = ImGui::GetTime();
  }
  ImGui::SameLine();
  if (ImGui::GetTime() - saved_at < 3.0)
    ImGui::TextUnformatted("Saved to vr_config.txt");
  else
    ImGui::TextDisabled("L3 + R3 (hold) opens this menu in VR");
}

}  // namespace esvr

#else

namespace esvr {
bool ClipActive() { return false; }
void UpdateSceneKind() {}
bool HudSeparate() { return false; }
bool TraceFrame() { return false; }
void RequestTrace() {}
void Log(const char*, ...) {}
void ClipMatrix(float (*)[16]) {}
void CullClipScale(float*, float*) {}
bool VrRequested() { return false; }
void InitFromPlume(plume::RenderDevice*, plume::RenderCommandQueue*) {}
bool RenderExtent(uint32_t*, uint32_t*) { return false; }
plume::RenderCommandList* WrapCommands(plume::RenderCommandList* list) { return list; }
bool EyeConstants(plume::RenderBufferReference*) { return false; }
void BeforeFrameSubmit(plume::RenderCommandList*, bool) {}
void SubmitPresentImage() {}
bool MenuToHeadset() { return false; }
bool TakeMenuToggle() { return false; }
void DrawVrSettings() {}
}  // namespace esvr

#endif

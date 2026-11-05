// Truly native renderer (notes/native/design.md), milestones M0 and M1.
//
// With ng2_native_d3d off every override below forwards to the recompiled XDK function, so the
// default D3D12 path is untouched. With it on, no command buffer ever reaches the GPU front end:
// the segment kick is dropped, GPU fences complete at once, GPU callbacks run at once, draws and
// resolves are dropped, and swaps present through Vulkan and the SDK presenter.
//
// M1 limits: the library's setters and a few internal helpers still write into their own command
// segment memory. Those segments are recycled and never read by anything.
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/runtime.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/ui/presenter.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/provider.h>

REXCVAR_DEFINE_BOOL(ng2_native_d3d, false, "NG2",
                    "Truly native renderer: no PM4, no command processor (needs gpu_backend=vulkan)");

namespace {

// Guest D3D device fields (findings #96, notes/native/d3d_api.md).
constexpr uint32_t kDevCursor = 48;           // command segment write cursor
constexpr uint32_t kDevGpuBlock = 10896;      // GPU write-back block: +0 fence, +4 read position
constexpr uint32_t kDevFence = 10908;         // next fence value
constexpr uint32_t kDevFenceTag = 14904;      // low two bits go into the read position word
constexpr uint32_t kDevEndCursor = 13428;     // inline EndVertices: cursor = this
constexpr uint32_t kDevFlipRequests = 16548;  // pre-swap callbacks seen
constexpr uint32_t kDevFlips = 16552;         // flips done (callback or vblank handler)
constexpr uint32_t kDevVertexPtr = 13432;
constexpr uint32_t kDevIndexPtr = 13436;
constexpr uint32_t kDevVertexDwords = 13440;
constexpr uint32_t kDevIndexDwords = 13444;

constexpr uint32_t kScratchSize = 32 * 1024 * 1024;
constexpr uint32_t kFramesInFlight = 4;
constexpr uint32_t kOutputWidth = 1280;
constexpr uint32_t kOutputHeight = 720;

struct Stats {
  std::atomic<uint64_t> swaps{0}, draws{0}, begins{0}, clears{0}, resolves{0}, kicks{0}, fences{0},
      callbacks{0}, drops{0}, flip_timeouts{0};
} g_stats;

std::atomic<uint32_t> g_clear_argb{0xFF000000};

bool NativeOn() {
  static const bool on = [] {
    if (!REXCVAR_GET(ng2_native_d3d)) return false;
    const std::string backend = rex::cvar::GetFlagByName("gpu_backend");
    if (backend != "vulkan") {
      REXLOG_ERROR("NG2 NATIVE: ng2_native_d3d needs gpu_backend=vulkan (got {}), staying off",
                   backend);
      return false;
    }
    REXLOG_INFO("NG2 NATIVE: on, no PM4 reaches the command processor");
    return true;
  }();
  return on;
}

uint8_t* Host(uint32_t guest) {
  return REX_KERNEL_STATE()->memory()->TranslateVirtual<uint8_t*>(guest);
}
uint32_t Load(uint32_t guest) { return rex::memory::load_and_swap<uint32_t>(Host(guest)); }
void Store(uint32_t guest, uint32_t value) {
  rex::memory::store_and_swap<uint32_t>(Host(guest), value);
}

// Memory for BeginVertices. The engine fills it and has no End call to return it, so it is a ring
// that wraps; a frame uses far less than its size.
uint32_t ScratchAlloc(uint32_t bytes) {
  static const uint32_t scratch = REX_KERNEL_STATE()->memory()->SystemHeapAlloc(
      kScratchSize, 4096, rex::memory::kSystemHeapPhysical);
  static std::atomic<uint32_t> offset{0};
  if (!scratch || bytes > kScratchSize / 4) return 0;
  const uint32_t size = (bytes + 63) & ~63u;
  uint32_t start = offset.fetch_add(size);
  if (start + size > kScratchSize) {
    start = 0;
    offset.store(size);
  }
  return scratch + start;
}

// ---- Present --------------------------------------------------------------------------------

struct VulkanOutput {
  const rex::ui::vulkan::VulkanDevice* device = nullptr;
  rex::ui::Presenter* presenter = nullptr;
  VkRenderPass render_pass = VK_NULL_HANDLE;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer commands[kFramesInFlight] = {};
  VkFence fences[kFramesInFlight] = {};
  std::unordered_map<VkImageView, VkFramebuffer> framebuffers;
  uint32_t frame = 0;
  bool ready = false;
} g_vk;

bool InitVulkan() {
  auto* graphics = REX_KERNEL_STATE()->emulator()->graphics_system();
  if (!graphics || !graphics->presenter() || !graphics->provider()) return false;
  auto* provider = static_cast<rex::ui::vulkan::VulkanProvider*>(graphics->provider());
  g_vk.device = provider->vulkan_device();
  g_vk.presenter = graphics->presenter();
  const auto& dfn = g_vk.device->functions();
  const VkDevice device = g_vk.device->device();

  VkAttachmentDescription attachment = {};
  attachment.format = rex::ui::vulkan::VulkanPresenter::kGuestOutputFormat;
  attachment.samples = VK_SAMPLE_COUNT_1_BIT;
  attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  attachment.finalLayout = rex::ui::vulkan::VulkanPresenter::kGuestOutputInternalLayout;
  VkAttachmentReference reference = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription subpass = {};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &reference;
  VkSubpassDependency dependencies[2] = {};
  dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
  dependencies[0].dstSubpass = 0;
  dependencies[0].srcStageMask = rex::ui::vulkan::VulkanPresenter::kGuestOutputInternalStageMask;
  dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependencies[0].srcAccessMask = rex::ui::vulkan::VulkanPresenter::kGuestOutputInternalAccessMask;
  dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  dependencies[1].srcSubpass = 0;
  dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
  dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependencies[1].dstStageMask = rex::ui::vulkan::VulkanPresenter::kGuestOutputInternalStageMask;
  dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  dependencies[1].dstAccessMask = rex::ui::vulkan::VulkanPresenter::kGuestOutputInternalAccessMask;
  VkRenderPassCreateInfo pass_info = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  pass_info.attachmentCount = 1;
  pass_info.pAttachments = &attachment;
  pass_info.subpassCount = 1;
  pass_info.pSubpasses = &subpass;
  pass_info.dependencyCount = 2;
  pass_info.pDependencies = dependencies;
  if (dfn.vkCreateRenderPass(device, &pass_info, nullptr, &g_vk.render_pass) != VK_SUCCESS) {
    return false;
  }

  VkCommandPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool_info.queueFamilyIndex = g_vk.device->queue_family_graphics_compute();
  if (dfn.vkCreateCommandPool(device, &pool_info, nullptr, &g_vk.pool) != VK_SUCCESS) return false;
  VkCommandBufferAllocateInfo alloc = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  alloc.commandPool = g_vk.pool;
  alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc.commandBufferCount = kFramesInFlight;
  if (dfn.vkAllocateCommandBuffers(device, &alloc, g_vk.commands) != VK_SUCCESS) return false;
  VkFenceCreateInfo fence_info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
  for (VkFence& fence : g_vk.fences) {
    if (dfn.vkCreateFence(device, &fence_info, nullptr, &fence) != VK_SUCCESS) return false;
  }
  return true;
}

void Present() {
  static const bool usable = [] {
    g_vk.ready = InitVulkan();
    if (!g_vk.ready) REXLOG_ERROR("NG2 NATIVE: Vulkan output setup failed");
    return g_vk.ready;
  }();
  if (!usable) return;

  const uint32_t argb = g_clear_argb.load(std::memory_order_relaxed);
  const uint32_t swap = uint32_t(g_stats.swaps.load(std::memory_order_relaxed));
  g_vk.presenter->RefreshGuestOutput(
      kOutputWidth, kOutputHeight, kOutputWidth, kOutputHeight,
      [argb, swap](rex::ui::Presenter::GuestOutputRefreshContext& context) -> bool {
        auto& output =
            static_cast<rex::ui::vulkan::VulkanPresenter::VulkanGuestOutputRefreshContext&>(context);
        const auto& dfn = g_vk.device->functions();
        const VkDevice device = g_vk.device->device();
        const uint32_t slot = g_vk.frame++ % kFramesInFlight;
        const VkFence fence = g_vk.fences[slot];
        const VkCommandBuffer cb = g_vk.commands[slot];
        dfn.vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
        dfn.vkResetFences(device, 1, &fence);

        VkFramebuffer& framebuffer = g_vk.framebuffers[output.image_view()];
        if (framebuffer == VK_NULL_HANDLE) {
          VkImageView view = output.image_view();
          VkFramebufferCreateInfo info = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
          info.renderPass = g_vk.render_pass;
          info.attachmentCount = 1;
          info.pAttachments = &view;
          info.width = kOutputWidth;
          info.height = kOutputHeight;
          info.layers = 1;
          if (dfn.vkCreateFramebuffer(device, &info, nullptr, &framebuffer) != VK_SUCCESS) {
            return false;
          }
        }

        VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        dfn.vkBeginCommandBuffer(cb, &begin);
        VkClearValue clear = {};
        clear.color.float32[0] = float((argb >> 16) & 0xFF) / 255.0f;
        clear.color.float32[1] = float((argb >> 8) & 0xFF) / 255.0f;
        clear.color.float32[2] = float(argb & 0xFF) / 255.0f;
        clear.color.float32[3] = 1.0f;
        VkRenderPassBeginInfo pass = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        pass.renderPass = g_vk.render_pass;
        pass.framebuffer = framebuffer;
        pass.renderArea.extent = {kOutputWidth, kOutputHeight};
        pass.clearValueCount = 1;
        pass.pClearValues = &clear;
        dfn.vkCmdBeginRenderPass(cb, &pass, VK_SUBPASS_CONTENTS_INLINE);
        // A bar that moves with every swap: frames are reaching the screen even when every clear
        // of the frame is black.
        VkClearAttachment bar = {};
        bar.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        bar.clearValue.color.float32[0] = 0.1f;
        bar.clearValue.color.float32[1] = 0.8f;
        bar.clearValue.color.float32[2] = 0.3f;
        bar.clearValue.color.float32[3] = 1.0f;
        VkClearRect rect = {};
        rect.rect.offset = {int32_t((swap * 8) % (kOutputWidth - 32)), 0};
        rect.rect.extent = {32, 16};
        rect.layerCount = 1;
        dfn.vkCmdClearAttachments(cb, 1, &bar, 1, &rect);
        dfn.vkCmdEndRenderPass(cb);
        dfn.vkEndCommandBuffer(cb);

        VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cb;
        auto queue = g_vk.device->AcquireQueue(g_vk.device->queue_family_graphics_compute(), 0);
        return dfn.vkQueueSubmit(queue.queue(), 1, &submit, fence) == VK_SUCCESS;
      });
}

// The GPU stalls on the swap until the vblank handler flips, and the fence on the next swap makes
// the CPU wait for that: this is the game's frame pacing. Nothing stalls now, so wait here until
// the flips caught up with the swaps. The timeout covers a stopped vblank.
void WaitForFlip(uint32_t dev) {
  const int64_t limit = int64_t(GetTickCount64()) + 100;
  while (Load(dev + kDevFlips) != Load(dev + kDevFlipRequests)) {
    if (int64_t(GetTickCount64()) > limit) {
      g_stats.flip_timeouts.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    SwitchToThread();
  }
}

void LogStats() {
  const uint64_t swaps = g_stats.swaps.load();
  if (swaps % 120 != 0) return;
  REXLOG_INFO(
      "NG2 NATIVE swaps={} draws={} begins={} clears={} resolves={} kicks_dropped={} fences={} "
      "callbacks={} other_pm4_drops={} flip_timeouts={} clear_argb={:08X}",
      swaps, g_stats.draws.load(), g_stats.begins.load(), g_stats.clears.load(),
      g_stats.resolves.load(), g_stats.kicks.load(), g_stats.fences.load(),
      g_stats.callbacks.load(), g_stats.drops.load(), g_stats.flip_timeouts.load(),
      g_clear_argb.load());
}

}  // namespace

#define NG2_IMP(addr) extern "C" REX_FUNC(__imp__sub_##addr)

NG2_IMP(83762240);
NG2_IMP(83761E58);
NG2_IMP(83761488);
NG2_IMP(83761928);
NG2_IMP(8375E290);
NG2_IMP(83762CA8);
NG2_IMP(8376A9F8);
NG2_IMP(8375FB08);
NG2_IMP(8375FCE0);
NG2_IMP(837613A0);
NG2_IMP(8375FC08);
NG2_IMP(8375FF08);
NG2_IMP(8375F428);
NG2_IMP(8375F7C0);

// ---- Draws ----------------------------------------------------------------------------------

// DrawIndexedVertices
extern "C" REX_FUNC(sub_83762240) {
  if (!NativeOn()) return __imp__sub_83762240(ctx, base);
  g_stats.draws.fetch_add(1, std::memory_order_relaxed);
}

// DrawVertices
extern "C" REX_FUNC(sub_83761E58) {
  if (!NativeOn()) return __imp__sub_83761E58(ctx, base);
  g_stats.draws.fetch_add(1, std::memory_order_relaxed);
}

// BeginVertices(dev, prim, count, stride) -> pointer the engine fills. The End is inlined at 72
// sites as dev+48 = dev+13428, so that field keeps the cursor where it is.
extern "C" REX_FUNC(sub_83761488) {
  if (!NativeOn()) return __imp__sub_83761488(ctx, base);
  g_stats.begins.fetch_add(1, std::memory_order_relaxed);
  const uint32_t dev = ctx.r3.u32;
  const uint32_t bytes = ctx.r5.u32 * ctx.r6.u32;
  const uint32_t vertices = ScratchAlloc(bytes);
  Store(dev + kDevEndCursor, Load(dev + kDevCursor));
  Store(dev + kDevVertexPtr, vertices);
  Store(dev + kDevVertexDwords, bytes / 4);
  ctx.r3.u64 = vertices;
}

// BeginIndexedVertices(dev, prim, _, vertex count, index count, flags, stride, out indices,
// out vertices on the stack) -> 0. Flag 4 = 32-bit indices.
extern "C" REX_FUNC(sub_83761928) {
  if (!NativeOn()) return __imp__sub_83761928(ctx, base);
  g_stats.begins.fetch_add(1, std::memory_order_relaxed);
  const uint32_t dev = ctx.r3.u32;
  const uint32_t vertex_bytes = ctx.r6.u32 * ctx.r9.u32;
  const uint32_t index_bytes =
      (ctx.r8.u32 & 4) ? ctx.r7.u32 * 4 : ((ctx.r7.u32 + 1) & ~1u) * 2;
  const uint32_t vertices = ScratchAlloc(vertex_bytes);
  const uint32_t indices = ScratchAlloc(index_bytes);
  Store(dev + kDevEndCursor, Load(dev + kDevCursor));
  Store(dev + kDevVertexPtr, vertices);
  Store(dev + kDevIndexPtr, indices);
  Store(dev + kDevVertexDwords, vertex_bytes / 4);
  Store(dev + kDevIndexDwords, index_bytes / 4);
  Store(ctx.r10.u32, indices);
  Store(Load(ctx.r1.u32 + 84), vertices);
  ctx.r3.u64 = 0;
}

// ---- Targets --------------------------------------------------------------------------------

// Clear(dev, count, rects, flags, color, z, stencil)
extern "C" REX_FUNC(sub_8375E290) {
  if (!NativeOn()) return __imp__sub_8375E290(ctx, base);
  g_stats.clears.fetch_add(1, std::memory_order_relaxed);
  if (ctx.r6.u32 & 0xF0) g_clear_argb.store(ctx.r7.u32, std::memory_order_relaxed);
}

// Resolve(dev, flags, rect, texture, point, level, slice, clear color*, ...)
extern "C" REX_FUNC(sub_83762CA8) {
  if (!NativeOn()) return __imp__sub_83762CA8(ctx, base);
  g_stats.resolves.fetch_add(1, std::memory_order_relaxed);
  const uint32_t color = ctx.r10.u32;
  if ((ctx.r4.u32 & 0x100) && color) {
    uint32_t argb = 0xFF000000;
    for (int i = 0; i < 3; ++i) {
      const uint32_t bits = Load(color + i * 4);
      float value;
      std::memcpy(&value, &bits, 4);
      const float clamped = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
      argb |= uint32_t(clamped * 255.0f + 0.5f) << (16 - i * 8);
    }
    g_clear_argb.store(argb, std::memory_order_relaxed);
  }
}

// ---- Present --------------------------------------------------------------------------------

// Swap: the library keeps its frame bookkeeping. VdSwap's packet lands in segment memory that is
// never submitted.
extern "C" REX_FUNC(sub_8376A9F8) {
  const uint32_t dev = ctx.r3.u32;
  __imp__sub_8376A9F8(ctx, base);
  if (!NativeOn()) return;
  g_stats.swaps.fetch_add(1, std::memory_order_relaxed);
  PPCContext saved = ctx;
  Present();
  ctx = saved;
  WaitForFlip(dev);
  LogStats();
}

// ---- Ring and fences ------------------------------------------------------------------------

// Segment kick: writes an INDIRECT_BUFFER to the primary ring and bumps CP_RB_WPTR. Dropped, which
// is what keeps the command processor empty.
extern "C" REX_FUNC(sub_8375F428) {
  if (!NativeOn()) return __imp__sub_8375F428(ctx, base);
  g_stats.kicks.fetch_add(1, std::memory_order_relaxed);
}

// Emit fence: the packet would have the GPU store the fence and read position in the write-back
// block. Nothing runs later, so they are stored now.
extern "C" REX_FUNC(sub_8375F7C0) {
  if (!NativeOn()) return __imp__sub_8375F7C0(ctx, base);
  const uint32_t dev = ctx.r3.u32;
  const uint32_t fence = Load(dev + kDevFence);
  const uint32_t position = Load(dev + kDevCursor) | (Load(dev + kDevFenceTag) & 3);
  const uint32_t block = Load(dev + kDevGpuBlock);
  __imp__sub_8375F7C0(ctx, base);
  if (block) {
    Store(block, fence);
    Store(block + 4, position);
  }
  g_stats.fences.fetch_add(1, std::memory_order_relaxed);
}

// InsertCallback(dev, cursor, type, callback, context) -> cursor. The GPU would raise an interrupt
// when it reached this point; it runs now, on this thread, so timestamps are real host times.
extern "C" REX_FUNC(sub_8375FCE0) {
  if (!NativeOn()) return __imp__sub_8375FCE0(ctx, base);
  const uint32_t cursor = ctx.r4.u32;
  const uint32_t type = ctx.r5.u32;
  const uint32_t callback = ctx.r6.u32;
  if (((type & 6) == 0 || (type & 2)) && callback) {
    g_stats.callbacks.fetch_add(1, std::memory_order_relaxed);
    PPCContext saved = ctx;
    ctx.r3.u64 = saved.r7.u32;
    rex::runtime::ResolveIndirectFunction(callback)(ctx, base);
    ctx = saved;
  }
  ctx.r3.u64 = cursor;
}

// Shader GPR split: the three INDIRECT_BUFFERs are pipeline drains (#97).
extern "C" REX_FUNC(sub_837613A0) {
  if (!NativeOn()) return __imp__sub_837613A0(ctx, base);
  g_stats.drops.fetch_add(1, std::memory_order_relaxed);
}

// KickOff and BlockOnFence stay as they are: fences complete in sub_8375F7C0, and the kick is what
// recycles segment memory.
extern "C" REX_FUNC(sub_8375FB08) { __imp__sub_8375FB08(ctx, base); }
extern "C" REX_FUNC(sub_8375FC08) { __imp__sub_8375FC08(ctx, base); }

// BlockUntilIdle: nothing is ever outstanding. The kick recycles the segment.
extern "C" REX_FUNC(sub_8375FF08) {
  if (!NativeOn()) return __imp__sub_8375FF08(ctx, base);
  __imp__sub_8375FB08(ctx, base);
}

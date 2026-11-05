// Native Vulkan renderer, see native_renderer.h and notes/native/design.md.
//
// Frame model: everything is recorded on the guest thread that calls D3D. Colour and depth images
// stay in VK_IMAGE_LAYOUT_GENERAL for their whole life, and a full memory barrier separates passes.
// Vertex data lives in a storage buffer that mirrors guest physical memory, so a vertex fetch
// constant's address is already the heap offset the shaders expect; only immediate vertices are
// redirected (to a per-frame area past the mirror). Bytes stay in guest order, shaders swap them.
#include "native_renderer.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/ui/presenter.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/provider.h>

#include "native_shaders.h"
#include "native_textures.h"
#include "shaders/present_frag.h"
#include "shaders/present_vert.h"
#include "shaders/rect_geom.h"

REXCVAR_DEFINE_STRING(ng2_native_shader_pack, "ng2_native_shaders.pack", "NG2",
                      "Native renderer shader pack, relative to the executable");
REXCVAR_DEFINE_INT32(ng2_native_show_resolve, -1, "NG2",
                     "Native renderer debug: present the Nth resolve of the frame instead");
REXCVAR_DEFINE_INT32(ng2_native_dump_frame, 0, "NG2",
                     "Native renderer debug: log the passes of this frame");

namespace ng2::native {
namespace {

using rex::ui::vulkan::VulkanDevice;
using rex::ui::vulkan::VulkanPresenter;

// ---- Guest device layout (findings #96, #119, #124) ------------------------------------------
constexpr uint32_t kDevFetch = 1152;          // 32 x 6 dwords
constexpr uint32_t kDevVsFloat = 1920;        // 256 vec4
constexpr uint32_t kDevPsFloat = 6016;        // 256 vec4
constexpr uint32_t kDevBoolLoop = 10112;      // 8 + 32 dwords
constexpr uint32_t kDevDecl = 11812;
constexpr uint32_t kDevRenderTargets = 12432;  // 4 surfaces
constexpr uint32_t kDevDepthSurface = 12448;
constexpr uint32_t kDevPixelShader = 12684;
constexpr uint32_t kDevVertexShader = 12688;

uint32_t RegOffset(uint32_t reg) {
  if (reg >= 0x2380) return 10832 + (reg - 0x2380) * 4;
  if (reg >= 0x2300) return 10680 + (reg - 0x2300) * 4;
  if (reg >= 0x2280) return 10596 + (reg - 0x2280) * 4;
  if (reg >= 0x2200) return 10548 + (reg - 0x2200) * 4;
  if (reg >= 0x2180) return 10528 + (reg - 0x2180) * 4;
  if (reg >= 0x2100) return 10444 + (reg - 0x2100) * 4;
  if (reg >= 0x2080) return 10432 + (reg - 0x2080) * 4;
  return 10368 + (reg - 0x2000) * 4;
}
constexpr uint32_t kRbSurfaceInfo = 0x2000, kRbColorInfo = 0x2001, kRbDepthInfo = 0x2002,
                   kScissorTl = 0x2081, kScissorBr = 0x2082, kRbColorMask = 0x2104,
                   kRbBlendRed = 0x2105, kRbStencilRefMaskBf = 0x210C, kRbStencilRefMask = 0x210D,
                   kRbAlphaRef = 0x210E, kVportXScale = 0x210F, kRbDepthControl = 0x2200,
                   kRbBlendControl0 = 0x2201, kRbColorControl = 0x2202, kPaSuScModeCntl = 0x2205,
                   kPaClVteCntl = 0x2206, kRbModeControl = 0x2208, kRbBlendControl1 = 0x2209,
                   kPolyOffsetFrontScale = 0x2380;

// SpirvShaderTranslator::kSysFlag_* shifts.
constexpr uint32_t kSysXYDividedByW = 1u << 8, kSysZDividedByW = 1u << 9,
                   kSysWNotReciprocal = 1u << 10, kSysPrimitivePolygonal = 1u << 11,
                   kSysPrimitiveLine = 1u << 12, kSysDepthFloat24 = 1u << 15;
constexpr uint32_t kSysAlphaFuncShift = 16;

constexpr VkDeviceSize kMirrorSize = 512ull * 1024 * 1024;
constexpr VkDeviceSize kImmediateSize = 64ull * 1024 * 1024;
constexpr VkDeviceSize kUploadSize = 256ull * 1024 * 1024;
constexpr VkDeviceSize kUploadAlign = 256;
constexpr uint32_t kFrames = 2;
constexpr uint32_t kDescriptorSetsPerFrame = 8192;
constexpr VkDeviceSize kChunkSize = 256ull * 1024 * 1024;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT_S8_UINT;

uint8_t* Virt(uint32_t guest) {
  return REX_KERNEL_STATE()->memory()->TranslateVirtual<uint8_t*>(guest);
}
uint8_t* Phys(uint32_t guest) {
  return REX_KERNEL_STATE()->memory()->TranslatePhysical<uint8_t*>(guest);
}
uint32_t Load(uint32_t guest) { return rex::memory::load_and_swap<uint32_t>(Virt(guest)); }
uint32_t Reg(uint32_t dev, uint32_t reg) { return Load(dev + RegOffset(reg)); }
float RegFloat(uint32_t dev, uint32_t reg) {
  const uint32_t bits = Reg(dev, reg);
  float value;
  std::memcpy(&value, &bits, 4);
  return value;
}
// The XDK's virtual to physical rule.
uint32_t ToPhysical(uint32_t address) {
  return (address & 0x1FFFFFFF) + ((((address >> 20) + 0x200) & 0x1000) << 0);
}
void SwapDwords(void* dst, const void* src, size_t dwords) {
  auto* d = static_cast<uint32_t*>(dst);
  auto* s = static_cast<const uint32_t*>(src);
  for (size_t i = 0; i < dwords; ++i) d[i] = _byteswap_ulong(s[i]);
}

struct Stats {
  uint64_t drawn = 0, no_vs = 0, no_ps = 0, no_pipeline = 0, no_target = 0, prim_unsupported = 0;
  uint64_t passes = 0, heap_bytes = 0, upload_bytes = 0, textures = 0, texture_misses = 0;
  uint64_t resolves = 0, resolves_skipped = 0, clears = 0, msaa_surfaces = 0, frames = 0;
  uint64_t upload_full = 0, bad_mode = 0, indexed_seen = 0;
} g_s;

#define NG2_VK_FUNCS(F)                                                                       \
  F(vkCreateBuffer) F(vkGetBufferMemoryRequirements) F(vkAllocateMemory) F(vkBindBufferMemory) \
  F(vkMapMemory) F(vkCreateImage) F(vkGetImageMemoryRequirements) F(vkBindImageMemory)         \
  F(vkCreateImageView) F(vkCreateSampler) F(vkCreateDescriptorSetLayout)                       \
  F(vkCreatePipelineLayout) F(vkCreateDescriptorPool) F(vkResetDescriptorPool)                 \
  F(vkAllocateDescriptorSets) F(vkUpdateDescriptorSets) F(vkCreateShaderModule)                \
  F(vkCreateGraphicsPipelines) F(vkCreateCommandPool) F(vkAllocateCommandBuffers)              \
  F(vkBeginCommandBuffer) F(vkEndCommandBuffer) F(vkResetCommandBuffer) F(vkCreateFence)       \
  F(vkWaitForFences) F(vkResetFences) F(vkCmdPipelineBarrier) F(vkCmdBindPipeline)             \
  F(vkCmdBindDescriptorSets) F(vkCmdBindIndexBuffer) F(vkCmdDraw) F(vkCmdDrawIndexed)          \
  F(vkCmdSetViewport) F(vkCmdSetScissor) F(vkCmdSetBlendConstants)                             \
  F(vkCmdSetStencilReference) F(vkCmdSetStencilCompareMask) F(vkCmdSetStencilWriteMask)        \
  F(vkCmdSetDepthBias) F(vkCmdBeginRendering) F(vkCmdEndRendering) F(vkCmdCopyImage)           \
  F(vkCmdBlitImage) F(vkCmdCopyBufferToImage) F(vkCmdClearColorImage)                          \
  F(vkCmdClearDepthStencilImage) F(vkCmdClearAttachments) F(vkCmdWriteTimestamp)               \
  F(vkCmdResetQueryPool) F(vkCreateQueryPool) F(vkGetQueryPoolResults) F(vkCmdBeginRenderPass) \
  F(vkCmdEndRenderPass) F(vkCreateRenderPass) F(vkCreateFramebuffer) F(vkQueueSubmit)

struct Fn {
#define NG2_F(name) PFN_##name name = nullptr;
  NG2_VK_FUNCS(NG2_F)
#undef NG2_F
} vk;

struct Image {
  VkImage image = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;  // Attachment / plain 2D view, full aspect of the first level.
  VkFormat format = VK_FORMAT_UNDEFINED;
  uint32_t width = 0, height = 0, levels = 1;
  bool depth = false;
  std::map<uint32_t, VkImageView> sample_views;  // By swizzle.
};

struct Surface {
  Image image;
  uint32_t guest = 0, base = 0;  // Guest object, EDRAM base (only for aliased clears, #102).
  bool depth = false;
  bool has_pending_clear = false;
  VkClearColorValue pending_clear = {};
};

struct PipelineKey {
  uint64_t vs = 0, ps = 0;
  uint32_t topology = 0, rect = 0, cull = 0, front_cw = 0, depth_bias = 0;
  uint32_t color_formats[4] = {};
  uint32_t depth_format = 0;
  uint32_t color_masks[4] = {};
  uint32_t blend[4] = {};
  uint32_t depth_control = 0;  // With disabled parts zeroed.
  uint32_t set_counts[4] = {};
};

struct Frame {
  VkCommandBuffer upload_cb = VK_NULL_HANDLE, main_cb = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  VkDescriptorPool pool = VK_NULL_HANDLE;
  VkDescriptorSet constants = VK_NULL_HANDLE;
  VkDeviceSize upload_offset = 0, immediate_offset = 0;
  uint8_t* upload_map = nullptr;
  VkDeviceSize upload_base = 0;
  bool timestamps_valid = false;
};

struct PendingImmediate {
  uint32_t guest = 0;
  VkDeviceSize heap_offset = 0;
  uint32_t bytes = 0;
};

struct R {
  const VulkanDevice* device = nullptr;
  rex::ui::Presenter* presenter = nullptr;
  VkDevice dev = VK_NULL_HANDLE;
  VkPhysicalDeviceMemoryProperties memory = {};
  bool usable = false;

  VkBuffer heap = VK_NULL_HANDLE, upload = VK_NULL_HANDLE;
  uint8_t* heap_map = nullptr;
  uint8_t* upload_map = nullptr;
  VkDescriptorSetLayout heap_layout = VK_NULL_HANDLE, constants_layout = VK_NULL_HANDLE;
  VkDescriptorPool static_pool = VK_NULL_HANDLE;
  VkDescriptorSet heap_set = VK_NULL_HANDLE;
  VkCommandPool cmd_pool = VK_NULL_HANDLE;
  VkQueryPool queries = VK_NULL_HANDLE;
  float timestamp_ms = 0, timestamp_period = 1.0f;
  Frame frames[kFrames];
  uint32_t frame_index = 0;
  bool recording = false;
  Frame* f = nullptr;

  // Present.
  VkRenderPass present_pass = VK_NULL_HANDLE;
  VkPipeline present_pipeline = VK_NULL_HANDLE;
  VkPipelineLayout present_layout = VK_NULL_HANDLE;
  VkDescriptorSetLayout present_set_layout = VK_NULL_HANDLE;
  VkSampler present_sampler = VK_NULL_HANDLE;
  std::unordered_map<VkImageView, VkFramebuffer> framebuffers;

  // Device-local chunks; textures are kept for the session.
  VkDeviceMemory chunk = VK_NULL_HANDLE;
  VkDeviceSize chunk_used = 0;
  uint32_t chunk_type = UINT32_MAX;
  uint64_t image_bytes = 0;

  ShaderPack pack;
  std::unordered_map<uint64_t, VkShaderModule> modules;
  std::unordered_map<uint64_t, const NativeShader*> shader_by_key;
  std::unordered_set<uint64_t> missed;
  VkShaderModule rect_gs = VK_NULL_HANDLE;

  std::map<std::pair<uint32_t, uint32_t>, VkDescriptorSetLayout> set_layouts;
  std::map<std::array<uint32_t, 4>, VkPipelineLayout> pipeline_layouts;
  std::unordered_map<uint64_t, std::pair<PipelineKey, VkPipeline>> pipelines;
  std::unordered_map<uint64_t, VkSampler> samplers;

  std::unordered_map<uint64_t, Surface> surfaces;
  std::unordered_map<uint64_t, Image> resolved;  // Key: ResolvedKey.
  std::unordered_map<uint64_t, Image> textures;
  Image white_2d, white_3d, white_cube;
  bool white_uploaded = false;

  // Current pass.
  bool in_pass = false;
  Surface* pass_color[4] = {};
  Surface* pass_depth = nullptr;
  uint32_t pass_width = 0, pass_height = 0;

  std::unordered_set<uint64_t> heap_copied;  // (address, size) copied this frame.
  std::vector<PendingImmediate> pending;

  // Constants reused when nothing changed.
  uint8_t last_bool_loop[160] = {};
  VkDeviceSize last_bool_loop_offset = VK_WHOLE_SIZE;
  uint32_t resolve_index = 0;
  Image* show_override = nullptr;
  bool dumping = false;
} r;

std::recursive_mutex g_mutex;

// ---- Memory ---------------------------------------------------------------------------------

uint32_t FindMemoryType(uint32_t bits, VkMemoryPropertyFlags want) {
  for (uint32_t i = 0; i < r.memory.memoryTypeCount; ++i) {
    if ((bits & (1u << i)) && (r.memory.memoryTypes[i].propertyFlags & want) == want) return i;
  }
  return UINT32_MAX;
}

// Host visible memory, in video memory when the BAR is large enough to hold it.
bool MakeMappedBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buffer, uint8_t*& map) {
  VkBufferCreateInfo info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size = size;
  info.usage = usage;
  if (vk.vkCreateBuffer(r.dev, &info, nullptr, &buffer) != VK_SUCCESS) return false;
  VkMemoryRequirements req;
  vk.vkGetBufferMemoryRequirements(r.dev, buffer, &req);
  VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  alloc.allocationSize = req.size;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  const VkMemoryPropertyFlags kHost =
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  uint32_t type = FindMemoryType(req.memoryTypeBits, kHost | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (type != UINT32_MAX &&
      r.memory.memoryHeaps[r.memory.memoryTypes[type].heapIndex].size > size * 2) {
    alloc.memoryTypeIndex = type;
    if (vk.vkAllocateMemory(r.dev, &alloc, nullptr, &memory) == VK_SUCCESS) {
      REXLOG_INFO("NG2 NATIVE: {} MB buffer in video memory", size >> 20);
    }
  }
  if (memory == VK_NULL_HANDLE) {
    alloc.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, kHost);
    if (vk.vkAllocateMemory(r.dev, &alloc, nullptr, &memory) != VK_SUCCESS) return false;
    REXLOG_INFO("NG2 NATIVE: {} MB buffer in system memory", size >> 20);
  }
  void* mapped = nullptr;
  if (vk.vkBindBufferMemory(r.dev, buffer, memory, 0) != VK_SUCCESS ||
      vk.vkMapMemory(r.dev, memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
    return false;
  }
  map = static_cast<uint8_t*>(mapped);
  return true;
}

bool BindImageMemory(VkImage image) {
  VkMemoryRequirements req;
  vk.vkGetImageMemoryRequirements(r.dev, image, &req);
  const uint32_t type = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  r.image_bytes += req.size;
  VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  alloc.memoryTypeIndex = type;
  if (req.size > kChunkSize / 4) {
    alloc.allocationSize = req.size;
    VkDeviceMemory memory;
    if (vk.vkAllocateMemory(r.dev, &alloc, nullptr, &memory) != VK_SUCCESS) return false;
    return vk.vkBindImageMemory(r.dev, image, memory, 0) == VK_SUCCESS;
  }
  VkDeviceSize offset = (r.chunk_used + req.alignment - 1) & ~(req.alignment - 1);
  if (r.chunk == VK_NULL_HANDLE || r.chunk_type != type || offset + req.size > kChunkSize) {
    alloc.allocationSize = kChunkSize;
    if (vk.vkAllocateMemory(r.dev, &alloc, nullptr, &r.chunk) != VK_SUCCESS) return false;
    r.chunk_type = type;
    offset = 0;
  }
  r.chunk_used = offset + req.size;
  return vk.vkBindImageMemory(r.dev, image, r.chunk, offset) == VK_SUCCESS;
}

bool IsDepthFormat(VkFormat format) { return format == kDepthFormat; }

bool MakeImage(Image& out, VkFormat format, uint32_t width, uint32_t height, uint32_t levels,
               VkImageUsageFlags usage, uint32_t layers = 1, VkImageCreateFlags flags = 0,
               VkImageType type = VK_IMAGE_TYPE_2D) {
  VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.flags = flags;
  info.imageType = type;
  info.format = format;
  info.extent = {width, height, 1};
  info.mipLevels = levels;
  info.arrayLayers = layers;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = usage;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (vk.vkCreateImage(r.dev, &info, nullptr, &out.image) != VK_SUCCESS) return false;
  if (!BindImageMemory(out.image)) return false;
  out.format = format;
  out.width = width;
  out.height = height;
  out.levels = levels;
  out.depth = IsDepthFormat(format);
  if (usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
    VkImageViewCreateInfo view = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = out.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange.aspectMask =
        out.depth ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
                  : VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.layerCount = 1;
    if (vk.vkCreateImageView(r.dev, &view, nullptr, &out.view) != VK_SUCCESS) return false;
  }
  return true;
}

// All image layouts are GENERAL; this brings a new image there.
void ToGeneral(VkCommandBuffer cb, const Image& image, uint32_t layers = 1) {
  VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image.image;
  barrier.subresourceRange.aspectMask = image.depth
                                            ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
                                            : VK_IMAGE_ASPECT_COLOR_BIT;
  barrier.subresourceRange.levelCount = image.levels;
  barrier.subresourceRange.layerCount = layers;
  vk.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                          0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void FullBarrier(VkCommandBuffer cb) {
  VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  vk.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                          0, 1, &barrier, 0, nullptr, 0, nullptr);
}

VkImageView SampleView(Image& image, uint32_t swizzle_key) {
  const auto found = image.sample_views.find(swizzle_key);
  if (found != image.sample_views.end()) return found->second;
  VkImageViewCreateInfo view = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = image.image;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
  view.format = image.format;
  view.components.r = VkComponentSwizzle(swizzle_key & 0xFF);
  view.components.g = VkComponentSwizzle((swizzle_key >> 8) & 0xFF);
  view.components.b = VkComponentSwizzle((swizzle_key >> 16) & 0xFF);
  view.components.a = VkComponentSwizzle((swizzle_key >> 24) & 0xFF);
  view.subresourceRange.aspectMask = image.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
  view.subresourceRange.levelCount = image.levels;
  view.subresourceRange.layerCount = 1;
  VkImageView out = VK_NULL_HANDLE;
  vk.vkCreateImageView(r.dev, &view, nullptr, &out);
  image.sample_views[swizzle_key] = out;
  return out;
}

// ---- Setup ----------------------------------------------------------------------------------

VkShaderModule MakeModule(const uint32_t* code, size_t bytes) {
  VkShaderModuleCreateInfo info = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  info.codeSize = bytes;
  info.pCode = code;
  VkShaderModule module = VK_NULL_HANDLE;
  vk.vkCreateShaderModule(r.dev, &info, nullptr, &module);
  return module;
}

bool InitPresent() {
  VkAttachmentDescription attachment = {};
  attachment.format = VulkanPresenter::kGuestOutputFormat;
  attachment.samples = VK_SAMPLE_COUNT_1_BIT;
  attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  attachment.finalLayout = VulkanPresenter::kGuestOutputInternalLayout;
  VkAttachmentReference reference = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription subpass = {};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &reference;
  VkSubpassDependency dependencies[2] = {};
  dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
  dependencies[0].dstSubpass = 0;
  dependencies[0].srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
  dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependencies[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
  dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  dependencies[1].srcSubpass = 0;
  dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
  dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependencies[1].dstStageMask = VulkanPresenter::kGuestOutputInternalStageMask;
  dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  dependencies[1].dstAccessMask = VulkanPresenter::kGuestOutputInternalAccessMask;
  VkRenderPassCreateInfo pass_info = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  pass_info.attachmentCount = 1;
  pass_info.pAttachments = &attachment;
  pass_info.subpassCount = 1;
  pass_info.pSubpasses = &subpass;
  pass_info.dependencyCount = 2;
  pass_info.pDependencies = dependencies;
  if (vk.vkCreateRenderPass(r.dev, &pass_info, nullptr, &r.present_pass) != VK_SUCCESS) return false;

  VkDescriptorSetLayoutBinding binding = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                          VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
  VkDescriptorSetLayoutCreateInfo set_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_info.bindingCount = 1;
  set_info.pBindings = &binding;
  if (vk.vkCreateDescriptorSetLayout(r.dev, &set_info, nullptr, &r.present_set_layout) != VK_SUCCESS) {
    return false;
  }
  VkPipelineLayoutCreateInfo layout = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout.setLayoutCount = 1;
  layout.pSetLayouts = &r.present_set_layout;
  if (vk.vkCreatePipelineLayout(r.dev, &layout, nullptr, &r.present_layout) != VK_SUCCESS) return false;

  VkSamplerCreateInfo sampler = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
  sampler.addressModeU = sampler.addressModeV = sampler.addressModeW =
      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  vk.vkCreateSampler(r.dev, &sampler, nullptr, &r.present_sampler);

  VkPipelineShaderStageCreateInfo stages[2] = {};
  stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = MakeModule(kPresentVert, sizeof(kPresentVert));
  stages[0].pName = "main";
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = MakeModule(kPresentFrag, sizeof(kPresentFrag));
  stages[1].pName = "main";
  VkPipelineVertexInputStateCreateInfo vertex = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo assembly = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo viewport = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport.viewportCount = viewport.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo raster = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo multisample = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState blend = {};
  blend.colorWriteMask = 0xF;
  VkPipelineColorBlendStateCreateInfo blends = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  blends.attachmentCount = 1;
  blends.pAttachments = &blend;
  const VkDynamicState dynamics[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = 2;
  dynamic.pDynamicStates = dynamics;
  VkGraphicsPipelineCreateInfo info = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  info.stageCount = 2;
  info.pStages = stages;
  info.pVertexInputState = &vertex;
  info.pInputAssemblyState = &assembly;
  info.pViewportState = &viewport;
  info.pRasterizationState = &raster;
  info.pMultisampleState = &multisample;
  info.pColorBlendState = &blends;
  info.pDynamicState = &dynamic;
  info.layout = r.present_layout;
  info.renderPass = r.present_pass;
  return vk.vkCreateGraphicsPipelines(r.dev, VK_NULL_HANDLE, 1, &info, nullptr,
                                      &r.present_pipeline) == VK_SUCCESS;
}

bool Init() {
  auto* graphics = REX_KERNEL_STATE()->emulator()->graphics_system();
  if (!graphics || !graphics->presenter() || !graphics->provider()) return false;
  auto* provider = static_cast<rex::ui::vulkan::VulkanProvider*>(graphics->provider());
  r.device = provider->vulkan_device();
  r.presenter = graphics->presenter();
  r.dev = r.device->device();
  const auto& ifn = r.device->vulkan_instance()->functions();
  ifn.vkGetPhysicalDeviceMemoryProperties(r.device->physical_device(), &r.memory);
  VkPhysicalDeviceProperties device_properties;
  ifn.vkGetPhysicalDeviceProperties(r.device->physical_device(), &device_properties);
  r.timestamp_period = device_properties.limits.timestampPeriod;
#define NG2_F(name)                                                           \
  vk.name = reinterpret_cast<PFN_##name>(ifn.vkGetDeviceProcAddr(r.dev, #name)); \
  if (!vk.name) REXLOG_ERROR("NG2 NATIVE: no device function " #name);
  NG2_VK_FUNCS(NG2_F)
#undef NG2_F
  if (!r.device->properties().dynamicRendering) {
    REXLOG_ERROR("NG2 NATIVE: the device has no dynamic rendering");
    return false;
  }

  wchar_t module_path[MAX_PATH];
  GetModuleFileNameW(nullptr, module_path, MAX_PATH);
  const auto pack_path = std::filesystem::path(module_path).parent_path() /
                         REXCVAR_GET(ng2_native_shader_pack);
  std::string error;
  if (!r.pack.Load(pack_path, &error)) {
    REXLOG_ERROR("NG2 NATIVE: shader pack {}: {}", pack_path.string(), error);
    return false;
  }
  REXLOG_INFO("NG2 NATIVE: {} shaders in the pack", r.pack.size());

  if (!MakeMappedBuffer(kMirrorSize + kImmediateSize * kFrames, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                        r.heap, r.heap_map) ||
      !MakeMappedBuffer(kUploadSize * kFrames,
                        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        r.upload, r.upload_map)) {
    REXLOG_ERROR("NG2 NATIVE: buffer setup failed");
    return false;
  }

  VkDescriptorSetLayoutBinding heap_binding = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                               VK_SHADER_STAGE_ALL_GRAPHICS, nullptr};
  VkDescriptorSetLayoutCreateInfo set_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_info.bindingCount = 1;
  set_info.pBindings = &heap_binding;
  vk.vkCreateDescriptorSetLayout(r.dev, &set_info, nullptr, &r.heap_layout);
  VkDescriptorSetLayoutBinding constant_bindings[5];
  for (uint32_t i = 0; i < 5; ++i) {
    constant_bindings[i] = {i, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1,
                            VK_SHADER_STAGE_ALL_GRAPHICS, nullptr};
  }
  set_info.bindingCount = 5;
  set_info.pBindings = constant_bindings;
  vk.vkCreateDescriptorSetLayout(r.dev, &set_info, nullptr, &r.constants_layout);

  VkDescriptorPoolSize static_sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
                                         {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 5 * kFrames},
                                         {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4096}};
  VkDescriptorPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.maxSets = 1 + kFrames + 4096;
  pool_info.poolSizeCount = 3;
  pool_info.pPoolSizes = static_sizes;
  vk.vkCreateDescriptorPool(r.dev, &pool_info, nullptr, &r.static_pool);
  VkDescriptorSetAllocateInfo alloc = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc.descriptorPool = r.static_pool;
  alloc.descriptorSetCount = 1;
  alloc.pSetLayouts = &r.heap_layout;
  vk.vkAllocateDescriptorSets(r.dev, &alloc, &r.heap_set);
  VkDescriptorBufferInfo heap_info = {r.heap, 0, VK_WHOLE_SIZE};
  VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  write.dstSet = r.heap_set;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  write.pBufferInfo = &heap_info;
  vk.vkUpdateDescriptorSets(r.dev, 1, &write, 0, nullptr);

  VkCommandPoolCreateInfo cmd_pool = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  cmd_pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  cmd_pool.queueFamilyIndex = r.device->queue_family_graphics_compute();
  if (vk.vkCreateCommandPool(r.dev, &cmd_pool, nullptr, &r.cmd_pool) != VK_SUCCESS) return false;
  VkQueryPoolCreateInfo query = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
  query.queryType = VK_QUERY_TYPE_TIMESTAMP;
  query.queryCount = 2 * kFrames;
  vk.vkCreateQueryPool(r.dev, &query, nullptr, &r.queries);

  VkDescriptorPoolSize frame_sizes[] = {
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kDescriptorSetsPerFrame * 8},
      {VK_DESCRIPTOR_TYPE_SAMPLER, kDescriptorSetsPerFrame * 8}};
  for (uint32_t i = 0; i < kFrames; ++i) {
    Frame& f = r.frames[i];
    VkCommandBuffer cbs[2];
    VkCommandBufferAllocateInfo cb_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cb_info.commandPool = r.cmd_pool;
    cb_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cb_info.commandBufferCount = 2;
    if (vk.vkAllocateCommandBuffers(r.dev, &cb_info, cbs) != VK_SUCCESS) return false;
    f.upload_cb = cbs[0];
    f.main_cb = cbs[1];
    VkFenceCreateInfo fence = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vk.vkCreateFence(r.dev, &fence, nullptr, &f.fence);
    VkDescriptorPoolCreateInfo frame_pool = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    frame_pool.maxSets = kDescriptorSetsPerFrame;
    frame_pool.poolSizeCount = 2;
    frame_pool.pPoolSizes = frame_sizes;
    vk.vkCreateDescriptorPool(r.dev, &frame_pool, nullptr, &f.pool);
    f.upload_base = kUploadSize * i;
    f.upload_map = r.upload_map + f.upload_base;

    VkDescriptorSetAllocateInfo set_alloc = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    set_alloc.descriptorPool = r.static_pool;
    set_alloc.descriptorSetCount = 1;
    set_alloc.pSetLayouts = &r.constants_layout;
    vk.vkAllocateDescriptorSets(r.dev, &set_alloc, &f.constants);
    const VkDeviceSize ranges[5] = {sizeof(NativeSystemConstants), 4096, 4096, 160, 768};
    VkDescriptorBufferInfo infos[5];
    VkWriteDescriptorSet writes[5];
    for (uint32_t b = 0; b < 5; ++b) {
      infos[b] = {r.upload, 0, ranges[b]};
      writes[b] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      writes[b].dstSet = f.constants;
      writes[b].dstBinding = b;
      writes[b].descriptorCount = 1;
      writes[b].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
      writes[b].pBufferInfo = &infos[b];
    }
    vk.vkUpdateDescriptorSets(r.dev, 5, writes, 0, nullptr);
  }

  r.rect_gs = MakeModule(kRectGeom, sizeof(kRectGeom));
  const VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  if (!MakeImage(r.white_2d, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, usage)) return false;
  return InitPresent();
}

bool Usable() {
  static const bool usable = [] {
    r.usable = Init();
    if (!r.usable) REXLOG_ERROR("NG2 NATIVE: renderer setup failed");
    return r.usable;
  }();
  return usable;
}

// ---- Frame ----------------------------------------------------------------------------------

void BeginFrame() {
  if (r.recording) return;
  r.f = &r.frames[r.frame_index];
  Frame& f = *r.f;
  vk.vkWaitForFences(r.dev, 1, &f.fence, VK_TRUE, UINT64_MAX);
  vk.vkResetFences(r.dev, 1, &f.fence);
  if (f.timestamps_valid) {
    uint64_t stamps[2];
    if (vk.vkGetQueryPoolResults(r.dev, r.queries, r.frame_index * 2, 2, sizeof(stamps), stamps, 8,
                                 VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
      const float period = r.timestamp_period;
      r.timestamp_ms = float(stamps[1] - stamps[0]) * period * 1e-6f;
    }
  }
  vk.vkResetDescriptorPool(r.dev, f.pool, 0);
  f.upload_offset = 0;
  f.immediate_offset = 0;
  r.heap_copied.clear();
  r.last_bool_loop_offset = VK_WHOLE_SIZE;
  r.resolve_index = 0;
  r.show_override = nullptr;
  VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vk.vkResetCommandBuffer(f.upload_cb, 0);
  vk.vkResetCommandBuffer(f.main_cb, 0);
  vk.vkBeginCommandBuffer(f.upload_cb, &begin);
  vk.vkBeginCommandBuffer(f.main_cb, &begin);
  vk.vkCmdResetQueryPool(f.main_cb, r.queries, r.frame_index * 2, 2);
  vk.vkCmdWriteTimestamp(f.main_cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, r.queries, r.frame_index * 2);
  if (!r.white_uploaded) {
    r.white_uploaded = true;
    ToGeneral(f.upload_cb, r.white_2d);
    VkClearColorValue white = {{1.0f, 1.0f, 1.0f, 1.0f}};
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vk.vkCmdClearColorImage(f.upload_cb, r.white_2d.image, VK_IMAGE_LAYOUT_GENERAL, &white, 1, &range);
  }
  r.recording = true;
}

VkDeviceSize UploadAlloc(VkDeviceSize bytes, VkDeviceSize slack = 8192) {
  Frame& f = *r.f;
  const VkDeviceSize offset = (f.upload_offset + kUploadAlign - 1) & ~(kUploadAlign - 1);
  if (offset + bytes + slack > kUploadSize) {
    ++g_s.upload_full;
    return VK_WHOLE_SIZE;
  }
  f.upload_offset = offset + bytes;
  g_s.upload_bytes += bytes;
  return offset;
}

void EndPass() {
  if (!r.in_pass) return;
  vk.vkCmdEndRendering(r.f->main_cb);
  r.in_pass = false;
  r.pass_width = 0;
}

void ApplyPendingClears(Surface& surface) {
  if (!surface.has_pending_clear) return;
  surface.has_pending_clear = false;
  VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vk.vkCmdClearColorImage(r.f->main_cb, surface.image.image, VK_IMAGE_LAYOUT_GENERAL,
                          &surface.pending_clear, 1, &range);
  FullBarrier(r.f->main_cb);
}

void BeginPass(Surface* const color[4], Surface* depth) {
  bool same = r.in_pass && depth == r.pass_depth;
  for (int i = 0; i < 4 && same; ++i) same = color[i] == r.pass_color[i];
  if (same) return;
  EndPass();
  VkCommandBuffer cb = r.f->main_cb;
  FullBarrier(cb);
  VkRenderingAttachmentInfo attachments[4] = {};
  VkRenderingAttachmentInfo depth_attachment = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  uint32_t width = UINT32_MAX, height = UINT32_MAX, count = 0;
  for (int i = 0; i < 4; ++i) {
    attachments[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    r.pass_color[i] = color[i];
    if (!color[i]) continue;
    ApplyPendingClears(*color[i]);
    attachments[i].imageView = color[i]->image.view;
    attachments[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    attachments[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachments[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    width = std::min(width, color[i]->image.width);
    height = std::min(height, color[i]->image.height);
    count = i + 1;
  }
  r.pass_depth = depth;
  if (depth) {
    depth_attachment.imageView = depth->image.view;
    depth_attachment.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    width = std::min(width, depth->image.width);
    height = std::min(height, depth->image.height);
  }
  VkRenderingInfo info = {VK_STRUCTURE_TYPE_RENDERING_INFO};
  info.renderArea.extent = {width, height};
  info.layerCount = 1;
  info.colorAttachmentCount = count;
  info.pColorAttachments = attachments;
  info.pDepthAttachment = depth ? &depth_attachment : nullptr;
  info.pStencilAttachment = depth ? &depth_attachment : nullptr;
  vk.vkCmdBeginRendering(cb, &info);
  r.in_pass = true;
  r.pass_width = width;
  r.pass_height = height;
  ++g_s.passes;
  if (r.dumping) {
    REXLOG_INFO("NG2 NATIVE PASS {}x{} rt0={:08X} fmt={} depth={:08X}", width, height,
                color[0] ? color[0]->guest : 0, color[0] ? int(color[0]->image.format) : 0,
                depth ? depth->guest : 0);
  }
}

// ---- Surfaces -------------------------------------------------------------------------------

VkFormat ColorFormat(uint32_t format) {
  switch (format) {
    case 0:
    case 1:
      return VK_FORMAT_R8G8B8A8_UNORM;
    case 2:
    case 10:
      return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 3:  // 7e3: no direct host format
    case 12:
      return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 4:
      return VK_FORMAT_R16G16_SNORM;
    case 5:
      return VK_FORMAT_R16G16B16A16_SNORM;
    case 6:
      return VK_FORMAT_R16G16_SFLOAT;
    case 7:
      return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 14:
      return VK_FORMAT_R32_SFLOAT;
    case 15:
      return VK_FORMAT_R32G32_SFLOAT;
    default:
      return VK_FORMAT_R8G8B8A8_UNORM;
  }
}

Surface* GetSurface(uint32_t guest, bool depth) {
  if (!guest) return nullptr;
  const uint32_t surface_info = Load(guest + 24);
  const uint32_t info = Load(guest + 28);
  const uint32_t size = Load(guest + 36);
  // The game makes new surface objects for the same memory, so the object is not the identity:
  // format, base and size are.
  const uint64_t key = (uint64_t(info) << 32) ^ (uint64_t(size) << 1) ^ uint64_t(depth);
  const auto found = r.surfaces.find(key);
  if (found != r.surfaces.end()) return &found->second;
  const uint32_t width = ((size >> 18) & 0x3FFF) + 1;
  const uint32_t height = ((size >> 3) & 0x7FFF) + 1;
  const uint32_t msaa = (surface_info >> 16) & 3;
  if (msaa) ++g_s.msaa_surfaces;
  Surface& s = r.surfaces[key];
  s.guest = guest;
  s.depth = depth;
  s.base = info & 0xFFF;
  const VkFormat format = depth ? kDepthFormat : ColorFormat((info >> 16) & 0xF);
  const VkImageUsageFlags usage =
      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
      (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
  if (!MakeImage(s.image, format, width, height, 1, usage)) {
    REXLOG_ERROR("NG2 NATIVE: surface {:08X} {}x{} failed", guest, width, height);
    r.surfaces.erase(key);
    return nullptr;
  }
  EndPass();
  ToGeneral(r.f->main_cb, s.image);
  // A new target starts out cleared: nothing on the guest can rely on its old contents.
  const VkImageSubresourceRange color_range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (depth) {
    VkClearDepthStencilValue value = {0.0f, 0};
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
    vk.vkCmdClearDepthStencilImage(r.f->main_cb, s.image.image, VK_IMAGE_LAYOUT_GENERAL, &value, 1, &range);
  } else {
    VkClearColorValue value = {};
    vk.vkCmdClearColorImage(r.f->main_cb, s.image.image, VK_IMAGE_LAYOUT_GENERAL, &value, 1, &color_range);
  }
  FullBarrier(r.f->main_cb);
  REXLOG_INFO("NG2 NATIVE surface {:08X} {}x{} {} info {:08X} surface_info {:08X} msaa {}", guest,
              width, height, depth ? "depth" : "color", info, surface_info, msaa);
  return &s;
}

// ---- Shaders --------------------------------------------------------------------------------

const NativeShader* FindShader(uint32_t ucode_guest_virtual_base, uint32_t bytes, uint32_t decl) {
  const uint64_t key = (uint64_t(ucode_guest_virtual_base) << 32) ^ (uint64_t(decl) << 2) ^ bytes;
  const auto found = r.shader_by_key.find(key);
  if (found != r.shader_by_key.end()) return found->second;
  const uint64_t hash = XXH3_64bits(Phys(ucode_guest_virtual_base), bytes);
  const NativeShader* shader = r.pack.Find(hash);
  if (!shader && r.missed.insert(hash).second) {
    REXLOG_WARN("NG2 NATIVE: shader miss hash {:016X} ucode {:08X} {} bytes decl {:08X}", hash,
                ucode_guest_virtual_base, bytes, decl);
  }
  r.shader_by_key[key] = shader;
  return shader;
}

VkShaderModule ModuleFor(const NativeShader& shader) {
  VkShaderModule& module = r.modules[shader.ucode_hash];
  if (module == VK_NULL_HANDLE) module = MakeModule(shader.spirv.data(), shader.spirv.size() * 4);
  return module;
}

VkDescriptorSetLayout TextureSetLayout(uint32_t images, uint32_t samplers) {
  auto& layout = r.set_layouts[{images, samplers}];
  if (layout != VK_NULL_HANDLE) return layout;
  std::vector<VkDescriptorSetLayoutBinding> bindings;
  for (uint32_t i = 0; i < images + samplers; ++i) {
    bindings.push_back({i, i < images ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER,
                        1, VK_SHADER_STAGE_ALL_GRAPHICS, nullptr});
  }
  VkDescriptorSetLayoutCreateInfo info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  info.bindingCount = uint32_t(bindings.size());
  info.pBindings = bindings.data();
  vk.vkCreateDescriptorSetLayout(r.dev, &info, nullptr, &layout);
  return layout;
}

VkPipelineLayout PipelineLayoutFor(const std::array<uint32_t, 4>& counts) {
  VkPipelineLayout& layout = r.pipeline_layouts[counts];
  if (layout != VK_NULL_HANDLE) return layout;
  const VkDescriptorSetLayout sets[4] = {r.heap_layout, r.constants_layout,
                                         TextureSetLayout(counts[0], counts[1]),
                                         TextureSetLayout(counts[2], counts[3])};
  VkPipelineLayoutCreateInfo info = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  info.setLayoutCount = 4;
  info.pSetLayouts = sets;
  vk.vkCreatePipelineLayout(r.dev, &info, nullptr, &layout);
  return layout;
}

VkBlendFactor BlendFactor(uint32_t f) {
  static const VkBlendFactor kMap[17] = {
      VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ZERO,
      VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, VK_BLEND_FACTOR_SRC_ALPHA,
      VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_DST_COLOR,
      VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_DST_ALPHA,
      VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, VK_BLEND_FACTOR_CONSTANT_COLOR,
      VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR, VK_BLEND_FACTOR_CONSTANT_ALPHA,
      VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA, VK_BLEND_FACTOR_SRC_ALPHA_SATURATE};
  return f < 17 ? kMap[f] : VK_BLEND_FACTOR_ZERO;
}
VkBlendOp BlendOp(uint32_t op) {
  switch (op) {
    case 1:
      return VK_BLEND_OP_SUBTRACT;
    case 2:
      return VK_BLEND_OP_MIN;
    case 3:
      return VK_BLEND_OP_MAX;
    case 4:
      return VK_BLEND_OP_REVERSE_SUBTRACT;
    default:
      return VK_BLEND_OP_ADD;
  }
}

VkPipeline GetPipeline(const PipelineKey& key, const NativeShader* vs, const NativeShader* ps,
                       VkPipelineLayout layout) {
  const uint64_t hash = XXH3_64bits(&key, sizeof(key));
  const auto found = r.pipelines.find(hash);
  if (found != r.pipelines.end() && !std::memcmp(&found->second.first, &key, sizeof(key))) {
    return found->second.second;
  }
  VkPipelineShaderStageCreateInfo stages[3] = {};
  uint32_t stage_count = 0;
  auto add = [&](VkShaderStageFlagBits stage, VkShaderModule module) {
    stages[stage_count].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[stage_count].stage = stage;
    stages[stage_count].module = module;
    stages[stage_count].pName = "main";
    ++stage_count;
  };
  add(VK_SHADER_STAGE_VERTEX_BIT, ModuleFor(*vs));
  if (key.rect) add(VK_SHADER_STAGE_GEOMETRY_BIT, r.rect_gs);
  if (ps) add(VK_SHADER_STAGE_FRAGMENT_BIT, ModuleFor(*ps));

  VkPipelineVertexInputStateCreateInfo vertex = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo assembly = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  assembly.topology = VkPrimitiveTopology(key.topology);
  assembly.primitiveRestartEnable = key.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP ||
                                    key.topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
  VkPipelineViewportStateCreateInfo viewport = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport.viewportCount = viewport.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo raster = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.cullMode = key.cull;
  raster.frontFace = key.front_cw ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.depthBiasEnable = key.depth_bias;
  raster.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo multisample = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  const uint32_t dc = key.depth_control;
  VkPipelineDepthStencilStateCreateInfo depth = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  depth.depthTestEnable = (dc >> 1) & 1;
  depth.depthWriteEnable = (dc >> 2) & 1;
  depth.depthCompareOp = VkCompareOp((dc >> 4) & 7);
  depth.stencilTestEnable = dc & 1;
  depth.front.compareOp = VkCompareOp((dc >> 8) & 7);
  depth.front.failOp = VkStencilOp((dc >> 11) & 7);
  depth.front.passOp = VkStencilOp((dc >> 14) & 7);
  depth.front.depthFailOp = VkStencilOp((dc >> 17) & 7);
  if ((dc >> 7) & 1) {
    depth.back.compareOp = VkCompareOp((dc >> 20) & 7);
    depth.back.failOp = VkStencilOp((dc >> 23) & 7);
    depth.back.passOp = VkStencilOp((dc >> 26) & 7);
    depth.back.depthFailOp = VkStencilOp((dc >> 29) & 7);
  } else {
    depth.back = depth.front;
  }

  VkPipelineColorBlendAttachmentState blends[4] = {};
  VkFormat formats[4] = {};
  uint32_t color_count = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    formats[i] = VkFormat(key.color_formats[i]);
    if (formats[i] != VK_FORMAT_UNDEFINED) color_count = i + 1;
    blends[i].colorWriteMask = key.color_masks[i];
    const uint32_t b = key.blend[i];
    const uint32_t src = b & 31, op = (b >> 5) & 7, dst = (b >> 8) & 31;
    const uint32_t asrc = (b >> 16) & 31, aop = (b >> 21) & 7, adst = (b >> 24) & 31;
    const bool blendable = formats[i] != VK_FORMAT_R32_SFLOAT && formats[i] != VK_FORMAT_R32G32_SFLOAT;
    blends[i].blendEnable =
        blendable && !(src == 1 && dst == 0 && op == 0 && asrc == 1 && adst == 0 && aop == 0);
    blends[i].srcColorBlendFactor = BlendFactor(src);
    blends[i].dstColorBlendFactor = BlendFactor(dst);
    blends[i].colorBlendOp = BlendOp(op);
    blends[i].srcAlphaBlendFactor = BlendFactor(asrc);
    blends[i].dstAlphaBlendFactor = BlendFactor(adst);
    blends[i].alphaBlendOp = BlendOp(aop);
  }
  VkPipelineColorBlendStateCreateInfo blend = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  blend.attachmentCount = color_count;
  blend.pAttachments = blends;
  const VkDynamicState dynamics[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                     VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_DEPTH_BIAS,
                                     VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
                                     VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
                                     VK_DYNAMIC_STATE_STENCIL_REFERENCE};
  VkPipelineDynamicStateCreateInfo dynamic = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = 7;
  dynamic.pDynamicStates = dynamics;
  VkPipelineRenderingCreateInfo rendering = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rendering.colorAttachmentCount = color_count;
  rendering.pColorAttachmentFormats = formats;
  rendering.depthAttachmentFormat = VkFormat(key.depth_format);
  rendering.stencilAttachmentFormat = VkFormat(key.depth_format);

  VkGraphicsPipelineCreateInfo info = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  info.pNext = &rendering;
  info.stageCount = stage_count;
  info.pStages = stages;
  info.pVertexInputState = &vertex;
  info.pInputAssemblyState = &assembly;
  info.pViewportState = &viewport;
  info.pRasterizationState = &raster;
  info.pMultisampleState = &multisample;
  info.pDepthStencilState = &depth;
  info.pColorBlendState = &blend;
  info.pDynamicState = &dynamic;
  info.layout = layout;
  VkPipeline pipeline = VK_NULL_HANDLE;
  const VkResult result =
      vk.vkCreateGraphicsPipelines(r.dev, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
  if (result != VK_SUCCESS) {
    REXLOG_ERROR("NG2 NATIVE: pipeline failed ({}) vs {:016X} ps {:016X}", int(result),
                 vs->ucode_hash, ps ? ps->ucode_hash : 0);
    pipeline = VK_NULL_HANDLE;
  }
  r.pipelines[hash] = {key, pipeline};
  return pipeline;
}

// ---- Textures -------------------------------------------------------------------------------

VkFormat ResolvedTextureFormat(uint32_t xenos_format) {
  switch (xenos_format) {
    case 2:
      return VK_FORMAT_R8_UNORM;
    case 10:
      return VK_FORMAT_R8G8_UNORM;
    case 7:
      return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 13:
    case 25:
      return VK_FORMAT_R16G16_UNORM;
    case 21:
    case 26:
      return VK_FORMAT_R16G16B16A16_UNORM;
    case 22:
    case 23:
      return kDepthFormat;
    case 31:
      return VK_FORMAT_R16G16_SFLOAT;
    case 32:
      return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 36:
      return VK_FORMAT_R32_SFLOAT;
    case 37:
      return VK_FORMAT_R32G32_SFLOAT;
    default:
      return VK_FORMAT_R8G8B8A8_UNORM;
  }
}

// A texture and its format; the game resolves depth and colour to the same memory (shadow moments).
uint64_t ResolvedKey(const uint32_t words[6]) {
  return (uint64_t(ToPhysical(words[1] & 0xFFFFF000)) << 8) | (words[1] & 0x3F);
}

// From the swizzle in fetch dword 3: 0-3 RGBA, 4 zero, 5 one.
uint32_t FetchSwizzleKey(uint32_t word3) {
  static const uint8_t kMap[8] = {VK_COMPONENT_SWIZZLE_R,    VK_COMPONENT_SWIZZLE_G,
                                  VK_COMPONENT_SWIZZLE_B,    VK_COMPONENT_SWIZZLE_A,
                                  VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE,
                                  VK_COMPONENT_SWIZZLE_ONE,  VK_COMPONENT_SWIZZLE_ONE};
  const uint32_t s = (word3 >> 1) & 0xFFF;
  return uint32_t(kMap[s & 7]) | (uint32_t(kMap[(s >> 3) & 7]) << 8) |
         (uint32_t(kMap[(s >> 6) & 7]) << 16) | (uint32_t(kMap[(s >> 9) & 7]) << 24);
}

VkImageView LoadStaticTexture(const uint32_t words[6]) {
  TextureDesc desc;
  if (!ParseTextureFetch(words, desc)) return VK_NULL_HANDLE;
  const uint32_t swizzle =
      desc.vk_swizzle[0] | (desc.vk_swizzle[1] << 8) | (desc.vk_swizzle[2] << 16) |
      (desc.vk_swizzle[3] << 24);
  const uint64_t key = XXH3_64bits(words, 8) ^ (uint64_t(words[2]) << 32) ^ words[5];
  auto found = r.textures.find(key);
  if (found != r.textures.end()) return SampleView(found->second, swizzle);
  if (!desc.supported || !desc.base_address) {
    if (!g_s.texture_misses++ || r.missed.insert(key).second) {
      REXLOG_WARN("NG2 NATIVE: texture unsupported format {} {}x{} base {:08X} words {:08X} {:08X} {:08X}",
                  desc.xenos_format, desc.width, desc.height, desc.base_address, words[0], words[1], words[5]);
    }
    return VK_NULL_HANDLE;
  }
  LoadedTexture loaded;
  if (!LoadTexture(desc, Phys(desc.base_address), desc.base_bytes,
                   desc.mip_address ? Phys(desc.mip_address) : nullptr, desc.mip_bytes, loaded) ||
      loaded.mips.empty()) {
    ++g_s.texture_misses;
    return VK_NULL_HANDLE;
  }
  const VkDeviceSize offset = UploadAlloc(loaded.data.size());
  if (offset == VK_WHOLE_SIZE) return VK_NULL_HANDLE;
  Image image;
  const uint32_t first = loaded.mips.front().level;
  if (!MakeImage(image, VkFormat(desc.vk_format), loaded.mips.front().width, loaded.mips.front().height,
                 uint32_t(loaded.mips.size()),
                 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
    ++g_s.texture_misses;
    return VK_NULL_HANDLE;
  }
  std::memcpy(r.f->upload_map + offset, loaded.data.data(), loaded.data.size());
  ToGeneral(r.f->upload_cb, image);
  std::vector<VkBufferImageCopy> copies;
  for (const LoadedMip& mip : loaded.mips) {
    VkBufferImageCopy copy = {};
    copy.bufferOffset = r.f->upload_base + offset + mip.offset;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip.level - first, 0, 1};
    copy.imageExtent = {mip.width, mip.height, 1};
    copies.push_back(copy);
  }
  vk.vkCmdCopyBufferToImage(r.f->upload_cb, r.upload, image.image, VK_IMAGE_LAYOUT_GENERAL,
                            uint32_t(copies.size()), copies.data());
  ++g_s.textures;
  auto& stored = r.textures[key];
  stored = std::move(image);
  return SampleView(stored, swizzle);
}

VkImageView TextureView(uint32_t dev, uint32_t slot) {
  uint32_t words[6];
  SwapDwords(words, Virt(dev + kDevFetch + slot * 24), 6);
  const auto found = r.resolved.find(ResolvedKey(words));
  if (found != r.resolved.end()) return SampleView(found->second, FetchSwizzleKey(words[3]));
  const VkImageView view = LoadStaticTexture(words);
  if (view != VK_NULL_HANDLE) return view;
  return SampleView(r.white_2d, 0);
}

VkSampler GetSampler(const NativeSamplerBinding& binding, const uint32_t words[6]) {
  const uint32_t clamp = (words[0] >> 10) & 0x1FF;
  uint32_t mag = (words[3] >> 19) & 3, min = (words[3] >> 21) & 3, mip = (words[3] >> 23) & 3;
  if (binding.mag_filter != 3) mag = binding.mag_filter;
  if (binding.min_filter != 3) min = binding.min_filter;
  if (binding.mip_filter != 3) mip = binding.mip_filter;
  const uint64_t key = clamp | (mag << 9) | (min << 11) | (mip << 13);
  VkSampler& sampler = r.samplers[key];
  if (sampler != VK_NULL_HANDLE) return sampler;
  auto address = [](uint32_t c) {
    switch (c) {
      case 0:
        return VK_SAMPLER_ADDRESS_MODE_REPEAT;
      case 1:
        return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
      case 2:
      case 3:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      case 6:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
      default:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
  };
  VkSamplerCreateInfo info = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  info.magFilter = mag == 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
  info.minFilter = min == 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
  info.mipmapMode = mip == 1 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
  info.addressModeU = address(clamp & 7);
  info.addressModeV = address((clamp >> 3) & 7);
  info.addressModeW = address((clamp >> 6) & 7);
  info.maxLod = 16.0f;
  info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
  vk.vkCreateSampler(r.dev, &info, nullptr, &sampler);
  return sampler;
}

VkDescriptorSet MakeTextureSet(uint32_t dev, const NativeShader& shader, uint32_t images,
                               uint32_t samplers) {
  VkDescriptorSetLayout layout = TextureSetLayout(images, samplers);
  VkDescriptorSetAllocateInfo alloc = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc.descriptorPool = r.f->pool;
  alloc.descriptorSetCount = 1;
  alloc.pSetLayouts = &layout;
  VkDescriptorSet set = VK_NULL_HANDLE;
  if (vk.vkAllocateDescriptorSets(r.dev, &alloc, &set) != VK_SUCCESS) return VK_NULL_HANDLE;
  VkDescriptorImageInfo infos[64];
  VkWriteDescriptorSet writes[64];
  uint32_t count = 0;
  for (uint32_t i = 0; i < images && count < 64; ++i, ++count) {
    infos[count] = {};
    infos[count].imageView = TextureView(dev, shader.textures[i].fetch_constant);
    if (r.dumping) {
      uint32_t w[6];
      SwapDwords(w, Virt(dev + kDevFetch + shader.textures[i].fetch_constant * 24), 6);
      REXLOG_INFO("NG2 NATIVE   tex slot {} dim {} words {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} resolved {}",
                  shader.textures[i].fetch_constant, shader.textures[i].dimension, w[0], w[1], w[2],
                  w[3], w[4], w[5], r.resolved.count(ResolvedKey(w)));
    }
    infos[count].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    writes[count] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[count].dstSet = set;
    writes[count].dstBinding = i;
    writes[count].descriptorCount = 1;
    writes[count].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[count].pImageInfo = &infos[count];
  }
  for (uint32_t i = 0; i < samplers && count < 64; ++i, ++count) {
    uint32_t words[6];
    SwapDwords(words, Virt(dev + kDevFetch + shader.samplers[i].fetch_constant * 24), 6);
    infos[count] = {};
    infos[count].sampler = GetSampler(shader.samplers[i], words);
    writes[count] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[count].dstSet = set;
    writes[count].dstBinding = images + i;
    writes[count].descriptorCount = 1;
    writes[count].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    writes[count].pImageInfo = &infos[count];
  }
  vk.vkUpdateDescriptorSets(r.dev, count, writes, 0, nullptr);
  return set;
}

// ---- Draw -----------------------------------------------------------------------------------

// Literal constants of a shader object (`def`). The library loads these straight from the shader's
// memory to the GPU (sub_83768568), so they never reach the device struct's shadow.
struct LiteralConstants {
  uint32_t physical[256] = {};  // Guest physical address of the vec4; 0 = use the device value.
};

const LiteralConstants& GetLiterals(uint32_t object, uint32_t info, uint32_t base) {
  static std::unordered_map<uint64_t, LiteralConstants> cache;
  const uint64_t key = (uint64_t(object) << 32) | info;
  const auto found = cache.find(key);
  if (found != cache.end()) return found->second;
  LiteralConstants& out = cache[key];
  const uint32_t table = Load(info + 20);
  if (!table) return out;
  uint32_t entry = table + info + 20;
  const uint32_t end = Load(table + info + 16) + entry;
  while (entry < end) {
    const uint32_t start = rex::memory::load_and_swap<uint16_t>(Virt(entry));
    const uint32_t dwords = rex::memory::load_and_swap<uint16_t>(Virt(entry + 2));
    if (!dwords) break;
    const uint32_t source = ToPhysical(Load(entry + 4) + base);
    entry += 8;
    for (uint32_t i = 0; i < dwords / 4; ++i) {
      out.physical[(start + i) & 255] = source + i * 16;
    }
  }
  return out;
}

// Vec4s the shader reads, in order; the shader indexes this packed list.
VkDeviceSize PackFloatConstants(uint32_t guest, const uint64_t bitmap[4], uint32_t count,
                                const LiteralConstants& literals) {
  const VkDeviceSize offset = UploadAlloc(std::max<uint32_t>(count, 1) * 16);
  if (offset == VK_WHOLE_SIZE) return offset;
  auto* out = reinterpret_cast<uint32_t*>(r.f->upload_map + offset);
  const auto* in = reinterpret_cast<const uint32_t*>(Virt(guest));
  uint32_t written = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    uint64_t bits = bitmap[i];
    while (bits) {
      const uint32_t index = i * 64 + uint32_t(_tzcnt_u64(bits));
      bits &= bits - 1;
      const uint32_t literal = literals.physical[index];
      SwapDwords(out + written * 4, literal ? Phys(literal) : reinterpret_cast<const uint8_t*>(in + index * 4), 4);
      ++written;
    }
  }
  return offset;
}

void CopyToHeap(uint32_t address, uint32_t bytes) {
  if (!bytes || address + uint64_t(bytes) > kMirrorSize) return;
  const uint64_t key = (uint64_t(address) << 32) | bytes;
  if (!r.heap_copied.insert(key).second) return;
  std::memcpy(r.heap_map + address, Phys(address), bytes);
  g_s.heap_bytes += bytes;
}

}  // namespace

void RendererDrainImmediate() {
  std::lock_guard lock(g_mutex);
  for (const PendingImmediate& p : r.pending) {
    std::memcpy(r.heap_map + p.heap_offset, Virt(p.guest), p.bytes);
    if (r.dumping) {
      uint32_t v[12] = {};
      SwapDwords(v, Virt(p.guest), std::min<uint32_t>(p.bytes / 4, 12));
      float* f = reinterpret_cast<float*>(v);
      REXLOG_INFO("NG2 NATIVE   imm {} bytes: {} {} {} {} {} {} {} {} {} {} {} {}", p.bytes, f[0], f[1],
                  f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11]);
    }
  }
  r.pending.clear();
}

void RendererDraw(const DrawCall& d) {
  std::lock_guard lock(g_mutex);
  if (!Usable()) return;
  RendererDrainImmediate();
  BeginFrame();
  const uint32_t dev = d.dev;
  if (d.indexed) ++g_s.indexed_seen;

  const uint32_t mode = Reg(dev, kRbModeControl) & 7;
  if (mode != 4 && mode != 5) {  // 4 colour + depth, 5 depth only.
    if (!g_s.bad_mode++) REXLOG_WARN("NG2 NATIVE: draw in mode {} indexed {}", mode, int(d.indexed));
    return;
  }

  const uint32_t vs_object = Load(dev + kDevVertexShader);
  const uint32_t ps_object = Load(dev + kDevPixelShader);
  const uint32_t decl = Load(dev + kDevDecl);
  if (!vs_object) {
    ++g_s.no_vs;
    return;
  }
  // sub_83768758: variant 1 is the depth-only form of a VS when no pixel shader is bound.
  const uint32_t variant = (!ps_object && (Load(vs_object + 872) & 0x20)) ? 1 : 0;
  const uint32_t vs_header = vs_object + Load(vs_object + (variant + 112) * 8);
  const uint32_t vs_ucode = Load(vs_header + 872) + Load(vs_object + 32);
  const NativeShader* vs = FindShader(ToPhysical(vs_ucode), Load(vs_header + 876), decl);
  if (!vs) {
    ++g_s.no_vs;
    return;
  }
  const NativeShader* ps = nullptr;
  if (ps_object) {
    const uint32_t ps_header = ps_object + Load(ps_object + 64);
    const uint32_t ps_ucode = Load(ps_header + 40) + Load(ps_object + 24);
    ps = FindShader(ToPhysical(ps_ucode), Load(ps_header + 44), 0);
    if (!ps) {
      ++g_s.no_ps;
      return;
    }
  }

  // Targets.
  Surface* color[4] = {};
  const uint32_t color_mask = Reg(dev, kRbColorMask);
  if (mode == 4) {
    for (uint32_t i = 0; i < 4; ++i) {
      color[i] = GetSurface(Load(dev + kDevRenderTargets + i * 4), false);
    }
  }
  Surface* depth = GetSurface(Load(dev + kDevDepthSurface), true);
  if (!color[0] && !color[1] && !color[2] && !color[3] && !depth) {
    ++g_s.no_target;
    return;
  }

  PipelineKey key = {};
  key.vs = vs->ucode_hash;
  key.ps = ps ? ps->ucode_hash : 0;
  bool polygonal = true, line = false;
  std::vector<uint16_t> quad_indices;
  switch (d.prim) {
    case 1:
      key.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
      polygonal = false;
      break;
    case 2:
      key.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
      polygonal = false;
      line = true;
      break;
    case 3:
      key.topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
      polygonal = false;
      line = true;
      break;
    case 4:
      key.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      break;
    case 5:
      key.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
      break;
    case 6:
      key.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
      break;
    case 8:
      key.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      key.rect = 1;
      break;
    case 13:
      key.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      if (d.indexed) {
        ++g_s.prim_unsupported;
        return;
      }
      for (uint32_t q = 0; q + 3 < d.count; q += 4) {
        const uint16_t v = uint16_t(d.first_vertex + q);
        for (uint16_t offset : {0, 1, 2, 0, 2, 3}) quad_indices.push_back(uint16_t(v + offset));
      }
      break;
    default:
      ++g_s.prim_unsupported;
      return;
  }

  const uint32_t mode_cntl = Reg(dev, kPaSuScModeCntl);
  key.cull = 0;
  if (mode_cntl & 1) key.cull |= VK_CULL_MODE_FRONT_BIT;
  if (mode_cntl & 2) key.cull |= VK_CULL_MODE_BACK_BIT;
  key.front_cw = (mode_cntl >> 2) & 1;
  key.depth_bias = (mode_cntl >> 11) & 3 ? 1 : 0;
  for (uint32_t i = 0; i < 4; ++i) {
    if (color[i]) {
      key.color_formats[i] = uint32_t(color[i]->image.format);
      key.color_masks[i] = (color_mask >> (i * 4)) & 0xF;
      key.blend[i] = Reg(dev, i == 0 ? kRbBlendControl0 : kRbBlendControl1 + i - 1);
    }
  }
  uint32_t depth_control = Reg(dev, kRbDepthControl);
  if (!depth) depth_control = 0;
  if (!(depth_control & 2)) depth_control &= ~uint32_t(0x74);
  if (!(depth_control & 1)) depth_control &= 0xFFu & ~uint32_t(0x80);
  key.depth_control = depth_control;
  key.depth_format = depth ? uint32_t(depth->image.format) : 0;
  const uint32_t vs_images = uint32_t(vs->textures.size()), vs_samplers = uint32_t(vs->samplers.size());
  const uint32_t ps_images = ps ? uint32_t(ps->textures.size()) : 0;
  const uint32_t ps_samplers = ps ? uint32_t(ps->samplers.size()) : 0;
  key.set_counts[0] = vs_images;
  key.set_counts[1] = vs_samplers;
  key.set_counts[2] = ps_images;
  key.set_counts[3] = ps_samplers;
  const std::array<uint32_t, 4> counts = {vs_images, vs_samplers, ps_images, ps_samplers};
  VkPipelineLayout layout = PipelineLayoutFor(counts);
  VkPipeline pipeline = GetPipeline(key, vs, ps, layout);
  if (pipeline == VK_NULL_HANDLE) {
    ++g_s.no_pipeline;
    return;
  }

  // Textures come first: a first use records into the upload command buffer.
  const VkDescriptorSet vs_set =
      vs_images + vs_samplers ? MakeTextureSet(dev, *vs, vs_images, vs_samplers) : VK_NULL_HANDLE;
  const VkDescriptorSet ps_set =
      ps_images + ps_samplers ? MakeTextureSet(dev, *ps, ps_images, ps_samplers) : VK_NULL_HANDLE;

  // Vertex data.
  uint32_t fetch[32 * 6];
  SwapDwords(fetch, Virt(dev + kDevFetch), 32 * 6);
  for (uint32_t slot : vs->vertex_fetch_slots) {
    const uint32_t* words = fetch + slot * 2;
    if ((words[0] & 3) != 3) continue;
    CopyToHeap(words[0] & 0xFFFFFFFC, ((words[1] >> 2) & 0xFFFFFF) * 4);
  }
  if (d.immediate) {
    const uint32_t bytes = d.immediate_dwords * 4;
    Frame& f = *r.f;
    if (f.immediate_offset + bytes > kImmediateSize) return;
    const VkDeviceSize heap_offset = kMirrorSize + kImmediateSize * r.frame_index + f.immediate_offset;
    f.immediate_offset += (bytes + 15) & ~15u;
    r.pending.push_back({d.immediate_guest, heap_offset, bytes});
    fetch[95 * 2] = uint32_t(heap_offset) | 3;
    fetch[95 * 2 + 1] = (d.immediate_dwords << 2) | 2;
  }

  // Constants.
  NativeSystemConstants system = {};
  const uint32_t vte = Reg(dev, kPaClVteCntl);
  const float pass_w = float(r.pass_width), pass_h = float(r.pass_height);
  uint32_t width = UINT32_MAX, height = UINT32_MAX;
  for (Surface* s : color) {
    if (s) {
      width = std::min(width, s->image.width);
      height = std::min(height, s->image.height);
    }
  }
  if (depth) {
    width = std::min(width, depth->image.width);
    height = std::min(height, depth->image.height);
  }
  (void)pass_w;
  (void)pass_h;
  const float w = float(width), h = float(height);
  const float sx = (vte & 1) ? RegFloat(dev, kVportXScale) : 1.0f;
  const float ox = (vte & 2) ? RegFloat(dev, kVportXScale + 1) : 0.0f;
  const float sy = (vte & 4) ? RegFloat(dev, kVportXScale + 2) : 1.0f;
  const float oy = (vte & 8) ? RegFloat(dev, kVportXScale + 3) : 0.0f;
  const float sz = (vte & 16) ? RegFloat(dev, kVportXScale + 4) : 1.0f;
  const float oz = (vte & 32) ? RegFloat(dev, kVportXScale + 5) : 0.0f;
  system.ndc_scale[0] = 2.0f * sx / w;
  system.ndc_offset[0] = 2.0f * ox / w - 1.0f;
  system.ndc_scale[1] = 2.0f * sy / h;
  system.ndc_offset[1] = 2.0f * oy / h - 1.0f;
  system.ndc_scale[2] = sz;
  system.ndc_offset[2] = oz;
  uint32_t flags = 0;
  if (vte & (1 << 8)) flags |= kSysXYDividedByW;
  if (vte & (1 << 9)) flags |= kSysZDividedByW;
  if (vte & (1 << 10)) flags |= kSysWNotReciprocal;
  if (polygonal) flags |= kSysPrimitivePolygonal;
  if (line) flags |= kSysPrimitiveLine;
  if ((Reg(dev, kRbDepthInfo) >> 16) & 1) flags |= kSysDepthFloat24;
  const uint32_t color_control = Reg(dev, kRbColorControl);
  const uint32_t alpha_func = (color_control & 8) ? (color_control & 7) : 7;
  flags |= alpha_func << kSysAlphaFuncShift;
  system.flags = flags;
  system.line_loop_closing_index = UINT32_MAX;
  system.vertex_index_max = 0xFFFFFF;
  system.alpha_test_reference = RegFloat(dev, kRbAlphaRef);
  for (uint32_t i = 0; i < 4; ++i) {
    system.color_exp_bias[i] = 1.0f;
    if (!color[i]) continue;
    if (color[i]->image.format == VK_FORMAT_R16G16_SNORM ||
        color[i]->image.format == VK_FORMAT_R16G16B16A16_SNORM) {
      system.color_exp_bias[i] = 1.0f / 32.0f;
    }
  }
  const VkDeviceSize system_offset = UploadAlloc(sizeof(system));
  const VkDeviceSize vs_offset =
      PackFloatConstants(dev + kDevVsFloat, vs->float_bitmap, vs->float_constant_count,
                         GetLiterals(vs_object, vs_object + 872, Load(vs_object + 32)));
  const VkDeviceSize ps_offset =
      ps ? PackFloatConstants(dev + kDevPsFloat, ps->float_bitmap, ps->float_constant_count,
                              GetLiterals(ps_object, ps_object + 40, Load(ps_object + 24)))
         : vs_offset;
  const VkDeviceSize fetch_offset = UploadAlloc(sizeof(fetch));
  uint8_t bool_loop[160];
  SwapDwords(bool_loop, Virt(dev + kDevBoolLoop), 40);
  if (r.last_bool_loop_offset == VK_WHOLE_SIZE || std::memcmp(bool_loop, r.last_bool_loop, 160)) {
    r.last_bool_loop_offset = UploadAlloc(160);
    if (r.last_bool_loop_offset != VK_WHOLE_SIZE) {
      std::memcpy(r.f->upload_map + r.last_bool_loop_offset, bool_loop, 160);
      std::memcpy(r.last_bool_loop, bool_loop, 160);
    }
  }
  if (system_offset == VK_WHOLE_SIZE || vs_offset == VK_WHOLE_SIZE || ps_offset == VK_WHOLE_SIZE ||
      fetch_offset == VK_WHOLE_SIZE || r.last_bool_loop_offset == VK_WHOLE_SIZE) {
    return;
  }
  std::memcpy(r.f->upload_map + system_offset, &system, sizeof(system));
  std::memcpy(r.f->upload_map + fetch_offset, fetch, sizeof(fetch));

  // Indices: guest byte order to host, per draw.
  VkDeviceSize index_offset = 0;
  uint32_t index_count = 0;
  VkIndexType index_type = VK_INDEX_TYPE_UINT16;
  if (d.indexed) {
    const uint32_t bytes = d.count * (d.index_32 ? 4 : 2);
    index_offset = UploadAlloc(bytes);
    if (index_offset == VK_WHOLE_SIZE) return;
    uint8_t* out = r.f->upload_map + index_offset;
    const uint8_t* in = Virt(d.index_guest);
    if (d.index_32) {
      index_type = VK_INDEX_TYPE_UINT32;
      SwapDwords(out, in, d.count);
    } else {
      auto* dst = reinterpret_cast<uint16_t*>(out);
      auto* src = reinterpret_cast<const uint16_t*>(in);
      for (uint32_t i = 0; i < d.count; ++i) dst[i] = _byteswap_ushort(src[i]);
    }
    index_count = d.count;
  } else if (!quad_indices.empty()) {
    const uint32_t bytes = uint32_t(quad_indices.size() * 2);
    index_offset = UploadAlloc(bytes);
    if (index_offset == VK_WHOLE_SIZE) return;
    std::memcpy(r.f->upload_map + index_offset, quad_indices.data(), bytes);
    index_count = uint32_t(quad_indices.size());
  }

  BeginPass(color, depth);
  VkCommandBuffer cb = r.f->main_cb;
  vk.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  const VkDescriptorSet sets[2] = {r.heap_set, r.f->constants};
  const uint32_t base = uint32_t(r.f->upload_base);
  const uint32_t offsets[5] = {base + uint32_t(system_offset), base + uint32_t(vs_offset),
                               base + uint32_t(ps_offset), base + uint32_t(r.last_bool_loop_offset),
                               base + uint32_t(fetch_offset)};
  vk.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 2, sets, 5, offsets);
  if (vs_set != VK_NULL_HANDLE) {
    vk.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 2, 1, &vs_set, 0, nullptr);
  }
  if (ps_set != VK_NULL_HANDLE) {
    vk.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 3, 1, &ps_set, 0, nullptr);
  }
  VkViewport viewport = {0.0f, 0.0f, w, h, 0.0f, 1.0f};
  vk.vkCmdSetViewport(cb, 0, 1, &viewport);
  const uint32_t tl = Reg(dev, kScissorTl), br = Reg(dev, kScissorBr);
  int32_t x0 = tl & 0x7FFF, y0 = (tl >> 16) & 0x7FFF;
  int32_t x1 = br & 0x7FFF, y1 = (br >> 16) & 0x7FFF;
  x1 = std::min(x1, int32_t(width));
  y1 = std::min(y1, int32_t(height));
  VkRect2D scissor = {{x0, y0}, {uint32_t(std::max(x1 - x0, 0)), uint32_t(std::max(y1 - y0, 0))}};
  vk.vkCmdSetScissor(cb, 0, 1, &scissor);
  float blend_constants[4];
  for (uint32_t i = 0; i < 4; ++i) blend_constants[i] = RegFloat(dev, kRbBlendRed + i);
  vk.vkCmdSetBlendConstants(cb, blend_constants);
  const uint32_t ref_front = Reg(dev, kRbStencilRefMask);
  const uint32_t ref_back = (depth_control & 0x80) ? Reg(dev, kRbStencilRefMaskBf) : ref_front;
  vk.vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_BIT, ref_front & 0xFF);
  vk.vkCmdSetStencilReference(cb, VK_STENCIL_FACE_BACK_BIT, ref_back & 0xFF);
  vk.vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_BIT, (ref_front >> 8) & 0xFF);
  vk.vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_BACK_BIT, (ref_back >> 8) & 0xFF);
  vk.vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_BIT, (ref_front >> 16) & 0xFF);
  vk.vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_BACK_BIT, (ref_back >> 16) & 0xFF);
  const float bias_scale = RegFloat(dev, kPolyOffsetFrontScale) * (1.0f / 16.0f);
  const float bias_constant = RegFloat(dev, kPolyOffsetFrontScale + 1) * float(1 << 24);
  vk.vkCmdSetDepthBias(cb, bias_constant, 0.0f, bias_scale);
  if (index_count) {
    vk.vkCmdBindIndexBuffer(cb, r.upload, r.f->upload_base + index_offset, index_type);
    vk.vkCmdDrawIndexed(cb, index_count, 1, 0, 0, 0);
  } else {
    vk.vkCmdDraw(cb, d.count, 1, d.first_vertex, 0);
  }
  ++g_s.drawn;
  if (r.dumping) {
    std::string slots;
    for (uint32_t slot : vs->vertex_fetch_slots) slots += fmt::format("{}({:08X} {:08X}) ", slot, fetch[slot * 2], fetch[slot * 2 + 1]);
    REXLOG_INFO("NG2 NATIVE   vs flags {:X} floats {} slots {} ps floats {} sys flags {:X} ndc {} {} {} {}",
                vs->flags, vs->float_constant_count, slots, ps ? ps->float_constant_count : 0, flags,
                system.ndc_scale[0], system.ndc_offset[0], system.ndc_scale[1], system.ndc_offset[1]);
    if (vs->float_constant_count) {
      auto* c = reinterpret_cast<float*>(r.f->upload_map + vs_offset);
      REXLOG_INFO("NG2 NATIVE   vs c: {} {} {} {} / {} {} {} {}", c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7]);
    }
    if (ps && ps->float_constant_count) {
      auto* c = reinterpret_cast<float*>(r.f->upload_map + ps_offset);
      std::string text;
      for (uint32_t i = 0; i < ps->float_constant_count; ++i) text += fmt::format("{} ", c[i * 4]);
      REXLOG_INFO("NG2 NATIVE   ps c.x: {} bitmap {:X} {:X}", text, ps->float_bitmap[0], ps->float_bitmap[1]);
    }
    REXLOG_INFO(
        "NG2 NATIVE DRAW prim {} n {} idx {} vs {:016X} ps {:016X} {}x{} vport {:.1f} {:.1f} {:.1f} "
        "{:.1f} vte {:04X} depthctl {:08X} sc {} {} {} {} cull {} imm {} blend {:08X} mask {:04X}",
        d.prim, d.count, int(d.indexed), vs->ucode_hash, ps ? ps->ucode_hash : 0, width, height, sx,
        ox, sy, oy, vte, depth_control, x0, y0, x1, y1, key.cull, int(d.immediate), key.blend[0],
        color_mask);
  }
}

// ---- Clears and resolves --------------------------------------------------------------------

namespace {

Surface* CurrentSurface(uint32_t dev, uint32_t index) {
  return GetSurface(Load(dev + kDevRenderTargets + index * 4), false);
}

void ClearColorSurface(Surface& s, const VkClearColorValue& value) {
  VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vk.vkCmdClearColorImage(r.f->main_cb, s.image.image, VK_IMAGE_LAYOUT_GENERAL, &value, 1, &range);
  s.has_pending_clear = false;
  // Surfaces at the same EDRAM base saw this clear on the guest (#102).
  for (auto& [key, other] : r.surfaces) {
    if (&other != &s && !other.depth && other.base == s.base) {
      other.has_pending_clear = true;
      other.pending_clear = value;
    }
  }
}

void ClearDepthSurface(Surface& s, float z, uint32_t stencil) {
  VkClearDepthStencilValue value = {z, stencil};
  VkImageSubresourceRange range = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
  vk.vkCmdClearDepthStencilImage(r.f->main_cb, s.image.image, VK_IMAGE_LAYOUT_GENERAL, &value, 1, &range);
}

Image* GetResolvedImage(uint32_t texture) {
  uint32_t words[6];
  SwapDwords(words, Virt(texture + 28), 6);
  const uint32_t base = ToPhysical(words[1] & 0xFFFFF000);
  const uint32_t format = words[1] & 0x3F;
  const uint64_t key = ResolvedKey(words);
  const uint32_t width = (words[2] & 0x1FFF) + 1, height = ((words[2] >> 13) & 0x1FFF) + 1;
  const uint32_t levels = std::max<uint32_t>(((words[4] >> 6) & 0xF) + 1, 1);
  const VkFormat vk_format = ResolvedTextureFormat(format);
  auto found = r.resolved.find(key);
  if (found != r.resolved.end() && found->second.width == width && found->second.height == height) {
    return &found->second;
  }
  Image image;
  const VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  if (!MakeImage(image, vk_format, width, height, levels, usage)) return nullptr;
  ToGeneral(r.f->main_cb, image);
  REXLOG_INFO("NG2 NATIVE resolve target {:08X} {}x{} format {} levels {}", base, width, height,
              format, levels);
  Image& stored = r.resolved[key];
  stored = std::move(image);
  return &stored;
}

bool SameTexelBlock(VkFormat a, VkFormat b) {
  auto bytes = [](VkFormat f) {
    switch (f) {
      case VK_FORMAT_R8_UNORM:
        return 1;
      case VK_FORMAT_R8G8_UNORM:
        return 2;
      case VK_FORMAT_R8G8B8A8_UNORM:
      case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
      case VK_FORMAT_R16G16_SNORM:
      case VK_FORMAT_R16G16_UNORM:
      case VK_FORMAT_R16G16_SFLOAT:
      case VK_FORMAT_R32_SFLOAT:
        return 4;
      case VK_FORMAT_R16G16B16A16_SFLOAT:
      case VK_FORMAT_R16G16B16A16_SNORM:
      case VK_FORMAT_R16G16B16A16_UNORM:
      case VK_FORMAT_R32G32_SFLOAT:
        return 8;
      default:
        return 0;
    }
  };
  return bytes(a) && bytes(a) == bytes(b);
}

}  // namespace

void RendererClear(uint32_t dev, uint32_t rect_count, uint32_t rects, uint32_t flags, uint32_t argb,
                   float z, uint32_t stencil) {
  std::lock_guard lock(g_mutex);
  if (!Usable()) return;
  RendererDrainImmediate();
  BeginFrame();
  ++g_s.clears;
  EndPass();
  (void)rect_count;
  (void)rects;
  VkClearColorValue value = {};
  value.float32[0] = float((argb >> 16) & 0xFF) / 255.0f;
  value.float32[1] = float((argb >> 8) & 0xFF) / 255.0f;
  value.float32[2] = float(argb & 0xFF) / 255.0f;
  value.float32[3] = float(argb >> 24) / 255.0f;
  FullBarrier(r.f->main_cb);
  for (uint32_t i = 0; i < 4; ++i) {
    if (!(flags & (0x10u << i))) continue;
    if (Surface* s = CurrentSurface(dev, i)) ClearColorSurface(*s, value);
  }
  if (flags & 3) {
    if (Surface* s = GetSurface(Load(dev + kDevDepthSurface), true)) ClearDepthSurface(*s, z, stencil);
  }
  FullBarrier(r.f->main_cb);
  if (r.dumping) REXLOG_INFO("NG2 NATIVE CLEAR flags {:X} argb {:08X} z {}", flags, argb, z);
}

void RendererResolve(uint32_t dev, uint32_t flags, uint32_t source_rect, uint32_t texture,
                     uint32_t dest_point, uint32_t level, uint32_t clear_color, float clear_z,
                     uint32_t clear_stencil) {
  std::lock_guard lock(g_mutex);
  if (!Usable()) return;
  RendererDrainImmediate();
  BeginFrame();
  ++g_s.resolves;
  EndPass();
  VkCommandBuffer cb = r.f->main_cb;
  FullBarrier(cb);
  const uint32_t which = flags & 7;
  const bool from_depth = which == 4;
  Surface* source = from_depth ? GetSurface(Load(dev + kDevDepthSurface), true)
                               : CurrentSurface(dev, which);
  if (source) ApplyPendingClears(*source);
  Image* dest = texture ? GetResolvedImage(texture) : nullptr;
  if (source && dest) {
    int32_t x0 = 0, y0 = 0, x1 = int32_t(source->image.width), y1 = int32_t(source->image.height);
    if (source_rect) {
      x0 = int32_t(Load(source_rect));
      y0 = int32_t(Load(source_rect + 4));
      x1 = int32_t(Load(source_rect + 8));
      y1 = int32_t(Load(source_rect + 12));
    }
    int32_t dx = 0, dy = 0;
    if (dest_point) {
      dx = int32_t(Load(dest_point));
      dy = int32_t(Load(dest_point + 4));
    }
    const int32_t dest_w = int32_t(std::max(dest->width >> level, 1u));
    const int32_t dest_h = int32_t(std::max(dest->height >> level, 1u));
    x1 = std::min({x1, int32_t(source->image.width), x0 + dest_w - dx});
    y1 = std::min({y1, int32_t(source->image.height), y0 + dest_h - dy});
    if (x1 > x0 && y1 > y0) {
      const VkImageAspectFlags aspect = from_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
      if (from_depth != dest->depth) {
        ++g_s.resolves_skipped;
      } else if (source->image.format == dest->format ||
                 SameTexelBlock(source->image.format, dest->format)) {
        VkImageCopy copy = {};
        copy.srcSubresource = {aspect, 0, 0, 1};
        copy.srcOffset = {x0, y0, 0};
        copy.dstSubresource = {aspect, level, 0, 1};
        copy.dstOffset = {dx, dy, 0};
        copy.extent = {uint32_t(x1 - x0), uint32_t(y1 - y0), 1};
        vk.vkCmdCopyImage(cb, source->image.image, VK_IMAGE_LAYOUT_GENERAL, dest->image,
                          VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
      } else {
        VkImageBlit blit = {};
        blit.srcSubresource = {aspect, 0, 0, 1};
        blit.srcOffsets[0] = {x0, y0, 0};
        blit.srcOffsets[1] = {x1, y1, 1};
        blit.dstSubresource = {aspect, level, 0, 1};
        blit.dstOffsets[0] = {dx, dy, 0};
        blit.dstOffsets[1] = {dx + x1 - x0, dy + y1 - y0, 1};
        vk.vkCmdBlitImage(cb, source->image.image, VK_IMAGE_LAYOUT_GENERAL, dest->image,
                          VK_IMAGE_LAYOUT_GENERAL, 1, &blit, VK_FILTER_NEAREST);
      }
      if (r.dumping) {
        REXLOG_INFO("NG2 NATIVE RESOLVE #{} rt {} {} -> {:08X} fmt {} -> {} rect {} {} {} {}",
                    r.resolve_index, which, source->guest, Load(texture + 28 + 4), int(source->image.format),
                    int(dest->format), x0, y0, x1, y1);
      }
      if (int32_t(r.resolve_index) == REXCVAR_GET(ng2_native_show_resolve)) r.show_override = dest;
      ++r.resolve_index;
    }
  } else {
    ++g_s.resolves_skipped;
  }
  FullBarrier(cb);
  if ((flags & 0x100) && !from_depth && clear_color) {
    VkClearColorValue value = {};
    for (uint32_t i = 0; i < 4; ++i) {
      const uint32_t bits = Load(clear_color + i * 4);
      std::memcpy(&value.float32[i], &bits, 4);
    }
    if (Surface* s = CurrentSurface(dev, which < 4 ? which : 0)) ClearColorSurface(*s, value);
  }
  if (flags & 0x200) {
    if (Surface* s = GetSurface(Load(dev + kDevDepthSurface), true)) ClearDepthSurface(*s, clear_z, clear_stencil);
  }
  FullBarrier(cb);
}

void RendererSwap(uint32_t dev, uint32_t front_texture) {
  std::lock_guard lock(g_mutex);
  if (!Usable()) return;
  RendererDrainImmediate();
  BeginFrame();
  EndPass();
  ++g_s.frames;
  r.dumping = uint64_t(REXCVAR_GET(ng2_native_dump_frame)) == g_s.frames;
  (void)dev;

  Image* front = r.show_override;
  if (!front && front_texture) {
    uint32_t words[6];
    SwapDwords(words, Virt(front_texture + 28), 6);
    const auto found = r.resolved.find(ResolvedKey(words));
    if (found != r.resolved.end()) front = &found->second;
  }
  Frame& f = *r.f;
  FullBarrier(f.main_cb);
  vk.vkEndCommandBuffer(f.upload_cb);
  bool submitted = false;
  auto submit = [&]() {
    vk.vkCmdWriteTimestamp(f.main_cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, r.queries, r.frame_index * 2 + 1);
    vk.vkEndCommandBuffer(f.main_cb);
    VkCommandBuffer cbs[2] = {f.upload_cb, f.main_cb};
    VkSubmitInfo info = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    info.commandBufferCount = 2;
    info.pCommandBuffers = cbs;
    auto queue = r.device->AcquireQueue(r.device->queue_family_graphics_compute(), 0);
    submitted = vk.vkQueueSubmit(queue.queue(), 1, &info, f.fence) == VK_SUCCESS;
    f.timestamps_valid = submitted;
  };
  const uint32_t width = front ? front->width : 1280, height = front ? front->height : 720;
  r.presenter->RefreshGuestOutput(
      width, height, 1280, 720,
      [&](rex::ui::Presenter::GuestOutputRefreshContext& context) -> bool {
        auto& output = static_cast<VulkanPresenter::VulkanGuestOutputRefreshContext&>(context);
        VkCommandBuffer cb = f.main_cb;
        VkFramebuffer& framebuffer = r.framebuffers[output.image_view()];
        if (framebuffer == VK_NULL_HANDLE) {
          VkImageView view = output.image_view();
          VkFramebufferCreateInfo info = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
          info.renderPass = r.present_pass;
          info.attachmentCount = 1;
          info.pAttachments = &view;
          info.width = width;
          info.height = height;
          info.layers = 1;
          if (vk.vkCreateFramebuffer(r.dev, &info, nullptr, &framebuffer) != VK_SUCCESS) return false;
        }
        VkClearValue clear = {};
        VkRenderPassBeginInfo pass = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        pass.renderPass = r.present_pass;
        pass.framebuffer = framebuffer;
        pass.renderArea.extent = {width, height};
        pass.clearValueCount = 1;
        pass.pClearValues = &clear;
        vk.vkCmdBeginRenderPass(cb, &pass, VK_SUBPASS_CONTENTS_INLINE);
        if (front) {
          VkDescriptorSetAllocateInfo alloc = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
          alloc.descriptorPool = r.static_pool;
          alloc.descriptorSetCount = 1;
          alloc.pSetLayouts = &r.present_set_layout;
          static std::unordered_map<VkImageView, VkDescriptorSet> sets;
          VkImageView sample = SampleView(*front, 0);
          VkDescriptorSet& set = sets[sample];
          if (set == VK_NULL_HANDLE) {
            vk.vkAllocateDescriptorSets(r.dev, &alloc, &set);
            VkDescriptorImageInfo image = {r.present_sampler, sample, VK_IMAGE_LAYOUT_GENERAL};
            VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = set;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &image;
            vk.vkUpdateDescriptorSets(r.dev, 1, &write, 0, nullptr);
          }
          VkViewport viewport = {0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f};
          VkRect2D scissor = {{0, 0}, {width, height}};
          vk.vkCmdSetViewport(cb, 0, 1, &viewport);
          vk.vkCmdSetScissor(cb, 0, 1, &scissor);
          vk.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, r.present_pipeline);
          vk.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, r.present_layout, 0, 1, &set, 0, nullptr);
          vk.vkCmdDraw(cb, 3, 1, 0, 0);
        }
        vk.vkCmdEndRenderPass(cb);
        submit();
        return submitted;
      });
  if (!submitted) submit();
  r.recording = false;
  r.frame_index = (r.frame_index + 1) % kFrames;
}

void RendererLogStats(uint64_t swaps) {
  std::lock_guard lock(g_mutex);
  if (!r.usable) return;
  REXLOG_INFO("NG2 VK GPU frame time: {:.2f} ms", r.timestamp_ms);
  if (swaps % 240) return;
  const float n = float(std::max<uint64_t>(g_s.frames, 1));
  REXLOG_INFO(
      "NG2 NATIVE frames={} drawn/f={:.1f} passes/f={:.1f} no_vs={} no_ps={} no_pipe={} no_target={} "
      "bad_prim={} heap_mb/f={:.1f} upload_mb/f={:.1f} textures={} tex_miss={} resolves/f={:.1f} "
      "skipped={} msaa={} upload_full={} pipelines={} surfaces={} resolved={} image_mb={} bad_mode={} "
      "indexed={}",
      g_s.frames, float(g_s.drawn) / n, float(g_s.passes) / n, g_s.no_vs, g_s.no_ps, g_s.no_pipeline,
      g_s.no_target, g_s.prim_unsupported, float(g_s.heap_bytes) / n / 1048576.0f,
      float(g_s.upload_bytes) / n / 1048576.0f, g_s.textures, g_s.texture_misses,
      float(g_s.resolves) / n, g_s.resolves_skipped, g_s.msaa_surfaces, g_s.upload_full,
      r.pipelines.size(), r.surfaces.size(), r.resolved.size(), r.image_bytes >> 20, g_s.bad_mode,
      g_s.indexed_seen);
}

}  // namespace ng2::native

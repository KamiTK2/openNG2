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
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <set>
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
#include <rex/ui/vulkan/mem_alloc.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/provider.h>

#include "native_shaders.h"
#include "native_textures.h"
#include "shaders/present_frag.h"
#include "shaders/present_vert.h"
#include "shaders/rect_geom.h"

REXCVAR_DEFINE_STRING(ng2_native_shader_pack, "ng2_native_shaders.pack", "NG2",
                      "Native renderer shader pack, relative to the executable");
REXCVAR_DEFINE_BOOL(ng2_native_log_resolves, false, "NG2", "NG2 DIAG: log resolves that clear");
REXCVAR_DEFINE_BOOL(ng2_native_msaa_depth_clear, true, "NG2",
                    "Native renderer: clear the 2x session depth on a full colour clear (#129)");
REXCVAR_DEFINE_BOOL(ng2_native_alias_depth_clear, true, "NG2",
                    "Native renderer: a colour clear also clears depth surfaces placed inside it");
REXCVAR_DEFINE_INT32(ng2_native_texture_mb, 2048, "NG2",
                     "Native renderer: game texture memory budget in MB; least recently used are freed");
REXCVAR_DEFINE_INT32(ng2_native_show_resolve, -1, "NG2",
                     "Native renderer debug: present the Nth resolve of the frame instead");
REXCVAR_DEFINE_STRING(ng2_native_nodepth_ps, "", "NG2", "NG2 DIAG: pixel shader hashes drawn with no depth test");
REXCVAR_DEFINE_INT32(ng2_native_skip_from, 0, "NG2", "NG2 DIAG: first draw of ng2_native_skip_ps to skip in a frame");
REXCVAR_DEFINE_INT32(ng2_native_skip_to, 100000, "NG2", "NG2 DIAG: end of the range");
REXCVAR_DEFINE_STRING(ng2_native_skip_ps, "", "NG2",
                      "Native renderer debug: skip draws with these pixel shader hashes");
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

// Per-frame ring areas. Vertex streams and immediate vertices are copied here each frame.
constexpr VkDeviceSize kHeapSize = 128ull * 1024 * 1024;
constexpr uint64_t kStreamSlack = 64 * 1024;
// Constants and index data the GPU reads directly: video memory, so small.
constexpr VkDeviceSize kUploadSize = 48ull * 1024 * 1024;
// Texture bytes, only ever a copy source: system memory.
constexpr VkDeviceSize kStagingSize = 256ull * 1024 * 1024;
constexpr VkDeviceSize kUploadAlign = 256;
constexpr uint32_t kFrames = 2;
constexpr uint32_t kDescriptorSetsPerFrame = 8192;
constexpr VkDeviceSize kChunkSize = 256ull * 1024 * 1024;
constexpr uint32_t kDevGammaRamp = 14988;     // D3DGAMMARAMP: 256 u16 red, green, blue
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
  uint64_t bind_translations = 0, drawn = 0, no_vs = 0, no_ps = 0, no_pipeline = 0, no_target = 0, prim_unsupported = 0;
  uint64_t passes = 0, heap_bytes = 0, upload_bytes = 0, textures = 0, texture_misses = 0;
  uint64_t resolves = 0, resolves_skipped = 0, clears = 0, msaa_surfaces = 0, frames = 0;
  uint64_t upload_full = 0, heap_full = 0, bad_mode = 0, indexed_seen = 0, texture_reloads = 0, texture_evictions = 0;
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
  F(vkCmdPushConstants) F(vkCmdBindDescriptorSets) F(vkCmdBindIndexBuffer) F(vkCmdDraw) F(vkCmdDrawIndexed)          \
  F(vkCmdSetViewport) F(vkCmdSetScissor) F(vkCmdSetBlendConstants)                             \
  F(vkCmdSetStencilReference) F(vkCmdSetStencilCompareMask) F(vkCmdSetStencilWriteMask)        \
  F(vkCmdSetDepthBias) F(vkCmdBeginRendering) F(vkCmdEndRendering) F(vkCmdCopyImage)           \
  F(vkCmdBlitImage) F(vkCmdCopyBufferToImage) F(vkCmdClearColorImage)                          \
  F(vkDestroyImageView) F(vkCmdClearDepthStencilImage) F(vkCmdClearAttachments) F(vkCmdWriteTimestamp)               \
  F(vkCmdResetQueryPool) F(vkCreateQueryPool) F(vkGetQueryPoolResults) F(vkCmdBeginRenderPass) \
  F(vkCmdEndRenderPass) F(vkCreateRenderPass) F(vkCreateFramebuffer) F(vkQueueSubmit)

struct Fn {
#define NG2_F(name) PFN_##name name = nullptr;
  NG2_VK_FUNCS(NG2_F)
#undef NG2_F
} vk;

std::atomic<int> g_desired_scale{1};

struct Image {
  VkImage image = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;  // Attachment / plain 2D view, full aspect of the first level.
  VkFormat format = VK_FORMAT_UNDEFINED;
  uint32_t width = 0, height = 0, levels = 1;
  bool depth = false;
  std::map<uint64_t, VkImageView> sample_views;  // By swizzle, bit 32 = signed view.
  // Game textures only: these are freed and reloaded, so they use the VMA heap.
  VmaAllocation allocation = nullptr;
  uint64_t content_hash = 0, last_frame = 0, bytes = 0;
};

// A freed image the GPU may still be reading.
struct RetiredImage {
  VkImage image = VK_NULL_HANDLE;
  std::vector<VkImageView> views;
  VmaAllocation allocation = nullptr;
  uint64_t frame = 0;
};

struct Surface {
  Image image;
  uint32_t guest = 0, base = 0;  // Guest object, EDRAM base (only for aliased clears, #102).
  bool depth = false;
  // MSAA surfaces are single-sample images with one pixel per sample: guest 2x doubles the rows,
  // 4x the rows and columns. The resolve filters them back down.
  uint32_t msaa_x = 1, msaa_y = 1;
  bool has_pending_clear = false;
  VkClearColorValue pending_clear = {};
  uint32_t tiles = 0;  // Size in guest layout units, only to find what a clear covers.
  uint64_t depth_fit_frame = UINT64_MAX;  // Last frame the 2x scene depth clear ran.
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
  VkDeviceSize upload_offset = 0, staging_offset = 0, heap_offset = 0;
  uint8_t* upload_map = nullptr;
  uint8_t* staging_map = nullptr;
  VkDeviceSize upload_base = 0, staging_base = 0, heap_base = 0;
  bool timestamps_valid = false;
};

struct PendingImmediate {
  uint32_t guest = 0;
  VkDeviceSize heap_offset = 0;
  uint32_t bytes = 0;
  // BeginIndexedVertices: indices the engine writes after the call returns.
  uint8_t* index_out = nullptr;
  uint32_t index_count = 0;
  bool index_32 = false, quads = false;
};

struct R {
  const VulkanDevice* device = nullptr;
  rex::ui::Presenter* presenter = nullptr;
  // Internal resolution: every render target and resolved texture is this many times its guest
  // size. Shaders use normalised coordinates, so they don't need to know. Read once at startup.
  uint32_t scale = 1;
  VkDevice dev = VK_NULL_HANDLE;
  VkPhysicalDeviceMemoryProperties memory = {};
  bool usable = false;

  VkBuffer heap = VK_NULL_HANDLE, upload = VK_NULL_HANDLE, staging = VK_NULL_HANDLE;
  uint8_t* heap_map = nullptr;
  uint8_t* upload_map = nullptr;
  uint8_t* staging_map = nullptr;
  VkDescriptorSetLayout heap_layout = VK_NULL_HANDLE, constants_layout = VK_NULL_HANDLE;
  VkDescriptorPool static_pool = VK_NULL_HANDLE;
  VkDescriptorSet heap_set = VK_NULL_HANDLE;
  VkCommandPool cmd_pool = VK_NULL_HANDLE;
  VkQueryPool queries = VK_NULL_HANDLE;
  float timestamp_ms = 0, timestamp_period = 1.0f;
  uint32_t timestamp_frames = 0;
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
  Image gamma_ramp;  // 256x1, the display ramp the guest set (D3DGAMMARAMP)
  uint16_t gamma_ramp_guest[768] = {};
  bool gamma_ramp_valid = false;
  std::unordered_map<VkImageView, VkFramebuffer> framebuffers;

  // Device-local chunks; textures are kept for the session.
  VkDeviceMemory chunk = VK_NULL_HANDLE;
  VkDeviceSize chunk_used = 0;
  uint32_t chunk_type = UINT32_MAX;
  uint64_t image_bytes = 0;
  VmaAllocator vma = nullptr;
  std::vector<RetiredImage> retired;
  uint64_t frame_count = 0, texture_bytes = 0;

  ShaderPack pack;
  // Shaders translated at bind time, kept on disk beside the pack. A deque: draws hold pointers.
  std::deque<NativeShader> runtime_shaders;
  std::unordered_map<uint64_t, const NativeShader*> runtime_by_hash;
  std::filesystem::path runtime_pack_path;
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
  std::unordered_map<uint64_t, uint64_t> resolved_latest;  // ResolvedAddressKey -> ResolvedKey.
  std::unordered_map<uint64_t, Image> textures;
  Image white_2d, white_3d, white_cube;
  bool white_uploaded = false;

  // Current pass.
  bool in_pass = false;
  Surface* pass_color[4] = {};
  Surface* pass_depth = nullptr;
  uint32_t pass_width = 0, pass_height = 0;

  std::unordered_map<uint64_t, uint32_t> heap_copied;  // (address, size) -> heap offset, this frame.
  std::vector<PendingImmediate> pending;

  // Constants reused when nothing changed.
  uint8_t last_bool_loop[160] = {};
  VkDeviceSize last_bool_loop_offset = VK_WHOLE_SIZE;
  uint32_t resolve_index = 0;
  Image* show_override = nullptr;
  std::unordered_map<VkImageView, VkDescriptorSet> present_sets;
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
bool MakeMappedBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buffer, uint8_t*& map,
                      bool video = true) {
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
  if (video && type != UINT32_MAX &&
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

// The same texels read as two's complement, for fetch constants with a signed component. A format
// with no such view (BC1-3) stays unsigned; that combination is logged, see NoteTextureSigns.
VkFormat SignedViewFormat(VkFormat format) {
  switch (format) {
    case VK_FORMAT_R8_UNORM:
      return VK_FORMAT_R8_SNORM;
    case VK_FORMAT_R8G8_UNORM:
      return VK_FORMAT_R8G8_SNORM;
    case VK_FORMAT_R8G8B8A8_UNORM:
      return VK_FORMAT_R8G8B8A8_SNORM;
    case VK_FORMAT_R16G16_UNORM:
      return VK_FORMAT_R16G16_SNORM;
    case VK_FORMAT_R16G16B16A16_UNORM:
      return VK_FORMAT_R16G16B16A16_SNORM;
    case VK_FORMAT_BC4_UNORM_BLOCK:
      return VK_FORMAT_BC4_SNORM_BLOCK;
    case VK_FORMAT_BC5_UNORM_BLOCK:
      return VK_FORMAT_BC5_SNORM_BLOCK;
    default:
      return format;
  }
}

bool MakeImage(Image& out, VkFormat format, uint32_t width, uint32_t height, uint32_t levels,
               VkImageUsageFlags usage, uint32_t layers = 1, VkImageCreateFlags flags = 0,
               VkImageType type = VK_IMAGE_TYPE_2D, bool pooled = false) {
  VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  if ((usage & VK_IMAGE_USAGE_SAMPLED_BIT) && SignedViewFormat(format) != format) {
    flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
  }
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
  if (pooled) {
    VmaAllocationCreateInfo pool = {};
    pool.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VmaAllocationInfo allocated = {};
    if (vmaCreateImage(r.vma, &info, &pool, &out.image, &out.allocation, &allocated) != VK_SUCCESS) {
      return false;
    }
    out.bytes = allocated.size;
  } else {
    if (vk.vkCreateImage(r.dev, &info, nullptr, &out.image) != VK_SUCCESS) return false;
    if (!BindImageMemory(out.image)) return false;
  }
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

VkImageView SampleView(Image& image, uint32_t swizzle_key, bool is_signed = false) {
  const uint64_t key = uint64_t(swizzle_key) | (uint64_t(is_signed) << 32);
  const auto found = image.sample_views.find(key);
  if (found != image.sample_views.end()) return found->second;
  VkImageViewCreateInfo view = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = image.image;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
  view.format = is_signed ? SignedViewFormat(image.format) : image.format;
  view.components.r = VkComponentSwizzle(swizzle_key & 0xFF);
  view.components.g = VkComponentSwizzle((swizzle_key >> 8) & 0xFF);
  view.components.b = VkComponentSwizzle((swizzle_key >> 16) & 0xFF);
  view.components.a = VkComponentSwizzle((swizzle_key >> 24) & 0xFF);
  view.subresourceRange.aspectMask = image.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
  view.subresourceRange.levelCount = image.levels;
  view.subresourceRange.layerCount = 1;
  VkImageView out = VK_NULL_HANDLE;
  vk.vkCreateImageView(r.dev, &view, nullptr, &out);
  image.sample_views[key] = out;
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
  VkDescriptorSetLayoutBinding bindings[2] = {binding, binding};
  bindings[1].binding = 1;
  set_info.bindingCount = 2;
  set_info.pBindings = bindings;
  if (vk.vkCreateDescriptorSetLayout(r.dev, &set_info, nullptr, &r.present_set_layout) != VK_SUCCESS) {
    return false;
  }
  VkPipelineLayoutCreateInfo layout = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout.setLayoutCount = 1;
  layout.pSetLayouts = &r.present_set_layout;
  VkPushConstantRange present_constants = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 24};
  layout.pushConstantRangeCount = 1;
  layout.pPushConstantRanges = &present_constants;
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
  r.vma = rex::ui::vulkan::CreateVmaAllocator(r.device, false);
  r.scale = uint32_t(std::clamp(std::atoi(rex::cvar::GetFlagByName("resolution_scale").c_str()), 1, 8));
  g_desired_scale = int(r.scale);
  rex::cvar::RegisterChangeCallback("resolution_scale", [](std::string_view, std::string_view value) {
    g_desired_scale = std::atoi(std::string(value).c_str());
  });
  REXLOG_INFO("NG2 NATIVE: internal resolution {}x", r.scale);
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
  r.runtime_pack_path = pack_path.parent_path() / "ng2_native_shaders_runtime.pack";
  if (std::filesystem::exists(r.runtime_pack_path)) {
    ShaderPack runtime;
    if (runtime.Load(r.runtime_pack_path, &error)) {
      for (const NativeShader& shader : runtime.shaders()) {
        r.runtime_by_hash[shader.ucode_hash] = &r.runtime_shaders.emplace_back(shader);
      }
    } else {
      REXLOG_WARN("NG2 NATIVE: ignoring runtime shader pack: {}", error);
    }
  }
  REXLOG_INFO("NG2 NATIVE: {} shaders in the pack, {} translated at bind time earlier", r.pack.size(),
              r.runtime_shaders.size());

  if (!MakeMappedBuffer(kStagingSize * kFrames, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, r.staging,
                        r.staging_map, false) ||
      !MakeMappedBuffer(kHeapSize * kFrames, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, r.heap, r.heap_map) ||
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
    f.staging_base = kStagingSize * i;
    f.staging_map = r.staging_map + f.staging_base;
    f.heap_base = kHeapSize * i;

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
  if (!MakeImage(r.gamma_ramp, VK_FORMAT_R16G16B16A16_UNORM, 256, 1, 1, usage)) return false;
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

void FreeRetiredImages();

void DestroyScaledImage(Image& image) {
  for (auto& [swizzle, view] : image.sample_views) vk.vkDestroyImageView(r.dev, view, nullptr);
  if (image.view) vk.vkDestroyImageView(r.dev, image.view, nullptr);
  vmaDestroyImage(r.vma, image.image, image.allocation);
  r.image_bytes -= image.bytes;
}

// resolution_scale changed (Recomp Settings applies it live): everything sized by the scale is
// recreated on demand. Happens between frames; the next frame's history is simply gone.
void ApplyResolutionScale() {
  const uint32_t want = uint32_t(std::clamp(g_desired_scale.load(), 1, 8));
  if (want == r.scale) return;
  for (Frame& frame : r.frames) vk.vkWaitForFences(r.dev, 1, &frame.fence, VK_TRUE, UINT64_MAX);
  REXLOG_INFO("NG2 NATIVE: internal resolution {}x -> {}x", r.scale, want);
  for (auto& [key, surface] : r.surfaces) DestroyScaledImage(surface.image);
  for (auto& [key, image] : r.resolved) DestroyScaledImage(image);
  r.surfaces.clear();
  r.resolved.clear();
  r.resolved_latest.clear();
  r.present_sets.clear();
  r.scale = want;
}

void BeginFrame() {
  if (r.recording) return;
  ApplyResolutionScale();
  r.f = &r.frames[r.frame_index];
  Frame& f = *r.f;
  vk.vkWaitForFences(r.dev, 1, &f.fence, VK_TRUE, UINT64_MAX);
  vk.vkResetFences(r.dev, 1, &f.fence);
  if (f.timestamps_valid) {
    uint64_t stamps[2];
    if (vk.vkGetQueryPoolResults(r.dev, r.queries, r.frame_index * 2, 2, sizeof(stamps), stamps, 8,
                                 VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
      const float period = r.timestamp_period;
      const float gpu_ms = float(stamps[1] - stamps[0]) * period * 1e-6f;
      r.timestamp_ms += gpu_ms;
      ++r.timestamp_frames;
    }
  }
  ++r.frame_count;
  FreeRetiredImages();
  vk.vkResetDescriptorPool(r.dev, f.pool, 0);
  f.upload_offset = 0;
  f.staging_offset = 0;
  f.heap_offset = 0;
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

VkDeviceSize StagingAlloc(VkDeviceSize bytes) {
  Frame& f = *r.f;
  const VkDeviceSize offset = (f.staging_offset + kUploadAlign - 1) & ~(kUploadAlign - 1);
  if (offset + bytes > kStagingSize) {
    ++g_s.upload_full;
    return VK_WHOLE_SIZE;
  }
  f.staging_offset = offset + bytes;
  g_s.upload_bytes += bytes;
  return offset;
}

// Room in this frame's heap area, as an offset into the heap buffer; UINT32_MAX when full.
uint32_t HeapAlloc(uint32_t bytes) {
  Frame& f = *r.f;
  const VkDeviceSize aligned = (bytes + 15ull) & ~15ull;
  if (f.heap_offset + aligned > kHeapSize) {
    ++g_s.heap_full;
    return UINT32_MAX;
  }
  const VkDeviceSize offset = f.heap_base + f.heap_offset;
  f.heap_offset += aligned;
  return uint32_t(offset);
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
  const uint32_t width = ((size >> 18) & 0x3FFF) + 1;
  const uint32_t height = ((size >> 3) & 0x7FFF) + 1;
  // Formats 3 and 12 are the same bits viewed two ways, so the key is the host format.
  const VkFormat format = depth ? kDepthFormat : ColorFormat((info >> 16) & 0xF);
  const uint32_t msaa = (surface_info >> 16) & 3;
  const uint64_t key = (uint64_t(info & 0xFFF) << 48) ^ (uint64_t(format) << 32) ^
                       (uint64_t(width) << 16) ^ (uint64_t(height) << 1) ^ uint64_t(depth) ^
                       (uint64_t(msaa) << 62);
  const auto found = r.surfaces.find(key);
  if (found != r.surfaces.end()) return &found->second;
  if (msaa) ++g_s.msaa_surfaces;
  Surface& s = r.surfaces[key];
  s.guest = guest;
  s.depth = depth;
  s.base = info & 0xFFF;
  s.msaa_x = msaa == 2 ? 2 : 1;
  s.msaa_y = msaa ? 2 : 1;
  {
    const uint32_t color_format = (info >> 16) & 0xF;
    const uint32_t bytes_per_pixel_units =
        (!depth && (color_format == 5 || color_format == 7 || color_format == 15)) ? 2 : 1;
    s.tiles = (((surface_info & 0x3FFF) * s.msaa_x + 79) / 80) * ((height * s.msaa_y + 15) / 16) *
              bytes_per_pixel_units;
  }
  const VkImageUsageFlags usage =
      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
      (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
  if (!MakeImage(s.image, format, width * r.scale * s.msaa_x, height * r.scale * s.msaa_y, 1, usage, 1, 0,
                 VK_IMAGE_TYPE_2D, true)) {
    REXLOG_ERROR("NG2 NATIVE: surface {:08X} {}x{} failed", guest, width, height);
    r.surfaces.erase(key);
    return nullptr;
  }
  r.image_bytes += s.image.bytes;
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

// Patched vertex shaders can differ per declaration, so the pack can't list them all: a miss is
// translated here once and saved. Only a translator failure drops the draw.
const NativeShader* TranslateAtBind(ShaderStage stage, uint32_t ucode_guest_physical, uint32_t bytes,
                                    uint64_t hash) {
  const auto found = r.runtime_by_hash.find(hash);
  if (found != r.runtime_by_hash.end()) return found->second;
  NativeShader shader;
  std::string error;
  const auto start = std::chrono::steady_clock::now();
  if (!TranslateShader(stage, reinterpret_cast<const uint32_t*>(Phys(ucode_guest_physical)),
                       bytes / 4, hash, shader, &error)) {
    REXLOG_ERROR("NG2 NATIVE: shader {:016X} failed to translate: {}", hash, error);
    r.runtime_by_hash[hash] = nullptr;
    return nullptr;
  }
  shader.guest_address = ucode_guest_physical;
  const NativeShader* stored = &r.runtime_shaders.emplace_back(std::move(shader));
  r.runtime_by_hash[hash] = stored;
  ShaderPack runtime;
  for (const NativeShader& s : r.runtime_shaders) runtime.Add(s);
  const bool saved = runtime.Save(r.runtime_pack_path);
  ++g_s.bind_translations;
  REXLOG_INFO("NG2 NATIVE: translated {} shader {:016X} at bind, {} bytes, {:.1f} ms, saved {}",
              stage == ShaderStage::kVertex ? "vertex" : "pixel", hash, bytes,
              std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                  .count(),
              saved);
  return stored;
}

const NativeShader* FindShader(ShaderStage stage, uint32_t ucode_guest_virtual_base, uint32_t bytes,
                               uint32_t decl) {
  const uint64_t key = (uint64_t(ucode_guest_virtual_base) << 32) ^ (uint64_t(decl) << 2) ^ bytes;
  const auto found = r.shader_by_key.find(key);
  if (found != r.shader_by_key.end()) return found->second;
  const uint64_t hash = XXH3_64bits(Phys(ucode_guest_virtual_base), bytes);
  const NativeShader* shader = r.pack.Find(hash);
  if (!shader) shader = TranslateAtBind(stage, ucode_guest_virtual_base, bytes, hash);
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
    case 28:  // k_16_16_EXPAND
    case 31:
      return VK_FORMAT_R16G16_SFLOAT;
    case 29:  // k_16_16_16_16_EXPAND
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
// The game resolves to one address at more than one size (1CE10000 at 1120x584 and 1280x720 in the
// same cutscene frame), so size is part of the identity.
uint64_t ResolvedAddressKey(const uint32_t words[6]) {
  return (uint64_t(ToPhysical(words[1] & 0xFFFFF000)) << 8) | (words[1] & 0x3F);
}
uint64_t ResolvedKey(const uint32_t words[6]) {
  return (ResolvedAddressKey(words) << 24) ^ (uint64_t(words[2]) & 0x3FFFFFF);
}
// What a texture fetch finds: the same size, else the newest resolve at that address and format.
Image* FindResolved(const uint32_t words[6]) {
  auto found = r.resolved.find(ResolvedKey(words));
  if (found != r.resolved.end()) return &found->second;
  const auto latest = r.resolved_latest.find(ResolvedAddressKey(words));
  if (latest == r.resolved_latest.end()) return nullptr;
  found = r.resolved.find(latest->second);
  return found != r.resolved.end() ? &found->second : nullptr;
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

// No write watching in the native path: bound textures are sampled once per frame instead. Small
// ones whole, large ones at 256 spots, which catches a level load or a reused address. A CPU edit
// of a few bytes of a large texture could slip through.
uint64_t HashTextureContent(const TextureDesc& desc) {
  constexpr uint32_t kWhole = 64 * 1024, kSpots = 256, kSpot = 64;
  const uint32_t bytes[2] = {desc.base_bytes, desc.mip_address ? desc.mip_bytes : 0};
  const uint32_t address[2] = {desc.base_address, desc.mip_address};
  uint64_t hash = 0;
  for (uint32_t part = 0; part < 2; ++part) {
    if (!bytes[part] || !address[part]) continue;
    const uint8_t* data = Phys(address[part]);
    if (bytes[part] <= kWhole) {
      hash = XXH3_64bits_withSeed(data, bytes[part], hash);
      continue;
    }
    const uint32_t step = (bytes[part] - kSpot) / (kSpots - 1);
    for (uint32_t i = 0; i < kSpots; ++i) hash = XXH3_64bits_withSeed(data + i * step, kSpot, hash);
  }
  return hash;
}

void RetireTexture(Image& image) {
  RetiredImage retired;
  retired.image = image.image;
  retired.allocation = image.allocation;
  retired.frame = r.frame_count;
  for (const auto& view : image.sample_views) retired.views.push_back(view.second);
  r.retired.push_back(std::move(retired));
  r.texture_bytes -= std::min(r.texture_bytes, image.bytes);
}

void FreeRetiredImages() {
  size_t kept = 0;
  for (RetiredImage& image : r.retired) {
    if (r.frame_count < image.frame + kFrames + 1) {
      if (&r.retired[kept] != &image) r.retired[kept] = std::move(image);
      ++kept;
      continue;
    }
    for (VkImageView view : image.views) vk.vkDestroyImageView(r.dev, view, nullptr);
    vmaDestroyImage(r.vma, image.image, image.allocation);
  }
  r.retired.resize(kept);
}

// Least recently bound first, never anything bound in the last two seconds.
void EvictTextures() {
  const uint64_t budget = uint64_t(std::max(REXCVAR_GET(ng2_native_texture_mb), 16)) << 20;
  if (r.texture_bytes <= budget) return;
  std::vector<std::pair<uint64_t, uint64_t>> order;
  for (const auto& entry : r.textures) order.push_back({entry.second.last_frame, entry.first});
  std::sort(order.begin(), order.end());
  for (const auto& entry : order) {
    if (r.texture_bytes <= budget || entry.first + 120 > r.frame_count) break;
    auto found = r.textures.find(entry.second);
    RetireTexture(found->second);
    r.textures.erase(found);
    ++g_s.texture_evictions;
  }
}

VkImageView LoadStaticTexture(const uint32_t words[6], bool is_signed) {
  TextureDesc desc;
  if (!ParseTextureFetch(words, desc)) return VK_NULL_HANDLE;
  const uint32_t swizzle =
      desc.vk_swizzle[0] | (desc.vk_swizzle[1] << 8) | (desc.vk_swizzle[2] << 16) |
      (desc.vk_swizzle[3] << 24);
  const uint64_t key = XXH3_64bits(words, 8) ^ (uint64_t(words[2]) << 32) ^ words[5];
  auto found = r.textures.find(key);
  if (found != r.textures.end()) {
    Image& image = found->second;
    if (image.last_frame == r.frame_count) return SampleView(image, swizzle, is_signed);
    image.last_frame = r.frame_count;
    if (HashTextureContent(desc) == image.content_hash) return SampleView(image, swizzle, is_signed);
    ++g_s.texture_reloads;
    RetireTexture(image);
    r.textures.erase(found);
  }
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
  const VkDeviceSize offset = StagingAlloc(loaded.data.size());
  if (offset == VK_WHOLE_SIZE) return VK_NULL_HANDLE;
  Image image;
  const uint32_t first = loaded.mips.front().level;
  if (!MakeImage(image, VkFormat(desc.vk_format), loaded.mips.front().width, loaded.mips.front().height,
                 uint32_t(loaded.mips.size()),
                 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 1, 0, VK_IMAGE_TYPE_2D,
                 true)) {
    ++g_s.texture_misses;
    return VK_NULL_HANDLE;
  }
  std::memcpy(r.f->staging_map + offset, loaded.data.data(), loaded.data.size());
  ToGeneral(r.f->upload_cb, image);
  std::vector<VkBufferImageCopy> copies;
  for (const LoadedMip& mip : loaded.mips) {
    VkBufferImageCopy copy = {};
    copy.bufferOffset = r.f->staging_base + offset + mip.offset;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip.level - first, 0, 1};
    copy.imageExtent = {mip.width, mip.height, 1};
    copies.push_back(copy);
  }
  vk.vkCmdCopyBufferToImage(r.f->upload_cb, r.staging, image.image, VK_IMAGE_LAYOUT_GENERAL,
                            uint32_t(copies.size()), copies.data());
  ++g_s.textures;
  image.content_hash = HashTextureContent(desc);
  image.last_frame = r.frame_count;
  r.texture_bytes += image.bytes;
  auto& stored = r.textures[key];
  stored = std::move(image);
  return SampleView(stored, swizzle, is_signed);
}

// Per component after the fetch swizzle, two bits each: what the shader does with the sample
// (xenos::TextureSign). The shader reads its signed binding for kSigned components.
uint8_t TextureSwizzledSigns(const uint32_t words[6]) {
  uint32_t signs = 0, constant_mask = 0;
  bool any_signed = false, any_not_signed = false;
  const uint32_t swizzle = (words[3] >> 1) & 0xFFF;
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t component = (swizzle >> (i * 3)) & 7;
    if (component & 4) {
      constant_mask |= 1u << (i * 2);
      continue;
    }
    const uint32_t sign = (words[0] >> (2 + component * 2)) & 3;
    signs |= sign << (i * 2);
    (sign == 1 ? any_signed : any_not_signed) = true;
  }
  uint32_t constants_sign = 0;
  if (constant_mask == 0x55) {
    if (((words[0] >> 2) & 0xFF) == 0x55) constants_sign = 1;
  } else if (any_signed && !any_not_signed) {
    constants_sign = 1;
  }
  return uint8_t(signs | (constants_sign * constant_mask));
}

// Logs each texture format + sign combination once, with whether a signed view exists for it.
void NoteTextureSigns(const uint32_t words[6], VkFormat format, bool resolved) {
  const uint32_t signs = (words[0] >> 2) & 0xFF;
  if (!signs) return;
  static std::set<uint64_t> seen;
  const uint64_t key = (uint64_t(words[1] & 0x3F) << 32) | (signs << 8) | uint32_t(resolved);
  if (!seen.insert(key).second) return;
  REXLOG_INFO("NG2 NATIVE signs format {} signs {:02X} swizzle {:03X} resolved {} signed view {}",
              words[1] & 0x3F, signs, (words[3] >> 1) & 0xFFF, resolved,
              SignedViewFormat(format) != format);
}

VkImageView TextureView(uint32_t dev, uint32_t slot, bool is_signed) {
  uint32_t words[6];
  SwapDwords(words, Virt(dev + kDevFetch + slot * 24), 6);
  if (Image* resolved = FindResolved(words)) {
    // The guest's resolve swaps red and blue to the texture's memory order and the fetch swizzle
    // swaps them back. Host images hold the target's components as they are.
    uint32_t swizzle = FetchSwizzleKey(words[3]);
    if ((swizzle & 0xFF) == VK_COMPONENT_SWIZZLE_B && ((swizzle >> 16) & 0xFF) == VK_COMPONENT_SWIZZLE_R) {
      swizzle = (swizzle & 0xFF00FF00) | VK_COMPONENT_SWIZZLE_R | (VK_COMPONENT_SWIZZLE_B << 16);
    }
    NoteTextureSigns(words, resolved->format, true);
    return SampleView(*resolved, swizzle, is_signed);
  }
  const VkImageView view = LoadStaticTexture(words, is_signed);
  if (view != VK_NULL_HANDLE) return view;
  return SampleView(r.white_2d, 0);
}

VkSampler GetSampler(const NativeSamplerBinding& binding, const uint32_t words[6]) {
  const uint32_t clamp = (words[0] >> 10) & 0x1FF;
  uint32_t mag = (words[3] >> 19) & 3, min = (words[3] >> 21) & 3, mip = (words[3] >> 23) & 3;
  if (binding.mag_filter != 3) mag = binding.mag_filter;
  if (binding.min_filter != 3) min = binding.min_filter;
  if (binding.mip_filter != 3) mip = binding.mip_filter;
  uint32_t aniso = (words[3] >> 25) & 7;
  if (binding.aniso_filter != 7) aniso = binding.aniso_filter;
  const uint32_t bias = (words[4] >> 12) & 0x3FF, border = words[5] & 3;
  const uint64_t key = clamp | (mag << 9) | (min << 11) | (mip << 13) | (aniso << 15) | (bias << 18) |
                       (uint64_t(border) << 28);
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
  info.mipLodBias = float(int32_t(bias << 22) >> 22) / 32.0f;  // Signed, 5 fractional bits.
  if (aniso >= 2 && aniso <= 5 && r.device->properties().samplerAnisotropy) {
    info.anisotropyEnable = VK_TRUE;
    info.maxAnisotropy = float(1u << (aniso - 1));
  }
  // The two YCbCr-style blacks fall back to transparent black: they are not seen outside movies.
  info.borderColor =
      border == 1 ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
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
    infos[count].imageView =
        TextureView(dev, shader.textures[i].fetch_constant, shader.textures[i].is_signed != 0);
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

// A vertex stream's place in this frame's heap area, copied on first use; UINT32_MAX if none.
uint32_t CopyToHeap(uint32_t address, uint32_t bytes) {
  if (!bytes || address + uint64_t(bytes) > 0x20000000) return UINT32_MAX;
  const uint64_t key = (uint64_t(address) << 32) | bytes;
  auto [it, added] = r.heap_copied.try_emplace(key, UINT32_MAX);
  if (!added) return it->second;
  // Fetches can run past the declared size, as they could on the console: keep what follows.
  const uint32_t copy = bytes + uint32_t(std::min<uint64_t>(kStreamSlack, 0x20000000ull - (address + bytes)));
  const uint32_t offset = HeapAlloc(copy);
  if (offset == UINT32_MAX) return offset;
  std::memcpy(r.heap_map + offset, Phys(address), copy);
  g_s.heap_bytes += copy;
  it->second = offset;
  return offset;
}

}  // namespace

// Big-endian guest indices to host, with quad lists turned into two triangles each.
void WriteIndices(uint8_t* out, const uint8_t* in, uint32_t count, bool index_32, bool quads) {
  auto read = [&](uint32_t i) -> uint32_t {
    return index_32 ? _byteswap_ulong(reinterpret_cast<const uint32_t*>(in)[i])
                    : _byteswap_ushort(reinterpret_cast<const uint16_t*>(in)[i]);
  };
  auto write = [&](uint32_t i, uint32_t value) {
    if (index_32) {
      reinterpret_cast<uint32_t*>(out)[i] = value;
    } else {
      reinterpret_cast<uint16_t*>(out)[i] = uint16_t(value);
    }
  };
  if (!quads) {
    for (uint32_t i = 0; i < count; ++i) write(i, read(i));
    return;
  }
  uint32_t o = 0;
  for (uint32_t q = 0; q + 3 < count; q += 4) {
    for (uint32_t corner : {0u, 1u, 2u, 0u, 2u, 3u}) write(o++, read(q + corner));
  }
}

void RendererDrainImmediate() {
  std::lock_guard lock(g_mutex);
  for (const PendingImmediate& p : r.pending) {
    if (p.index_out) {
      WriteIndices(p.index_out, Virt(p.guest), p.index_count, p.index_32, p.quads);
      continue;
    }
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
  const NativeShader* vs = FindShader(ShaderStage::kVertex, ToPhysical(vs_ucode), Load(vs_header + 876), decl);
  if (!vs) {
    ++g_s.no_vs;
    return;
  }
  const NativeShader* ps = nullptr;
  if (ps_object) {
    const uint32_t ps_header = ps_object + Load(ps_object + 64);
    const uint32_t ps_ucode = Load(ps_header + 40) + Load(ps_object + 24);
    ps = FindShader(ShaderStage::kPixel, ToPhysical(ps_ucode), Load(ps_header + 44), 0);
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
      for (uint32_t q = 0; !d.indexed && q + 3 < d.count; q += 4) {
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
  if (ps && !REXCVAR_GET(ng2_native_nodepth_ps).empty() &&
      REXCVAR_GET(ng2_native_nodepth_ps).find(fmt::format("{:016X}", ps->ucode_hash)) != std::string::npos) {
    depth_control = 0;
  }
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

  if (ps && !REXCVAR_GET(ng2_native_skip_ps).empty() &&
      REXCVAR_GET(ng2_native_skip_ps).find(fmt::format("{:016X}", ps->ucode_hash)) != std::string::npos) {
    static uint64_t frame = UINT64_MAX;
    static int count = 0;
    if (frame != r.frame_count) {
      frame = r.frame_count;
      count = 0;
    }
    const int nth = count++;
    if (nth >= REXCVAR_GET(ng2_native_skip_from) && nth < REXCVAR_GET(ng2_native_skip_to)) return;
  }

  // Vertex data.
  uint32_t fetch[32 * 6];
  SwapDwords(fetch, Virt(dev + kDevFetch), 32 * 6);
  for (uint32_t slot : vs->vertex_fetch_slots) {
    uint32_t* words = fetch + slot * 2;
    if ((words[0] & 3) != 3) continue;
    const uint32_t heap_offset = CopyToHeap(words[0] & 0xFFFFFFFC, ((words[1] >> 2) & 0xFFFFFF) * 4);
    if (heap_offset != UINT32_MAX) words[0] = heap_offset | (words[0] & 3);
  }
  if (d.immediate) {
    const uint32_t bytes = d.immediate_dwords * 4;
    const uint32_t heap_offset = HeapAlloc(bytes);
    if (heap_offset == UINT32_MAX) return;
    r.pending.push_back({d.immediate_guest, heap_offset, bytes});
    fetch[95 * 2] = heap_offset | 3;
    fetch[95 * 2 + 1] = (d.immediate_dwords << 2) | 2;
  }

  // Constants.
  NativeSystemConstants system = {};
  const uint32_t vte = Reg(dev, kPaClVteCntl);
  const float pass_w = float(r.pass_width), pass_h = float(r.pass_height);
  uint32_t width = UINT32_MAX, height = UINT32_MAX, msaa_x = 1, msaa_y = 1;
  for (Surface* s : color) {
    if (s) {
      width = std::min(width, s->image.width);
      height = std::min(height, s->image.height);
      msaa_x = s->msaa_x;
      msaa_y = s->msaa_y;
    }
  }
  if (depth) {
    width = std::min(width, depth->image.width);
    height = std::min(height, depth->image.height);
    msaa_x = depth->msaa_x;
    msaa_y = depth->msaa_y;
  }
  (void)pass_w;
  (void)pass_h;
  // Guest pixels: the viewport transform and scissor registers are in them.
  const float w = float(width / (r.scale * msaa_x)), h = float(height / (r.scale * msaa_y));
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
    // 7e3 (formats 3 and 12), see the translator: negative asks for the format's output range.
    const uint32_t target_format = (Load(color[i]->guest + 28) >> 16) & 0xF;
    if (target_format == 3 || target_format == 12) system.color_exp_bias[i] = -1.0f;
  }
  const auto set_signs = [&](const NativeShader& shader) {
    for (const NativeTextureBinding& binding : shader.textures) {
      uint32_t words[6];
      SwapDwords(words, Virt(dev + kDevFetch + binding.fetch_constant * 24), 6);
      const uint32_t shift = 8 * (binding.fetch_constant & 3);
      uint32_t& packed = system.texture_swizzled_signs[binding.fetch_constant >> 2];
      packed = (packed & ~(0xFFu << shift)) | (uint32_t(TextureSwizzledSigns(words)) << shift);
    }
  };
  set_signs(*vs);
  if (ps) set_signs(*ps);
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
    const bool quads = d.prim == 13;
    index_count = quads ? d.count / 4 * 6 : d.count;
    index_offset = UploadAlloc(index_count * (d.index_32 ? 4 : 2));
    if (index_offset == VK_WHOLE_SIZE) return;
    if (d.index_32) index_type = VK_INDEX_TYPE_UINT32;
    uint8_t* out = r.f->upload_map + index_offset;
    if (d.immediate) {
      PendingImmediate pending;
      pending.guest = d.index_guest;
      pending.index_out = out;
      pending.index_count = d.count;
      pending.index_32 = d.index_32;
      pending.quads = quads;
      r.pending.push_back(pending);
    } else {
      WriteIndices(out, Virt(d.index_guest), d.count, d.index_32, quads);
    }
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
  VkViewport viewport = {0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f};
  vk.vkCmdSetViewport(cb, 0, 1, &viewport);
  const uint32_t tl = Reg(dev, kScissorTl), br = Reg(dev, kScissorBr);
  const int32_t scale_x = int32_t(r.scale * msaa_x), scale_y = int32_t(r.scale * msaa_y);
  int32_t x0 = (tl & 0x7FFF) * scale_x, y0 = ((tl >> 16) & 0x7FFF) * scale_y;
  int32_t x1 = (br & 0x7FFF) * scale_x, y1 = ((br >> 16) & 0x7FFF) * scale_y;
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
    if (Reg(dev, 0x2204) & 0x3F) {
      REXLOG_INFO("NG2 NATIVE   clip cntl {:08X} ucp0 {} {} {} {}", Reg(dev, 0x2204), RegFloat(dev, 0x2388),
                  RegFloat(dev, 0x2389), RegFloat(dev, 0x238A), RegFloat(dev, 0x238B));
    }
    REXLOG_INFO("NG2 NATIVE   colorcontrol {:08X} mode {:08X} alpha ref {}", color_control, mode_cntl,
                system.alpha_test_reference);
    REXLOG_INFO("NG2 NATIVE   vs flags {:X} floats {} slots {} ps floats {} sys flags {:X} ndc {} {} {} {}",
                vs->flags, vs->float_constant_count, slots, ps ? ps->float_constant_count : 0, flags,
                system.ndc_scale[0], system.ndc_offset[0], system.ndc_scale[1], system.ndc_offset[1]);
    if (vs->float_constant_count) {
      auto* c = reinterpret_cast<float*>(r.f->upload_map + vs_offset);
      std::string text;
      for (uint32_t i = 0; i < vs->float_constant_count; ++i) {
        text += fmt::format("({} {} {} {}) ", c[i * 4], c[i * 4 + 1], c[i * 4 + 2], c[i * 4 + 3]);
      }
      REXLOG_INFO("NG2 NATIVE   vs c: {} bitmap {:X} {:X} {:X} {:X}", text, vs->float_bitmap[0],
                  vs->float_bitmap[1], vs->float_bitmap[2], vs->float_bitmap[3]);
    }
    if (ps && ps->float_constant_count) {
      auto* c = reinterpret_cast<float*>(r.f->upload_map + ps_offset);
      std::string text;
      for (uint32_t i = 0; i < ps->float_constant_count; ++i) {
        text += fmt::format("({} {} {} {}) ", c[i * 4], c[i * 4 + 1], c[i * 4 + 2], c[i * 4 + 3]);
      }
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
  // The game lays its surfaces out so that some depth surfaces lie inside a colour surface, and it
  // relies on the colour clear to reset them: the scene colour at base 0 covers the reflection
  // depth at 0x20, which only ever gets a stencil clear.
  if (REXCVAR_GET(ng2_native_alias_depth_clear)) {
    for (auto& [key, other] : r.surfaces) {
      if (other.depth && other.base >= s.base && other.base < s.base + s.tiles) {
        const VkClearDepthStencilValue depth = {0.0f, 0};
        const VkImageSubresourceRange all = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
        vk.vkCmdClearDepthStencilImage(r.f->main_cb, other.image.image, VK_IMAGE_LAYOUT_GENERAL, &depth, 1, &all);
      }
    }
  }
}

// mask: 1 stencil, 2 depth.
void ClearDepthSurface(Surface& s, float z, uint32_t stencil, uint32_t mask) {
  VkClearDepthStencilValue value = {z, stencil};
  VkImageSubresourceRange range = {0, 0, 1, 0, 1};
  if (mask & 1) range.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
  if (mask & 2) range.aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
  vk.vkCmdClearDepthStencilImage(r.f->main_cb, s.image.image, VK_IMAGE_LAYOUT_GENERAL, &value, 1, &range);
}

Image* GetResolvedImage(uint32_t texture) {
  uint32_t words[6];
  SwapDwords(words, Virt(texture + 28), 6);
  const uint32_t base = ToPhysical(words[1] & 0xFFFFF000);
  const uint32_t format = words[1] & 0x3F;
  const uint64_t key = ResolvedKey(words);
  const uint32_t width = ((words[2] & 0x1FFF) + 1) * r.scale;
  const uint32_t height = (((words[2] >> 13) & 0x1FFF) + 1) * r.scale;
  const uint32_t levels = std::max<uint32_t>(((words[4] >> 6) & 0xF) + 1, 1);
  const VkFormat vk_format = ResolvedTextureFormat(format);
  r.resolved_latest[ResolvedAddressKey(words)] = key;
  auto found = r.resolved.find(key);
  if (found != r.resolved.end()) return &found->second;
  Image image;
  const VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  if (!MakeImage(image, vk_format, width, height, levels, usage, 1, 0, VK_IMAGE_TYPE_2D, true)) return nullptr;
  r.image_bytes += image.bytes;
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
                   float z, uint32_t stencil, uint32_t color_floats) {
  std::lock_guard lock(g_mutex);
  if (!Usable()) return;
  RendererDrainImmediate();
  BeginFrame();
  ++g_s.clears;
  EndPass();
  (void)rect_count;
  (void)rects;
  VkClearColorValue value = {};
  if (color_floats) {
    for (uint32_t i = 0; i < 4; ++i) {
      const uint32_t bits = Load(color_floats + i * 4);
      std::memcpy(&value.float32[i], &bits, 4);
    }
  } else {
    value.float32[0] = float((argb >> 16) & 0xFF) / 255.0f;
    value.float32[1] = float((argb >> 8) & 0xFF) / 255.0f;
    value.float32[2] = float(argb & 0xFF) / 255.0f;
    value.float32[3] = float(argb >> 24) / 255.0f;
  }
  FullBarrier(r.f->main_cb);
  // Flags: bits 0-3 colour targets, 0x10 stencil, 0x20 depth (seen as 0x3F, 0x1F, 0x30).
  for (uint32_t i = 0; i < 4; ++i) {
    if (!(flags & (1u << i))) continue;
    if (Surface* s = CurrentSurface(dev, i)) ClearColorSurface(*s, value);
  }
  if (Surface* s = GetSurface(Load(dev + kDevDepthSurface), true)) {
    // Still a fit: the 2x session asks for colour only (0x0F where the 1x session asks for 0x3F), its
    // depth at 0x3FE sits just past the 2x scene colour so the rule above doesn't reach it, and no
    // other clear of it arrives, yet the frame needs it cleared (#129). Once per frame per surface,
    // so a later colour-only clear in the same scene keeps its depth.
    const bool msaa_scene_clear = REXCVAR_GET(ng2_native_msaa_depth_clear) && (flags & 0xF) == 0xF &&
                                  s->msaa_y > 1 && s->depth_fit_frame != r.frame_count;
    if (msaa_scene_clear) s->depth_fit_frame = r.frame_count;
    const uint32_t mask = (flags >> 4) | (msaa_scene_clear ? 2 : 0);
    if (mask & 3) ClearDepthSurface(*s, z, stencil, mask);
  }
  FullBarrier(r.f->main_cb);
  if (r.dumping) {
    REXLOG_INFO("NG2 NATIVE CLEAR flags {:X} argb {:08X} z {} stencil {}", flags, argb, z, stencil);
    for (uint32_t i = 0; i < 4; ++i) {
      if (const uint32_t rt = Load(dev + kDevRenderTargets + i * 4)) {
        REXLOG_INFO("NG2 NATIVE   clear rt{} {:08X} info {:08X} surface_info {:08X}", i, rt, Load(rt + 28), Load(rt + 24));
      }
    }
    if (const uint32_t ds = Load(dev + kDevDepthSurface)) {
      REXLOG_INFO("NG2 NATIVE   clear depth {:08X} info {:08X} surface_info {:08X}", ds, Load(ds + 28), Load(ds + 24));
    }
  }
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
  if (REXCVAR_GET(ng2_native_log_resolves) && (flags & 0x300)) {
    static uint32_t logged = 0;
    if (logged++ < 40) {
      REXLOG_INFO("NG2 NATIVE resolve clear flags {:X} z {} depth surface {:08X}", flags, clear_z,
                  Load(dev + kDevDepthSurface));
    }
  }
  const bool from_depth = which == 4;
  Surface* source = from_depth ? GetSurface(Load(dev + kDevDepthSurface), true)
                               : CurrentSurface(dev, which);
  if (source) ApplyPendingClears(*source);
  Image* dest = texture ? GetResolvedImage(texture) : nullptr;
  if (source && dest) {
    int32_t x0 = 0, y0 = 0, x1 = int32_t(source->image.width), y1 = int32_t(source->image.height);
    if (source_rect) {
      const int32_t sx = int32_t(r.scale * source->msaa_x), sy = int32_t(r.scale * source->msaa_y);
      x0 = int32_t(Load(source_rect)) * sx;
      y0 = int32_t(Load(source_rect + 4)) * sy;
      x1 = int32_t(Load(source_rect + 8)) * sx;
      y1 = int32_t(Load(source_rect + 12)) * sy;
    }
    int32_t dx = 0, dy = 0;
    if (dest_point) {
      dx = int32_t(Load(dest_point)) * int32_t(r.scale);
      dy = int32_t(Load(dest_point + 4)) * int32_t(r.scale);
    }
    const int32_t dest_w = int32_t(std::max(dest->width >> level, 1u));
    const int32_t dest_h = int32_t(std::max(dest->height >> level, 1u));
    x1 = std::min({x1, int32_t(source->image.width), x0 + (dest_w - dx) * int32_t(source->msaa_x)});
    y1 = std::min({y1, int32_t(source->image.height), y0 + (dest_h - dy) * int32_t(source->msaa_y)});
    const bool msaa_source = source->msaa_x > 1 || source->msaa_y > 1;
    if (x1 > x0 && y1 > y0) {
      const VkImageAspectFlags aspect = from_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
      if (from_depth != dest->depth) {
        ++g_s.resolves_skipped;
      } else if (!msaa_source && (source->image.format == dest->format ||
                                  SameTexelBlock(source->image.format, dest->format))) {
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
        blit.dstOffsets[1] = {dx + (x1 - x0) / int32_t(source->msaa_x),
                              dy + (y1 - y0) / int32_t(source->msaa_y), 1};
        // All fragments averages the samples; a single fragment (flags 0x10, 0x20) and depth take
        // one sample per pixel.
        const bool one_sample = from_depth || (flags & 0x30);
        vk.vkCmdBlitImage(cb, source->image.image, VK_IMAGE_LAYOUT_GENERAL, dest->image,
                          VK_IMAGE_LAYOUT_GENERAL, 1, &blit,
                          one_sample || !msaa_source ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
      }
      if (r.dumping) {
        REXLOG_INFO("NG2 NATIVE RESOLVE #{} flags {:X} rt {} {} -> {:08X} fmt {} -> {} rect {} {} {} {}",
                    r.resolve_index, flags, which, source->guest, Load(texture + 28 + 4), int(source->image.format),
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
    if (Surface* s = GetSurface(Load(dev + kDevDepthSurface), true)) ClearDepthSurface(*s, clear_z, clear_stencil, 3);
  }
  FullBarrier(cb);
}

// The display ramp lives on the device; the display hardware applies it, not the frame.
void UpdateGammaRamp(uint32_t dev) {
  uint16_t ramp[768];
  std::memcpy(ramp, Virt(dev + kDevGammaRamp), sizeof(ramp));
  if (r.gamma_ramp_valid && !std::memcmp(ramp, r.gamma_ramp_guest, sizeof(ramp))) return;
  const VkDeviceSize offset = UploadAlloc(256 * 8);
  if (offset == VK_WHOLE_SIZE) return;
  std::memcpy(r.gamma_ramp_guest, ramp, sizeof(ramp));
  auto* out = reinterpret_cast<uint16_t*>(r.f->upload_map + offset);
  for (uint32_t i = 0; i < 256; ++i) {
    for (uint32_t c = 0; c < 3; ++c) out[i * 4 + c] = _byteswap_ushort(ramp[c * 256 + i]);
    out[i * 4 + 3] = 0xFFFF;
  }
  REXLOG_INFO("NG2 NATIVE: gamma ramp R {} {} {} {} {} G128 {} B128 {}", out[0], out[16 * 4],
              out[64 * 4], out[128 * 4], out[255 * 4], out[128 * 4 + 1], out[128 * 4 + 2]);
  ToGeneral(r.f->upload_cb, r.gamma_ramp);
  VkBufferImageCopy copy = {};
  copy.bufferOffset = r.f->upload_base + offset;
  copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copy.imageExtent = {256, 1, 1};
  vk.vkCmdCopyBufferToImage(r.f->upload_cb, r.upload, r.gamma_ramp.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
  r.gamma_ramp_valid = true;
}

void RendererSwap(uint32_t dev, uint32_t front_texture) {
  std::lock_guard lock(g_mutex);
  if (!Usable()) return;
  RendererDrainImmediate();
  BeginFrame();
  EndPass();
  ++g_s.frames;
  r.dumping = uint64_t(REXCVAR_GET(ng2_native_dump_frame)) == g_s.frames ||
              REXCVAR_GET(ng2_native_dump_frame) == -1;
  if (REXCVAR_GET(ng2_native_dump_frame) == -1) rex::cvar::SetFlagByName("ng2_native_dump_frame", "0");
  (void)dev;

  Image* front = r.show_override;
  if (!front && front_texture) {
    uint32_t words[6];
    SwapDwords(words, Virt(front_texture + 28), 6);
    front = FindResolved(words);
  }
  Frame& f = *r.f;
  UpdateGammaRamp(dev);
  EvictTextures();
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
          VkImageView sample = SampleView(*front, 0);
          VkDescriptorSet& set = r.present_sets[sample];
          if (set == VK_NULL_HANDLE) {
            vk.vkAllocateDescriptorSets(r.dev, &alloc, &set);
            VkDescriptorImageInfo image[2] = {{r.present_sampler, sample, VK_IMAGE_LAYOUT_GENERAL},
                                              {r.present_sampler, SampleView(r.gamma_ramp, 0), VK_IMAGE_LAYOUT_GENERAL}};
            VkWriteDescriptorSet writes[2] = {};
            for (uint32_t i = 0; i < 2; ++i) {
              writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
              writes[i].dstSet = set;
              writes[i].dstBinding = i;
              writes[i].descriptorCount = 1;
              writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
              writes[i].pImageInfo = &image[i];
            }
            vk.vkUpdateDescriptorSets(r.dev, 2, writes, 0, nullptr);
          }
          VkViewport viewport = {0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f};
          VkRect2D scissor = {{0, 0}, {width, height}};
          vk.vkCmdSetViewport(cb, 0, 1, &viewport);
          vk.vkCmdSetScissor(cb, 0, 1, &scissor);
          vk.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, r.present_pipeline);
          vk.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, r.present_layout, 0, 1, &set, 0, nullptr);
          // swap_post_effect, read per frame so the Recomp Settings row is live
          const std::string aa = rex::cvar::GetFlagByName("swap_post_effect");
          const bool extreme = aa == "fxaa_extreme";
          struct {
            float texel[2];
            float edge_threshold, edge_threshold_min, span_max;
            int32_t fxaa;
          } constants = {{1.0f / float(width), 1.0f / float(height)},
                         extreme ? 0.063f : 0.166f,
                         extreme ? 0.0312f : 0.0833f,
                         extreme ? 12.0f : 8.0f,
                         (extreme || aa == "fxaa") ? 1 : 0};
          vk.vkCmdPushConstants(cb, r.present_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(constants),
                                &constants);
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
  // Mean over the window: on a 30 fps scene every other swap draws nothing, so one sample misleads.
  REXLOG_INFO("NG2 VK GPU frame time: {:.2f} ms", r.timestamp_ms / float(std::max(r.timestamp_frames, 1u)));
  r.timestamp_ms = 0;
  r.timestamp_frames = 0;
  if (swaps % 240) return;
  const float n = float(std::max<uint64_t>(g_s.frames, 1));
  REXLOG_INFO(
      "NG2 NATIVE frames={} drawn/f={:.1f} passes/f={:.1f} no_vs={} no_ps={} no_pipe={} no_target={} "
      "bad_prim={} heap_mb/f={:.1f} upload_mb/f={:.1f} textures={} tex_miss={} resolves/f={:.1f} "
      "skipped={} msaa={} upload_full={} heap_full={} pipelines={} surfaces={} resolved={} image_mb={} bad_mode={} "
      "indexed={} tex_mb={} reloads={} evicted={}",
      g_s.frames, float(g_s.drawn) / n, float(g_s.passes) / n, g_s.no_vs, g_s.no_ps, g_s.no_pipeline,
      g_s.no_target, g_s.prim_unsupported, float(g_s.heap_bytes) / n / 1048576.0f,
      float(g_s.upload_bytes) / n / 1048576.0f, g_s.textures, g_s.texture_misses,
      float(g_s.resolves) / n, g_s.resolves_skipped, g_s.msaa_surfaces, g_s.upload_full, g_s.heap_full,
      r.pipelines.size(), r.surfaces.size(), r.resolved.size(), r.image_bytes >> 20, g_s.bad_mode,
      g_s.indexed_seen, r.texture_bytes >> 20, g_s.texture_reloads, g_s.texture_evictions);
}

}  // namespace ng2::native

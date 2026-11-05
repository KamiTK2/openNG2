// DLSS and FSR behind the C interface in upscale_bridge.h.
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cwchar>

#include "upscale_bridge.h"

#include <nvsdk_ngx_vk.h>
#include <nvsdk_ngx_helpers_vk.h>

#include <ffx_api.h>
#include <ffx_api_loader.h>
#include <ffx_upscale.h>
#include <vk/ffx_api_vk.h>

#define NG2_EXPORT extern "C" __declspec(dllexport)

namespace {

// Next to the vendor logs: ng2's own log can't be reached from here.
void Log(const char* text) {
  OutputDebugStringA(text);
  wchar_t path[MAX_PATH];
  if (!GetTempPathW(MAX_PATH, path)) return;
  wcscat_s(path, L"openNG2\\ng2_upscalers.log");
  FILE* file = nullptr;
  if (_wfopen_s(&file, path, L"a") == 0 && file) {
    std::fputs(text, file);
    std::fclose(file);
  }
}

void FfxMessage(uint32_t type, const wchar_t* message) {
  char text[512];
  std::snprintf(text, sizeof(text), "ng2_upscalers: FSR message %u: %ls\n", type, message);
  Log(text);
}

// ---- DLSS ---------------------------------------------------------------------------------------

struct Dlss {
  bool tried = false, ready = false;
  VkDevice device = VK_NULL_HANDLE;
  NVSDK_NGX_Parameter* parameters = nullptr;
  NVSDK_NGX_Handle* feature = nullptr;
} g_dlss;

bool DlssInit(const Ng2UpscaleDevice& d) {
  if (g_dlss.tried) return g_dlss.ready;
  g_dlss.tried = true;
  // Licensed for NVIDIA GPUs only.
  const auto properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
      d.get_instance_proc_addr(d.instance, "vkGetPhysicalDeviceProperties"));
  VkPhysicalDeviceProperties p;
  properties(d.physical_device, &p);
  if (p.vendorID != 0x10DE) return false;
  const char* project = "5b2f4a4e-6a0e-4c1b-9f6d-0c6e4e2a7d11";
  if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_Init_with_ProjectID(
          project, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", d.data_path, d.instance, d.physical_device,
          d.device, d.get_instance_proc_addr, d.get_device_proc_addr))) {
    Log("ng2_upscalers: NGX init failed\n");
    return false;
  }
  g_dlss.device = d.device;
  if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_GetCapabilityParameters(&g_dlss.parameters))) return false;
  int available = 0;
  g_dlss.parameters->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available);
  g_dlss.ready = available != 0;
  return g_dlss.ready;
}

NVSDK_NGX_PerfQuality_Value DlssQuality(int quality) {
  switch (quality) {
    case kNg2UpscaleUltraPerformance: return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
    case kNg2UpscalePerformance: return NVSDK_NGX_PerfQuality_Value_MaxPerf;
    case kNg2UpscaleBalanced: return NVSDK_NGX_PerfQuality_Value_Balanced;
    case kNg2UpscaleQuality: return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    default: return NVSDK_NGX_PerfQuality_Value_DLAA;
  }
}

void DlssDestroy() {
  if (g_dlss.feature) NVSDK_NGX_VULKAN_ReleaseFeature(g_dlss.feature);
  g_dlss.feature = nullptr;
}

bool DlssCreate(VkCommandBuffer cb, uint32_t in_w, uint32_t in_h, uint32_t out_w, uint32_t out_h, int quality) {
  DlssDestroy();
  NVSDK_NGX_DLSS_Create_Params create = {};
  create.Feature.InWidth = in_w;
  create.Feature.InHeight = in_h;
  create.Feature.InTargetWidth = out_w;
  create.Feature.InTargetHeight = out_h;
  create.Feature.InPerfQualityValue = DlssQuality(quality);
  create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
  const NVSDK_NGX_Result result =
      NGX_VULKAN_CREATE_DLSS_EXT(cb, 1, 1, &g_dlss.feature, g_dlss.parameters, &create);
  if (NVSDK_NGX_FAILED(result)) {
    char text[80];
    std::snprintf(text, sizeof(text), "ng2_upscalers: DLSS create failed %08X\n", unsigned(result));
    Log(text);
    g_dlss.feature = nullptr;
    return false;
  }
  return true;
}

NVSDK_NGX_Resource_VK DlssResource(const Ng2UpscaleImage& image, bool write) {
  VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  return NVSDK_NGX_Create_ImageView_Resource_VK(image.view, image.image, range, image.format, image.width,
                                                image.height, write);
}

bool DlssEvaluate(const Ng2UpscaleFrame& f) {
  if (!g_dlss.feature) return false;
  NVSDK_NGX_Resource_VK color = DlssResource(f.color, false), depth = DlssResource(f.depth, false);
  NVSDK_NGX_Resource_VK motion = DlssResource(f.motion, false), output = DlssResource(f.output, true);
  NVSDK_NGX_VK_DLSS_Eval_Params eval = {};
  eval.Feature.pInColor = &color;
  eval.Feature.pInOutput = &output;
  eval.pInDepth = &depth;
  eval.pInMotionVectors = &motion;
  eval.InJitterOffsetX = f.jitter_x;
  eval.InJitterOffsetY = f.jitter_y;
  eval.InRenderSubrectDimensions = {f.color.width, f.color.height};
  eval.InReset = f.reset;
  eval.InMVScaleX = float(f.motion.width) * 0.5f;
  eval.InMVScaleY = float(f.motion.height) * 0.5f;
  eval.InFrameTimeDeltaInMsec = f.frame_ms;
  return NVSDK_NGX_SUCCEED(
      NGX_VULKAN_EVALUATE_DLSS_EXT(f.command_buffer, g_dlss.feature, g_dlss.parameters, &eval));
}

// ---- FSR ----------------------------------------------------------------------------------------

struct Fsr {
  bool tried = false, ready = false;
  ffxFunctions functions = {};
  ffxContext context = nullptr;
  VkDeviceContext device = {};
} g_fsr;

bool FsrInit(const Ng2UpscaleDevice& d) {
  if (g_fsr.tried) return g_fsr.ready;
  g_fsr.tried = true;
  HMODULE module = LoadLibraryW(L"amd_fidelityfx_loader_vk.dll");
  if (!module) {
    Log("ng2_upscalers: FSR loader not found\n");
    return false;
  }
  ffxLoadFunctions(&g_fsr.functions, module);
  g_fsr.device = {d.device, d.physical_device, d.get_device_proc_addr};
  g_fsr.ready = g_fsr.functions.CreateContext != nullptr;
  return g_fsr.ready;
}

void FsrDestroy() {
  if (g_fsr.context) g_fsr.functions.DestroyContext(&g_fsr.context, nullptr);
  g_fsr.context = nullptr;
}

bool FsrCreate(uint32_t in_w, uint32_t in_h, uint32_t out_w, uint32_t out_h) {
  FsrDestroy();
  ffxCreateBackendVKDesc backend = {};
  backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
  backend.deviceContext = g_fsr.device;
  ffxCreateContextDescUpscale create = {};
  create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
  create.header.pNext = &backend.header;
  create.flags = FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
  create.maxRenderSize = {in_w, in_h};
  create.maxUpscaleSize = {out_w, out_h};
  create.fpMessage = FfxMessage;
  const ffxReturnCode_t result = g_fsr.functions.CreateContext(&g_fsr.context, &create.header, nullptr);
  if (result != FFX_API_RETURN_OK) {
    char text[80];
    std::snprintf(text, sizeof(text), "ng2_upscalers: FSR create failed %u\n", unsigned(result));
    Log(text);
    g_fsr.context = nullptr;
    return false;
  }
  return true;
}

FfxApiResource FsrResource(const Ng2UpscaleImage& image, uint32_t usage, uint32_t state) {
  FfxApiResourceDescription description = {};
  description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
  description.format = ffxApiGetSurfaceFormatVK(image.format);
  description.width = image.width;
  description.height = image.height;
  description.depth = 1;
  description.mipCount = 1;
  description.usage = usage;
  return ffxApiGetResourceVK(image.image, description, state);
}

bool FsrEvaluate(const Ng2UpscaleFrame& f) {
  if (!g_fsr.context) return false;
  ffxDispatchDescUpscale d = {};
  d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
  d.commandList = f.command_buffer;
  d.color = FsrResource(f.color, FFX_API_RESOURCE_USAGE_READ_ONLY, FFX_API_RESOURCE_STATE_COMPUTE_READ);
  d.depth = FsrResource(f.depth, FFX_API_RESOURCE_USAGE_READ_ONLY, FFX_API_RESOURCE_STATE_COMPUTE_READ);
  d.motionVectors = FsrResource(f.motion, FFX_API_RESOURCE_USAGE_READ_ONLY, FFX_API_RESOURCE_STATE_COMPUTE_READ);
  d.output = FsrResource(f.output, FFX_API_RESOURCE_USAGE_UAV, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
  d.jitterOffset = {f.jitter_x, f.jitter_y};
  d.motionVectorScale = {float(f.motion.width) * 0.5f, float(f.motion.height) * 0.5f};
  d.renderSize = {f.color.width, f.color.height};
  d.upscaleSize = {f.output.width, f.output.height};
  d.enableSharpening = f.sharpness > 0.0f;
  d.sharpness = f.sharpness;
  d.frameTimeDelta = f.frame_ms;
  d.preExposure = 1.0f;
  d.reset = f.reset;
  // inferred: the game's near and far aren't read from it; FSR uses them only for disocclusion.
  d.cameraNear = 0.1f;
  d.cameraFar = 3000.0f;
  d.cameraFovAngleVertical = 1.0f;
  d.viewSpaceToMetersFactor = 1.0f;
  return g_fsr.functions.Dispatch(&g_fsr.context, &d.header) == FFX_API_RETURN_OK;
}

}  // namespace

NG2_EXPORT bool Ng2UpscaleAvailable(int kind, const Ng2UpscaleDevice* device) {
  return kind == kNg2UpscaleDlss ? DlssInit(*device) : kind == kNg2UpscaleFsr ? FsrInit(*device) : false;
}

NG2_EXPORT bool Ng2UpscaleCreate(int kind, const Ng2UpscaleDevice* device, VkCommandBuffer command_buffer,
                                 uint32_t input_width, uint32_t input_height, uint32_t output_width,
                                 uint32_t output_height, int quality) {
  if (!Ng2UpscaleAvailable(kind, device)) return false;
  return kind == kNg2UpscaleDlss
             ? DlssCreate(command_buffer, input_width, input_height, output_width, output_height, quality)
             : FsrCreate(input_width, input_height, output_width, output_height);
}

NG2_EXPORT bool Ng2UpscaleEvaluate(int kind, const Ng2UpscaleFrame* frame) {
  return kind == kNg2UpscaleDlss ? DlssEvaluate(*frame) : FsrEvaluate(*frame);
}

NG2_EXPORT void Ng2UpscaleDestroy(int kind) {
  if (kind == kNg2UpscaleDlss) {
    DlssDestroy();
  } else if (kind == kNg2UpscaleFsr) {
    FsrDestroy();
  }
}

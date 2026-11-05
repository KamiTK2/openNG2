// C interface of ng2_upscalers.dll (tools/upscaler-bridge), which holds all DLSS and FSR code. ng2
// loads it at runtime and needs no vendor headers or libraries, so none reach this BSD source tree
// or a player's build. Prebuilt, it ships with the vendors' own DLLs.
#pragma once

#ifndef VK_VERSION_1_0  // ng2 has the SDK's headers in by now.
#include <vulkan/vulkan.h>
#endif

extern "C" {

enum Ng2UpscaleKind { kNg2UpscaleDlss = 1, kNg2UpscaleFsr = 2 };
// Matches the Recomp Settings row.
enum Ng2UpscaleQuality {
  kNg2UpscaleUltraPerformance,
  kNg2UpscalePerformance,
  kNg2UpscaleBalanced,
  kNg2UpscaleQuality,
  kNg2UpscaleNative,  // DLAA, or FSR Native AA
};

struct Ng2UpscaleDevice {
  VkInstance instance;
  VkPhysicalDevice physical_device;
  VkDevice device;
  PFN_vkGetInstanceProcAddr get_instance_proc_addr;
  PFN_vkGetDeviceProcAddr get_device_proc_addr;
  const wchar_t* data_path;  // Writable, for vendor logs and model updates.
};

struct Ng2UpscaleImage {
  VkImage image;
  VkImageView view;  // Plain 2D view.
  VkFormat format;
  uint32_t width, height;
};

// Inputs must be in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, output in GENERAL.
struct Ng2UpscaleFrame {
  VkCommandBuffer command_buffer;
  Ng2UpscaleImage color;   // Display-referred, as the game's composite leaves it.
  Ng2UpscaleImage depth;   // R32_SFLOAT, 1 is far.
  Ng2UpscaleImage motion;  // R16G16_SFLOAT, last frame's position minus this one's, in NDC.
  Ng2UpscaleImage output;
  float jitter_x, jitter_y;  // Pixels the scene was moved by, y down.
  float frame_ms;
  float sharpness;  // 0..1
  bool reset;       // Camera cut: no history.
};

typedef bool (*Ng2UpscaleAvailableFn)(int kind, const Ng2UpscaleDevice* device);
typedef bool (*Ng2UpscaleCreateFn)(int kind, const Ng2UpscaleDevice* device, VkCommandBuffer command_buffer,
                                   uint32_t input_width, uint32_t input_height, uint32_t output_width,
                                   uint32_t output_height, int quality);
typedef bool (*Ng2UpscaleEvaluateFn)(int kind, const Ng2UpscaleFrame* frame);
typedef void (*Ng2UpscaleDestroyFn)(int kind);
}

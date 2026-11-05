// Compiles every shader in a native pack into a real Vulkan graphics pipeline on the first
// discrete GPU, against the binding contract in native_shaders.h. spirv-val proves the modules are
// legal; this proves the driver takes them and that the contract's layout matches them.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "native/native_shaders.h"

using namespace ng2::native;

namespace {
VkDescriptorSetLayout MakeLayout(VkDevice device, const std::vector<VkDescriptorSetLayoutBinding>& b) {
  VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  info.bindingCount = uint32_t(b.size());
  info.pBindings = b.data();
  VkDescriptorSetLayout layout = VK_NULL_HANDLE;
  vkCreateDescriptorSetLayout(device, &info, nullptr, &layout);
  return layout;
}

VkDescriptorSetLayout TextureLayout(VkDevice device, const NativeShader* shader) {
  std::vector<VkDescriptorSetLayoutBinding> b;
  if (shader) {
    const uint32_t images = uint32_t(shader->textures.size());
    const uint32_t samplers = uint32_t(shader->samplers.size());
    for (uint32_t i = 0; i < images + samplers; ++i) {
      b.push_back({i, i < images ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER, 1,
                   VK_SHADER_STAGE_ALL_GRAPHICS, nullptr});
    }
  }
  return MakeLayout(device, b);
}

VkShaderModule MakeModule(VkDevice device, const NativeShader& shader) {
  VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  info.codeSize = shader.spirv.size() * 4;
  info.pCode = shader.spirv.data();
  VkShaderModule module = VK_NULL_HANDLE;
  vkCreateShaderModule(device, &info, nullptr, &module);
  return module;
}
}  // namespace

int RunVulkanCheck(const char* pack_path) {
  ShaderPack pack;
  std::string error;
  if (!pack.Load(pack_path, &error)) {
    std::printf("cannot load pack: %s\n", error.c_str());
    return 1;
  }

  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.apiVersion = VK_API_VERSION_1_3;
  VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  instance_info.pApplicationInfo = &app;
  VkInstance instance;
  if (vkCreateInstance(&instance_info, nullptr, &instance) != VK_SUCCESS) return 1;
  uint32_t gpu_count = 0;
  vkEnumeratePhysicalDevices(instance, &gpu_count, nullptr);
  std::vector<VkPhysicalDevice> gpus(gpu_count);
  vkEnumeratePhysicalDevices(instance, &gpu_count, gpus.data());
  VkPhysicalDevice gpu = VK_NULL_HANDLE;
  VkPhysicalDeviceProperties props{};
  for (VkPhysicalDevice candidate : gpus) {
    vkGetPhysicalDeviceProperties(candidate, &props);
    if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
      gpu = candidate;
      break;
    }
  }
  if (!gpu) return 1;

  float priority = 1.0f;
  VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  queue.queueCount = 1;
  queue.pQueuePriorities = &priority;
  VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  f13.dynamicRendering = VK_TRUE;
  f13.shaderDemoteToHelperInvocation = VK_TRUE;
  VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  f2.pNext = &f13;
  f2.features.sampleRateShading = VK_TRUE;
  VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  device_info.pNext = &f2;
  device_info.queueCreateInfoCount = 1;
  device_info.pQueueCreateInfos = &queue;
  VkDevice device;
  if (vkCreateDevice(gpu, &device_info, nullptr, &device) != VK_SUCCESS) {
    std::puts("vkCreateDevice failed");
    return 1;
  }
  std::printf("GPU: %s\n", props.deviceName);

  const VkDescriptorSetLayout set0 = MakeLayout(
      device, {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_ALL_GRAPHICS, nullptr}});
  std::vector<VkDescriptorSetLayoutBinding> ubos;
  for (uint32_t i = 0; i < 5; ++i) {
    ubos.push_back({i, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_ALL_GRAPHICS, nullptr});
  }
  const VkDescriptorSetLayout set1 = MakeLayout(device, ubos);

  // Partner vertex shader for pixel shaders: any VS without textures. All VS output every
  // interpolator, so the partner choice does not matter for linkage.
  const NativeShader* partner = nullptr;
  for (const NativeShader& s : pack.shaders()) {
    if (s.stage == ShaderStage::kVertex && s.textures.empty() && !(s.flags & kShaderFlagWmv) &&
        !(s.flags & kShaderFlagNeedsVfetchPatch)) {
      partner = &s;
      break;
    }
  }
  if (!partner) return 1;
  const VkShaderModule partner_module = MakeModule(device, *partner);

  std::map<std::string, int> results;
  const auto all_start = std::chrono::steady_clock::now();
  double slowest = 0;
  for (const NativeShader& s : pack.shaders()) {
    const bool vertex = s.stage == ShaderStage::kVertex;
    const VkDescriptorSetLayout set2 = TextureLayout(device, vertex ? &s : nullptr);
    const VkDescriptorSetLayout set3 = TextureLayout(device, vertex ? nullptr : &s);
    const VkDescriptorSetLayout sets[] = {set0, set1, set2, set3};
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount = 4;
    layout_info.pSetLayouts = sets;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    vkCreatePipelineLayout(device, &layout_info, nullptr, &layout);

    const VkShaderModule module = MakeModule(device, s);
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex ? module : partner_module;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = module;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport viewport{0, 0, 1280, 720, 0, 1};
    VkRect2D scissor{{0, 0}, {1280, 720}};
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    vp.pViewports = &viewport;
    vp.pScissors = &scissor;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.lineWidth = 1.0f;
    rs.rasterizerDiscardEnable = vertex ? VK_TRUE : VK_FALSE;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    uint32_t color_count = 0;
    for (uint32_t i = 0; i < 4; ++i) {
      if (s.color_targets & (1u << i)) color_count = i + 1;
    }
    if (vertex) color_count = 0;
    VkFormat color_formats[4] = {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT,
                                 VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT};
    VkPipelineColorBlendAttachmentState blend[4]{};
    for (auto& b : blend) b.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = color_count;
    cb.pAttachments = blend;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = color_count;
    rendering.pColorAttachmentFormats = color_formats;
    rendering.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;

    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &rendering;
    info.stageCount = vertex ? 1 : 2;
    info.pStages = stages;
    info.pVertexInputState = &vi;
    info.pInputAssemblyState = &ia;
    info.pViewportState = &vp;
    info.pRasterizationState = &rs;
    info.pMultisampleState = &ms;
    info.pDepthStencilState = &ds;
    info.pColorBlendState = &cb;
    info.layout = layout;

    const auto start = std::chrono::steady_clock::now();
    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult result =
        vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
    const double ms_taken =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    slowest = std::max(slowest, ms_taken);
    ++results[std::string(vertex ? "vs " : "ps ") + (result == VK_SUCCESS ? "ok" : "FAIL")];
    if (result != VK_SUCCESS) {
      std::printf("FAIL %016llX result %d\n", (unsigned long long)s.ucode_hash, int(result));
    }
    vkDestroyPipeline(device, pipeline, nullptr);
    vkDestroyShaderModule(device, module, nullptr);
    vkDestroyPipelineLayout(device, layout, nullptr);
    vkDestroyDescriptorSetLayout(device, set2, nullptr);
    vkDestroyDescriptorSetLayout(device, set3, nullptr);
  }
  const double total =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - all_start).count();
  for (const auto& [name, count] : results) std::printf("%-8s %d\n", name.c_str(), count);
  std::printf("total %.1f s, slowest pipeline %.0f ms\n", total, slowest);
  return 0;
}

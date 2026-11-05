// Native Vulkan renderer (notes/native/design.md). The D3D overrides in ng2_native_d3d.cpp call
// these with the guest device; state is read from the device struct at draw time. No PM4, no EDRAM.
#pragma once

#include <cstdint>

namespace ng2::native {

struct DrawCall {
  uint32_t dev = 0;
  uint32_t prim = 0;          // xenos::PrimitiveType
  uint32_t count = 0;         // vertices, or indices when indexed
  uint32_t first_vertex = 0;  // auto-indexed draws
  bool indexed = false;
  uint32_t index_guest = 0;  // virtual address of the first index
  bool index_32 = false;
  uint32_t index_endian = 0;
  // BeginVertices: the engine fills this after the call returns, so the bytes are copied at the
  // next D3D entry. The draw is recorded now, with the state of now.
  bool immediate = false;
  uint32_t immediate_guest = 0;
  uint32_t immediate_dwords = 0;
};

void RendererDraw(const DrawCall& draw);
void RendererClear(uint32_t dev, uint32_t rect_count, uint32_t rects, uint32_t flags, uint32_t argb,
                   float z, uint32_t stencil, uint32_t color_floats = 0);
void RendererResolve(uint32_t dev, uint32_t flags, uint32_t source_rect, uint32_t texture,
                     uint32_t dest_point, uint32_t level, uint32_t clear_color, float clear_z,
                     uint32_t clear_stencil);
// Ends the frame and presents the guest texture at front_texture.
void RendererSwap(uint32_t dev, uint32_t front_texture);
// Immediate vertex bytes the engine wrote since the last call.
void RendererDrainImmediate();
void RendererLogStats(uint64_t swaps);

// The window's client size, for the output of a temporal upscaler. Any thread.
void SetOutputSize(int width, int height);
// True when this PC has a Vulkan 1.3 device. Safe before the GPU plugin is loaded.
bool HostSupportsNative();
// Builds the shader pack from the loaded game image if it's missing or out of date. The pack is
// game-derived, so releases can't ship it. Runs once before the game starts.
void EnsureShaderPack();

}  // namespace ng2::native

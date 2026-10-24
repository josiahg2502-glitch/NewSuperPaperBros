#pragma once

#include "common.hpp"

#include <dolphin/gx/GXEnum.h>

namespace aurora::gfx::tex_copy_conv {

enum class SampleFilter : uint8_t {
  Nearest,
  Linear,
};

struct ConvRequest {
  GXTexFmt fmt;
  wgpu::TextureView srcView; // View of resolved EFB / offscreen color/depth
  Range uniformRange;        // UV transform uniform (offset + scale)
  TextureHandle dst;         // Destination texture
  SampleFilter sampleFilter = SampleFilter::Nearest;
  bool forceOpaqueAlpha = false;
  // A copy whose source reaches past the EFB only has data for the part inside it: that part is drawn 1:1
  // into the top-left of the destination (viewport in destination pixels) instead of stretched over all of it.
  uint32_t viewportWidth = 0;
  uint32_t viewportHeight = 0;
};

bool needs_conversion(GXTexFmt fmt);

void initialize();
void shutdown();
void run(const wgpu::CommandEncoder& cmd, const ConvRequest& req);
void blit(const wgpu::CommandEncoder& cmd, const ConvRequest& req);

} // namespace aurora::gfx::tex_copy_conv

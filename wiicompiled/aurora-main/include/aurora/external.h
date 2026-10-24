#pragma once
#include <webgpu/webgpu.h>
#ifdef __cplusplus
extern "C" {
#endif
// Borrowed handles, valid from successful windowless initialization until shutdown.
WGPUInstance aurora_external_instance(void);
WGPUDevice aurora_external_device(void);
// Render the last complete GX output into a borrowed RGBA8 render-attachment target.
// Called after aurora_end_frame on the same thread, before the next frame begins.
WGPUTexture aurora_external_output(void);
bool aurora_external_healthy(void);
bool aurora_external_copy_output(WGPUTexture target);
#ifdef __cplusplus
}
#endif

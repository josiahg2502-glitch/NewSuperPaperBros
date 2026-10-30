#include <filesystem>
#include "runtime_log.h"
// gx_pixel.cpp - Pixel Processing, Blending, and Fog
#include "gx_internal.h"

// ============================================================================
// Blend Mode
// ============================================================================

extern "C" void GX__SetBlendMode_8017277c(uint32_t t, uint32_t s, uint32_t d, uint32_t op) {
    if (SpmDumpFrame()) RT_LOGF(RT_TAG_GX, "[drawdump] blend %u %u %u %u\n", t, s, d, op);
    GXSetBlendMode(static_cast<GXBlendMode>(t), static_cast<GXBlendFactor>(s),
                   static_cast<GXBlendFactor>(d), static_cast<GXLogicOp>(op));
}
PPC_NATIVE_OVERRIDE_VOID(80290828, GX__SetBlendMode_8017277c, (uint32_t t, uint32_t s, uint32_t d, uint32_t op), (t, s, d, op));

extern "C" void GX__SetColorUpdate_801727cc(uint32_t en) {
    GXSetColorUpdate(static_cast<GXBool>(en));
}
PPC_NATIVE_OVERRIDE_VOID(80290878, GX__SetColorUpdate_801727cc, (uint32_t en), (en));

extern "C" void GX__SetAlphaUpdate_801727f8(uint32_t en) {
    GXSetAlphaUpdate(static_cast<GXBool>(en));
}
PPC_NATIVE_OVERRIDE_VOID(802908a4, GX__SetAlphaUpdate_801727f8, (uint32_t en), (en));

// ============================================================================
// Z Buffer
// ============================================================================

extern "C" void GX__SetZMode_80172824(uint32_t ce, uint32_t f, uint32_t ue) {
    if (SpmDumpFrame()) RT_LOGF(RT_TAG_GX, "[drawdump] zmode %u %u %u\n", ce, f, ue);
    GXSetZMode(static_cast<GXBool>(ce), static_cast<GXCompare>(f), static_cast<GXBool>(ue));
}
PPC_NATIVE_OVERRIDE_VOID(802908D0, GX__SetZMode_80172824, (uint32_t ce, uint32_t f, uint32_t ue), (ce, f, ue));

extern "C" void GX__SetZCompLoc_80172858(uint32_t bt) {
    GXSetZCompLoc(static_cast<GXBool>(bt));
}
PPC_NATIVE_OVERRIDE_VOID(80290904, GX__SetZCompLoc_80172858, (uint32_t bt), (bt));

// ============================================================================
// Pixel Format and Dither
// ============================================================================

extern "C" void GX__SetPixelFmt_80172888(uint32_t pf, uint32_t zf) {
    GXSetPixelFmt(static_cast<GXPixelFmt>(pf), static_cast<GXZFmt16>(zf));
}
PPC_NATIVE_OVERRIDE_VOID(80290934, GX__SetPixelFmt_80172888, (uint32_t pf, uint32_t zf), (pf, zf));

extern "C" void GX__SetDither_80172930(uint32_t d) {
    GXSetDither(static_cast<GXBool>(d));
}
PPC_NATIVE_OVERRIDE_VOID(802909DC, GX__SetDither_80172930, (uint32_t d), (d));

extern "C" void GX__SetDstAlpha_8017295c(uint32_t en, uint32_t a) {
    GXSetDstAlpha(static_cast<GXBool>(en), static_cast<u8>(a));
}
PPC_NATIVE_OVERRIDE_VOID(80290a08, GX__SetDstAlpha_8017295c, (uint32_t en, uint32_t a), (en, a));

// ============================================================================
// Fog and Alpha Compare
// ============================================================================

extern "C" void GX__SetFog_801722cc(uint32_t t, float sz, float ez, float nz, float fz, uint32_t cp) {
    GXSetFog((GXFogType)t, sz, ez, nz, fz, DecodeGxColor(Memory::Read32(cp)));
}
PPC_NATIVE_OVERRIDE_VOID(802904d8, GX__SetFog_801722cc, (uint32_t t, float sz, float ez, float nz, float fz, uint32_t cp), (t, sz, ez, nz, fz, cp));

extern "C" void GX__SetAlphaCompare_80172088(uint32_t c0, uint32_t r0, uint32_t op, uint32_t c1, uint32_t r1) {
    if (SpmDumpFrame()) RT_LOGF(RT_TAG_GX, "[drawdump] alphacmp %u %u %u %u %u\n", c0, r0, op, c1, r1);
    g_alphaCompareValid = true;
    static const bool s_alphaExperiment = std::filesystem::exists("alphatest_off.txt");  // SPM bring-up experiment
    if (s_alphaExperiment && c0 == 6 && r0 == 128) { c0 = 7; op = 1; c1 = 7; }
    GXSetAlphaCompare((GXCompare)c0, (u8)r0, (GXAlphaOp)op, (GXCompare)c1, (u8)r1);
}
PPC_NATIVE_OVERRIDE_VOID(80290294, GX__SetAlphaCompare_80172088, (uint32_t c0, uint32_t r0, uint32_t op, uint32_t c1, uint32_t r1), (c0, r0, op, c1, r1));

extern "C" void GX__SetZTexture_801720c0(uint32_t op, uint32_t f, uint32_t b) { GXSetZTexture((GXZTexOp)op, (GXTexFmt)f, b); }
PPC_NATIVE_OVERRIDE_VOID(802902cc, GX__SetZTexture_801720c0, (uint32_t op, uint32_t f, uint32_t b), (op, f, b));

// ============================================================================
// Culling and Clipping
// ============================================================================

bool SpmAuroraDlRecording();
extern "C" void GX__SetCullMode_8016f3b8(uint32_t m) {
    if (SpmDumpFrame()) RT_LOGF(RT_TAG_GX, "[drawdump] cull %u\n", m);
    GXSetCullMode(static_cast<GXCullMode>(m));
}
PPC_NATIVE_OVERRIDE_VOID(8028d67c, GX__SetCullMode_8016f3b8, (uint32_t m), (m));

extern "C" void GX__SetCoPlanar_8016f3e0(uint32_t en) { GXSetCoPlanar((GXBool)en); }
PPC_NATIVE_OVERRIDE_VOID(8028d6c0, GX__SetCoPlanar_8016f3e0, (uint32_t en), (en));

extern "C" void GX__SetClipMode_8017351c(uint32_t m) { GXSetClipMode((GXClipMode)m); }
PPC_NATIVE_OVERRIDE_VOID(80291b38, GX__SetClipMode_8017351c, (uint32_t m), (m));

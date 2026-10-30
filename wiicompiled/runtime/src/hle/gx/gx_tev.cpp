// gx_tev.cpp - TEV Stage Configuration
#include "gx_internal.h"
#include "runtime_log.h"

// Aurora's CHECK() on these tev array indices compiles to nothing under NDEBUG, so this
// guest->native boundary must reject out-of-range IDs itself; a bad ID here is a malformed
// display list, never legitimate traffic.
namespace {

bool GxTevIdOk(uint32_t value, uint32_t limit, const char* what) {
    if (value < limit) return true;
    RT_LOGF(RT_TAG_GX, "%s out of range: %u (max %u), ignoring\n", what, value, limit - 1u);
    return false;
}

inline bool TevStageOk(uint32_t s) { return GxTevIdOk(s, GX_MAX_TEVSTAGE, "TEV stage"); }
inline bool TevRegOk(uint32_t id) { return GxTevIdOk(id, GX_MAX_TEVREG, "TEV register"); }
inline bool TevKColorOk(uint32_t id) { return GxTevIdOk(id, GX_MAX_KCOLOR, "TEV konstant color"); }
inline bool TevSwapOk(uint32_t id) { return GxTevIdOk(id, GX_MAX_TEVSWAP, "TEV swap selector"); }

} // namespace

// ============================================================================
// TEV Stage Count and Order
// ============================================================================

extern "C" void GX__SetNumTevStages_801722a8(uint32_t n) {
    // GXSetNumTevStages takes a count, not an index, so the inclusive bound is
    // GX_MAX_TEVSTAGE itself.
    if (n > GX_MAX_TEVSTAGE) {
        RT_LOGF(RT_TAG_GX, "GXSetNumTevStages: invalid count %u, ignoring\n", n);
        return;
    }
    GXSetNumTevStages((u8)n);
}
PPC_NATIVE_OVERRIDE_VOID(802904b4, GX__SetNumTevStages_801722a8, (uint32_t n), (n));

extern "C" void GX__SetTevOp_80171c4c(uint32_t s, uint32_t m) {
    if (!TevStageOk(s)) return;
    GXSetTevOp((GXTevStageID)s, (GXTevMode)m);
}
PPC_NATIVE_OVERRIDE_VOID(8028fe58, GX__SetTevOp_80171c4c, (uint32_t s, uint32_t m), (s, m));

bool SpmAuroraDlRecording();
extern "C" void GX__SetTevOrder_8017214c(uint32_t s, uint32_t c, uint32_t m, uint32_t col) {
    if (!TevStageOk(s)) return;
    GXSetTevOrder((GXTevStageID)s, (GXTexCoordID)c, (GXTexMapID)m, (GXChannelID)col);
}
PPC_NATIVE_OVERRIDE_VOID(80290358, GX__SetTevOrder_8017214c, (uint32_t s, uint32_t c, uint32_t m, uint32_t col), (s, c, m, col));

// ============================================================================
// TEV Color/Alpha Inputs
// ============================================================================

extern "C" void GX__SetTevColorIn_80171ce0(uint32_t s, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    if (!TevStageOk(s)) return;
    GXSetTevColorIn((GXTevStageID)s, (GXTevColorArg)a, (GXTevColorArg)b, (GXTevColorArg)c, (GXTevColorArg)d);
}
PPC_NATIVE_OVERRIDE_VOID(8028feec, GX__SetTevColorIn_80171ce0, (uint32_t s, uint32_t a, uint32_t b, uint32_t c, uint32_t d), (s, a, b, c, d));

extern "C" void GX__SetTevAlphaIn_80171d20(uint32_t s, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    if (!TevStageOk(s)) return;
    GXSetTevAlphaIn((GXTevStageID)s, (GXTevAlphaArg)a, (GXTevAlphaArg)b, (GXTevAlphaArg)c, (GXTevAlphaArg)d);
}
PPC_NATIVE_OVERRIDE_VOID(8028ff2c, GX__SetTevAlphaIn_80171d20, (uint32_t s, uint32_t a, uint32_t b, uint32_t c, uint32_t d), (s, a, b, c, d));

// ============================================================================
// TEV Color/Alpha Operations
// ============================================================================

extern "C" void GX__SetTevColorOp_80171d60(uint32_t s, uint32_t op, uint32_t b, uint32_t sc, uint32_t cl, uint32_t or_) {
    if (!TevStageOk(s) || !TevRegOk(or_)) return;
    GXSetTevColorOp((GXTevStageID)s, (GXTevOp)op, (GXTevBias)b, (GXTevScale)sc, (GXBool)cl, (GXTevRegID)or_);
}
PPC_NATIVE_OVERRIDE_VOID(8028ff6c, GX__SetTevColorOp_80171d60, (uint32_t s, uint32_t op, uint32_t b, uint32_t sc, uint32_t cl, uint32_t or_), (s, op, b, sc, cl, or_));

extern "C" void GX__SetTevAlphaOp_80171db8(uint32_t s, uint32_t op, uint32_t b, uint32_t sc, uint32_t cl, uint32_t or_) {
    if (!TevStageOk(s) || !TevRegOk(or_)) return;
    GXSetTevAlphaOp((GXTevStageID)s, (GXTevOp)op, (GXTevBias)b, (GXTevScale)sc, (GXBool)cl, (GXTevRegID)or_);
}
PPC_NATIVE_OVERRIDE_VOID(8028ffc4, GX__SetTevAlphaOp_80171db8, (uint32_t s, uint32_t op, uint32_t b, uint32_t sc, uint32_t cl, uint32_t or_), (s, op, b, sc, cl, or_));

// ============================================================================
// TEV Color Registers
// ============================================================================

extern "C" void GX__SetTevColor_80171e10(uint32_t id, uint32_t cp) {
    if (!TevRegOk(id)) return;
    const uint8_t* p=Memory::GetPointer(cp, 4); GXColor c; c.r=p[0]; c.g=p[1]; c.b=p[2]; c.a=p[3];
    if (SpmDumpFrame()) { const CpuContext* x = TryGetCpuContext(); RT_LOGF(RT_TAG_GX, "[drawdump] tevcolor %u %02X%02X%02X%02X lr %08X\n", id, c.r, c.g, c.b, c.a, x ? x->lr : 0);
        if (x && x->lr == 0x8004ED88u) {
            uint32_t bg = 0, mw = 0, idx = 0, b12 = 0, fl = 0, e26 = 0, e30 = 0;
            Memory::TryRead32(0x8056CF80u, bg); Memory::TryRead32(0x8056D010u, mw);
            if (bg) Memory::TryRead32(bg + 12, b12);
            if (mw) { Memory::TryRead32(mw + 1688, idx); const uint32_t e = mw + idx * 844; Memory::TryRead32(e + 4, fl); Memory::TryRead32(e + 26, e26); Memory::TryRead32(e + 30, e30); }
            RT_LOGF(RT_TAG_GX, "[drawdump] bgcolor bg %08X col %08X map %08X idx %u flags %08X blend %08X blend2 %08X\n", bg, b12, mw, idx, fl, e26, e30);
            uint32_t k0 = 0, k1 = 0, s20 = 0, s24 = 0;
            Memory::TryRead32(0x8056F21Cu, k0); Memory::TryRead32(0x8056F220u, k1);
            Memory::TryRead32(x->gpr[1] + 20, s20); Memory::TryRead32(x->gpr[1] + 24, s24);
            RT_LOGF(RT_TAG_GX, "[drawdump] bgconst %08X %08X stack20 %08X stack24 %08X sp %08X\n", k0, k1, s20, s24, x->gpr[1]);
        } }
    GXSetTevColor((GXTevRegID)id, c);
}
PPC_NATIVE_OVERRIDE_VOID(8029001c, GX__SetTevColor_80171e10, (uint32_t id, uint32_t cp), (id, cp));

extern "C" void GX__SetTevColorS10_80171e70(uint32_t id, uint32_t cp) {
    if (!TevRegOk(id)) return;
    const uint8_t* p=Memory::GetPointer(cp, 8); GXColorS10 c;
    c.r=(p[0]<<8)|p[1]; c.g=(p[2]<<8)|p[3]; c.b=(p[4]<<8)|p[5]; c.a=(p[6]<<8)|p[7];
    if (SpmDumpFrame()) { const CpuContext* x = TryGetCpuContext(); RT_LOGF(RT_TAG_GX, "[drawdump] tevcolorS10 %u %d %d %d %d lr %08X\n", id, (int16_t)c.r, (int16_t)c.g, (int16_t)c.b, (int16_t)c.a, x ? x->lr : 0); }
    GXSetTevColorS10((GXTevRegID)id, c);
}
PPC_NATIVE_OVERRIDE_VOID(8029007c, GX__SetTevColorS10_80171e70, (uint32_t id, uint32_t cp), (id, cp));

extern "C" void GX__SetTevKColor_80171ed4(uint32_t id, uint32_t cp) {
    if (!TevKColorOk(id)) return;
    const uint8_t* p=Memory::GetPointer(cp, 4); GXColor c; c.r=p[0]; c.g=p[1]; c.b=p[2]; c.a=p[3];
    { static int n = 0; const CpuContext* x = TryGetCpuContext();
      if (n++ < 150) RT_LOGF(RT_TAG_GX, "[spmkcol] kcolor %u %02X%02X%02X%02X lr %08X\n", id, c.r, c.g, c.b, c.a, x ? x->lr : 0); }
    GXSetTevKColor((GXTevKColorID)id, c);
}
PPC_NATIVE_OVERRIDE_VOID(802900e0, GX__SetTevKColor_80171ed4, (uint32_t id, uint32_t cp), (id, cp));

extern "C" void GX__SetTevKColorSel_80171f30(uint32_t s, uint32_t sel) { if (!TevStageOk(s)) return; GXSetTevKColorSel((GXTevStageID)s, (GXTevKColorSel)sel); }
PPC_NATIVE_OVERRIDE_VOID(8029013c, GX__SetTevKColorSel_80171f30, (uint32_t s, uint32_t sel), (s, sel));

extern "C" void GX__SetTevKAlphaSel_80171f80(uint32_t s, uint32_t sel) { if (!TevStageOk(s)) return; GXSetTevKAlphaSel((GXTevStageID)s, (GXTevKAlphaSel)sel); }
PPC_NATIVE_OVERRIDE_VOID(8029018c, GX__SetTevKAlphaSel_80171f80, (uint32_t s, uint32_t sel), (s, sel));

// ============================================================================
// TEV Swap Tables
// ============================================================================

extern "C" void GX__SetTevSwapModeTable_8017200c(uint32_t id, uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    if (!TevSwapOk(id)) return;
    GXSetTevSwapModeTable((GXTevSwapSel)id, (GXTevColorChan)r, (GXTevColorChan)g, (GXTevColorChan)b, (GXTevColorChan)a);
}
PPC_NATIVE_OVERRIDE_VOID(80290218, GX__SetTevSwapModeTable_8017200c, (uint32_t id, uint32_t r, uint32_t g, uint32_t b, uint32_t a), (id, r, g, b, a));

extern "C" void GX__SetTevSwapMode_80171fd0(uint32_t s, uint32_t rs, uint32_t ts) {
    if (!TevStageOk(s) || !TevSwapOk(rs) || !TevSwapOk(ts)) return;
    GXSetTevSwapMode((GXTevStageID)s, (GXTevSwapSel)rs, (GXTevSwapSel)ts);
}
PPC_NATIVE_OVERRIDE_VOID(802901dc, GX__SetTevSwapMode_80171fd0, (uint32_t s, uint32_t rs, uint32_t ts), (s, rs, ts));

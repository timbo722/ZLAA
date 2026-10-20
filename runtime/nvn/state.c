/*
 * state.c - NVN's small state objects (blend, depth/stencil, ...), NVNprogram and NVNwindow.
 *
 * State objects are only 4-24 bytes, so some fields are C bitfields: `uint8_t target : 3`
 * means "use 3 bits of a byte for target", and the compiler does the shifting and masking.
 */
#include "nvn.h"
#include <stdio.h>

/* ---- blend state (8 bytes) ---- */


#define BS ((BlendState *)gptr(ARG(0)))
static void nvnBlendStateSetDefaults(CPU *c) {
    *BS = (BlendState){ .premultiplied_src = 1, .normalized_dst = 1, .src_color = 2, .dst_color = 1,
                        .src_alpha = 2, .dst_alpha = 1, .eq_color = 1, .eq_alpha = 1 };
}
static void nvnBlendStateSetBlendTarget(CPU *c) { BS->target = ARG(1) & 7; }
static void nvnBlendStateSetBlendFunc(CPU *c) {
    BS->src_color = (uint8_t)ARG(1); BS->dst_color = (uint8_t)ARG(2);
    BS->src_alpha = (uint8_t)ARG(3); BS->dst_alpha = (uint8_t)ARG(4);
}
static void nvnBlendStateSetBlendEquation(CPU *c) { BS->eq_color = (uint8_t)ARG(1); BS->eq_alpha = (uint8_t)ARG(2); }
static void nvnBlendStateSetAdvancedMode(CPU *c)  { BS->advanced_mode = (uint8_t)ARG(1); }
static void nvnBlendStateSetAdvancedOverlap(CPU *c) { BS->overlap = ARG(1) & 3; }
static void nvnBlendStateSetAdvancedPremultipliedSrc(CPU *c) { BS->premultiplied_src = ARG(1) & 1; }
static void nvnBlendStateSetAdvancedNormalizedDst(CPU *c)    { BS->normalized_dst = ARG(1) & 1; }
static void nvnBlendStateGetBlendTarget(CPU *c)  { RET(BS->target); }
static void nvnBlendStateGetBlendFunc(CPU *c) {
    g_st32(ARG(1), BS->src_color); g_st32(ARG(2), BS->dst_color);
    g_st32(ARG(3), BS->src_alpha); g_st32(ARG(4), BS->dst_alpha);
}
static void nvnBlendStateGetBlendEquation(CPU *c) { g_st32(ARG(1), BS->eq_color); g_st32(ARG(2), BS->eq_alpha); }
static void nvnBlendStateGetAdvancedMode(CPU *c)  { RET(BS->advanced_mode); }
static void nvnBlendStateGetAdvancedOverlap(CPU *c) { RET(BS->overlap); }
static void nvnBlendStateGetAdvancedPremultipliedSrc(CPU *c) { RET(BS->premultiplied_src); }
static void nvnBlendStateGetAdvancedNormalizedDst(CPU *c)    { RET(BS->normalized_dst); }

/* ---- channel mask state (4 bytes): 4 bits (r, g, b, a) per color target ---- */

static void nvnChannelMaskStateSetDefaults(CPU *c) { g_st32(ARG(0), 0xFFFFFFFF); }
/* (state, int target, bool r, bool g, bool b, bool a) */
static void nvnChannelMaskStateSetChannelMask(CPU *c) {
    unsigned shift = (ARG(1) & 7) * 4;
    uint32_t bits = (ARG(2) & 1) | (ARG(3) & 1) << 1 | (ARG(4) & 1) << 2 | (ARG(5) & 1) << 3;
    g_st32(ARG(0), (g_ld32(ARG(0)) & ~(0xFu << shift)) | bits << shift);
}
static void nvnChannelMaskStateGetChannelMask(CPU *c) {
    uint32_t bits = g_ld32(ARG(0)) >> (ARG(1) & 7) * 4;
    for (int i = 0; i < 4; i++) *(uint8_t *)gptr(ARG(2 + i)) = (bits >> i) & 1;
}

/* ---- color state (4 bytes) ---- */

#define CS ((ColorState *)gptr(ARG(0)))
static void nvnColorStateSetDefaults(CPU *c) {
    *CS = (ColorState){ .logic_op = 3, .alpha_test = 8 };   /* COPY, ALWAYS */
}
static void nvnColorStateSetBlendEnable(CPU *c) {
    uint8_t bit = (uint8_t)(1u << (ARG(1) & 7));
    CS->blend_enable = (ARG(2) & 1) ? CS->blend_enable | bit : CS->blend_enable & ~bit;
}
static void nvnColorStateSetLogicOp(CPU *c)     { CS->logic_op = (uint8_t)ARG(1); }
static void nvnColorStateSetAlphaTest(CPU *c)   { CS->alpha_test = (uint8_t)ARG(1); }
static void nvnColorStateGetBlendEnable(CPU *c) { RET((CS->blend_enable >> (ARG(1) & 7)) & 1); }
static void nvnColorStateGetLogicOp(CPU *c)     { RET(CS->logic_op); }
static void nvnColorStateGetAlphaTest(CPU *c)   { RET(CS->alpha_test); }

/* ---- depth/stencil state (8 bytes) ---- */


#define DS ((DepthStencilState *)gptr(ARG(0)))
static void nvnDepthStencilStateSetDefaults(CPU *c) {
    *DS = (DepthStencilState){ .depth_write = 1, .depth_func = 2, .stencil_func = { 8, 8 },
                               .stencil_ops = { 0x111, 0x111 } };          /* KEEP = 1 */
}
static void nvnDepthStencilStateSetDepthTestEnable(CPU *c)   { DS->depth_test = ARG(1) & 1; }
static void nvnDepthStencilStateSetDepthWriteEnable(CPU *c)  { DS->depth_write = ARG(1) & 1; }
static void nvnDepthStencilStateSetDepthFunc(CPU *c)         { DS->depth_func = (uint8_t)ARG(1); }
static void nvnDepthStencilStateSetStencilTestEnable(CPU *c) { DS->stencil_test = ARG(1) & 1; }
/* (state, NVNface: FRONT = 1, BACK = 2, FRONT_AND_BACK = 3, ...) */
static void nvnDepthStencilStateSetStencilFunc(CPU *c) {
    for (int f = 0; f < 2; f++) if (ARG(1) & (1 << f)) DS->stencil_func[f] = (uint8_t)ARG(2);
}
static void nvnDepthStencilStateSetStencilOp(CPU *c) {
    uint16_t ops = (uint16_t)((ARG(2) & 0xF) | (ARG(3) & 0xF) << 4 | (ARG(4) & 0xF) << 8);
    for (int f = 0; f < 2; f++) if (ARG(1) & (1 << f)) DS->stencil_ops[f] = ops;
}
static void nvnDepthStencilStateGetDepthTestEnable(CPU *c)   { RET(DS->depth_test); }
static void nvnDepthStencilStateGetDepthWriteEnable(CPU *c)  { RET(DS->depth_write); }
static void nvnDepthStencilStateGetDepthFunc(CPU *c)         { RET(DS->depth_func); }
static void nvnDepthStencilStateGetStencilTestEnable(CPU *c) { RET(DS->stencil_test); }
static void nvnDepthStencilStateGetStencilFunc(CPU *c)       { RET(DS->stencil_func[ARG(1) == 2]); }
static void nvnDepthStencilStateGetStencilOp(CPU *c) {
    uint16_t ops = DS->stencil_ops[ARG(1) == 2];
    for (int i = 0; i < 3; i++) g_st32(ARG(2 + i), (ops >> 4 * i) & 0xF);
}

/* ---- multisample state (24 bytes) ---- */


#define MS ((MultisampleState *)gptr(ARG(0)))
static void nvnMultisampleStateSetDefaults(CPU *c) { *MS = (MultisampleState){ .enable = 1, .alpha_to_coverage_dither = 1 }; }
static void nvnMultisampleStateSetMultisampleEnable(CPU *c)     { MS->enable = ARG(1) & 1; }
static void nvnMultisampleStateSetSamples(CPU *c)               { MS->samples = (uint32_t)ARG(1); }
static void nvnMultisampleStateSetRasterSamples(CPU *c)         { MS->raster_samples = (uint32_t)ARG(1); }
static void nvnMultisampleStateSetAlphaToCoverageEnable(CPU *c) { MS->alpha_to_coverage = ARG(1) & 1; }
static void nvnMultisampleStateSetAlphaToCoverageDither(CPU *c) { MS->alpha_to_coverage_dither = ARG(1) & 1; }
static void nvnMultisampleStateSetCoverageToColorEnable(CPU *c) { MS->coverage_to_color = ARG(1) & 1; }
static void nvnMultisampleStateSetCoverageToColorOutput(CPU *c) { MS->coverage_to_color_output = (uint8_t)ARG(1); }
static void nvnMultisampleStateSetCoverageModulationMode(CPU *c) { MS->coverage_modulation_mode = (uint8_t)ARG(1); }
static void nvnMultisampleStateSetSampleLocationsEnable(CPU *c) { MS->sample_locations_enable = ARG(1) & 1; }
static void nvnMultisampleStateSetSampleLocationsGridEnable(CPU *c) { MS->sample_locations_grid_enable = ARG(1) & 1; }
static void nvnMultisampleStateGetMultisampleEnable(CPU *c)     { RET(MS->enable); }
static void nvnMultisampleStateGetSamples(CPU *c)               { RET(MS->samples); }
static void nvnMultisampleStateGetRasterSamples(CPU *c)         { RET(MS->raster_samples); }
static void nvnMultisampleStateGetAlphaToCoverageEnable(CPU *c) { RET(MS->alpha_to_coverage); }
static void nvnMultisampleStateGetAlphaToCoverageDither(CPU *c) { RET(MS->alpha_to_coverage_dither); }
static void nvnMultisampleStateGetCoverageToColorEnable(CPU *c) { RET(MS->coverage_to_color); }
static void nvnMultisampleStateGetCoverageToColorOutput(CPU *c) { RET(MS->coverage_to_color_output); }
static void nvnMultisampleStateGetCoverageModulationMode(CPU *c) { RET(MS->coverage_modulation_mode); }
static void nvnMultisampleStateGetSampleLocationsEnable(CPU *c) { RET(MS->sample_locations_enable); }
static void nvnMultisampleStateGetSampleLocationsGridEnable(CPU *c) { RET(MS->sample_locations_grid_enable); }
/* (state, int *width, int *height) */
static void nvnMultisampleStateGetSampleLocationsGrid(CPU *c) { g_st32(ARG(1), 1); g_st32(ARG(2), 1); }

/* ---- polygon state (4 bytes) ---- */

#define PS ((PolygonState *)gptr(ARG(0)))
static void nvnPolygonStateSetDefaults(CPU *c) {
    *PS = (PolygonState){ .front_face = 1, .polygon_mode = 2 };   /* cull NONE, CCW, FILL */
}
static void nvnPolygonStateSetCullFace(CPU *c)             { PS->cull_face = (uint8_t)ARG(1); }
static void nvnPolygonStateSetFrontFace(CPU *c)            { PS->front_face = (uint8_t)ARG(1); }
static void nvnPolygonStateSetPolygonMode(CPU *c)          { PS->polygon_mode = (uint8_t)ARG(1); }
static void nvnPolygonStateSetPolygonOffsetEnables(CPU *c) { PS->offset_enables = (uint8_t)ARG(1); }
static void nvnPolygonStateGetCullFace(CPU *c)             { RET(PS->cull_face); }
static void nvnPolygonStateGetFrontFace(CPU *c)            { RET(PS->front_face); }
static void nvnPolygonStateGetPolygonMode(CPU *c)          { RET(PS->polygon_mode); }
static void nvnPolygonStateGetPolygonOffsetEnables(CPU *c) { RET(PS->offset_enables); }

/* ---- vertex attribute state (4 bytes) and vertex stream state (8 bytes) ---- */

#define VA ((VertexAttribState *)gptr(ARG(0)))
static void nvnVertexAttribStateSetDefaults(CPU *c)    { *VA = (VertexAttribState){ 0 }; }
/* (state, NVNformat, ptrdiff_t relative offset) */
static void nvnVertexAttribStateSetFormat(CPU *c)      { VA->format = ARG(1) & 0xFF; VA->offset = ARG(2) & 0x7FFFF; }
static void nvnVertexAttribStateSetStreamIndex(CPU *c) { VA->stream = ARG(1) & 31; }
static void nvnVertexAttribStateGetFormat(CPU *c)      { g_st32(ARG(1), VA->format); g_st64(ARG(2), VA->offset); }
static void nvnVertexAttribStateGetStreamIndex(CPU *c) { RET(VA->stream); }

#define VS ((VertexStreamState *)gptr(ARG(0)))
static void nvnVertexStreamStateSetDefaults(CPU *c) { *VS = (VertexStreamState){ 0 }; }
static void nvnVertexStreamStateSetStride(CPU *c)   { VS->stride = (uint32_t)ARG(1); }
static void nvnVertexStreamStateSetDivisor(CPU *c)  { VS->divisor = (uint32_t)ARG(1); }
static void nvnVertexStreamStateGetStride(CPU *c)   { RET(VS->stride); }
static void nvnVertexStreamStateGetDivisor(CPU *c)  { RET(VS->divisor); }

/* ---- program ---- */

static void nvnProgramInitialize(CPU *c) { *(Program *)gptr(ARG(0)) = (Program){ 0 }; RET(1); }

/* (program, int count, const NVNshaderData *): each NVNshaderData is {GPU address of the
 * compiled shader, pointer to its control section}. A compiled graphics shader starts with a
 * 0x30-byte NVN header (magic 0x12345678) and then the Maxwell "shader program header", whose
 * first word says which stage it is (bits 10-13). Compute shaders have no program header. */
static void set_shaders(CPU *c, int stride) {
    Program *p = gptr(ARG(0));
    *p = (Program){ 0 };
    for (int i = 0; i < (int)ARG(1); i++) {
        uint64_t entry = ARG(2) + (uint64_t)stride * i;
        uint64_t code = g_ld64(entry), control = g_ld64(entry + 8);
        int stage = STAGE_COMPUTE;
        if (g_ld32(code) == 0x12345678) {
            static const int by_type[16] = { [1] = STAGE_VERTEX, [2] = STAGE_TESS_CONTROL, [3] = STAGE_TESS_EVAL,
                                             [4] = STAGE_GEOMETRY, [5] = STAGE_FRAGMENT };
            uint32_t type = (g_ld32(code + 0x30) >> 10) & 0xF;
            stage = type >= 1 && type <= 5 ? by_type[type] : STAGE_COMPUTE;
        }
        p->code[stage] = code;
        p->control[stage] = control;
        p->stages |= 1u << stage;
    }
    RET(1);
}
static void nvnProgramSetShaders(CPU *c)    { set_shaders(c, 16); }
static void nvnProgramSetShadersExt(CPU *c) { set_shaders(c, 24); }   /* NVNshaderDataExt adds a hash */

/* ---- window: the textures the game renders into and presents ---- */

#define WB ((WindowBuilder *)gptr(ARG(0)))
static void nvnWindowBuilderSetDevice(CPU *c)  { WB->device = ARG(1); }
static void nvnWindowBuilderSetDefaults(CPU *c) {
    uint64_t device = WB->device;
    *WB = (WindowBuilder){ .device = device, .present_interval = 1 };
}
static void nvnWindowBuilderSetNativeWindow(CPU *c)     { WB->native_window = ARG(1); }
/* (builder, int count, NVNtexture * const *textures) */
static void nvnWindowBuilderSetTextures(CPU *c)         { WB->num_textures = (uint32_t)ARG(1); WB->textures = ARG(2); }
static void nvnWindowBuilderSetPresentInterval(CPU *c)  { WB->present_interval = (uint32_t)ARG(1); }
static void nvnWindowBuilderSetNumActiveTextures(CPU *c) { WB->num_active = (uint32_t)ARG(1); }
static void nvnWindowBuilderGetDevice(CPU *c)           { RET(WB->device); }
static void nvnWindowBuilderGetNativeWindow(CPU *c)     { RET(WB->native_window); }
static void nvnWindowBuilderGetNumTextures(CPU *c)      { RET(WB->num_textures); }
static void nvnWindowBuilderGetPresentInterval(CPU *c)  { RET(WB->present_interval); }
static void nvnWindowBuilderGetNumActiveTextures(CPU *c) { RET(WB->num_active); }
static void nvnWindowBuilderGetTexture(CPU *c) {
    RET(ARG(1) < WB->num_textures ? g_ld64(WB->textures + 8 * ARG(1)) : 0);
}

static void nvnWindowInitialize(CPU *c) {
    WindowBuilder *b = gptr(ARG(1));
    Window *w = gptr(ARG(0));
    *w = (Window){ .native_window = b->native_window, .present_interval = b->present_interval };
    w->num_textures = b->num_textures < 8 ? b->num_textures : 8;
    for (uint32_t i = 0; i < w->num_textures; i++) w->textures[i] = g_ld64(b->textures + 8 * i);
    w->num_active = b->num_active ? b->num_active : w->num_textures;
    Texture *t = w->num_textures ? gptr(w->textures[0]) : NULL;
    fprintf(stderr, "nvn: window with %u textures (%ux%u, format %#x), present interval %u\n",
            w->num_textures, t ? t->d.width : 0, t ? t->d.height : 0, t ? t->d.format : 0, w->present_interval);
    RET(1);
}
#define WIN ((Window *)gptr(ARG(0)))
static void nvnWindowGetNativeWindow(CPU *c)     { RET(WIN->native_window); }
static void nvnWindowGetNumTextures(CPU *c)      { RET(WIN->num_textures); }
static void nvnWindowGetNumActiveTextures(CPU *c) { RET(WIN->num_active); }
static void nvnWindowSetNumActiveTextures(CPU *c) { WIN->num_active = (uint32_t)ARG(1); }
static void nvnWindowGetPresentInterval(CPU *c)  { RET(WIN->present_interval); }
static void nvnWindowSetPresentInterval(CPU *c)  { WIN->present_interval = (uint32_t)ARG(1); }
/* (window, x, y, width, height) */
static void nvnWindowSetCrop(CPU *c)             { for (int i = 0; i < 4; i++) WIN->crop[i] = (int32_t)ARG(1 + i); }
static void nvnWindowGetCrop(CPU *c)             { memcpy(gptr(ARG(1)), WIN->crop, 16); }

/* (window or queue+window, sync, int *index) -> NVNwindowAcquireTextureResult (0 = success).
 * Textures are handed out in turn; our presents finish at once, so each is free again. */
static uint32_t acquire(Window *w, uint64_t sync, uint64_t index_out) {
    if (sync) ((Sync *)gptr(sync))->signaled = 1;
    g_st32(index_out, w->next);
    w->next = (w->next + 1) % (w->num_active ? w->num_active : 1);
    return 0;
}
static void nvnWindowAcquireTexture(CPU *c) { RET(acquire(WIN, ARG(1), ARG(2))); }
static void nvnQueueAcquireTexture(CPU *c)  { RET(acquire(gptr(ARG(1)), 0, ARG(2))); }
/* (queue, window, int index) */
static void nvnQueuePresentTexture(CPU *c) {
    Window *w = gptr(ARG(1));
    uint32_t i = (uint32_t)ARG(2);
    nvn_present(w, i < w->num_textures ? w->textures[i] : 0);
}

const NvnEntry nvn_state_functions[] = {
    NVN_ENTRY(nvnBlendStateSetDefaults), NVN_ENTRY(nvnBlendStateSetBlendTarget), NVN_ENTRY(nvnBlendStateSetBlendFunc),
    NVN_ENTRY(nvnBlendStateSetBlendEquation), NVN_ENTRY(nvnBlendStateSetAdvancedMode),
    NVN_ENTRY(nvnBlendStateSetAdvancedOverlap), NVN_ENTRY(nvnBlendStateSetAdvancedPremultipliedSrc),
    NVN_ENTRY(nvnBlendStateSetAdvancedNormalizedDst), NVN_ENTRY(nvnBlendStateGetBlendTarget),
    NVN_ENTRY(nvnBlendStateGetBlendFunc), NVN_ENTRY(nvnBlendStateGetBlendEquation),
    NVN_ENTRY(nvnBlendStateGetAdvancedMode), NVN_ENTRY(nvnBlendStateGetAdvancedOverlap),
    NVN_ENTRY(nvnBlendStateGetAdvancedPremultipliedSrc), NVN_ENTRY(nvnBlendStateGetAdvancedNormalizedDst),

    NVN_ENTRY(nvnChannelMaskStateSetDefaults), NVN_ENTRY(nvnChannelMaskStateSetChannelMask),
    NVN_ENTRY(nvnChannelMaskStateGetChannelMask),

    NVN_ENTRY(nvnColorStateSetDefaults), NVN_ENTRY(nvnColorStateSetBlendEnable), NVN_ENTRY(nvnColorStateSetLogicOp),
    NVN_ENTRY(nvnColorStateSetAlphaTest), NVN_ENTRY(nvnColorStateGetBlendEnable), NVN_ENTRY(nvnColorStateGetLogicOp),
    NVN_ENTRY(nvnColorStateGetAlphaTest),

    NVN_ENTRY(nvnDepthStencilStateSetDefaults), NVN_ENTRY(nvnDepthStencilStateSetDepthTestEnable),
    NVN_ENTRY(nvnDepthStencilStateSetDepthWriteEnable), NVN_ENTRY(nvnDepthStencilStateSetDepthFunc),
    NVN_ENTRY(nvnDepthStencilStateSetStencilTestEnable), NVN_ENTRY(nvnDepthStencilStateSetStencilFunc),
    NVN_ENTRY(nvnDepthStencilStateSetStencilOp), NVN_ENTRY(nvnDepthStencilStateGetDepthTestEnable),
    NVN_ENTRY(nvnDepthStencilStateGetDepthWriteEnable), NVN_ENTRY(nvnDepthStencilStateGetDepthFunc),
    NVN_ENTRY(nvnDepthStencilStateGetStencilTestEnable), NVN_ENTRY(nvnDepthStencilStateGetStencilFunc),
    NVN_ENTRY(nvnDepthStencilStateGetStencilOp),

    NVN_ENTRY(nvnMultisampleStateSetDefaults), NVN_ENTRY(nvnMultisampleStateSetMultisampleEnable),
    NVN_ENTRY(nvnMultisampleStateSetSamples), NVN_ENTRY(nvnMultisampleStateSetRasterSamples),
    NVN_ENTRY(nvnMultisampleStateSetAlphaToCoverageEnable), NVN_ENTRY(nvnMultisampleStateSetAlphaToCoverageDither),
    NVN_ENTRY(nvnMultisampleStateSetCoverageToColorEnable), NVN_ENTRY(nvnMultisampleStateSetCoverageToColorOutput),
    NVN_ENTRY(nvnMultisampleStateSetCoverageModulationMode), NVN_ENTRY(nvnMultisampleStateSetSampleLocationsEnable),
    NVN_ENTRY(nvnMultisampleStateSetSampleLocationsGridEnable), { "nvnMultisampleStateSetSampleLocations", nvn_nothing },
    NVN_ENTRY(nvnMultisampleStateGetMultisampleEnable), NVN_ENTRY(nvnMultisampleStateGetSamples),
    NVN_ENTRY(nvnMultisampleStateGetRasterSamples), NVN_ENTRY(nvnMultisampleStateGetAlphaToCoverageEnable),
    NVN_ENTRY(nvnMultisampleStateGetAlphaToCoverageDither), NVN_ENTRY(nvnMultisampleStateGetCoverageToColorEnable),
    NVN_ENTRY(nvnMultisampleStateGetCoverageToColorOutput), NVN_ENTRY(nvnMultisampleStateGetCoverageModulationMode),
    NVN_ENTRY(nvnMultisampleStateGetSampleLocationsEnable), NVN_ENTRY(nvnMultisampleStateGetSampleLocationsGridEnable),
    NVN_ENTRY(nvnMultisampleStateGetSampleLocationsGrid),

    NVN_ENTRY(nvnPolygonStateSetDefaults), NVN_ENTRY(nvnPolygonStateSetCullFace), NVN_ENTRY(nvnPolygonStateSetFrontFace),
    NVN_ENTRY(nvnPolygonStateSetPolygonMode), NVN_ENTRY(nvnPolygonStateSetPolygonOffsetEnables),
    NVN_ENTRY(nvnPolygonStateGetCullFace), NVN_ENTRY(nvnPolygonStateGetFrontFace),
    NVN_ENTRY(nvnPolygonStateGetPolygonMode), NVN_ENTRY(nvnPolygonStateGetPolygonOffsetEnables),

    NVN_ENTRY(nvnVertexAttribStateSetDefaults), NVN_ENTRY(nvnVertexAttribStateSetFormat),
    NVN_ENTRY(nvnVertexAttribStateSetStreamIndex), NVN_ENTRY(nvnVertexAttribStateGetFormat),
    NVN_ENTRY(nvnVertexAttribStateGetStreamIndex),
    NVN_ENTRY(nvnVertexStreamStateSetDefaults), NVN_ENTRY(nvnVertexStreamStateSetStride),
    NVN_ENTRY(nvnVertexStreamStateSetDivisor), NVN_ENTRY(nvnVertexStreamStateGetStride),
    NVN_ENTRY(nvnVertexStreamStateGetDivisor),

    NVN_ENTRY(nvnProgramInitialize), { "nvnProgramFinalize", nvn_nothing }, { "nvnProgramSetDebugLabel", nvn_nothing },
    NVN_ENTRY(nvnProgramSetShaders), NVN_ENTRY(nvnProgramSetShadersExt),
    { "nvnProgramSetSampleShading", nvn_nothing }, { "nvnProgramSetSubroutineLinkage", nvn_true },

    NVN_ENTRY(nvnWindowBuilderSetDevice), NVN_ENTRY(nvnWindowBuilderSetDefaults),
    NVN_ENTRY(nvnWindowBuilderSetNativeWindow), NVN_ENTRY(nvnWindowBuilderSetTextures),
    NVN_ENTRY(nvnWindowBuilderSetPresentInterval), NVN_ENTRY(nvnWindowBuilderSetNumActiveTextures),
    NVN_ENTRY(nvnWindowBuilderGetDevice), NVN_ENTRY(nvnWindowBuilderGetNativeWindow),
    NVN_ENTRY(nvnWindowBuilderGetNumTextures), NVN_ENTRY(nvnWindowBuilderGetPresentInterval),
    NVN_ENTRY(nvnWindowBuilderGetNumActiveTextures), NVN_ENTRY(nvnWindowBuilderGetTexture),
    NVN_ENTRY(nvnWindowInitialize), { "nvnWindowFinalize", nvn_nothing }, { "nvnWindowSetDebugLabel", nvn_nothing },
    NVN_ENTRY(nvnWindowGetNativeWindow), NVN_ENTRY(nvnWindowGetNumTextures), NVN_ENTRY(nvnWindowGetNumActiveTextures),
    NVN_ENTRY(nvnWindowSetNumActiveTextures), NVN_ENTRY(nvnWindowGetPresentInterval),
    NVN_ENTRY(nvnWindowSetPresentInterval), NVN_ENTRY(nvnWindowSetCrop), NVN_ENTRY(nvnWindowGetCrop),
    NVN_ENTRY(nvnWindowAcquireTexture), NVN_ENTRY(nvnQueueAcquireTexture), NVN_ENTRY(nvnQueuePresentTexture),
    { NULL, NULL },
};
